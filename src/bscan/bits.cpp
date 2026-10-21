// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/bits.h"

namespace bscan
{

static int hexValue( char c )
{
    if( c >= '0' && c <= '9' )
        return c - '0';
    if( c >= 'a' && c <= 'f' )
        return c - 'a' + 10;
    if( c >= 'A' && c <= 'F' )
        return c - 'A' + 10;
    return -1;
}


std::optional<Bits> Bits::FromHex( std::string_view aHex, size_t aCount )
{
    if( aHex.starts_with( "0x" ) || aHex.starts_with( "0X" ) )
        aHex.remove_prefix( 2 );

    while( !aHex.empty() && ( aHex.back() == '\n' || aHex.back() == '\r' || aHex.back() == ' ' ) )
        aHex.remove_suffix( 1 );

    if( aHex.empty() )
        return std::nullopt;

    Bits   out( aCount );
    size_t bit = 0;

    for( auto it = aHex.rbegin(); it != aHex.rend(); ++it )
    {
        int v = hexValue( *it );

        if( v < 0 )
            return std::nullopt;

        for( int k = 0; k < 4; ++k, ++bit )
        {
            bool b = ( v >> k ) & 1;

            if( bit < aCount )
                out.m_bits[bit] = b;
            else if( b )
                return std::nullopt;   // value wider than aCount
        }
    }

    return out;
}


std::optional<Bits> Bits::FromBitString( std::string_view aStr, bool aX )
{
    Bits out( aStr.size() );

    for( size_t i = 0; i < aStr.size(); ++i )
    {
        char   c   = aStr[aStr.size() - 1 - i];
        bool   v   = false;

        if( c == '1' )
            v = true;
        else if( c == 'X' || c == 'x' )
            v = aX;
        else if( c != '0' )
            return std::nullopt;

        out.m_bits[i] = v;
    }

    return out;
}


std::string Bits::ToHex() const
{
    static const char digits[] = "0123456789abcdef";
    size_t            nibbles  = ( m_bits.size() + 3 ) / 4;
    std::string       s;

    for( size_t n = nibbles; n-- > 0; )
    {
        int v = 0;

        for( int k = 3; k >= 0; --k )
        {
            size_t i = n * 4 + static_cast<size_t>( k );
            v        = ( v << 1 ) | ( i < m_bits.size() && m_bits[i] ? 1 : 0 );
        }

        s += digits[v];
    }

    return s.empty() ? std::string( "0" ) : s;
}


std::string Bits::ToBitString() const
{
    std::string s;

    for( size_t i = m_bits.size(); i-- > 0; )
        s += m_bits[i] ? '1' : '0';

    return s;
}


unsigned long long Bits::ToU64() const
{
    unsigned long long v = 0;

    for( size_t i = 0; i < m_bits.size() && i < 64; ++i )
        if( m_bits[i] )
            v |= 1ULL << i;

    return v;
}

} // namespace bscan
