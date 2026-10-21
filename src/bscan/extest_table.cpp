// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/extest_table.h"

namespace bscan
{

void ExtestTable::RequestEnter( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                                const Bits& aImage )
{
    Entry e;
    e.tap    = aTap;
    e.device = std::move( aDevice );
    e.image  = aImage;
    e.phase  = Phase::PendingEnter;

    std::lock_guard lock( m_mutex );
    m_entries[aTap.name] = std::move( e );
}


void ExtestTable::RequestTakeOver( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                                   const Bits& aImage )
{
    std::lock_guard lock( m_mutex );
    Entry&          e = m_entries[aTap.name];

    e.tap           = aTap;
    e.device        = std::move( aDevice );
    e.image         = aImage;
    e.phase         = Phase::PendingTakeOver;
    e.dirty         = false;
    e.lost          = false;
    e.interruptions = 0;
}


void ExtestTable::RequestForceLeave( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice )
{
    std::lock_guard lock( m_mutex );
    Entry&          e = m_entries[aTap.name];

    e.tap    = aTap;
    e.device = std::move( aDevice );
    e.phase  = Phase::PendingLeave;
    e.lost   = false;
}


bool ExtestTable::RequestImage( const std::string& aTap, const Bits& aImage )
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );

    if( it == m_entries.end() || it->second.phase == Phase::PendingLeave )
        return false;

    it->second.image = aImage;
    it->second.dirty = true;
    return true;
}


ExtestTable::LeaveRequest ExtestTable::RequestLeave( const std::string& aTap )
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );

    if( it == m_entries.end() )
        return LeaveRequest::Unknown;

    if( it->second.phase == Phase::PendingEnter )
    {
        m_entries.erase( it );   // never entered
        return LeaveRequest::Cancelled;
    }

    it->second.phase = Phase::PendingLeave;
    return LeaveRequest::Pending;
}


ExtestTable::Entries ExtestTable::All() const
{
    std::lock_guard lock( m_mutex );
    return m_entries;
}


ExtestTable::Entries ExtestTable::InPhase( Phase aPhase ) const
{
    std::lock_guard lock( m_mutex );
    Entries         out;

    for( const auto& [name, e] : m_entries )
        if( e.phase == aPhase )
            out[name] = e;

    return out;
}


ExtestTable::Entries ExtestTable::Active() const
{
    std::lock_guard lock( m_mutex );
    Entries         out;

    for( const auto& [name, e] : m_entries )
        if( e.phase == Phase::Active && !e.lost )
            out[name] = e;

    return out;
}


std::string ExtestTable::OtherHeld( const std::string& aTap ) const
{
    std::lock_guard lock( m_mutex );

    for( const auto& [name, e] : m_entries )
        if( name != aTap && e.phase != Phase::PendingEnter && e.phase != Phase::PendingTakeOver )
            return name;

    return {};
}


bool ExtestTable::Contains( const std::string& aTap ) const
{
    std::lock_guard lock( m_mutex );
    return m_entries.contains( aTap );
}


bool ExtestTable::Empty() const
{
    std::lock_guard lock( m_mutex );
    return m_entries.empty();
}


bool ExtestTable::WorkPending() const
{
    std::lock_guard lock( m_mutex );

    for( const auto& [name, e] : m_entries )
        if( e.phase != Phase::Active || e.dirty )
            return true;

    return false;
}


bool ExtestTable::Settle( const std::string& aTap, Phase aPending, bool aOk )
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );

    if( it == m_entries.end() )
        return false;

    if( !aOk )
        m_entries.erase( it );
    else if( it->second.phase == aPending )
        it->second.phase = Phase::Active;

    return true;
}


void ExtestTable::SettleIfPending( const std::string& aTap, Phase aPending, bool aOk )
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );

    if( it == m_entries.end() || it->second.phase != aPending )
        return;

    if( aOk )
        it->second.phase = Phase::Active;
    else
        m_entries.erase( it );
}


void ExtestTable::PutActive( Entry aEntry )
{
    aEntry.phase = Phase::Active;

    std::lock_guard lock( m_mutex );
    std::string     name = aEntry.tap.name;
    m_entries[name]      = std::move( aEntry );
}


void ExtestTable::Erase( const std::string& aTap )
{
    std::lock_guard lock( m_mutex );
    m_entries.erase( aTap );
}


void ExtestTable::SetLost( const std::string& aTap )
{
    std::lock_guard lock( m_mutex );

    if( auto it = m_entries.find( aTap ); it != m_entries.end() )
        it->second.lost = true;
}


void ExtestTable::MarkActiveLost()
{
    std::lock_guard lock( m_mutex );

    for( auto& [name, e] : m_entries )
        if( e.phase == Phase::Active )
            e.lost = true;
}


void ExtestTable::LeaveLost()
{
    std::lock_guard lock( m_mutex );

    for( auto& [name, e] : m_entries )
        if( e.lost && e.phase == Phase::Active )
            e.phase = Phase::PendingLeave;
}


void ExtestTable::Shifted( const std::string& aTap, const Bits& aImage )
{
    std::lock_guard lock( m_mutex );

    if( auto it = m_entries.find( aTap ); it != m_entries.end() && it->second.image == aImage )
        it->second.dirty = false;
}


bool ExtestTable::Dirty( const std::string& aTap ) const
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );
    return it != m_entries.end() && it->second.dirty;
}


std::optional<int> ExtestTable::Interrupted( const std::string& aTap )
{
    std::lock_guard lock( m_mutex );
    auto            it = m_entries.find( aTap );

    if( it == m_entries.end() || it->second.phase != Phase::Active )
        return std::nullopt;

    it->second.dirty = true;
    return ++it->second.interruptions;
}


ExtestTable::Entries ExtestTable::TakeAll()
{
    std::lock_guard lock( m_mutex );
    Entries         out;
    out.swap( m_entries );
    return out;
}

} // namespace bscan
