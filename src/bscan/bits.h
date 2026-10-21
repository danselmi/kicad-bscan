// SPDX-License-Identifier: GPL-3.0-only
// Bit vector for scan data. Bit i corresponds to boundary cell i
// (cell 0 is closest to TDO and is shifted out first).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bscan
{

class Bits
{
public:
    Bits() = default;
    explicit Bits( size_t aCount, bool aValue = false ) : m_bits( aCount, aValue ) {}

    size_t size() const { return m_bits.size(); }
    bool   operator[]( size_t i ) const { return m_bits[i]; }
    void   set( size_t i, bool v ) { m_bits[i] = v; }

    bool operator==( const Bits& ) const = default;

    // Hex as printed by OpenOCD's drscan: MSB first, no prefix, possibly
    // zero-padded to whole bytes. Returns nullopt on invalid characters or
    // if the value does not fit into aCount bits.
    static std::optional<Bits> FromHex( std::string_view aHex, size_t aCount );

    // Bit string as used in BSDL: MSB first, '0'/'1'; 'X' becomes aX.
    static std::optional<Bits> FromBitString( std::string_view aStr, bool aX = false );

    std::string ToHex() const;          // MSB first, no prefix, at least one digit
    std::string ToBitString() const;    // MSB first

    // Value of the lowest 64 bits
    unsigned long long ToU64() const;

private:
    std::vector<bool> m_bits;
};

} // namespace bscan
