// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/package_layout.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace bscan
{

// Row letters of BGA packages without the ones JEDEC leaves out
static constexpr std::string_view JEDEC_ROWS = "ABCDEFGHJKLMNPRTUVWY";
static constexpr std::string_view ALL_ROWS   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";


static std::string upper( std::string aText )
{
    std::ranges::transform( aText, aText.begin(),
                            []( unsigned char c ) { return static_cast<char>( std::toupper( c ) ); } );
    return aText;
}


std::optional<PackageShape> PackageShape::Parse( const std::string& aText )
{
    std::string text = upper( aText );
    text.erase( std::remove_if( text.begin(), text.end(),
                                []( unsigned char c ) { return std::isspace( c ); } ),
                text.end() );

    PackageShape shape;

    if( text.empty() || text == "AUTO" )
        return shape;

    if( text == "BGA" )
    {
        shape.type = Type::Bga;
        return shape;
    }

    if( text == "DUAL" )
    {
        shape.type = Type::Dual;
        return shape;
    }

    if( text == "QUAD" )
    {
        shape.type = Type::Quad;
        return shape;
    }

    if( text.starts_with( "QUAD:" ) )
    {
        shape.type = Type::Quad;
        std::string_view rest = std::string_view( text ).substr( 5 );

        while( !rest.empty() )
        {
            size_t comma = rest.find( ',' );
            std::string_view part = rest.substr( 0, comma );
            int              n    = 0;
            auto [end, ec]        = std::from_chars( part.data(), part.data() + part.size(), n );

            if( ec != std::errc() || end != part.data() + part.size() || n < 0 )
                return std::nullopt;

            shape.sides.push_back( n );
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr( comma + 1 );
        }

        if( shape.sides.size() != 4 )
            return std::nullopt;

        return shape;
    }

    return std::nullopt;
}


std::string PackageShape::ToString() const
{
    switch( type )
    {
    case Type::Auto: return "";
    case Type::Bga:  return "bga";
    case Type::Dual: return "dual";
    case Type::Quad: break;
    }

    if( sides.size() != 4 )
        return "quad";

    return "quad:" + std::to_string( sides[0] ) + "," + std::to_string( sides[1] ) + ","
           + std::to_string( sides[2] ) + "," + std::to_string( sides[3] );
}


// "AB12" -> ("AB", 12); nullopt if not a ball name
static std::optional<std::pair<std::string, int>> splitBall( const std::string& aPin )
{
    std::string p = upper( aPin );
    size_t      i = 0;

    while( i < p.size() && std::isalpha( static_cast<unsigned char>( p[i] ) ) )
        ++i;

    if( i == 0 || i > 2 || i == p.size() )
        return std::nullopt;

    int  col = 0;
    auto [end, ec] = std::from_chars( p.data() + i, p.data() + p.size(), col );

    if( ec != std::errc() || end != p.data() + p.size() || col <= 0 )
        return std::nullopt;

    return std::make_pair( p.substr( 0, i ), col );
}


static std::optional<int> pinNumber( const std::string& aPin )
{
    int  n = 0;
    auto [end, ec] = std::from_chars( aPin.data(), aPin.data() + aPin.size(), n );

    if( ec != std::errc() || end != aPin.data() + aPin.size() || n <= 0 )
        return std::nullopt;

    return n;
}


// Index of a row name ("A" -> 0, "AA" -> letters.size()), nullopt if a letter
// is not in aLetters
static std::optional<int> rowIndex( const std::string& aRow, std::string_view aLetters )
{
    int n = static_cast<int>( aLetters.size() );
    int index = 0;

    for( size_t k = 0; k < aRow.size(); ++k )
    {
        size_t pos = aLetters.find( aRow[k] );

        if( pos == std::string_view::npos )
            return std::nullopt;

        bool last = k + 1 == aRow.size();
        index     = last ? index + static_cast<int>( pos ) : ( index + static_cast<int>( pos ) + 1 ) * n;
    }

    return index;
}


static std::string rowName( int aIndex, std::string_view aLetters )
{
    int n = static_cast<int>( aLetters.size() );

    if( aIndex < n )
        return std::string( 1, aLetters[aIndex] );

    return std::string( 1, aLetters[aIndex / n - 1] ) + aLetters[aIndex % n];
}


// Pins without a place in the arrangement: a column in the middle of the body
static void placeCentre( PackageLayout& aLayout, const std::vector<std::string>& aPins )
{
    double y0 = aLayout.height / 2 - ( static_cast<double>( aPins.size() ) - 1 ) / 2;

    for( size_t k = 0; k < aPins.size(); ++k )
        aLayout.pins.push_back( { aPins[k], aLayout.width / 2, y0 + static_cast<double>( k ),
                                  PlacedPin::Side::Centre } );
}


static PackageLayout layoutBga( const std::vector<std::string>& aPins )
{
    PackageLayout layout;
    layout.type = PackageShape::Type::Bga;

    std::vector<std::pair<std::string, int>> balls;
    std::vector<std::string>                 others;
    bool                                     jedec = true;

    for( const std::string& pin : aPins )
    {
        if( auto ball = splitBall( pin ) )
        {
            balls.push_back( *ball );
            jedec = jedec && rowIndex( ball->first, JEDEC_ROWS ).has_value();
        }
        else
        {
            others.push_back( pin );
        }
    }

    std::string_view letters = jedec ? JEDEC_ROWS : ALL_ROWS;
    int              rows = 0, cols = 0;

    for( size_t k = 0; k < balls.size(); ++k )
    {
        int row = rowIndex( balls[k].first, letters ).value_or( 0 );
        int col = balls[k].second;
        rows    = std::max( rows, row + 1 );
        cols    = std::max( cols, col );
    }

    layout.width   = cols;
    layout.height  = rows;
    layout.columns = cols;

    for( int r = 0; r < rows; ++r )
        layout.rowNames.push_back( rowName( r, letters ) );

    size_t ball = 0;

    for( const std::string& pin : aPins )
    {
        if( !splitBall( pin ) )
            continue;

        const auto& [row, col] = balls[ball++];
        layout.pins.push_back( { pin, col - 0.5, rowIndex( row, letters ).value_or( 0 ) + 0.5,
                                 PlacedPin::Side::Grid } );
    }

    placeCentre( layout, others );
    return layout;
}


// Pins 1..aCount around a quad (aSides: left, bottom, right, top) or in two
// rows (aSides: left, 0, right, 0)
static PackageLayout layoutSides( const std::vector<std::string>& aPins,
                                  const std::vector<int>& aSides, PackageShape::Type aType )
{
    PackageLayout layout;
    layout.type = aType;

    const int left = aSides[0], bottom = aSides[1], right = aSides[2], top = aSides[3];
    layout.width   = std::max( std::max( bottom, top ) + 1, 3 );
    layout.height  = std::max( std::max( left, right ) + 1, 3 );

    const double w = layout.width, h = layout.height;
    const double out = 0.6;   // pin centres outside the body

    std::vector<std::string> others;

    for( const std::string& pin : aPins )
    {
        auto n = pinNumber( pin );

        if( !n || *n > left + bottom + right + top )
        {
            others.push_back( pin );
            continue;
        }

        int i = *n - 1;

        if( i < left )
        {
            layout.pins.push_back( { pin, -out, ( h - left ) / 2 + i + 0.5, PlacedPin::Side::Left } );
            continue;
        }

        i -= left;

        if( i < bottom )
        {
            layout.pins.push_back( { pin, ( w - bottom ) / 2 + i + 0.5, h + out,
                                     PlacedPin::Side::Bottom } );
            continue;
        }

        i -= bottom;

        if( i < right )
        {
            layout.pins.push_back( { pin, w + out, ( h - right ) / 2 + ( right - 1 - i ) + 0.5,
                                     PlacedPin::Side::Right } );
            continue;
        }

        i -= right;
        layout.pins.push_back( { pin, ( w - top ) / 2 + ( top - 1 - i ) + 0.5, -out,
                                 PlacedPin::Side::Top } );
    }

    placeCentre( layout, others );
    return layout;
}


static int highestNumber( const std::vector<std::string>& aPins )
{
    int n = 0;

    for( const std::string& pin : aPins )
        n = std::max( n, pinNumber( pin ).value_or( 0 ) );

    return n;
}


static std::vector<int> evenSides( int aCount )
{
    // Remainder to the left and right sides first, then bottom: 30 -> 8,7,8,7
    std::vector<int> sides( 4, aCount / 4 );
    const int        order[] = { 0, 2, 1, 3 };

    for( int k = 0; k < aCount % 4; ++k )
        ++sides[order[k]];

    return sides;
}


static bool looksLikeBga( const std::vector<std::string>& aPins )
{
    size_t balls = std::ranges::count_if( aPins, []( const std::string& p )
                                          { return splitBall( p ).has_value(); } );

    return balls > 0 && balls * 5 >= aPins.size() * 4;   // at least 80 %
}


PackageLayout LayoutPackage( const std::vector<std::string>& aPins, const PackageShape& aShape )
{
    const int count = highestNumber( aPins );

    // An override that does not fit the pin names at all: the guess instead
    if( aShape.type == PackageShape::Type::Bga && !looksLikeBga( aPins ) )
    {
        PackageLayout layout = LayoutPackage( aPins, {} );
        layout.problem = "the pin names are not ball names (A1, B2, ...); laid out as guessed";
        return layout;
    }

    if( ( aShape.type == PackageShape::Type::Quad || aShape.type == PackageShape::Type::Dual )
        && count == 0 && !aPins.empty() )
    {
        PackageLayout layout = LayoutPackage( aPins, {} );
        layout.problem = "the pin names are not numbers; laid out as guessed";
        return layout;
    }

    switch( aShape.type )
    {
    case PackageShape::Type::Bga:
        return layoutBga( aPins );

    case PackageShape::Type::Dual:
    {
        int left = ( count + 1 ) / 2;
        return layoutSides( aPins, { left, 0, count - left, 0 }, PackageShape::Type::Dual );
    }

    case PackageShape::Type::Quad:
    {
        if( aShape.sides.size() == 4 )
        {
            int sum = aShape.sides[0] + aShape.sides[1] + aShape.sides[2] + aShape.sides[3];

            if( sum >= count )
                return layoutSides( aPins, aShape.sides, PackageShape::Type::Quad );

            PackageLayout layout = layoutSides( aPins, evenSides( count ), PackageShape::Type::Quad );
            layout.problem = "the sides have " + std::to_string( sum ) + " pins, the package has "
                             + std::to_string( count ) + "; pins spread evenly instead";
            return layout;
        }

        return layoutSides( aPins, evenSides( count ), PackageShape::Type::Quad );
    }

    case PackageShape::Type::Auto:
        break;
    }

    if( looksLikeBga( aPins ) )
        return layoutBga( aPins );

    return layoutSides( aPins, evenSides( count ), PackageShape::Type::Quad );
}

} // namespace bscan
