// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/interconnect.h"

#include <algorithm>

namespace bscan
{

std::map<std::string, PinTestRole> PinTestRoles( const InterconnectPlan& aPlan, int aDevice )
{
    std::map<std::string, PinTestRole> roles;

    for( const TestNet& n : aPlan.nets )
    {
        for( size_t i = 0; i < n.bsPins.size(); ++i )
        {
            if( n.bsPins[i].device != aDevice )
                continue;

            PinTestRole& r = roles[n.bsPins[i].pad];
            r.net          = n.name;
            r.reason       = n.reason;

            if( n.status == TestNet::Status::Tested )
                r.kind = PinTestRole::Kind::Tested;
            else if( std::ranges::find( n.fixedBy, i ) != n.fixedBy.end() )
            {
                r.kind         = PinTestRole::Kind::FixedHere;
                r.currentState = aPlan.devices[aDevice].currentState.contains( n.bsPins[i].pad );
            }
            else
                r.kind = PinTestRole::Kind::NotTested;
        }
    }

    return roles;
}


const char* ToString( TestNet::Status aStatus )
{
    switch( aStatus )
    {
    case TestNet::Status::Tested:       return "tested";
    case TestNet::Status::FewPins:      return "few pins";
    case TestNet::Status::OtherParts:   return "other parts";
    case TestNet::Status::Fixed:        return "fixed";
    case TestNet::Status::NoDriver:     return "no driver";
    case TestNet::Status::SharedEnable: return "shared enable";
    }

    return "?";
}


size_t InterconnectPlan::TestedCount() const
{
    return std::ranges::count_if( nets, []( const TestNet& n )
                                  { return n.status == TestNet::Status::Tested; } );
}


static std::string pinLabel( const NetPin& aPin )
{
    return aPin.reference + " pin " + aPin.pad;
}


static bool canDisable( const PinCells& aCells )
{
    return aCells.controlCell >= 0 && aCells.disableValue >= 0;
}


static bool bitAt( const Bits& aBits, int aCell )
{
    return aCell >= 0 && static_cast<size_t>( aCell ) < aBits.size()
           && aBits[static_cast<size_t>( aCell )];
}


static void setBit( Bits& aBits, int aCell, bool aValue )
{
    if( aCell >= 0 && static_cast<size_t>( aCell ) < aBits.size() )
        aBits.set( static_cast<size_t>( aCell ), aValue );
}


// Pins of a tested net that are not the driver and can receive
static std::vector<size_t> receivers( const TestNet& aNet )
{
    std::vector<size_t> out;

    for( size_t i = 0; i < aNet.bsPins.size(); ++i )
        if( static_cast<int>( i ) != aNet.driver && aNet.bsPins[i].cells.inputCell >= 0 )
            out.push_back( i );

    return out;
}


// Can aPin drive the net while all other boundary-scan pins listen? Empty
// if yes, otherwise why not.
static std::string driverProblem( const TestNet& aNet, size_t aDriver )
{
    const NetPin& d = aNet.bsPins[aDriver];

    if( d.cells.outputCell < 0 )
        return "no output cell";

    bool receiver = false;

    for( size_t i = 0; i < aNet.bsPins.size(); ++i )
    {
        if( i == aDriver )
            continue;

        const PinCells& c = aNet.bsPins[i].cells;

        // A 2-state output (no output enable) always drives
        if( c.outputCell >= 0 && !canDisable( c ) )
            return pinLabel( aNet.bsPins[i] ) + " always drives (2-state output)";

        receiver = receiver || c.inputCell >= 0;
    }

    return receiver ? std::string() : "no other pin can receive";
}


InterconnectPlan PlanInterconnect( std::vector<IcDevice> aDevices, const PadNets& aPadNets,
                                   const std::set<std::string>& aIncluded )
{
    InterconnectPlan plan;
    plan.devices = std::move( aDevices );

    std::map<std::string, int> byRef;

    for( size_t i = 0; i < plan.devices.size(); ++i )
        if( !plan.devices[i].reference.empty() && plan.devices[i].device )
            byRef[plan.devices[i].reference] = static_cast<int>( i );

    // --- nets: boundary-scan pins and other parts ---
    std::map<std::string, TestNet> nets;

    for( const auto& [ref, pads] : aPadNets )
    {
        auto dev = byRef.find( ref );

        for( const auto& [pad, net] : pads )
        {
            if( net.empty() )
                continue;

            TestNet& n = nets[net];
            n.name     = net;

            std::optional<PinCells> cells;

            if( dev != byRef.end() )
            {
                const IcDevice& d = plan.devices[dev->second];
                cells             = d.device->LookupPin( d.pinMap, pad );
            }

            if( cells && cells->HasCells() )
                n.bsPins.push_back( { ref, pad, dev->second, *cells } );
            else if( std::ranges::find( n.otherParts, ref ) == n.otherParts.end() )
                n.otherParts.push_back( ref );   // incl. pins of test devices without cells
        }
    }

    for( auto& [name, n] : nets )
    {
        if( n.bsPins.empty() )
            continue;

        std::ranges::sort( n.otherParts );

        if( n.bsPins.size() < 2 )
        {
            n.status = TestNet::Status::FewPins;
            n.reason = "only one boundary-scan pin";
        }
        else
        {
            n.status = TestNet::Status::Tested;

            // Other parts first: then the starting states do not matter
            if( n.Mixed() && !aIncluded.contains( name ) )
            {
                n.status = TestNet::Status::OtherParts;
                n.reason = "other parts on the net:";

                for( const std::string& r : n.otherParts )
                    n.reason += " " + r;
            }

            for( size_t i = 0; i < n.bsPins.size() && n.status != TestNet::Status::OtherParts; ++i )
            {
                const NetPin& p           = n.bsPins[i];
                const auto&   constraints = plan.devices[p.device].constraints;
                auto          c           = constraints.find( p.pad );

                if( c == constraints.end() )
                    continue;

                bool now = plan.devices[p.device].currentState.contains( p.pad );
                n.reason += std::string( n.fixedBy.empty() ? "fixed by " : ", " )
                            + ( now ? "the current EXTEST state of " : "the starting state of " )
                            + pinLabel( p ) + " (" + ToString( c->second ) + ")";
                n.fixedBy.push_back( i );
                n.status = TestNet::Status::Fixed;
            }

            if( n.status == TestNet::Status::Tested )
            {
                std::string problem;

                for( size_t i = 0; i < n.bsPins.size() && n.driver < 0; ++i )
                {
                    std::string p = driverProblem( n, i );

                    if( p.empty() )
                        n.driver = static_cast<int>( i );
                    else if( problem.empty() || n.bsPins[i].cells.outputCell >= 0 )
                        problem = p;
                }

                if( n.driver < 0 )
                {
                    n.status = TestNet::Status::NoDriver;
                    n.reason = problem.empty() ? "no pin can drive" : problem;
                }
            }
        }

        plan.nets.push_back( std::move( n ) );
    }

    // --- shared output enables: enabling the driver must not make another
    //     pin drive that should not (receiver, or pin outside the test whose
    //     output is off) ---
    // device -> control cell -> pins (by pad) with that control cell
    std::vector<std::map<int, std::vector<OutputPin>>> groups( plan.devices.size() );

    for( size_t d = 0; d < plan.devices.size(); ++d )
    {
        if( !plan.devices[d].device )
            continue;

        for( const OutputPin& p : OutputPins( *plan.devices[d].device, plan.devices[d].pinMap ) )
            if( canDisable( p.cells ) )
                groups[d][p.cells.controlCell].push_back( p );
    }

    for( bool changed = true; changed; )
    {
        changed = false;

        // Role of every pin of a tested net: driver or receiver
        std::map<std::pair<int, std::string>, bool> isDriver;

        for( const TestNet& n : plan.nets )
            if( n.status == TestNet::Status::Tested )
                for( size_t i = 0; i < n.bsPins.size(); ++i )
                    isDriver[{ n.bsPins[i].device, n.bsPins[i].pad }] = static_cast<int>( i ) == n.driver;

        for( TestNet& n : plan.nets )
        {
            if( n.status != TestNet::Status::Tested )
                continue;

            auto conflict = [&]( size_t aDriver ) -> std::string
            {
                const NetPin& d = n.bsPins[aDriver];

                if( !canDisable( d.cells ) )
                    return {};

                const IcDevice& dev = plan.devices[d.device];
                bool            enabled =
                        bitAt( dev.baseImage, d.cells.controlCell ) != ( d.cells.disableValue != 0 );

                for( const OutputPin& q : groups[d.device][d.cells.controlCell] )
                {
                    if( q.pin == d.pad )
                        continue;

                    auto role = isDriver.find( { d.device, q.pin } );

                    if( role != isDriver.end() && role->second )
                        continue;   // drives its own net: fine

                    // A receiver, or a pin outside the test that is off now
                    if( role != isDriver.end() || !enabled )
                        return "output enable shared with " + d.reference + " pin " + q.pin;
                }

                return {};
            };

            std::string problem = conflict( static_cast<size_t>( n.driver ) );

            if( problem.empty() )
                continue;

            // Another pin of the net as driver?
            int other = -1;

            for( size_t i = 0; i < n.bsPins.size() && other < 0; ++i )
            {
                if( static_cast<int>( i ) == n.driver || !driverProblem( n, i ).empty() )
                    continue;

                int saved = n.driver;
                n.driver  = static_cast<int>( i );

                if( conflict( i ).empty() )
                    other = static_cast<int>( i );

                n.driver = saved;
            }

            if( other >= 0 )
            {
                n.driver = other;
            }
            else
            {
                n.status = TestNet::Status::SharedEnable;
                n.reason = problem;
                n.driver = -1;
            }

            changed = true;
            break;   // roles changed: check again
        }
    }

    // --- codes and patterns ---
    unsigned long long count = 0;

    for( TestNet& n : plan.nets )
        if( n.status == TestNet::Status::Tested )
            n.code = ++count;

    if( count == 0 )
        return plan;

    // Codes 1 .. 2^(w-1) - 1: the top bit is always 0, so no code is the
    // complement of another. Otherwise a wired-AND short of two
    // complementary codes would read all 0 (like stuck-at-0) and a wired-OR
    // all 1 in both the true and the complement half.
    plan.width = 2;

    while( ( 1ULL << ( plan.width - 1 ) ) - 1 < count )
        ++plan.width;

    const int patterns = 2 * plan.width;

    for( int p = 0; p < patterns; ++p )
    {
        std::vector<Bits> images;

        for( const IcDevice& d : plan.devices )
            images.push_back( d.baseImage );

        // Receivers off first, then the drivers (a shared enable of two
        // drivers stays on)
        for( const TestNet& n : plan.nets )
        {
            if( n.status != TestNet::Status::Tested )
                continue;

            for( size_t i : receivers( n ) )
            {
                const NetPin& r = n.bsPins[i];

                if( r.cells.outputCell >= 0 && canDisable( r.cells ) )
                    setBit( images[r.device], r.cells.controlCell, r.cells.disableValue != 0 );
            }
        }

        for( const TestNet& n : plan.nets )
        {
            if( n.status != TestNet::Status::Tested )
                continue;

            const NetPin& d   = n.bsPins[n.driver];
            bool          bit = p < plan.width ? ( ( n.code >> p ) & 1 )
                                               : !( ( n.code >> ( p - plan.width ) ) & 1 );

            if( canDisable( d.cells ) )
                setBit( images[d.device], d.cells.controlCell, d.cells.disableValue == 0 );

            setBit( images[d.device], d.cells.outputCell, bit );
        }

        plan.patterns.push_back( std::move( images ) );
    }

    return plan;
}


std::vector<NetResult> EvaluateInterconnect( const InterconnectPlan& aPlan,
                                             const std::vector<std::vector<Bits>>& aCaptured )
{
    std::vector<NetResult> results;
    const int              w    = aPlan.width;
    const unsigned long long mask = w >= 64 ? ~0ULL : ( 1ULL << w ) - 1;

    struct Code
    {
        unsigned long long t = 0;   // read during the true patterns
        unsigned long long c = 0;   // read during the complement patterns
    };

    auto expected = [&]( unsigned long long aCode ) { return Code{ aCode, ~aCode & mask }; };

    // Read code of a receiver pin
    auto observed = [&]( const NetPin& aPin )
    {
        Code code;

        for( int p = 0; p < 2 * w && p < static_cast<int>( aCaptured.size() ); ++p )
        {
            if( aPin.device >= static_cast<int>( aCaptured[p].size() ) )
                continue;

            bool bit = bitAt( aCaptured[p][aPin.device], aPin.cells.inputCell );

            if( p < w )
                code.t |= static_cast<unsigned long long>( bit ) << p;
            else
                code.c |= static_cast<unsigned long long>( bit ) << ( p - w );
        }

        return code;
    };

    for( const TestNet& n : aPlan.nets )
    {
        if( n.status != TestNet::Status::Tested )
            continue;

        NetResult r;
        r.net = n.name;

        const Code want = expected( n.code );
        std::vector<std::string> bad;
        size_t                   stuck0 = 0, stuck1 = 0, total = 0;
        std::set<std::string>    shorts;

        for( size_t i : receivers( n ) )
        {
            ++total;
            const Code got = observed( n.bsPins[i] );

            if( got.t == want.t && got.c == want.c )
                continue;

            bad.push_back( pinLabel( n.bsPins[i] ) );

            if( got.t == 0 && got.c == 0 )
                ++stuck0;
            else if( got.t == mask && got.c == mask )
                ++stuck1;

            // Short: wired AND / OR with another net, or the other net's code
            for( const TestNet& m : aPlan.nets )
            {
                if( &m == &n || m.status != TestNet::Status::Tested )
                    continue;

                const Code o = expected( m.code );

                if( ( got.t == ( want.t & o.t ) && got.c == ( want.c & o.c ) )
                    || ( got.t == ( want.t | o.t ) && got.c == ( want.c | o.c ) )
                    || ( got.t == o.t && got.c == o.c ) )
                {
                    shorts.insert( m.name );
                }
            }
        }

        if( bad.empty() )
        {
            r.kind = NetResult::Kind::Ok;
            r.text = "OK";
        }
        else if( !shorts.empty() )
        {
            r.kind      = NetResult::Kind::Short;
            r.shortWith.assign( shorts.begin(), shorts.end() );
            r.pins      = bad;
            r.text      = "short with";

            for( const std::string& s : r.shortWith )
                r.text += " " + s;
        }
        else if( bad.size() == total && ( stuck0 == total || stuck1 == total ) )
        {
            const std::string driver = pinLabel( n.bsPins[n.driver] );
            r.kind = stuck0 ? NetResult::Kind::Stuck0 : NetResult::Kind::Stuck1;
            r.pins = { driver };
            r.text = stuck0 ? "all receivers read 0: open at " + driver
                                      + ( total == 1 ? " or " + bad[0] : std::string() )
                                      + ", or short to GND"
                            : "all receivers read 1: open at " + driver
                                      + ( total == 1 ? " or " + bad[0] : std::string() )
                                      + ", or short to a supply";
        }
        else if( bad.size() < total && stuck0 + stuck1 == bad.size() )
        {
            r.kind = NetResult::Kind::Open;
            r.pins = bad;
            r.text = "open at";

            for( const std::string& p : bad )
                r.text += " " + p;
        }
        else
        {
            r.kind = NetResult::Kind::Wrong;
            r.pins = bad;
            r.text = "wrong values at";

            for( const std::string& p : bad )
                r.text += " " + p;
        }

        results.push_back( std::move( r ) );
    }

    return results;
}

} // namespace bscan
