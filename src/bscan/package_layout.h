// SPDX-License-Identifier: GPL-3.0-only
// Where the pins of a chip are drawn in the chip view. BSDL files have pin
// names but no geometry, so the arrangement is derived from the names:
//
//   ball names (A1, AB12)  -> BGA grid, A1 top left; rows A..Y without
//                             I, O, Q, S, X, Z (JEDEC), then AA, AB, ...
//   numbers                -> quad package (QFP/QFN): pin 1 top left,
//                             counter-clockwise (left side down, bottom to
//                             the right, right side up, top to the left)
//
// The user can override this per chip (PackageShape), e.g. for quad
// packages with unequal sides or dual-row packages. Positions are in pin
// pitches; the body is the rectangle (0, 0) - (width, height).

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bscan
{

struct PackageShape
{
    enum class Type
    {
        Auto,
        Bga,
        Quad,
        Dual
    };

    Type             type = Type::Auto;
    std::vector<int> sides;   // Quad: pins on left, bottom, right, top; empty: even split

    // "" or "auto", "bga", "quad", "quad:30,20,30,20", "dual" (case-insensitive);
    // nullopt if not understood
    static std::optional<PackageShape> Parse( const std::string& aText );
    std::string                        ToString() const;   // "" for Auto

    bool operator==( const PackageShape& ) const = default;
};

struct PlacedPin
{
    enum class Side
    {
        Left,
        Bottom,
        Right,
        Top,
        Grid,     // BGA ball
        Centre    // a pin the arrangement has no place for (e.g. "EP")
    };

    std::string pin;
    double      x = 0;   // centre, in pin pitches
    double      y = 0;
    Side        side = Side::Grid;
};

struct PackageLayout
{
    PackageShape::Type     type = PackageShape::Type::Quad;   // as resolved
    double                 width  = 0;                        // body, in pin pitches
    double                 height = 0;
    std::vector<PlacedPin> pins;
    std::string            problem;   // the override does not fit: auto layout used instead
    std::vector<std::string> rowNames;   // BGA: row letters, top to bottom
    int                      columns = 0; // BGA: number of columns
};

// aPins: the physical pins of the pin map (all of them, power included)
PackageLayout LayoutPackage( const std::vector<std::string>& aPins, const PackageShape& aShape );

} // namespace bscan
