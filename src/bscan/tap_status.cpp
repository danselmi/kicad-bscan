// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/tap_status.h"

namespace bscan
{

using Check = TapStatus::Check;

// The file, the chain and the board (EXTEST aside)
static Check basicCheck( const TapInfo& aTap, const TapFacts& aFacts, bool aFits )
{
    if( !aFacts.assigned )
        return Check::Bypass;

    if( !aFacts.device )
        return Check::BsdlError;

    if( !aFacts.present )
        return aFacts.connected ? Check::NotInChain : Check::Offline;

    if( !aFits )
        return Check::Mismatch;

    if( aFacts.assignedIdcode && aTap.idcode && aFacts.assignedIdcode != aTap.idcode )
        return Check::IdcodeChanged;

    return Check::Ok;
}


TapStatus EvaluateTap( const TapInfo& aTap, const TapFacts& aFacts )
{
    const bool fits = aFacts.present && aFacts.device && CheckTap( aTap, *aFacts.device ).ok;

    TapStatus s;
    s.check   = basicCheck( aTap, aFacts, fits );
    s.problem = s.check == Check::BsdlError || s.check == Check::Mismatch
                || s.check == Check::IdcodeChanged;

    if( aFacts.referenceUnknown )
    {
        s.check   = Check::UnknownReference;
        s.problem = true;
    }

    s.extestAllowed = fits && !aFacts.held && aFacts.elsewhere.empty() && aFacts.connected
                      && aFacts.device->Opcode( "EXTEST" ).has_value();

    // (a held device itself is taken over instead)
    s.blocked = !aFacts.elsewhere.empty() && !aFacts.inExtest && !aFacts.held;

    if( aFacts.inExtest )
    {
        s.check   = Check::Extest;
        s.problem = true;
    }
    else if( aFacts.held )
    {
        s.check   = Check::Held;
        s.problem = true;

        if( aFacts.canTakeOver )
        {
            s.takeOver      = true;
            s.extestAllowed = true;
        }
    }

    // A file that does not fit (or none): matching files may help
    s.lookForBsdl = aFacts.present && aTap.idcode && !aFacts.inExtest && !fits;
    return s;
}

} // namespace bscan
