// SPDX-License-Identifier: GPL-3.0-only
// C++ wrapper around a parsed BSDL file (libbsdl).

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bscan/bits.h"

namespace bscan
{

// Cells of one pin, resolved for a physical pin of a given package
struct PinCells
{
    std::string port;          // port element, e.g. "PA(3)" or "TDO"
    int inputCell    = -1;     // -1: none
    int outputCell   = -1;
    int controlCell  = -1;
    int disableValue = -1;     // value of controlCell that disables the output

    bool HasCells() const { return inputCell >= 0 || outputCell >= 0; }
};

// A physical pin with an output cell
struct OutputPin
{
    std::string pin;
    PinCells    cells;
};

class BsdlDevice
{
public:
    struct Diagnostic
    {
        bool        error;
        int         line;
        std::string message;
    };

    // Parses aPath (tolerant mode). Fails only if no device could be built;
    // warnings are available through Diagnostics().
    static std::expected<std::shared_ptr<const BsdlDevice>, std::string>
    Load( const std::string& aPath );

    ~BsdlDevice();

    const std::string&             Path() const { return m_path; }
    std::string                    EntityName() const;
    const std::vector<Diagnostic>& Diagnostics() const { return m_diags; }

    int IrLength() const;          // -1 if unknown
    int BoundaryLength() const;    // -1 if unknown

    // IDCODE from IDCODE_REGISTER, MSB first with 'X' for don't care; empty if none
    std::string IdcodePattern() const;

    // True if aIdcode matches IDCODE_REGISTER (X bits ignored).
    // nullopt if the BSDL file has no IDCODE_REGISTER.
    std::optional<bool> IdcodeMatches( uint32_t aIdcode ) const;

    // Opcode of an instruction ('X' bits as 0); nullopt if unknown or > 64 bits
    std::optional<uint64_t> Opcode( const std::string& aInstruction ) const;

    // Names of the instructions whose opcode (with 'X' as don't care) is
    // aValue; usually one, empty if none
    std::vector<std::string> InstructionsFor( uint64_t aValue ) const;

    // Safe values of all boundary cells ('X' as 0)
    Bits SafeBits() const;

    // Pins of a pin map (empty name: default map) with an output cell, in
    // pin map order; all of them, or only those whose output enable is
    // aControlCell (they change state together)
    std::vector<OutputPin> OutputPins( const std::string& aPinMap ) const;
    std::vector<OutputPin> PinsWithControlCell( const std::string& aPinMap,
                                                int aControlCell ) const;

    std::vector<std::string> PinMapNames() const;
    std::string              DefaultPinMap() const;   // empty if none

    // Physical pins of a pin map in file order (empty name: default map)
    std::vector<std::string> PinNames( const std::string& aPinMap ) const;

    // Resolves a physical pin (pad number / ball) in the given pin map
    // (empty name: default pin map). Port names are never used.
    std::optional<PinCells> LookupPin( const std::string& aPinMap,
                                       const std::string& aPin ) const;

private:
    BsdlDevice() = default;

    std::string             m_path;
    // bsdl.h types are typedefs of anonymous structs and cannot be
    // forward-declared, so they are stored untyped here.
    void*                   m_result = nullptr;   // bsdl_result*
    const void*             m_dev    = nullptr;   // const bsdl_device*
    std::vector<Diagnostic> m_diags;
};

} // namespace bscan
