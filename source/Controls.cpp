#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace honeydew
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
constexpr double kDishLow = 10.0, kDishHigh = 300.0;
constexpr double kDepthLow = 0.3, kDepthHigh = 20.0;
constexpr double kLapseLow = 1.0, kLapseHigh = 300.0;
constexpr double kDropLow = 1.0, kDropHigh = 20.0;
constexpr double kAutoLow = 1.0, kAutoHigh = 60.0;
/// The stir bar at full Stir: two turns a second of chemical time.
constexpr double kOmegaMax = 4.0 * 3.14159265358979323846;
/// Eddy diffusivity at full Stir, mm^2/s (water's molecular D is ~1e-3): a
/// closure, see AGENTS.md.
constexpr double kEddyMax = 2.0;
/// The whole-vessel relaxation rate at full Stir, 1/s of chemical time.
constexpr double kRelaxMax = 3.0;
} // namespace

const std::vector< unsigned int >& HostOrder( bool effect )
{
	//PT_CONVECTION is declared by neither plugin in 0.1.0: the depth-resolved
	//solver it would switch on is not built (AGENTS.md). A control nothing
	//reads would be a dead control.
	auto build = []( bool over ) {
		std::vector< unsigned int > order = { PT_REACTION, PT_CATALYST, PT_REACTOR, PT_RESET, PT_SEED,
			                                  PT_OXIDANT, PT_ACID_BASE, PT_REDUCTANT, PT_INDICATOR,
			                                  PT_VESSEL, PT_DISH_WIDTH, PT_DEPTH, PT_STIR, PT_TIMELAPSE, PT_DETAIL,
			                                  PT_DROP, PT_DROP_SIZE, PT_DROP_POSITION, PT_BREAK_WAVE, PT_SHAKE, PT_AUTO_DROP,
			                                  PT_AUDIO, PT_AUDIO_DROPS, PT_AUDIO_SHAKES, PT_CLOCK_SYNC };
		//The Over's light is the clip, filtered through the primaries: it has
		//no Lightbox (the sweep found the declared one dead, AGENTS.md).
		if( over )
			order.insert( order.end(), { PT_LIGHT_COUPLING, PT_EXPOSURE, PT_SEED_FROM_CLIP, PT_MIX } );
		else
			order.insert( order.end(), { PT_LIGHTBOX, PT_EXPOSURE } );
		for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
			order.push_back( id );
		return order;
	};
	static const std::vector< unsigned int > source = build( false ), over = build( true );
	return effect ? over : source;
}

const char* GroupOf( unsigned int id )
{
	if( id <= PT_SEED )
		return "Reaction";
	if( id <= PT_INDICATOR )
		return "Recipe";
	if( id <= PT_CONVECTION )
		return "Vessel";
	if( id <= PT_AUDIO_SHAKES )
		return "Drops";
	if( id <= PT_CLOCK_SYNC )
		return "Clock";
	if( id <= PT_MIX )
		return "Light";
	return "About";
}

const char* NameOf( unsigned int id )
{
	switch( id )
	{
	case PT_REACTION: return "Reaction";
	case PT_CATALYST: return "Catalyst";
	case PT_REACTOR: return "Reactor";
	case PT_RESET: return "Reset";
	case PT_SEED: return "Seed";
	case PT_OXIDANT: return "Oxidant";
	case PT_ACID_BASE: return "Acid or Base";
	case PT_REDUCTANT: return "Reductant";
	case PT_INDICATOR: return "Indicator";
	case PT_VESSEL: return "Vessel";
	case PT_DISH_WIDTH: return "Dish Width";
	case PT_DEPTH: return "Depth";
	case PT_STIR: return "Stir";
	case PT_TIMELAPSE: return "Time-lapse";
	case PT_DETAIL: return "Detail";
	case PT_CONVECTION: return "Convection";
	case PT_DROP: return "Drop";
	case PT_DROP_SIZE: return "Drop Size";
	case PT_DROP_POSITION: return "Drop Position";
	case PT_BREAK_WAVE: return "Break Wave";
	case PT_SHAKE: return "Shake";
	case PT_AUTO_DROP: return "Auto Drop";
	case PT_AUDIO: return "Audio";
	case PT_AUDIO_DROPS: return "Audio Drops";
	case PT_AUDIO_SHAKES: return "Audio Shakes";
	case PT_CLOCK_SYNC: return "Clock Sync";
	case PT_LIGHT_COUPLING: return "Light Coupling";
	case PT_LIGHTBOX: return "Lightbox";
	case PT_EXPOSURE: return "Exposure";
	case PT_SEED_FROM_CLIP: return "Seed From Clip";
	case PT_MIX: return "Mix";
	default: return "?";
	}
}

const char* ReactionName( Reaction r )
{
	switch( r )
	{
	case Reaction::BZ: return "Belousov-Zhabotinsky";
	case Reaction::BriggsRauscher: return "Briggs-Rauscher";
	case Reaction::IodineClock: return "Iodine Clock";
	case Reaction::CDIMA: return "CDIMA Turing";
	case Reaction::TrafficLight: return "Traffic Light";
	case Reaction::BlueBottle: return "Blue Bottle";
	case Reaction::Valentine: return "Vanishing Valentine";
	case Reaction::Chameleon: return "Chemical Chameleon";
	default: return "?";
	}
}

bool IsDyeFamily( Reaction r )
{
	return r == Reaction::TrafficLight || r == Reaction::BlueBottle || r == Reaction::Valentine;
}

const char* CatalystName( Catalyst c )
{
	switch( c )
	{
	case Catalyst::Ferroin: return "Ferroin";
	case Catalyst::Rubpy: return "Ru(bpy)3";
	case Catalyst::Cerium: return "Cerium";
	default: return "?";
	}
}

int DropPositionCount( bool effect )
{
	return effect ? static_cast< int >( DropPosition::Count ) : static_cast< int >( DropPosition::Brightest );
}

const char* LightboxName( Lightbox l )
{
	switch( l )
	{
	case Lightbox::D65: return "Daylight D65";
	case Lightbox::LED5000K: return "LED 5000K";
	case Lightbox::WarmWhite: return "Warm White";
	default: return "?";
	}
}

double RecipeMultiplierFromParam( float v )
{
	if( v <= 0.0f )
		return 0.0;
	return std::pow( 4.0, 2.0 * clamp01( v ) - 1.0 );
}
float ParamFromRecipeMultiplier( double m )
{
	if( m <= 0.0 )
		return 0.0f;
	return static_cast< float >( std::clamp( 0.5 * ( std::log( m ) / std::log( 4.0 ) + 1.0 ), 0.0, 1.0 ) );
}
double DishWidthFromParam( float v )
{
	return geometric( v, kDishLow, kDishHigh );
}
float ParamFromDishWidth( double mm )
{
	return inverseGeometric( mm, kDishLow, kDishHigh );
}
double DepthFromParam( float v )
{
	return geometric( v, kDepthLow, kDepthHigh );
}
float ParamFromDepth( double mm )
{
	return inverseGeometric( mm, kDepthLow, kDepthHigh );
}
double TimelapseFromParam( float v )
{
	return geometric( v, kLapseLow, kLapseHigh );
}
float ParamFromTimelapse( double x )
{
	return inverseGeometric( x, kLapseLow, kLapseHigh );
}
double DropSizeFromParam( float v )
{
	return geometric( v, kDropLow, kDropHigh );
}
float ParamFromDropSize( double mm )
{
	return inverseGeometric( mm, kDropLow, kDropHigh );
}
double AutoDropFromParam( float v )
{
	if( v <= 0.0f )
		return 0.0;
	return geometric( v, kAutoLow, kAutoHigh );
}
double ExposureStopsFromParam( float v )
{
	return 4.0 * clamp01( v ) - 2.0;
}
double StirOmegaFromParam( float v )
{
	return kOmegaMax * clamp01( v );
}
double StirEddyFromParam( float v )
{
	const double s = clamp01( v );
	return kEddyMax * s * s;
}
double StirRelaxFromParam( float v )
{
	//Only the top of the range relaxes the whole vessel: below 0.6 the stir
	//bar and the eddies do all the mixing. (0..1 over 0.6..1, then cubed.)
	const double s = std::max( 0.0, ( clamp01( v ) - 0.6 ) / 0.4 );
	return kRelaxMax * s * s * s;
}

int DetailCells( float v )
{
	return kDetailCells[ OptionIndex( v, kDetailCount ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

Grid GridFor( int cols, int width, int height )
{
	Grid grid;
	grid.cols           = std::max( cols, 2 );
	const double aspect = width > 0 ? static_cast< double >( height ) / width : 1.0;
	grid.rows           = std::max( 2, static_cast< int >( std::lround( grid.cols * aspect ) ) );
	return grid;
}

} // namespace honeydew
