/*
    The page's way in to the plugin: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call.

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Honeydew.cpp` and everything it calls -- Chemistry,
    Spectra, Controls, BrEngine, Audio, PassBuffer, Shaders, Diag -- compiled
    UNMODIFIED into the same WebAssembly module as this file (see
    demo/tools/build-wasm.sh for the list). This file only plays the host:

      - it constructs a `HoneydewPlugin`, as SourcePlugin.cpp / EffectPlugin.cpp's
        registrations would, and calls InitGL / DeInitGL on it;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters (CFFGLPluginManager::GetParamName and the rest,
        the SDK compiled unmodified too), so the page's panel is built from
        the plugin's constructor rather than from a copy of it;
      - it forwards the host's calls: SetFloatParameter, SetTime, SetBeatInfo,
        ProcessOpenGL with a viewport (the source) or one input texture (the
        Over), exactly as `tools/hdtest`'s Rig does;
      - it answers the page's read-outs through the plugin's own accessors --
        the ones the harness uses: ChemicalTime, SubstepsTaken, CappedFrames,
        CurrentGrid, CellMm, MeanState, CurrentRecipe -- and Controls.cpp's
        conversions for the number beside each slider.

    The clock is the page's: SetTime is handed the kit's seconds, and the
    plugin's own vote decides the unit (it settles on seconds within four
    frames, as it would in Arena). There is no host transport: the plugin
    keeps its own bar phase at the BPM the page sends (120 at first).
*/
#include "Chemistry.h"
#include "Controls.h"
#include "Diag.h"
#include "Honeydew.h"

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using namespace honeydew;

namespace
{
/// One plugin instance and the host-side structs it is handed.
struct Instance
{
	explicit Instance( bool effect ) :
		plugin( effect )
	{
	}

	HoneydewPlugin plugin;
	bool initialised = false;

	FFGLTextureStruct input {};
	FFGLTextureStruct* inputs[ 1 ] = { &input };
	ProcessOpenGLStruct process {};

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}

constexpr double kNaN = std::numeric_limits< double >::quiet_NaN();
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE Instance* hd_new( int effect )
{
	return new Instance( effect != 0 );
}

EMSCRIPTEN_KEEPALIVE void hd_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int hd_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< GLuint >( width );
	viewport.height = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int hd_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* hd_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int hd_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE int hd_param_usage( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamUsage( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* hd_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter (FF_TYPE_STANDARD already
/// clamped into 0..1 by the SDK, as a host sees it).
EMSCRIPTEN_KEEPALIVE float hd_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a text parameter.
EMSCRIPTEN_KEEPALIVE const char* hd_param_default_text( Instance* instance, int index )
{
	const FFMixed mixed = instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) );
	return mixed.PointerValue ? static_cast< const char* >( mixed.PointerValue ) : "";
}

EMSCRIPTEN_KEEPALIVE int hd_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* hd_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float hd_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE float hd_param_range_min( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).min;
}

EMSCRIPTEN_KEEPALIVE float hd_param_range_max( Instance* instance, int index )
{
	return instance->plugin.GetParamRange( static_cast< unsigned int >( index ) ).max;
}

/// The plugin's own id for a host index (Controls.h's ParamId): host indices
/// differ between the two plugins, ids do not. -1 past the end.
EMSCRIPTEN_KEEPALIVE int hd_param_id( Instance* instance, int index )
{
	const auto& order = HostOrder( instance->plugin.IsEffect() );
	return index >= 0 && static_cast< size_t >( index ) < order.size() ? static_cast< int >( order[ static_cast< size_t >( index ) ] ) : -1;
}

EMSCRIPTEN_KEEPALIVE int hd_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int hd_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float hd_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

/// Resolume's transport, as FFGL 2.1 hands it over. A NaN bar phase leaves the
/// plugin's own phase alone (Transport.h checks isfinite), so the page can set
/// a BPM without pretending to know where the bar is.
EMSCRIPTEN_KEEPALIVE void hd_set_beat_info( Instance* instance, float bpm, float barPhase )
{
	instance->plugin.SetBeatInfo( bpm, barPhase );
}

/// One frame: SetTime, then ProcessOpenGL into whatever the page has bound.
/// The source reads its size from the viewport the page set; the Over is
/// handed one input of clipWidth x clipHeight whose GL name is `clip` (a
/// texture the page registered with emscripten's GL tables). 1 on success.
EMSCRIPTEN_KEEPALIVE int hd_process( Instance* instance, double seconds, int clip, int clipWidth, int clipHeight )
{
	instance->plugin.SetTime( seconds );
	if( instance->plugin.IsEffect() )
	{
		instance->input.Width = instance->input.HardwareWidth = static_cast< FFUInt32 >( clipWidth );
		instance->input.Height = instance->input.HardwareHeight = static_cast< FFUInt32 >( clipHeight );
		instance->input.Handle                                 = static_cast< GLuint >( clip );
		instance->process.numInputTextures                     = 1;
		instance->process.inputTextures                        = instance->inputs;
	}
	else
	{
		instance->process.numInputTextures = 0;
		instance->process.inputTextures    = nullptr;
	}
	instance->process.HostFBO = 0;
	return instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS ? 1 : 0;
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.cpp's conversion for a parameter, by
// the plugin's id -- the one Honeydew.cpp applies to it. NaN where the plugin
// uses the host's 0..1 as it is (Stir has three closures; Light Coupling and
// Mix are fractions).
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double hd_convert( int id, float v )
{
	switch( id )
	{
	case PT_OXIDANT:
	case PT_ACID_BASE:
	case PT_REDUCTANT:
	case PT_INDICATOR: return RecipeMultiplierFromParam( v );
	case PT_DISH_WIDTH: return DishWidthFromParam( v );
	case PT_DEPTH: return DepthFromParam( v );
	case PT_TIMELAPSE: return TimelapseFromParam( v );
	case PT_DROP_SIZE: return DropSizeFromParam( v );
	case PT_AUTO_DROP: return AutoDropFromParam( v );
	case PT_EXPOSURE: return ExposureStopsFromParam( v );
	default: return kNaN;
	}
}

/// The inverse, for the page's presets: the host's 0..1 that gives a value in
/// the plugin's units, by Controls.cpp's own ParamFrom* functions. NaN where
/// the plugin has none.
EMSCRIPTEN_KEEPALIVE double hd_param_from( int id, double value )
{
	switch( id )
	{
	case PT_OXIDANT:
	case PT_ACID_BASE:
	case PT_REDUCTANT:
	case PT_INDICATOR: return ParamFromRecipeMultiplier( value );
	case PT_DISH_WIDTH: return ParamFromDishWidth( value );
	case PT_DEPTH: return ParamFromDepth( value );
	case PT_TIMELAPSE: return ParamFromTimelapse( value );
	case PT_DROP_SIZE: return ParamFromDropSize( value );
	default: return kNaN;
	}
}

//---------------------------------------------------------------------------
// The status line: the plugin's own accessors, the ones hdtest reads.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double hd_chem_time( Instance* instance )
{
	return instance->plugin.ChemicalTime();
}

EMSCRIPTEN_KEEPALIVE double hd_chem_time_total( Instance* instance )
{
	return instance->plugin.ChemicalTimeTotal();
}

EMSCRIPTEN_KEEPALIVE double hd_lost_chem( Instance* instance )
{
	return instance->plugin.LostChemicalTime();
}

EMSCRIPTEN_KEEPALIVE double hd_capped_frames( Instance* instance )
{
	return static_cast< double >( instance->plugin.CappedFrames() );
}

EMSCRIPTEN_KEEPALIVE double hd_substeps( Instance* instance )
{
	return static_cast< double >( instance->plugin.SubstepsTaken() );
}

EMSCRIPTEN_KEEPALIVE double hd_host_dt( Instance* instance )
{
	return instance->plugin.LastHostDt();
}

EMSCRIPTEN_KEEPALIVE int hd_grid_cols( Instance* instance )
{
	return instance->plugin.CurrentGrid().cols;
}

EMSCRIPTEN_KEEPALIVE int hd_grid_rows( Instance* instance )
{
	return instance->plugin.CurrentGrid().rows;
}

EMSCRIPTEN_KEEPALIVE double hd_cell_mm( Instance* instance )
{
	return instance->plugin.CellMm();
}

EMSCRIPTEN_KEEPALIVE double hd_drops( Instance* instance )
{
	return static_cast< double >( instance->plugin.DropsMade() );
}

EMSCRIPTEN_KEEPALIVE double hd_doses( Instance* instance )
{
	return static_cast< double >( instance->plugin.DosesMade() );
}

EMSCRIPTEN_KEEPALIVE int hd_seeded( Instance* instance )
{
	return instance->plugin.LastFrameSeeded() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int hd_reaction( Instance* instance )
{
	return static_cast< int >( instance->plugin.CurrentReaction() );
}

/// The dish's mean state as read back after the last frame: `which` 0 for A,
/// 1 for B; channel 0..3. What Clock Sync decides on.
EMSCRIPTEN_KEEPALIVE double hd_mean( Instance* instance, int which, int channel )
{
	double a[ 4 ], b[ 4 ];
	instance->plugin.MeanState( a, b );
	if( channel < 0 || channel > 3 )
		return kNaN;
	return which == 0 ? a[ channel ] : b[ channel ];
}

/// The recipe at the sliders, in mol/L: 0 oxidant, 1 acid or base, 2
/// reductant, 3 indicator (Chemistry.h's Recipe, by CurrentRecipe()).
EMSCRIPTEN_KEEPALIVE double hd_recipe( Instance* instance, int which )
{
	const chem::Recipe r = instance->plugin.CurrentRecipe();
	switch( which )
	{
	case 0: return r.oxidant;
	case 1: return r.acidBase;
	case 2: return r.reductant;
	case 3: return r.indicator;
	default: return kNaN;
	}
}

/// The Briggs-Rauscher engine's ROS2 steps so far (its own counter).
EMSCRIPTEN_KEEPALIVE double hd_br_steps( Instance* instance )
{
	return static_cast< double >( instance->plugin.BR().StepsTaken() );
}

/// The Briggs-Rauscher engine's mean of one of its ten species over its grid
/// (Chemistry.h's BRIndex order), as --briggs reads it. The plugin's MeanState
/// is the GPU path's and is not written while BR runs.
EMSCRIPTEN_KEEPALIVE double hd_br_mean( Instance* instance, int species )
{
	if( species < 0 || species >= chem::kBRSpecies )
		return kNaN;
	return instance->plugin.BR().MeanOf( species );
}

/// One texel of the state as it stands after the last frame (A for `which`
/// 0, B for 1), read back through the page's read framebuffer: what hdtest
/// reads through StateATextureID / StateBTextureID. For checking the page
/// from outside; the picture itself never goes through this.
EMSCRIPTEN_KEEPALIVE double hd_state_texel( Instance* instance, int which, int x, int y, int channel )
{
	if( channel < 0 || channel > 3 || !instance->initialised )
		return kNaN;
	const GLuint texture = which == 0 ? instance->plugin.StateATextureID() : instance->plugin.StateBTextureID();
	if( texture == 0 )
		return kNaN;
	static GLuint fbo = 0;
	if( fbo == 0 )
		glGenFramebuffers( 1, &fbo );
	GLint previousRead = 0;
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &previousRead );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	float texel[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glReadPixels( x, y, 1, 1, GL_RGBA, GL_FLOAT, texel );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, static_cast< GLuint >( previousRead ) );
	return texel[ channel ];
}

/// Give the plugin's log (Diag.cpp) somewhere to go: a directory in the
/// page's in-memory file system, named through the override Diag.cpp reads.
/// Its own `mkdir -p` goes through system(), which a browser does not have.
/// Call before the first InitGL, which is when the log opens.
EMSCRIPTEN_KEEPALIVE void hd_prepare_log()
{
	setenv( "HONEYDEW_LOG_DIR", "/honeydew/logs", 1 );
	mkdir( "/honeydew", 0755 );
	mkdir( "/honeydew/logs", 0755 );
}

/// The plugin's log file (Diag.cpp), in the page's in-memory file system.
EMSCRIPTEN_KEEPALIVE const char* hd_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

} // extern "C"
