#include "Honeydew.h"

#include "Diag.h"
#include "GLState.h"
#include "Hash.h"
#include "Shaders.h"
#include "Spectra.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

using namespace ffglex;

namespace honeydew
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

constexpr int kClockVotes = 4;
/// A shake's stirring burst decays over this many REAL seconds.
constexpr double kShakeTau = 0.7;
/// Clock Sync aims no closer than this to a boundary (real seconds).
constexpr double kSyncLead = 0.25;
/// Clock Sync holds the changed state for this share of a beat or bar before
/// the dose that runs the cycle again: a snap that cleared on the next frame
/// would be a 16 ms flash.
constexpr double kSyncHold = 0.25;
/// The Over's thumb for Drop Position Brightest.
constexpr int kThumbW = 32, kThumbH = 18;
/// The stir bar's core, as a fraction of the dish's radius.
constexpr double kCoreFraction = 0.2;

const char* const kReactorNames[]  = { "Batch", "Flow" };
const char* const kVesselNames[]   = { "Full Frame", "Petri Dish" };
const char* const kDropPosNames[]  = { "Random", "Centre", "Brightest" };
const char* const kSyncNames[]     = { "Off", "Beat", "Bar" };
const char* const kDetailNames[]   = { "128", "256", "512", "1024" };

void setFloats( FFGLShader& shader, const char* name, const float* v, int count )
{
	glUniform1fv( glGetUniformLocation( shader.GetGLID(), name ), count, v );
}
void setVec3s( FFGLShader& shader, const char* name, const float* v, int count )
{
	glUniform3fv( glGetUniformLocation( shader.GetGLID(), name ), count, v );
}
void setMat3s( FFGLShader& shader, const char* name, const float* v, int count )
{
	glUniformMatrix3fv( glGetUniformLocation( shader.GetGLID(), name ), count, GL_FALSE, v );
}
void setUint( FFGLShader& shader, const char* name, uint32_t value )
{
	glUniform1ui( glGetUniformLocation( shader.GetGLID(), name ), value );
}
void setIvec2( FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), x, y );
}
void setVec4( FFGLShader& shader, const char* name, float a, float b, float c, float d )
{
	glUniform4f( glGetUniformLocation( shader.GetGLID(), name ), a, b, c, d );
}
void setVec3Array( FFGLShader& shader, const char* name, const std::vector< float >& v )
{
	glUniform3fv( glGetUniformLocation( shader.GetGLID(), name ), static_cast< GLsizei >( v.size() / 3 ), v.data() );
}

/// Attach up to three textures to the MRT framebuffer and draw into them.
void bindMRT( GLuint fbo, GLuint t0, GLuint t1, GLuint t2, int w, int h )
{
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t0, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t1, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, t2, 0 );
	const GLenum buffers[ 3 ] = { GL_COLOR_ATTACHMENT0, static_cast< GLenum >( t1 ? GL_COLOR_ATTACHMENT1 : GL_NONE ), static_cast< GLenum >( t2 ? GL_COLOR_ATTACHMENT2 : GL_NONE ) };
	glDrawBuffers( 3, buffers );
	glViewport( 0, 0, w, h );
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
HoneydewPlugin::HoneydewPlugin( bool effect ) :
	isEffect( effect ),
	hostOrder( HostOrder( effect ) )
{
	SetMinInputs( isEffect ? 1 : 0 );
	SetMaxInputs( isEffect ? 1 : 0 );
	SetTimeSupported( true );

	std::fill( std::begin( idToHost ), std::end( idToHost ), -1 );
	for( size_t i = 0; i < hostOrder.size(); ++i )
		idToHost[ hostOrder[ i ] ] = static_cast< int >( i );

	//-------------------------------------------------------------------
	// Defaults: a ferroin BZ dish, 60 mm across and 1.5 mm deep, on a D65
	// lightbox, at 30x time-lapse, fed (Flow) so it oscillates for ever.
	//-------------------------------------------------------------------
	params[ PT_REACTION ]      = static_cast< float >( Reaction::BZ );
	params[ PT_CATALYST ]      = static_cast< float >( Catalyst::Ferroin );
	params[ PT_REACTOR ]       = static_cast< float >( Reactor::Flow );
	params[ PT_SEED ]          = 1.0f;
	params[ PT_OXIDANT ]       = 0.5f;
	params[ PT_ACID_BASE ]     = 0.5f;
	params[ PT_REDUCTANT ]     = 0.5f;
	params[ PT_INDICATOR ]     = 0.5f;
	params[ PT_VESSEL ]        = static_cast< float >( Vessel::FullFrame );
	params[ PT_DISH_WIDTH ]    = ParamFromDishWidth( 60.0 );
	params[ PT_DEPTH ]         = ParamFromDepth( 1.5 );
	params[ PT_STIR ]          = 0.0f;
	params[ PT_TIMELAPSE ]     = ParamFromTimelapse( 30.0 );
	params[ PT_DETAIL ]        = 1.0f;
	params[ PT_CONVECTION ]    = 0.0f;
	params[ PT_DROP_SIZE ]     = ParamFromDropSize( 4.0 );
	params[ PT_DROP_POSITION ] = static_cast< float >( DropPosition::Random );
	params[ PT_AUTO_DROP ]     = 0.0f;
	params[ PT_AUDIO_DROPS ]   = 0.0f;
	params[ PT_AUDIO_SHAKES ]  = 0.0f;
	params[ PT_CLOCK_SYNC ]    = static_cast< float >( ClockSync::Off );
	params[ PT_LIGHT_COUPLING ] = 0.5f;
	params[ PT_LIGHTBOX ]      = static_cast< float >( Lightbox::D65 );
	params[ PT_EXPOSURE ]      = 0.5f;
	params[ PT_SEED_FROM_CLIP ] = 1.0f;
	params[ PT_MIX ]           = 1.0f;

	for( unsigned int host = 0; host < hostOrder.size(); ++host )
	{
		const unsigned int id = hostOrder[ host ];
		const char* name      = NameOf( id );
		auto option = [ & ]( int count, const char* const* names ) {
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
		};
		switch( id )
		{
		case PT_REACTION:
		{
			const int count = static_cast< int >( Reaction::Count );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), ReactionName( static_cast< Reaction >( i ) ), static_cast< float >( i ) );
			break;
		}
		case PT_CATALYST:
		{
			const int count = static_cast< int >( Catalyst::Count );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), CatalystName( static_cast< Catalyst >( i ) ), static_cast< float >( i ) );
			break;
		}
		case PT_REACTOR: option( 2, kReactorNames ); break;
		case PT_VESSEL: option( 2, kVesselNames ); break;
		case PT_DROP_POSITION: option( DropPositionCount( isEffect ), kDropPosNames ); break;
		case PT_CLOCK_SYNC: option( 3, kSyncNames ); break;
		case PT_DETAIL: option( kDetailCount, kDetailNames ); break;
		case PT_LIGHTBOX:
		{
			const int count = static_cast< int >( Lightbox::Count );
			SetOptionParamInfo( host, name, static_cast< unsigned int >( count ), params[ id ] );
			for( int i = 0; i < count; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), LightboxName( static_cast< Lightbox >( i ) ), static_cast< float >( i ) );
			break;
		}
		case PT_SEED:
			//Only FF_TYPE_STANDARD has its default clamped into 0..1, so an
			//integer is declared with its real default and range.
			SetParamInfo( host, name, FF_TYPE_INTEGER, params[ id ] );
			SetParamRange( host, 0.0f, 9999.0f );
			break;
		case PT_RESET:
		case PT_DROP:
		case PT_BREAK_WAVE:
		case PT_SHAKE:
			SetParamInfo( host, name, FF_TYPE_EVENT, false );
			break;
		case PT_CONVECTION:
		case PT_AUDIO_DROPS:
		case PT_AUDIO_SHAKES:
		case PT_SEED_FROM_CLIP:
			SetParamInfo( host, name, FF_TYPE_BOOLEAN, params[ id ] > 0.5f );
			break;
		case PT_AUDIO:
			//An FFT buffer: Resolume shows it as an audio-source picker.
			SetBufferParamInfo( host, name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( host, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		case PT_ABOUT_TEXT:
			SetParamInfo( host, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
			break;
		default:
			if( id > PT_ABOUT_TEXT )
				SetParamInfo( host, stoatworks::about::buttons()[ id - PT_ABOUT_TEXT - 1 ].label, FF_TYPE_EVENT, false );
			else
				SetParamInfo( host, name, FF_TYPE_STANDARD, params[ id ] );
			break;
		}
		SetParamGroup( host, GroupOf( id ) );
	}
}

HoneydewPlugin::~HoneydewPlugin() = default;

//---------------------------------------------------------------------------
FFResult HoneydewPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	using namespace shaders;
	const std::string quadVertex = std::string( kVersion ) + kQuadVertex;
	struct Stage
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &seedShader, Assemble( kStateCommon, kSeedFragment ), "seed" },
		{ &stepShader, Assemble( kStateCommon, kStepFragmentA, kStepFragmentB ), "step" },
		{ &advectShader, Assemble( kStateCommon, kAdvectFragment ), "advect" },
		{ &relaxShader, Assemble( kStateCommon, kRelaxFragment ), "relax" },
		{ &columnsShader, Assemble( kStateCommon, kColumnsFragment ), "columns" },
		{ &totalShader, Assemble( kStateCommon, kTotalFragment ), "total" },
		{ &colourShader, Assemble( kStateCommon, kColourFragment ), "colour" },
		{ &compositeShader, Assemble( kCompositeFragment ), "composite" },
		{ &thumbShader, Assemble( kThumbFragment ), "thumb" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( quadVertex.c_str(), stage.fragment.c_str() ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Honeydew: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}
	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//A sampler bound to texture 0 is "unloadable" to Apple's GL (boreal's
	//trap): the source has no clip, so its clip samplers get this.
	glGenTextures( 1, &blankClip );
	glBindTexture( GL_TEXTURE_2D, blankClip );
	const unsigned char black[ 4 ] = { 0, 0, 0, 0 };
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

	//The spectra: every species' epsilon at the 41 samples.
	const std::vector< float > eps = spectra::EpsilonTable();
	glGenTextures( 1, &epsTexture );
	glBindTexture( GL_TEXTURE_2D, epsTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, cie::kLambdaCount, spectra::S_COUNT, 0, GL_RED, GL_FLOAT, eps.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glGenFramebuffers( 1, &mrtFBO );
	primaryWeights  = spectra::PrimaryWeights();
	lightWeightsFor = -1;

	diag::info( isEffect ? "initialised (Over)" : "initialised (source)" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void HoneydewPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale  = millisVotes > secondsVotes ? 0.001 : 1.0;
				settledJump = true;
			}
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

uint32_t HoneydewPlugin::seedSalt() const
{
	const uint32_t seed = static_cast< uint32_t >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
	return Pcg( seed * 2654435761u ^ Pcg( seedSerial + 0x9e3779b9u ) );
}

Reaction HoneydewPlugin::CurrentReaction() const
{
	return static_cast< Reaction >( OptionIndex( params[ PT_REACTION ], static_cast< int >( Reaction::Count ) ) );
}

chem::Recipe HoneydewPlugin::CurrentRecipe() const
{
	return chem::ScaledRecipe( CurrentReaction(), RecipeMultiplierFromParam( params[ PT_OXIDANT ] ),
	                           RecipeMultiplierFromParam( params[ PT_ACID_BASE ] ), RecipeMultiplierFromParam( params[ PT_REDUCTANT ] ),
	                           RecipeMultiplierFromParam( params[ PT_INDICATOR ] ) );
}

chem::Params HoneydewPlugin::CurrentParams() const
{
	const Reaction r = CurrentReaction();
	chem::Params p   = chem::MakeParams( r, CurrentRecipe(), static_cast< Catalyst >( OptionIndex( params[ PT_CATALYST ], 3 ) ),
	                                     static_cast< Reactor >( OptionIndex( params[ PT_REACTOR ], 2 ) ), DepthFromParam( params[ PT_DEPTH ] ),
	                                     std::clamp( params[ PT_STIR ], 0.0f, 1.0f ) );
	for( const auto& o : overrides )
		if( o.first >= 0 && o.first < chem::kParamCount )
			p[ static_cast< size_t >( o.first ) ] = o.second;
	return p;
}

//---------------------------------------------------------------------------
bool HoneydewPlugin::ensureBuffers( const Grid& want )
{
	bool ok = true;
	for( int i = 0; i < 2; ++i )
	{
		ok = ok && stateA[ i ].Ensure( want.cols, want.rows, GL_RGBA32F, PassBuffer::Filter::Linear );
		ok = ok && stateB[ i ].Ensure( want.cols, want.rows, GL_RGBA32F, PassBuffer::Filter::Linear );
	}
	ok = ok && colour0.Ensure( want.cols, want.rows, GL_RGBA32F );
	ok = ok && colour1.Ensure( want.cols, want.rows, GL_RGBA32F );
	ok = ok && colour2.Ensure( want.cols, want.rows, GL_RGBA32F );
	ok = ok && columnsA.Ensure( want.cols, 1, GL_RGBA32F ) && columnsB.Ensure( want.cols, 1, GL_RGBA32F );
	ok = ok && totalA.Ensure( 1, 1, GL_RGBA32F ) && totalB.Ensure( 1, 1, GL_RGBA32F );
	ok = ok && thumb.Ensure( kThumbW, kThumbH, GL_RGBA32F );
	return ok;
}

//---------------------------------------------------------------------------
void HoneydewPlugin::seed()
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	double a[ 4 ], b[ 4 ];
	chem::FreshState( r, p, a, b );
	ScopedShaderBinding shader( seedShader.GetGLID() );
	bindUnit( 0, stateA[ 1 - cur ].TextureID() );//unused by the seed, but every sampler needs a texture
	bindUnit( 1, stateB[ 1 - cur ].TextureID() );
	bindUnit( 2, isEffect && params[ PT_SEED_FROM_CLIP ] > 0.5f && lastClipTexture ? lastClipTexture : blankClip );
	seedShader.Set( "StateA", 0 );
	seedShader.Set( "StateB", 1 );
	seedShader.Set( "Clip", 2 );
	setIvec2( seedShader, "Grid", grid.cols, grid.rows );
	setFloats( seedShader, "Params", p.data(), chem::kParamCount );
	seedShader.Set( "Reaction", static_cast< int >( r ) );
	seedShader.Set( "Vessel", vesselNow );
	seedShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
	seedShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
	setVec4( seedShader, "FreshA", static_cast< float >( a[ 0 ] ), static_cast< float >( a[ 1 ] ), static_cast< float >( a[ 2 ] ), static_cast< float >( a[ 3 ] ) );
	setVec4( seedShader, "FreshB", static_cast< float >( b[ 0 ] ), static_cast< float >( b[ 1 ] ), static_cast< float >( b[ 2 ] ), static_cast< float >( b[ 3 ] ) );
	setUint( seedShader, "Salt", seedSalt() );
	seedShader.Set( "SeedFromClip", isEffect && params[ PT_SEED_FROM_CLIP ] > 0.5f && lastClipTexture ? 1 : 0 );
	seedShader.Set( "NoiseAmp", r == Reaction::CDIMA ? 0.02f : 0.01f );
	seedShader.Set( "PaceDensity", 0.3f );
	bindMRT( mrtFBO, stateA[ cur ].TextureID(), stateB[ cur ].TextureID(), 0, grid.cols, grid.rows );
	quad.Draw();
	unbindTextureUnits( 3 );
	chemTime        = 0.0;
	seededThisFrame = true;
	syncArmed       = true;
	chameleonDoseAt = -1.0;
	syncDoseAt      = -1.0;
	meanValid       = false;
}

//---------------------------------------------------------------------------
void HoneydewPlugin::stepGPU( int substeps, double dt, const std::vector< DropRequest >& drops, double dropAmount, bool bar,
                              const BarRequest& barReq, bool dose, double doseAmount, double aeration, double eddy )
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	float dA[ 4 ] = { 0, 0, 0, 0 }, dB[ 4 ] = { 0, 0, 0, 0 };
	switch( r )
	{
	case Reaction::BZ:
		dA[ 0 ] = p[ chem::P_BZ_DU ];
		dA[ 1 ] = p[ chem::P_BZ_DY ];
		dA[ 2 ] = p[ chem::P_BZ_DV ];
		break;
	case Reaction::CDIMA:
		dA[ 0 ] = p[ chem::P_LE_DU ] / std::max( p[ chem::P_LE_SIGMA ], 1.0f );
		dA[ 1 ] = p[ chem::P_LE_DU ] * p[ chem::P_LE_D ];
		break;
	case Reaction::IodineClock:
		dA[ 0 ] = p[ chem::P_CK_DH2O2 ];
		dA[ 1 ] = p[ chem::P_CK_DI ];
		dA[ 2 ] = p[ chem::P_CK_DI2 ];
		dA[ 3 ] = p[ chem::P_CK_DS ];
		break;
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
		dA[ 0 ] = p[ chem::P_DY_DO2 ];
		dA[ 1 ] = p[ chem::P_DY_DGL ];
		dA[ 2 ] = dA[ 3 ] = p[ chem::P_DY_DDYE ];
		dB[ 0 ] = p[ chem::P_DY_DDYE ];
		dB[ 1 ] = p[ chem::P_DY_DGL ];
		dB[ 2 ] = p[ chem::P_DY_DDYE ];
		break;
	case Reaction::Chameleon:
		dA[ 0 ] = dA[ 1 ] = p[ chem::P_CH_DMN ];
		dA[ 2 ]           = p[ chem::P_CH_DMNO2 ];
		dA[ 3 ]           = p[ chem::P_CH_DGL ];
		break;
	default: break;
	}
	ScopedShaderBinding shader( stepShader.GetGLID() );
	stepShader.Set( "StateA", 0 );
	stepShader.Set( "StateB", 1 );
	stepShader.Set( "Clip", 2 );
	bindUnit( 2, isEffect && lastClipTexture ? lastClipTexture : blankClip );
	setIvec2( stepShader, "Grid", grid.cols, grid.rows );
	setFloats( stepShader, "Params", p.data(), chem::kParamCount );
	stepShader.Set( "Reaction", static_cast< int >( r ) );
	stepShader.Set( "Vessel", vesselNow );
	stepShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
	stepShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
	stepShader.Set( "Dt", static_cast< float >( dt ) );
	stepShader.Set( "Aeration", static_cast< float >( aeration ) );
	stepShader.Set( "DropAmount", static_cast< float >( dropAmount ) );
	stepShader.Set( "DiffusionOffForTest", diffusionOff ? 1 : 0 );
	stepShader.Set( "PhotoOffForTest", photoOff ? 1 : 0 );
	stepShader.Set( "FuelOffForTest", fuelOff ? 1 : 0 );
	setVec4( stepShader, "DiffA", dA[ 0 ], dA[ 1 ], dA[ 2 ], dA[ 3 ] );
	setVec4( stepShader, "DiffB", dB[ 0 ], dB[ 1 ], dB[ 2 ], dB[ 3 ] );
	stepShader.Set( "CellMm", static_cast< float >( cellMm ) );
	stepShader.Set( "Eddy", static_cast< float >( eddy ) );
	float dropData[ 12 ] = {};
	const int dropCount  = static_cast< int >( std::min< size_t >( drops.size(), 4 ) );
	for( int i = 0; i < dropCount; ++i )
	{
		dropData[ i * 3 + 0 ] = static_cast< float >( drops[ static_cast< size_t >( i ) ].cx );
		dropData[ i * 3 + 1 ] = static_cast< float >( drops[ static_cast< size_t >( i ) ].cy );
		dropData[ i * 3 + 2 ] = static_cast< float >( drops[ static_cast< size_t >( i ) ].radius );
	}
	setVec3s( stepShader, "Drops", dropData, 4 );
	setVec4( stepShader, "Bar", static_cast< float >( barReq.x0 ), static_cast< float >( barReq.y0 ), static_cast< float >( barReq.x1 ), static_cast< float >( barReq.y1 ) );
	stepShader.Set( "BarHalfWidth", static_cast< float >( barReq.halfWidth ) );
	stepShader.Set( "DoseAll", static_cast< float >( doseAmount ) );
	stepShader.Set( "LightCoupling", isEffect ? std::clamp( params[ PT_LIGHT_COUPLING ], 0.0f, 1.0f ) : 0.0f );
	stepShader.Set( "HasClip", isEffect && lastClipTexture ? 1 : 0 );
	for( int s = 0; s < substeps; ++s )
	{
		//Drops, the bar and the dose land on the first substep only.
		stepShader.Set( "DropCount", s == 0 ? dropCount : 0 );
		stepShader.Set( "BarOn", s == 0 && bar ? 1 : 0 );
		stepShader.Set( "DoseOn", s == 0 && dose ? 1 : 0 );
		bindUnit( 0, stateA[ cur ].TextureID() );
		bindUnit( 1, stateB[ cur ].TextureID() );
		bindMRT( mrtFBO, stateA[ 1 - cur ].TextureID(), stateB[ 1 - cur ].TextureID(), 0, grid.cols, grid.rows );
		quad.Draw();
		swapState();
	}
	unbindTextureUnits( 3 );
}

void HoneydewPlugin::advect( double angle )
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	ScopedShaderBinding shader( advectShader.GetGLID() );
	bindUnit( 0, stateA[ cur ].TextureID() );
	bindUnit( 1, stateB[ cur ].TextureID() );
	advectShader.Set( "StateA", 0 );
	advectShader.Set( "StateB", 1 );
	setIvec2( advectShader, "Grid", grid.cols, grid.rows );
	setFloats( advectShader, "Params", p.data(), chem::kParamCount );
	advectShader.Set( "Reaction", static_cast< int >( r ) );
	advectShader.Set( "Vessel", vesselNow );
	advectShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
	advectShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
	advectShader.Set( "Angle", static_cast< float >( angle ) );
	advectShader.Set( "CoreCells", static_cast< float >( kCoreFraction * dishRadiusCells ) );
	bindMRT( mrtFBO, stateA[ 1 - cur ].TextureID(), stateB[ 1 - cur ].TextureID(), 0, grid.cols, grid.rows );
	quad.Draw();
	swapState();
	unbindTextureUnits( 2 );
}

void HoneydewPlugin::reduceMean()
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	{
		ScopedShaderBinding shader( columnsShader.GetGLID() );
		bindUnit( 0, stateA[ cur ].TextureID() );
		bindUnit( 1, stateB[ cur ].TextureID() );
		columnsShader.Set( "StateA", 0 );
		columnsShader.Set( "StateB", 1 );
		setIvec2( columnsShader, "Grid", grid.cols, grid.rows );
		setFloats( columnsShader, "Params", p.data(), chem::kParamCount );
		columnsShader.Set( "Reaction", static_cast< int >( r ) );
		columnsShader.Set( "Vessel", vesselNow );
		columnsShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
		columnsShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
		bindMRT( mrtFBO, columnsA.TextureID(), columnsB.TextureID(), 0, grid.cols, 1 );
		quad.Draw();
		unbindTextureUnits( 2 );
	}
	{
		ScopedShaderBinding shader( totalShader.GetGLID() );
		bindUnit( 0, stateA[ cur ].TextureID() );
		bindUnit( 1, stateB[ cur ].TextureID() );
		bindUnit( 2, columnsA.TextureID() );
		bindUnit( 3, columnsB.TextureID() );
		totalShader.Set( "StateA", 0 );
		totalShader.Set( "StateB", 1 );
		totalShader.Set( "ColumnsA", 2 );
		totalShader.Set( "ColumnsB", 3 );
		setIvec2( totalShader, "Grid", grid.cols, grid.rows );
		setFloats( totalShader, "Params", p.data(), chem::kParamCount );
		totalShader.Set( "Reaction", static_cast< int >( r ) );
		totalShader.Set( "Vessel", vesselNow );
		totalShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
		totalShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
		totalShader.Set( "CellCount", static_cast< float >( dishCells ) );
		bindMRT( mrtFBO, totalA.TextureID(), totalB.TextureID(), 0, 1, 1 );
		quad.Draw();
		unbindTextureUnits( 4 );
	}
}

void HoneydewPlugin::readMean()
{
	float a[ 4 ] = {}, b[ 4 ] = {};
	glBindTexture( GL_TEXTURE_2D, totalA.TextureID() );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, a );
	glBindTexture( GL_TEXTURE_2D, totalB.TextureID() );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, b );
	glBindTexture( GL_TEXTURE_2D, 0 );
	for( int i = 0; i < 4; ++i )
	{
		meanA[ i ] = a[ i ];
		meanB[ i ] = b[ i ];
	}
	meanValid = true;
}

void HoneydewPlugin::relax( double alpha )
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	ScopedShaderBinding shader( relaxShader.GetGLID() );
	bindUnit( 0, stateA[ cur ].TextureID() );
	bindUnit( 1, stateB[ cur ].TextureID() );
	bindUnit( 2, totalA.TextureID() );
	bindUnit( 3, totalB.TextureID() );
	relaxShader.Set( "StateA", 0 );
	relaxShader.Set( "StateB", 1 );
	relaxShader.Set( "MeanA", 2 );
	relaxShader.Set( "MeanB", 3 );
	setIvec2( relaxShader, "Grid", grid.cols, grid.rows );
	setFloats( relaxShader, "Params", p.data(), chem::kParamCount );
	relaxShader.Set( "Reaction", static_cast< int >( r ) );
	relaxShader.Set( "Vessel", vesselNow );
	relaxShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
	relaxShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
	relaxShader.Set( "Alpha", static_cast< float >( alpha ) );
	//BZ's pacemaker flag is dust, not solute: it stirs round but does not average.
	setVec4( relaxShader, "MaskA", 1.0f, 1.0f, 1.0f, 1.0f );
	setVec4( relaxShader, "MaskB", r == Reaction::BZ ? 0.0f : 1.0f, 1.0f, 1.0f, 1.0f );
	bindMRT( mrtFBO, stateA[ 1 - cur ].TextureID(), stateB[ 1 - cur ].TextureID(), 0, grid.cols, grid.rows );
	quad.Draw();
	swapState();
	unbindTextureUnits( 4 );
}

//---------------------------------------------------------------------------
void HoneydewPlugin::colour( const FFGLTextureStruct* )
{
	const Reaction r     = CurrentReaction();
	const chem::Params p = CurrentParams();
	const chem::Recipe recipe = CurrentRecipe();
	const int box = OptionIndex( params[ PT_LIGHTBOX ], static_cast< int >( Lightbox::Count ) );
	if( box != lightWeightsFor )
	{
		lightWeights    = spectra::LightboxWeights( static_cast< Lightbox >( box ) );
		lightWeightsFor = box;
	}
	ScopedShaderBinding shader( colourShader.GetGLID() );
	bindUnit( 0, stateA[ cur ].TextureID() );
	bindUnit( 1, stateB[ cur ].TextureID() );
	bindUnit( 2, epsTexture );
	colourShader.Set( "StateA", 0 );
	colourShader.Set( "StateB", 1 );
	colourShader.Set( "Eps", 2 );
	setIvec2( colourShader, "Grid", grid.cols, grid.rows );
	setFloats( colourShader, "Params", p.data(), chem::kParamCount );
	colourShader.Set( "Reaction", static_cast< int >( r ) );
	colourShader.Set( "Vessel", vesselNow );
	colourShader.Set( "DishCentre", static_cast< float >( dishCentre[ 0 ] ), static_cast< float >( dishCentre[ 1 ] ) );
	colourShader.Set( "DishRadius", static_cast< float >( dishRadiusCells ) );
	setVec3Array( colourShader, "LightWeights", lightWeights );
	setMat3s( colourShader, "PrimaryWeights", primaryWeights.data(), cie::kLambdaCount );
	colourShader.Set( "IsEffect", isEffect ? 1 : 0 );
	colourShader.Set( "DepthCm", static_cast< float >( DepthFromParam( params[ PT_DEPTH ] ) * 0.1 ) );
	colourShader.Set( "Catalyst", OptionIndex( params[ PT_CATALYST ], 3 ) );
	colourShader.Set( "KI3", static_cast< float >( chem::kClock.KI3 ) );
	//Starch: the Indicator slot of the iodine reactions; CDIMA's sites too.
	const bool starchy = r == Reaction::BriggsRauscher || r == Reaction::IodineClock || r == Reaction::CDIMA;
	colourShader.Set( "StarchSites", static_cast< float >( starchy ? recipe.indicator : 0.0 ) );
	colourShader.Set( "KStarch", static_cast< float >( chem::kClock.KStarch ) );
	colourShader.Set( "KDimer", static_cast< float >( r == Reaction::BlueBottle ? 2000.0 : 0.0 ) );
	colourShader.Set( "ClO2Pool", static_cast< float >( r == Reaction::CDIMA ? recipe.oxidant : 0.0 ) );
	colourShader.Set( "LEu0", static_cast< float >( r == Reaction::CDIMA ? p[ chem::P_LE_A ] / 5.0 : 1.0 ) );
	colourShader.Set( "NaiveDepthForTest", naiveDepth ? 1 : 0 );
	bindMRT( mrtFBO, colour0.TextureID(), colour1.TextureID(), colour2.TextureID(), grid.cols, grid.rows );
	quad.Draw();
	unbindTextureUnits( 3 );
}

void HoneydewPlugin::composite( const FFGLTextureStruct* input, const GLint* hostViewport, GLuint hostFBO, int width, int height )
{
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	ScopedShaderBinding shader( compositeShader.GetGLID() );
	bindUnit( 0, colour0.TextureID() );
	bindUnit( 1, colour1.TextureID() );
	bindUnit( 2, colour2.TextureID() );
	bindUnit( 3, input ? input->Handle : blankClip );
	compositeShader.Set( "Col0", 0 );
	compositeShader.Set( "Col1", 1 );
	compositeShader.Set( "Col2", 2 );
	compositeShader.Set( "InputTexture", 3 );
	setIvec2( compositeShader, "Grid", grid.cols, grid.rows );
	compositeShader.Set( "IsEffect", isEffect ? 1 : 0 );
	compositeShader.Set( "Raster", static_cast< float >( width ), static_cast< float >( height ) );
	compositeShader.Set( "ViewOrigin", static_cast< float >( hostViewport[ 0 ] ), static_cast< float >( hostViewport[ 1 ] ) );
	compositeShader.Set( "MixAmount", isEffect ? std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) : 1.0f );
	compositeShader.Set( "Gain", static_cast< float >( std::exp2( ExposureStopsFromParam( params[ PT_EXPOSURE ] ) ) ) );
	compositeShader.Set( "Vessel", vesselNow );
	const double px = static_cast< double >( width ) / grid.cols;
	compositeShader.Set( "DishCentrePx", static_cast< float >( dishCentre[ 0 ] * px ), static_cast< float >( dishCentre[ 1 ] * px ) );
	compositeShader.Set( "DishRadiusPx", static_cast< float >( dishRadiusCells * px ) );
	quad.Draw();
	unbindTextureUnits( 4 );
}

bool HoneydewPlugin::brightestCell( const FFGLTextureStruct* input, double& cx, double& cy )
{
	if( !input )
		return false;
	{
		ScopedShaderBinding shader( thumbShader.GetGLID() );
		bindUnit( 0, input->Handle );
		thumbShader.Set( "Clip", 0 );
		thumbShader.Set( "ThumbSize", static_cast< float >( kThumbW ), static_cast< float >( kThumbH ) );
		thumb.BindForDrawing();
		quad.Draw();
		unbindTextureUnits( 1 );
	}
	float pixels[ kThumbW * kThumbH * 4 ];
	glBindTexture( GL_TEXTURE_2D, thumb.TextureID() );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels );
	glBindTexture( GL_TEXTURE_2D, 0 );
	int best = 0;
	for( int i = 1; i < kThumbW * kThumbH; ++i )
		if( pixels[ i * 4 ] > pixels[ best * 4 ] )
			best = i;
	cx = ( best % kThumbW + 0.5 ) / kThumbW * grid.cols;
	cy = ( best / kThumbW + 0.5 ) / kThumbH * grid.rows;
	return true;
}

void HoneydewPlugin::uploadBR()
{
	stateA[ cur ].Upload( br.Texture().data() );
}

//---------------------------------------------------------------------------
FFResult HoneydewPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct* input = nullptr;
	if( isEffect )
	{
		if( pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;
		input = pgl->inputTextures[ 0 ];
	}
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = input ? static_cast< int >( input->Width ) : hostViewport[ 2 ];
	const int height          = input ? static_cast< int >( input->Height ) : hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );
	lastClipTexture = input ? input->Handle : 0;
	seededThisFrame = false;

	//-------------------------------------------------------------------
	// Time: the host's, as real elapsed seconds, frame to frame, in double.
	//-------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		clock.Reset();
		lastNow     = -1.0;
		settledJump = false;
	}
	double hostDt = 0.0;
	if( floatClock )
	{
		//The wrong model: the elapsed time from the host's clock as a float.
		if( lastNow >= 0.0 )
		{
			const double step = static_cast< double >( static_cast< float >( now ) - static_cast< float >( lastNow ) );
			hostDt            = ( step >= 0.0 && step <= ChemicalClock::kMaxFrameSeconds ) ? step : 0.0;
		}
		lastNow = now;
	}
	else
		hostDt = clock.Advance( now );
	if( clock.TakeJump() )
		analyser.Reset();
	lastHostDt = hostDt;
	transport.Advance( hostDt );
	shakeAge += hostDt;

	//-------------------------------------------------------------------
	// Audio: an onset is a drop and/or a shake.
	//-------------------------------------------------------------------
	{
		float bins[ audio::kBins ] = {};
		int binCount               = 0;
		if( const ParamInfo* info = FindParamInfo( static_cast< unsigned int >( idToHost[ PT_AUDIO ] ) ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		audio::Settings settings;
		analyser.SetPrimingForTest( !unprimed );
		analyser.Update( bins, binCount, static_cast< float >( hostDt ), settings );
		const bool wantDrops = params[ PT_AUDIO_DROPS ] > 0.5f, wantShakes = params[ PT_AUDIO_SHAKES ] > 0.5f;
		if( analyser.Fired() && ( wantDrops || wantShakes ) )
		{
			++onsetsUsed;
			if( wantDrops )
				pendingDrops.push_back( { -1.0, -1.0, 0.0 } );//placed below
			if( wantShakes )
				shakePending = true;
		}
	}

	//-------------------------------------------------------------------
	// The buttons (a press is the rising edge), and what reseeds.
	//-------------------------------------------------------------------
	const Reaction reaction = CurrentReaction();
	{
		const int seedValue = static_cast< int >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 9999L ) );
		if( seedValue != lastSeed )
		{
			seedSerial = 0;
			needSeed   = true;
		}
		lastSeed             = seedValue;
		const bool resetDown = params[ PT_RESET ] > 0.5f;
		if( resetDown && !resetHeld )
		{
			++seedSerial;
			needSeed = true;
		}
		resetHeld = resetDown;
		if( static_cast< int >( reaction ) != lastReaction )
			needSeed = true;
		lastReaction = static_cast< int >( reaction );
		const bool dropDown = params[ PT_DROP ] > 0.5f;
		if( dropDown && !dropHeld )
			pendingDrops.push_back( { -1.0, -1.0, 0.0 } );
		dropHeld           = dropDown;
		const bool barDown = params[ PT_BREAK_WAVE ] > 0.5f;
		if( barDown && !barHeld )
			barPending = true;
		barHeld              = barDown;
		const bool shakeDown = params[ PT_SHAKE ] > 0.5f;
		if( shakeDown && !shakeHeld )
			shakePending = true;
		shakeHeld = shakeDown;
	}

	//-------------------------------------------------------------------
	// The grid, and every allocation, before anything is bound.
	//-------------------------------------------------------------------
	const int detail = OptionIndex( params[ PT_DETAIL ], kDetailCount );
	brActive         = reaction == Reaction::BriggsRauscher;
	Grid want        = gridOverride.cols > 0 ? gridOverride : GridFor( kDetailCells[ detail ], width, height );
	if( brActive && gridOverride.cols <= 0 )
		want = GridFor( BrEngine::kMaxCols, width, height );
	const bool first   = !stateA[ 0 ].IsValid();
	const Grid oldGrid = grid;
	if( !first && want != grid )
	{
		//A new grid: the old state resampled into it (the advect pass with no
		//rotation) before the old buffers are freed. A Detail change keeps
		//the chemistry.
		PassBuffer oldA, oldB;
		stateA[ cur ].MoveTo( oldA );
		stateB[ cur ].MoveTo( oldB );
		if( !ensureBuffers( want ) )
		{
			diag::error( "could not allocate the state buffers" );
			return FF_FAIL;
		}
		grid   = want;
		cellMm = DishWidthFromParam( params[ PT_DISH_WIDTH ] ) / grid.cols;
		if( !brActive )
		{
			const chem::Params p = CurrentParams();
			ScopedShaderBinding shader( advectShader.GetGLID() );
			bindUnit( 0, oldA.TextureID() );
			bindUnit( 1, oldB.TextureID() );
			advectShader.Set( "StateA", 0 );
			advectShader.Set( "StateB", 1 );
			setIvec2( advectShader, "Grid", oldGrid.cols, oldGrid.rows );
			setFloats( advectShader, "Params", p.data(), chem::kParamCount );
			advectShader.Set( "Reaction", static_cast< int >( reaction ) );
			advectShader.Set( "Vessel", 0 );
			advectShader.Set( "DishCentre", 0.0f, 0.0f );
			advectShader.Set( "DishRadius", 0.0f );
			advectShader.Set( "Angle", 0.0f );
			advectShader.Set( "CoreCells", 1.0f );
			//The advect pass maps gl_FragCoord cells of the NEW grid onto the OLD
			//texture's uv: Grid must be the old one for the uv, and the viewport
			//the new size. The pass clamps to the old grid.
			bindMRT( mrtFBO, stateA[ cur ].TextureID(), stateB[ cur ].TextureID(), 0, grid.cols, grid.rows );
			//gl_FragCoord runs over the new size, so scale: handled by the
			//uv division by Grid (old) -- the new cell c maps to uv c/old, which
			//is wrong unless the sizes match. Resample by a scaled read instead:
			setIvec2( advectShader, "Grid", grid.cols, grid.rows );
			//With Grid = new, uv = c/new in [0,1): the old texture is read at
			//the same fraction of its extent: a bilinear resample.
			quad.Draw();
			unbindTextureUnits( 2 );
		}
		else
			needSeed = true;
		oldA.Destroy();
		oldB.Destroy();
	}
	else if( !ensureBuffers( want ) )
	{
		diag::error( "could not allocate the state buffers" );
		return FF_FAIL;
	}
	grid   = want;
	cellMm = fixedCell ? 0.1 : DishWidthFromParam( params[ PT_DISH_WIDTH ] ) / grid.cols;
	if( ( width != lastWidth || height != lastHeight ) && lastWidth != 0 && clearOnResize )
	{
		stateA[ cur ].Clear();
		stateB[ cur ].Clear();
	}
	lastWidth  = width;
	lastHeight = height;
	if( detail != lastDetail && lastDetail >= 0 && brActive )
		needSeed = true;
	lastDetail = detail;

	//The vessel, in cells.
	vesselNow        = OptionIndex( params[ PT_VESSEL ], 2 );
	dishCentre[ 0 ]  = 0.5 * grid.cols;
	dishCentre[ 1 ]  = 0.5 * grid.rows;
	dishRadiusCells  = 0.5 * std::min( grid.cols, grid.rows ) - 1.0;
	if( vesselNow == 0 )
		dishCells = static_cast< double >( grid.cols ) * grid.rows;
	else
		dishCells = kPi * dishRadiusCells * dishRadiusCells;
	const chem::Params params_ = CurrentParams();
	const chem::Recipe recipe  = CurrentRecipe();

	//-------------------------------------------------------------------
	// Seed, or load the harness's state.
	//-------------------------------------------------------------------
	if( loadPending && pendingA.size() == static_cast< size_t >( grid.cols ) * grid.rows * 4 && !brActive )
	{
		stateA[ cur ].Upload( pendingA.data() );
		if( pendingB.size() == pendingA.size() )
			stateB[ cur ].Upload( pendingB.data() );
		else
			stateB[ cur ].Clear();
		loadPending = false;
		needSeed    = false;
		chemTime    = 0.0;
		meanValid   = false;
	}
	else if( needSeed )
	{
		needSeed = false;
		if( brActive )
		{
			br.Reset( grid.cols, grid.rows, recipe, OptionIndex( params[ PT_REACTOR ], 2 ) == static_cast< int >( Reactor::Flow ), seedSalt() );
			uploadBR();
			chemTime        = 0.0;
			seededThisFrame = true;
		}
		else
			seed();
	}

	//-------------------------------------------------------------------
	// Chemical time this frame, and the substeps that cover it.
	//-------------------------------------------------------------------
	const double lapse = TimelapseFromParam( params[ PT_TIMELAPSE ] );
	double chemWanted  = hostDt * lapse;
	if( testChemOn )
	{
		chemWanted = testChemSeconds;
		testChemOn = false;
	}
	const double stir  = stirOff ? 0.0 : std::clamp( params[ PT_STIR ], 0.0f, 1.0f );
	const double burst = stirOff ? 0.0 : std::exp( -shakeAge / kShakeTau );
	const double omega = StirOmegaFromParam( static_cast< float >( stir ) ) + StirOmegaFromParam( 1.0f ) * burst;
	const double eddy  = StirEddyFromParam( static_cast< float >( stir ) ) + StirEddyFromParam( 1.0f ) * burst;
	const double relaxRate = StirRelaxFromParam( static_cast< float >( stir ) ) + 0.5 * StirRelaxFromParam( 1.0f ) * burst;
	const double aeration  = 1.0 + chem::kDye.kShakeAeration * burst;

	//The stable substep: diffusion (explicit) and the reaction's fastest scale.
	double maxD = eddy;
	double reactionLimit = 1e9;
	switch( reaction )
	{
	case Reaction::BZ:
		maxD += std::max( { params_[ chem::P_BZ_DU ], params_[ chem::P_BZ_DY ], params_[ chem::P_BZ_DV ] } );
		//The bromide is stiffer than the step resolves; the implicit step
		//carries it. The substep resolves the HBrO2 scale.
		reactionLimit = 0.25 * params_[ chem::P_BZ_EPS ] * params_[ chem::P_BZ_T0 ];
		break;
	case Reaction::CDIMA:
		maxD += params_[ chem::P_LE_DU ] * std::max( 1.0f / std::max( params_[ chem::P_LE_SIGMA ], 1.0f ), params_[ chem::P_LE_D ] );
		reactionLimit = 0.1 * params_[ chem::P_LE_TSCALE ] / std::max( params_[ chem::P_LE_B ], 1.0f );
		break;
	case Reaction::IodineClock:
		maxD += std::max( std::max( params_[ chem::P_CK_DH2O2 ], params_[ chem::P_CK_DI ] ), std::max( params_[ chem::P_CK_DI2 ], params_[ chem::P_CK_DS ] ) );
		reactionLimit = 0.1 / std::max( static_cast< double >( params_[ chem::P_CK_KP ] * params_[ chem::P_CK_I_0 ] ), 1e-9 );
		break;
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine:
	{
		maxD += std::max( params_[ chem::P_DY_DO2 ], std::max( params_[ chem::P_DY_DGL ], params_[ chem::P_DY_DDYE ] ) );
		const double kRed = params_[ chem::P_DY_K2 ] * params_[ chem::P_DY_OH ] * params_[ chem::P_DY_GL0 ];
		reactionLimit     = 0.2 / std::max( { kRed * std::max( 1.0f, params_[ chem::P_DY_KSQ ] ), static_cast< double >( params_[ chem::P_DY_KLA ] ) * aeration, 1e-9 } );
		break;
	}
	case Reaction::Chameleon:
		maxD += std::max( params_[ chem::P_CH_DMN ], params_[ chem::P_CH_DGL ] );
		break;
	default: break;
	}
	if( params_[ chem::P_FLOW_K0 ] > 0.0f )
		reactionLimit = std::min( reactionLimit, 0.2 / params_[ chem::P_FLOW_K0 ] );
	double maxSubstep = reactionLimit;
	if( maxD > 0.0 && cellMm > 0.0 )
		maxSubstep = std::min( maxSubstep, 0.2 * cellMm * cellMm / maxD );
	//The harness halves the substep to measure the step's own error: the
	//frame must then take at least twice as many substeps, even where the
	//stability bound asked for one.
	if( substepScale < 1.0 )
		maxSubstep = std::min( maxSubstep * substepScale, chemWanted * substepScale );
	double covered = 0.0, dt = 0.0;
	const int cap  = uncapped ? 1 << 20 : kMaxSubsteps;
	int substeps   = ChemicalClock::Plan( chemWanted, maxSubstep, cap, covered, dt );
	if( brActive )
	{
		//The CPU engine takes its own adaptive steps over the whole interval;
		//only the frame cap applies, as chemical seconds it will cover.
		substeps = chemWanted > 0.0 ? 1 : 0;
		covered  = std::min( chemWanted, 20.0 * lapse / 60.0 + 60.0 );//at most ~a minute of chemistry a frame beyond the lapse's own second
		covered  = std::min( covered, chemWanted );
	}
	if( chemWanted > covered + 1e-9 )
	{
		lostChem += chemWanted - covered;
		++cappedFrames;
		if( ++framesSinceCapLog > 600 )
		{
			framesSinceCapLog = 0;
			diag::warn( "the substep cap (" + std::to_string( kMaxSubsteps ) + ") bit: a frame covered " + std::to_string( covered ) + " of "
			            + std::to_string( chemWanted ) + " chemical seconds -- the chemistry is running slow (" + std::to_string( cappedFrames )
			            + " frames so far)" );
		}
	}
	else
		++framesSinceCapLog;

	//-------------------------------------------------------------------
	// Drops and doses.
	//-------------------------------------------------------------------
	const double dropMm = DropSizeFromParam( params[ PT_DROP_SIZE ] );
	const double dropRadiusCells = 0.5 * dropMm / std::max( cellMm, 1e-9 );
	const double autoRate = AutoDropFromParam( params[ PT_AUTO_DROP ] );
	if( autoRate > 0.0 && covered > 0.0 )
	{
		autoDropAccumulator += covered * autoRate / 60.0;
		while( autoDropAccumulator >= 1.0 )
		{
			pendingDrops.push_back( { -1.0, -1.0, 0.0 } );
			autoDropAccumulator -= 1.0;
		}
	}
	else if( autoRate <= 0.0 )
		autoDropAccumulator = 0.0;
	std::vector< DropRequest > drops;
	const int position = OptionIndex( params[ PT_DROP_POSITION ], DropPositionCount( isEffect ) );
	for( DropRequest& d : pendingDrops )
	{
		d.radius = dropRadiusCells;
		if( position == static_cast< int >( DropPosition::Centre ) )
		{
			d.cx = dishCentre[ 0 ];
			d.cy = dishCentre[ 1 ];
		}
		else if( position == static_cast< int >( DropPosition::Brightest ) && brightestCell( input, d.cx, d.cy ) )
		{
		}
		else
		{
			//Random, inside the dish, by the drop's serial.
			for( int attempt = 0; attempt < 8; ++attempt )
			{
				const uint32_t h = Pcg( seedSalt() ^ Pcg( static_cast< uint32_t >( dropsMade ) * 7919u + static_cast< uint32_t >( attempt ) ) );
				d.cx             = ( ( h & 0xFFFFu ) / 65536.0 ) * grid.cols;
				d.cy             = ( ( h >> 16 ) / 65536.0 ) * grid.rows;
				const double dx = d.cx - dishCentre[ 0 ], dy = d.cy - dishCentre[ 1 ];
				if( vesselNow == 0 || dx * dx + dy * dy <= dishRadiusCells * dishRadiusCells * 0.8 )
					break;
			}
		}
		++dropsMade;
		drops.push_back( d );
	}
	pendingDrops.clear();
	for( const DropRequest& d : testDrops )
	{
		drops.push_back( d );
		++dropsMade;
	}
	testDrops.clear();
	//What a drop is, per reaction (the GPU's DropAmount).
	double dropAmount = 0.0;
	switch( reaction )
	{
	case Reaction::CDIMA: dropAmount = 0.5 * params_[ chem::P_LE_A ] / 5.0; break;
	case Reaction::IodineClock: dropAmount = chem::BaseRecipe( Reaction::IodineClock ).reductant; break;//the demonstrator's thiosulfate, whatever the dish started with
	case Reaction::TrafficLight:
	case Reaction::BlueBottle:
	case Reaction::Valentine: dropAmount = params_[ chem::P_DY_O2SAT ]; break;
	case Reaction::Chameleon: dropAmount = recipe.oxidant; break;
	default: break;
	}

	//The Shake: a stirring burst, and for the dye family a lungful of air.
	bool dose          = false;
	double doseAmount  = 0.0;
	const int sync     = OptionIndex( params[ PT_CLOCK_SYNC ], 3 );
	const bool dyeFam  = IsDyeFamily( reaction );
	if( shakePending )
	{
		shakePending = false;
		shakeAge     = 0.0;
		if( dyeFam && sync == static_cast< int >( ClockSync::Off ) )
		{
			dose       = true;
			doseAmount = params_[ chem::P_DY_O2SAT ];
		}
	}
	if( testDoseOn )
	{
		dose       = true;
		doseAmount = testDose;
		testDoseOn = false;
	}
	//Clock Sync: size (or time) the next dose so the change lands on the beat
	//or bar. The clock and the dye family wait a quarter of the period in
	//their changed state first (kSyncHold); the chameleon's dose is timed.
	if( sync != static_cast< int >( ClockSync::Off ) && meanValid && hostDt > 0.0 )
	{
		const bool bar      = sync == static_cast< int >( ClockSync::Bar );
		const double period = bar ? transport.BarSeconds() : transport.BeatSeconds();
		if( reaction == Reaction::IodineClock )
		{
			const bool switched = meanA[ 3 ] <= 1e-7 && meanA[ 2 ] > 1e-6;
			if( switched && syncArmed && syncDoseAt < 0.0 )
				syncDoseAt = now + kSyncHold * period;
			if( syncDoseAt >= 0.0 && now >= syncDoseAt )
			{
				//The dose lands at the START of this frame's chemistry, one
				//host frame before `now`: the switch has that frame more to run.
				const double toNext = transport.SecondsToNext( bar, kSyncLead );
				const double tChem  = ( toNext + hostDt ) * lapse;
				const double H      = wrongDose ? 0.0 : recipe.acidBase;
				double S0           = chem::ClockDoseForSwitch( tChem, meanA[ 0 ], meanA[ 1 ], H, params_[ chem::P_FLOW_K0 ] );
				const double capS   = 1.98 * meanA[ 0 ];
				if( S0 > capS )
				{
					diag::warn( "Clock Sync: the next boundary is too far for the peroxide left; the dose was capped" );
					S0 = capS;
				}
				//Plus the iodine the held dish has made, which the dose takes first.
				S0 += 2.0 * std::max( meanA[ 2 ], 0.0 );
				dose             = true;
				doseAmount       = S0;
				lastDoseAmount   = S0;
				lastDoseAim      = toNext;
				lastDoseHostTime = now;
				++dosesMade;
				syncArmed  = false;
				syncDoseAt = -1.0;
			}
			if( meanA[ 3 ] > 1e-7 )
				syncArmed = true;
		}
		else if( dyeFam )
		{
			const double ctot       = params_[ chem::P_DY_CTOT ];
			const double oxFraction = ctot > 0.0 ? ( meanA[ 2 ] + ( reaction == Reaction::Valentine ? meanB[ 2 ] : 0.0 ) ) / ctot : 0.0;
			const bool faded        = meanA[ 0 ] < 0.02 * params_[ chem::P_DY_O2SAT ] && oxFraction < 0.5;
			if( faded && syncArmed && syncDoseAt < 0.0 )
				syncDoseAt = now + kSyncHold * period;
			if( syncDoseAt >= 0.0 && now >= syncDoseAt )
			{
				//Sized by the plugin's own well-mixed model (bisection, once a
				//cycle); the wrong model for the negative control halves the
				//reduction rate it integrates. A boundary nearer than the
				//chemistry can fade in is skipped for the next.
				chem::Params pp = params_;
				if( wrongDose )
					pp[ chem::P_DY_K2 ] *= 0.5f;
				double toNext = transport.SecondsToNext( bar, kSyncLead );
				double O2     = params_[ chem::P_DY_O2SAT ];
				bool landed   = false;
				for( int tries = 0; tries < 3 && !landed; ++tries )
				{
					const double tChem = ( toNext + hostDt ) * lapse;
					double achieved    = -1.0;
					const double trial = chem::DyeOxygenForFadeModel( reaction, pp, meanA, meanB, tChem, &achieved );
					if( achieved >= 0.0 && std::fabs( achieved - tChem ) <= std::max( 1.5 * hostDt * lapse, 1.0 ) && trial > 0.0 )
					{
						O2     = trial;
						landed = true;
					}
					else
						toNext += period;
				}
				if( !landed )
					diag::warn( "Clock Sync: no shake lands the fade on a boundary from here; a full shake was given" );
				//A sized shake is the oxygen alone: no stirring burst, whose
				//aeration would add to the dose for a while.
				dose             = true;
				doseAmount       = O2;
				lastDoseAmount   = O2;
				lastDoseAim      = toNext;
				lastDoseHostTime = now;
				++dosesMade;
				syncArmed  = false;
				syncDoseAt = -1.0;
			}
			if( meanA[ 0 ] > 0.1 * params_[ chem::P_DY_O2SAT ] )
				syncArmed = true;
		}
		else if( reaction == Reaction::Chameleon )
		{
			//The sequence is dose-independent, so the dose is TIMED: the
			//manganate peak lands on the boundary.
			const double dosePerm = recipe.oxidant;
			const bool finished   = meanA[ 0 ] < 0.05 * dosePerm && meanA[ 1 ] < 0.05 * dosePerm;
			if( finished && syncArmed && chameleonDoseAt < 0.0 )
			{
				const double tPeak = chem::ChameleonGreenPeakTime( wrongDose ? 0.5 * recipe.acidBase : recipe.acidBase, std::max( meanA[ 3 ], 0.0 ), params_[ chem::P_FLOW_K0 ] );
				const double tReal = tPeak / lapse;
				const double wait  = transport.SecondsToNext( bar, tReal + kSyncLead );
				//The dose lands at the start of the frame that fires, a frame
				//before its `now`: aim half a frame late to centre it.
				chameleonDoseAt = now + wait - tReal + 0.5 * hostDt;
				lastDoseAim     = wait;
			}
			if( chameleonDoseAt >= 0.0 && now >= chameleonDoseAt )
			{
				dose             = true;
				doseAmount       = dosePerm;
				lastDoseAmount   = dosePerm;
				lastDoseHostTime = now;
				chameleonDoseAt  = -1.0;
				syncArmed        = false;
				++dosesMade;
			}
			if( !finished )
				syncArmed = true;
		}
	}
	else
		syncDoseAt = -1.0;

	//-------------------------------------------------------------------
	// The chemistry.
	//-------------------------------------------------------------------
	BarRequest barReq {};
	bool barOn = false;
	if( testBarOn )
	{
		barReq    = testBar;
		barOn     = true;
		testBarOn = false;
	}
	else if( barPending )
	{
		//A pipette dragged across the middle of the dish, a third of its width.
		barReq.x0        = dishCentre[ 0 ] - 0.17 * grid.cols;
		barReq.x1        = dishCentre[ 0 ] + 0.17 * grid.cols;
		barReq.y0        = dishCentre[ 1 ];
		barReq.y1        = dishCentre[ 1 ];
		barReq.halfWidth = 1.5 * dropRadiusCells;
		barOn            = true;
	}
	barPending = false;

	if( brActive )
	{
		for( const DropRequest& d : drops )
			br.Drop( d.cx, d.cy, d.radius, 1e-5 );
		if( covered > 0.0 )
		{
			const int threads = static_cast< int >( std::min< unsigned >( std::max( 1u, std::thread::hardware_concurrency() / 2 ), 4u ) );
			substepsTaken += br.Advance( covered, cellMm, eddy, relaxRate, threads );
			uploadBR();
		}
	}
	else
	{
		if( ( omega > 0.0 || burst > 1e-3 ) && covered > 0.0 && vesselNow >= 0 )
			advect( omega * covered );
		if( substeps > 0 )
		{
			stepGPU( substeps, dt, drops, dropAmount, barOn, barReq, dose, doseAmount, aeration, eddy );
			substepsTaken += substeps;
		}
		else if( !drops.empty() || barOn || dose )
			stepGPU( 1, 0.0, drops, dropAmount, barOn, barReq, dose, doseAmount, aeration, eddy );
		reduceMean();
		if( relaxRate > 0.0 && covered > 0.0 )
		{
			relax( 1.0 - std::exp( -relaxRate * covered ) );
			reduceMean();
		}
		readMean();
	}
	chemTime += covered;
	chemTimeTotal += covered;

	//-------------------------------------------------------------------
	// The picture.
	//-------------------------------------------------------------------
	colour( input );
	composite( input, hostViewport, pgl->HostFBO, width, height );
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult HoneydewPlugin::DeInitGL()
{
	for( FFGLShader* shader : { &seedShader, &stepShader, &advectShader, &relaxShader, &columnsShader, &totalShader, &colourShader, &compositeShader, &thumbShader } )
		shader->FreeGLResources();
	quad.Release();
	for( PassBuffer* buffer : { &stateA[ 0 ], &stateA[ 1 ], &stateB[ 0 ], &stateB[ 1 ], &colour0, &colour1, &colour2, &columnsA, &columnsB, &totalA, &totalB, &thumb } )
		buffer->Destroy();
	for( GLuint* texture : { &epsTexture, &blankClip } )
		if( *texture )
		{
			glDeleteTextures( 1, texture );
			*texture = 0;
		}
	if( mrtFBO )
	{
		glDeleteFramebuffers( 1, &mrtFBO );
		mrtFBO = 0;
	}
	needSeed = true;
	return FF_SUCCESS;
}

FFResult HoneydewPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void HoneydewPlugin::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	transport.SetBeatInfo( bpm, barPhase );
}

char* HoneydewPlugin::GetTextParameter( unsigned int index )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult HoneydewPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index < hostOrder.size() && hostOrder[ index ] == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult HoneydewPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= hostOrder.size() )
		return FF_FAIL;
	const unsigned int id = hostOrder[ index ];
	if( id >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( id - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	params[ id ] = value;
	return FF_SUCCESS;
}

float HoneydewPlugin::GetFloatParameter( unsigned int index )
{
	return index < hostOrder.size() ? params[ hostOrder[ index ] ] : 0.0f;
}

void HoneydewPlugin::SetById( unsigned int id, float value )
{
	const int host = HostIndexOf( id );
	if( host >= 0 )
		SetFloatParameter( static_cast< unsigned int >( host ), value );
}

} // namespace honeydew
