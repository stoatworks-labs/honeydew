#pragma once

#include <cstdint>

namespace honeydew
{
/// The PCG output mix, the same integer arithmetic as the GLSL's pcg().
inline uint32_t Pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// The GLSL's cellHash( c, salt ).
inline uint32_t CellHash( int x, int y, uint32_t salt )
{
	return Pcg( static_cast< uint32_t >( x ) ^ Pcg( static_cast< uint32_t >( y ) ^ Pcg( salt ) ) );
}

} // namespace honeydew
