#pragma once

#include "StoatworksAboutLinks.h"

#include <cstdint>
#include <vector>

/**
    The host's parameters, and what they mean in the dish's units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control. Option, boolean and event parameters hold the element value
    itself; Seed is a real integer (`FF_TYPE_INTEGER`).

    Units: millimetres (the dish, the depth, a drop), seconds of CHEMICAL time
    (host seconds x Time-lapse), mol/L for every concentration (the recipe
    sliders are log multipliers of each reaction's cited recipe, Chemistry.h).
*/
namespace honeydew
{
/**
    Every control either plugin has, by what it is -- NOT by the index a host
    sees. Each plugin declares its own subset, densely, in the order of
    `HostOrder()`, with the About block LAST in both (radar's shape, via
    conway: the day a user guide adds a button, no control moves).
*/
enum ParamId : unsigned int
{
	// -- Reaction ------------------------------------------------------------
	PT_REACTION = 0,
	PT_CATALYST,///< BZ only
	PT_REACTOR,
	PT_RESET,
	PT_SEED,

	// -- Recipe (log multipliers of the cited 1x recipe; 0 = none) -----------
	PT_OXIDANT,
	PT_ACID_BASE,
	PT_REDUCTANT,
	PT_INDICATOR,

	// -- Vessel --------------------------------------------------------------
	PT_VESSEL,
	PT_DISH_WIDTH,
	PT_DEPTH,
	PT_STIR,
	PT_TIMELAPSE,
	PT_DETAIL,
	PT_CONVECTION,

	// -- Drops ---------------------------------------------------------------
	PT_DROP,
	PT_DROP_SIZE,
	PT_DROP_POSITION,
	PT_BREAK_WAVE,///< BZ only
	PT_SHAKE,
	PT_AUTO_DROP,
	PT_AUDIO,
	PT_AUDIO_DROPS,
	PT_AUDIO_SHAKES,

	// -- Clock ---------------------------------------------------------------
	PT_CLOCK_SYNC,

	// -- Light ---------------------------------------------------------------
	PT_LIGHT_COUPLING,///< Over only
	PT_LIGHTBOX,
	PT_EXPOSURE,
	PT_SEED_FROM_CLIP,///< Over only
	PT_MIX,           ///< Over only
	/// Appended in 0.1.1 (FFGL's ABI is by index: new controls go after the
	/// last existing one, with the About block still last). BZ only: the
	/// Oregonator's stoichiometric factor f, oscillatory below the Hopf point
	/// and excitable past it (AGENTS.md). Shown in the Reaction group.
	PT_EXCITABILITY,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_TEXT,
	PT_COUNT = PT_ABOUT_TEXT + 1 + stoatworks::about::kButtonCount
};

/// The ids a plugin declares, in the order the host sees them. Host index i
/// is `HostOrder( effect )[ i ]`. The About block is last in both.
const std::vector< unsigned int >& HostOrder( bool effect );
const char* GroupOf( unsigned int id );
const char* NameOf( unsigned int id );

/// The eight reactions, in the Reaction dropdown's order. The order is the
/// saved value: never reorder.
enum class Reaction
{
	BZ = 0,
	BriggsRauscher,
	IodineClock,
	CDIMA,
	TrafficLight,
	BlueBottle,
	Valentine,
	Chameleon,
	Count
};
const char* ReactionName( Reaction r );
/// The three blue-bottle-family dyes share one engine.
bool IsDyeFamily( Reaction r );

enum class Catalyst
{
	Ferroin = 0,
	Rubpy,
	Cerium,
	Count
};
const char* CatalystName( Catalyst c );

enum class Reactor
{
	Batch = 0,
	Flow,
	Count
};
enum class Vessel
{
	FullFrame = 0,
	PetriDish,
	Count
};
enum class DropPosition
{
	Random = 0,
	Centre,
	Brightest,///< Over only, LAST so the shared entries keep their indices
	Count
};
int DropPositionCount( bool effect );
enum class ClockSync
{
	Off = 0,
	Beat,
	Bar,
	Count
};
enum class Lightbox
{
	D65 = 0,
	LED5000K,
	WarmWhite,
	Count
};
const char* LightboxName( Lightbox l );

/// Detail: cells across the frame's width. The grid's rows follow the aspect.
constexpr int kDetailCells[] = { 128, 256, 512, 1024 };
constexpr int kDetailCount   = 4;
/// A host frame runs at most this many chemistry substeps; past it the
/// chemistry falls behind real time (counted, logged), never unstable.
constexpr int kMaxSubsteps = 96;

//---------------------------------------------------------------------------
// The mappings. Every "FromParam" takes the host's 0..1.
//---------------------------------------------------------------------------

/// A recipe slider: exactly 0 at the bottom (none of that reagent), else a
/// log multiplier of the cited 1x recipe from 1/4x (just above 0) to 4x (1);
/// 0.5 is 1x.
double RecipeMultiplierFromParam( float v );
float ParamFromRecipeMultiplier( double m );
/// Millimetres across the frame: 10 to 300, geometric.
double DishWidthFromParam( float v );
float ParamFromDishWidth( double mm );
/// The layer's depth in millimetres: 0.3 to 20, geometric.
double DepthFromParam( float v );
float ParamFromDepth( double mm );
/// Chemical seconds per host second: 1 to 300, geometric.
double TimelapseFromParam( float v );
float ParamFromTimelapse( double x );
/// A drop's diameter in millimetres: 1 to 20, geometric.
double DropSizeFromParam( float v );
float ParamFromDropSize( double mm );
/// Excitability: the Oregonator's f, 1 to 4 (geometric). 1.4 (the default)
/// oscillates in bulk; past the Hopf point 1 + sqrt 2 = 2.414 the rest state
/// is stable and excitable, where a broken wave winds a spiral.
double ExcitabilityFromParam( float v );
float ParamFromExcitability( double f );
/// Drops per minute of chemical time: 0 at the bottom, else 1 to 60.
double AutoDropFromParam( float v );
/// Exposure in stops: -2 to +2, 0 at 0.5.
double ExposureStopsFromParam( float v );
/// Stir, 0..1, as the stir bar's angular velocity (rad/s of chemical time)
/// and the eddy diffusivity (mm^2/s) and the whole-vessel relaxation rate
/// (1/s). See AGENTS.md: the closure is a model.
double StirOmegaFromParam( float v );
double StirEddyFromParam( float v );
double StirRelaxFromParam( float v );

int DetailCells( float v );
int OptionIndex( float value, int count );

struct Grid
{
	int cols = 0, rows = 0;
	bool operator==( const Grid& o ) const
	{
		return cols == o.cols && rows == o.rows;
	}
	bool operator!=( const Grid& o ) const
	{
		return !( *this == o );
	}
};
/// The grid for a raster: `cols` cells across, rows from the raster's ASPECT
/// (never its size), so a resize of the same shape keeps the chemistry.
Grid GridFor( int cols, int width, int height );

} // namespace honeydew
