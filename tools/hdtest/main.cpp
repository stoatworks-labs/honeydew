/**
    hdtest -- render Honeydew offline, and measure what the chemistry is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context. The chemistry
    checks read the state the shipped shaders computed (two RGBA32F textures,
    read back) and the picture checks read the output; the references are
    double-precision integrations of the same mechanisms (Chemistry.h) or
    closed forms.

        hdtest --out /tmp/dish.png        the source, the defaults
        hdtest --over --out /tmp/o.png    the Over effect on the harness's card
        hdtest --list                     every parameter and its default
        hdtest --offline                  the checks that need no GL context (CI)

    HDTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.

    The claims, one flag each -- see README "Building and testing".
*/

#include "Chemistry.h"
#include "Clock.h"
#include "Controls.h"
#include "Hash.h"
#include "Honeydew.h"
#include "Shaders.h"
#include "Spectra.h"
#include "Transport.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace honeydew;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 8192 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

void Note( const std::string& message )
{
	std::printf( "  note  %s\n", message.c_str() );
}

void Skip( const std::string& message )
{
	std::printf( "  skip  %s\n", message.c_str() );
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is LINEAR floats, row 0 at the BOTTOM; written sRGB-encoded, top row
/// first, alpha opaque.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : static_cast< float >( spectra::EncodeSRGB( rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] ) );
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );
	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ), kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ), kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 ) };
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ), kCGLPFAColorSize,
		static_cast< CGLPixelFormatAttribute >( 24 ), kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 ) };
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ), kCGLPFARendererID,
		static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ), kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ), static_cast< CGLPixelFormatAttribute >( 0 ) };
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "HDTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "hdtest: HDTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	default: return "other";
	}
}

//---------------------------------------------------------------------------
// Pictures: the Over effect's card -- a bright gradient with dark bands, so
// Light Coupling has somewhere to act and Beer-Lambert has light to filter.
//---------------------------------------------------------------------------
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double r = 0.85, g = 0.85, b = 0.85;
			//A dark vertical band and a coloured square.
			if( u > 0.42 && u < 0.58 )
				r = g = b = 0.05;
			if( u > 0.7 && u < 0.9 && v > 0.6 && v < 0.9 )
			{
				r = 0.9;
				g = 0.4;
				b = 0.1;
			}
			if( u < 0.25 && v < 0.3 )
			{
				r = 0.1;
				g = 0.3;
				b = 0.95;
			}
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = static_cast< float >( r );
			o[ 1 ]   = static_cast< float >( g );
			o[ 2 ]   = static_cast< float >( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Audio, written into the Audio buffer the way the host writes it.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses
};

void feedAudio( HoneydewPlugin& plugin, double seconds, AudioFeed feed )
{
	const int host = plugin.HostIndexOf( PT_AUDIO );
	if( host < 0 )
		return;
	const double beat  = std::fmod( std::max( seconds, 0.0 ), 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( static_cast< unsigned int >( host ), static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock.
//---------------------------------------------------------------------------
struct Rig
{
	HoneydewPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0, readFBO = 0;
	int frame          = 0;
	double fps         = 60.0;
	double clockOffset = 0.0;
	double hostUnit    = 1.0;
	double bpm         = 0.0;///< > 0: the host's transport, driven from the synthetic clock
	AudioFeed feed     = AudioFeed::Silence;
	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	explicit Rig( bool effect = false ) : plugin( effect )
	{
	}
	~Rig()
	{
		plugin.DeInitGL();
		release();
		if( readFBO )
			glDeleteFramebuffers( 1, &readFBO );
	}
	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}
	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO = outputFBO;
		if( plugin.IsEffect() )
		{
			const Floats card = picture ? *picture : buildCard( width, height );
			sourceTexture     = makeTexture( width, height, card.data() );
			inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
			inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
			inputStruct.Handle                              = sourceTexture;
			inputs[ 0 ]                                     = &inputStruct;
			process.numInputTextures                        = 1;
			process.inputTextures                           = inputs;
		}
		return true;
	}
	bool Init( int w, int h, const Floats* picture = nullptr, bool fixClock = true )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/honeydew for which shader\n" );
			return false;
		}
		if( fixClock )
			plugin.SetClockScaleForTest( 1.0 );
		return attach( w, h, picture );
	}
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}
	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	void Set( unsigned int id, float value )
	{
		plugin.SetById( id, value );
	}
	double TimeOf( int f ) const
	{
		return clockOffset + static_cast< double >( f ) / fps;
	}
	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = TimeOf( frame );
			plugin.SetTime( seconds * hostUnit );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( seconds * bpm / 240.0, 1.0 ) ) );
			feedAudio( plugin, seconds, feed );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}
	/// Exactly `chemSeconds` of chemistry in one frame (substep-capped unless uncapped).
	bool Chem( double chemSeconds )
	{
		plugin.SetChemTimeForTest( chemSeconds );
		return Render( 1 );
	}
	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
	/// The whole state A (or B), cols x rows x 4, row 0 at the bottom.
	Floats State( bool b = false )
	{
		const Grid grid = plugin.CurrentGrid();
		Floats cells( static_cast< size_t >( grid.cols ) * grid.rows * 4 );
		if( !readFBO )
			glGenFramebuffers( 1, &readFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, readFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b ? plugin.StateBTextureID() : plugin.StateATextureID(), 0 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, grid.cols, grid.rows, GL_RGBA, GL_FLOAT, cells.data() );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		return cells;
	}
};

/// A rig with a reaction set up, still, no auto drops, at a grid of exactly cols x rows.
void prepare( Rig& rig, Reaction r, int cols, int rows )
{
	rig.Set( PT_REACTION, static_cast< float >( r ) );
	rig.Set( PT_STIR, 0.0f );
	rig.Set( PT_AUTO_DROP, 0.0f );
	rig.Set( PT_CLOCK_SYNC, 0.0f );
	rig.Set( PT_VESSEL, 0.0f );
	rig.plugin.SetGridForTest( cols, rows );
	rig.plugin.SetUncappedForTest( true );
}

//---------------------------------------------------------------------------
// Pixel helpers.
//---------------------------------------------------------------------------
void meanRGB( const Floats& px, int width, int height, int x0, int y0, int x1, int y1, double rgb[ 3 ] )
{
	rgb[ 0 ] = rgb[ 1 ] = rgb[ 2 ] = 0.0;
	long n = 0;
	for( int y = std::max( 0, y0 ); y < std::min( height, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( width, x1 ); ++x )
		{
			const float* p = &px[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			for( int c = 0; c < 3; ++c )
				rgb[ c ] += p[ c ];
			++n;
		}
	if( n )
		for( int c = 0; c < 3; ++c )
			rgb[ c ] /= static_cast< double >( n );
}

double hueOf( const double rgb[ 3 ] )
{
	double h, s, v;
	spectra::RGBToHSV( rgb, h, s, v );
	return h;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( HoneydewPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

bool applySetting( HoneydewPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

//===========================================================================
// The checks. Each takes a Perturb: with every field at its default the check
// scores the plugin against the reference; `--negative` sets one field at a
// time to a deliberately wrong MODEL and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	bool spectraShift    = false;///< --spectra: the reference observer shifted 40 nm
	bool beerNaive       = false;///< --beer: the plugin squares the colour instead of doubling the depth
	bool oregonatorEps   = false;///< --oregonator: the plugin's eps doubled
	bool fieldNoyesNoDiff = false;///< --fieldnoyes: diffusion off in the plugin
	bool spiralNoBar     = false;///< --spiral: no break (a target, not a spiral pair)
	bool photoOff        = false;///< --photo: the phi term dropped
	bool briggsRate      = false;///< --briggs: the plugin integrates a wrong constant
	bool clockWrongAcid  = false;///< --clock: the acid term dropped in the plugin's rate
	bool syncWrongDose   = false;///< --sync: the dose from the wrong closed form
	bool trafficNoAir    = false;///< --traffic: oxygen never consumed (kOx = 0)
	bool bottleDouble    = false;///< --bluebottle: the plugin's k2 doubled
	bool chameleonSwap   = false;///< --chameleon: kA and kB swapped
	bool turingNoD       = false;///< --turing: d set to 1 (no differential diffusion)
	bool stirOff         = false;///< --stir: eddy diffusion and relaxation ignored (Stir dead)
	bool unitsWrong      = false;///< --units: the cell size not following Dish Width
	bool timebaseFloat   = false;///< --timebase: elapsed time from a float host clock
	bool overLightOn     = false;///< --over-check: Light Coupling 0 still couples
	bool primeOff        = false;///< --prime: the analyser unprimed
	bool resizeClears    = false;///< --resize: the state cleared on a resize
	bool transportBeat   = false;///< --transport: bar boundaries counted as beats
};
using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

//===========================================================================
// --names (no GL): unique parameter names, the 16-character limit, the About
// block last, and the GLSL's #define mirror of the C++ enums.
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	for( bool effect : { false, true } )
	{
		HoneydewPlugin plugin( effect );
		std::map< std::string, int > seen, address;
		int longNames = 0, dupes = 0, clashes = 0, slashes = 0;
		for( unsigned int i = 0; i < plugin.ParamCount(); ++i )
		{
			const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
			if( name.size() > 16 )
			{
				std::printf( "    too long: %s\n", name.c_str() );
				++longNames;
			}
			if( name.find( '/' ) != std::string::npos )
				++slashes;
			if( seen[ name ]++ > 0 )
				++dupes;
			std::string key;
			for( char c : name )
				if( c != ' ' )
					key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
			if( address[ key ]++ > 0 )
				++clashes;
		}
		const unsigned int aboutFirst = plugin.ParamCount() - stoatworks::about::kParamCount;
		const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
		Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast && slashes == 0,
		       fmt( "%s: %u parameters, %d too long, %d duplicated, %d clashing addresses, %d with a '/'; the About block is last (%s)",
		            effect ? "SW Honeydew Over" : "SW Honeydew", plugin.ParamCount(), longNames, dupes, clashes, slashes, aboutLast ? "yes" : "NO" ) );
	}
	{
		//The display names, as the registrations spell them.
		const std::string source = "SW Honeydew", over = "SW Honeydew Over";
		Check( source.size() <= 16 && over.size() <= 16, fmt( "display names: '%s' (%zu) and '%s' (%zu) fit the 16-byte field", source.c_str(), source.size(), over.c_str(), over.size() ) );
	}
	{
		//The GLSL's defines mirror the enums.
		std::map< std::string, int > expect = {
			{ "R_BZ", static_cast< int >( Reaction::BZ ) }, { "R_BR", static_cast< int >( Reaction::BriggsRauscher ) },
			{ "R_CLOCK", static_cast< int >( Reaction::IodineClock ) }, { "R_CDIMA", static_cast< int >( Reaction::CDIMA ) },
			{ "R_TRAFFIC", static_cast< int >( Reaction::TrafficLight ) }, { "R_BOTTLE", static_cast< int >( Reaction::BlueBottle ) },
			{ "R_VALENTINE", static_cast< int >( Reaction::Valentine ) }, { "R_CHAMELEON", static_cast< int >( Reaction::Chameleon ) },
			{ "P_FLOW_K0", chem::P_FLOW_K0 }, { "P_BATCH", chem::P_BATCH },
			{ "P_BZ_EPS", chem::P_BZ_EPS }, { "P_BZ_Q", chem::P_BZ_Q }, { "P_BZ_F", chem::P_BZ_F }, { "P_BZ_T0", chem::P_BZ_T0 },
			{ "P_BZ_DU", chem::P_BZ_DU }, { "P_BZ_DV", chem::P_BZ_DV }, { "P_BZ_PHIMAX", chem::P_BZ_PHIMAX },
			{ "P_BZ_FUELRATE", chem::P_BZ_FUELRATE }, { "P_BZ_PACE", chem::P_BZ_PACE }, { "P_BZ_CTOT", chem::P_BZ_CTOT },
			{ "P_BZ_EPSP", chem::P_BZ_EPSP }, { "P_BZ_DY", chem::P_BZ_DY }, { "P_BZ_ZREF", chem::P_BZ_ZREF },
			{ "P_LE_A", chem::P_LE_A }, { "P_LE_B", chem::P_LE_B }, { "P_LE_SIGMA", chem::P_LE_SIGMA }, { "P_LE_D", chem::P_LE_D },
			{ "P_LE_TSCALE", chem::P_LE_TSCALE }, { "P_LE_DU", chem::P_LE_DU }, { "P_LE_WMAX", chem::P_LE_WMAX },
			{ "P_CK_KP", chem::P_CK_KP }, { "P_CK_H2O2_0", chem::P_CK_H2O2_0 }, { "P_CK_I_0", chem::P_CK_I_0 }, { "P_CK_S_0", chem::P_CK_S_0 },
			{ "P_DY_KOX", chem::P_DY_KOX }, { "P_DY_K2", chem::P_DY_K2 }, { "P_DY_OH", chem::P_DY_OH }, { "P_DY_O2SAT", chem::P_DY_O2SAT },
			{ "P_DY_KLA", chem::P_DY_KLA }, { "P_DY_CTOT", chem::P_DY_CTOT }, { "P_DY_KSQ", chem::P_DY_KSQ }, { "P_DY_PKA", chem::P_DY_PKA },
			{ "P_DY_KRZ", chem::P_DY_KRZ }, { "P_DY_GL0", chem::P_DY_GL0 }, { "P_DY_TWOSTEP", chem::P_DY_TWOSTEP },
			{ "P_CH_KA", chem::P_CH_KA }, { "P_CH_KB", chem::P_CH_KB }, { "P_CH_OH", chem::P_CH_OH }, { "P_CH_GL0", chem::P_CH_GL0 }, { "P_CH_MN0", chem::P_CH_MN0 },
		};
		std::map< std::string, int > speciesExpect;
		for( int s = 0; s < spectra::S_COUNT; ++s )
		{
			static const char* names[] = { "S_FERROIN", "S_FERRIIN", "S_RU2", "S_RU3", "S_CE4", "S_I2", "S_I3", "S_STARCH_I3", "S_CLO2", "S_IC_BLUE",
				                           "S_IC_YELLOW", "S_IC_SEMI", "S_IC_LEUCO", "S_MB", "S_MB_DIMER", "S_RESAZURIN", "S_RESORUFIN", "S_MNO4", "S_MNO4_2", "S_MNO2" };
			speciesExpect[ names[ s ] ] = s;
		}
		const std::string glsl = std::string( shaders::kCommon ) + shaders::kColourFragment;
		std::regex define( "#define\\s+(\\w+)\\s+(-?\\d+)" );
		int found = 0, wrong = 0;
		std::string what;
		for( auto it = std::sregex_iterator( glsl.begin(), glsl.end(), define ); it != std::sregex_iterator(); ++it )
		{
			const std::string name = ( *it )[ 1 ];
			const int value        = std::stoi( ( *it )[ 2 ] );
			auto e                 = expect.find( name );
			auto s                 = speciesExpect.find( name );
			if( e != expect.end() )
			{
				++found;
				if( e->second != value )
				{
					++wrong;
					what += " " + name;
				}
			}
			else if( s != speciesExpect.end() )
			{
				++found;
				if( s->second != value )
				{
					++wrong;
					what += " " + name;
				}
			}
		}
		Check( wrong == 0 && found == static_cast< int >( expect.size() + speciesExpect.size() ) && glsl.find( "LAMBDAS 41" ) != std::string::npos,
		       fmt( "the GLSL's %d #defines mirror the C++ enums (%zu expected, %d wrong%s), 41 wavelength samples", found,
		            expect.size() + speciesExpect.size(), wrong, what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --spectra (no GL): the cited peaks reproduce, and the named solutions land
// in their hues under the CPU's own CIE integration in double.
//===========================================================================
int runSpectra( const Perturb& perturb )
{
	std::printf( "\n=== spectra: each species reproduces its cited lambda_max and eps_max; each cited solution lands in its named hue\n" );
	int peakWrong = 0, standIns = 0;
	std::string what;
	for( int s = 0; s < spectra::S_COUNT; ++s )
	{
		const spectra::Species sp  = static_cast< spectra::Species >( s );
		const spectra::Citation& c = spectra::CitationOf( sp );
		const double e             = spectra::Epsilon( sp, c.lambdaMax );
		//The cited band's height: the other Gaussians of the fit may add to it,
		//by at most a third; and the cited lambda is a local maximum (the band
		//has not been pulled off its peak by a shoulder), judged 12 nm either side.
		const bool height = e >= 0.999 * c.epsMax && e <= 1.35 * c.epsMax;
		bool local        = true;
		for( double d = -12.0; d <= 12.0; d += 1.0 )
			local = local && spectra::Epsilon( sp, c.lambdaMax + d ) <= e * 1.0001;
		if( !( height && local ) )
		{
			++peakWrong;
			what += fmt( " %s(%.0f: %.0f x%.2f%s)", spectra::SpeciesName( sp ), c.lambdaMax, e, e / c.epsMax, local ? "" : " not the maximum" );
		}
		standIns += c.standIn;
	}
	Check( peakWrong == 0, fmt( "%d species: eps( lambda_max ) reproduces the cited eps_max (%d wrong%s); %d of the peaks are STAND-INS, named in CHEMISTRY.md",
	                            spectra::S_COUNT, peakWrong, what.c_str(), standIns ) );
	int wrongHue = 0;
	std::string hues;
	for( const spectra::NamedSolution& s : spectra::NamedSolutions() )
	{
		double rgb[ 3 ];
		std::vector< spectra::Species > sp = { s.species };
		std::vector< double > conc         = { s.molar };
		//The wrong model: integrate against an observer shifted 40 nm.
		if( perturb.spectraShift )
		{
			double xyz[ 3 ] = { 0, 0, 0 }, xyzW[ 3 ] = { 0, 0, 0 };
			for( int k = 0; k < cie::kLambdaCount; ++k )
			{
				const int ks = std::clamp( k + 4, 0, cie::kLambdaCount - 1 );
				const double T = spectra::Transmittance( sp, conc, s.depthMm * 0.1, k );
				for( int c = 0; c < 3; ++c )
				{
					xyz[ c ] += cie::kXyzBar[ ks ][ c ] * cie::kD65[ k ] * T;
					xyzW[ c ] += cie::kXyzBar[ ks ][ c ] * cie::kD65[ k ];
				}
			}
			for( int c = 0; c < 3; ++c )
				xyz[ c ] /= xyzW[ 1 ];
			spectra::XYZToLinearSRGB( xyz, rgb );
		}
		else
			spectra::LayerColour( Lightbox::D65, sp, conc, s.depthMm * 0.1, rgb );
		const double h = hueOf( rgb );
		const bool in  = s.hueLo <= s.hueHi ? ( h >= s.hueLo && h <= s.hueHi ) : ( h >= s.hueLo || h <= s.hueHi );
		wrongHue += !in;
		hues += fmt( " %s %.0f%s", s.hue, h, in ? "" : "(OUT)" );
	}
	Check( wrongHue == 0, fmt( "%zu named solutions land in their hues (%d out):%s", spectra::NamedSolutions().size(), wrongHue, hues.c_str() ) );
	{
		//The whites: D65 through an empty dish is sRGB white to the matrix's rounding.
		double rgb[ 3 ];
		spectra::LayerColour( Lightbox::D65, {}, {}, 0.1, rgb );
		Check( std::fabs( rgb[ 0 ] - 1.0 ) < 2e-3 && std::fabs( rgb[ 1 ] - 1.0 ) < 2e-3 && std::fabs( rgb[ 2 ] - 1.0 ) < 2e-3,
		       fmt( "D65 through an empty dish is sRGB white: %.4f %.4f %.4f (within 2e-3, the XYZ->sRGB matrix's rounding)", rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] ) );
		//The purple + green question: the chameleon's mixture without hypomanganate.
		for( double frac : { 0.25, 0.5, 0.75 } )
		{
			const double total = 1.3e-3;
			spectra::LayerColour( Lightbox::D65, { spectra::S_MNO4, spectra::S_MNO4_2 }, { total * ( 1.0 - frac ), total * frac }, 0.15, rgb );
			double h, sat, val;
			spectra::RGBToHSV( rgb, h, sat, val );
			Note( fmt( "chameleon, %.0f%% of the Mn as manganate, 1.3 mM in 1.5 mm: hue %.0f sat %.2f (blue would be ~240): %s", 100.0 * frac, h, sat,
			           ( h > 215.0 && h < 265.0 ) ? "READS BLUE" : "does not read blue" ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --transport (no GL): the beat/bar arithmetic Clock Sync relies on.
//===========================================================================
int runTransport( const Perturb& perturb )
{
	std::printf( "\n=== transport: seconds to the next beat or bar, from the BPM and the bar phase\n" );
	Transport t;
	t.SetBeatInfo( 120.0f, 0.0f );
	t.Advance( 0.0 );
	int wrong = 0;
	std::string what;
	//At 120 BPM a beat is 0.5 s, a bar 2 s. From phase 0 (a downbeat) the next beat is 0.5 s away, the next bar 2 s.
	auto expectNear = [ & ]( double got, double want, const char* name ) {
		if( std::fabs( got - want ) > 1e-9 )
		{
			++wrong;
			what += fmt( " %s %.4f (want %.4f)", name, got, want );
		}
	};
	expectNear( t.SecondsToNext( false, 0.0 ), perturb.transportBeat ? 2.0 : 0.5, "beat@0" );
	expectNear( t.SecondsToNext( true, 0.0 ), 2.0, "bar@0" );
	t.Advance( 0.3 );//phase 0.15 of a bar
	expectNear( t.SecondsToNext( false, 0.0 ), 0.2, "beat@0.3s" );
	expectNear( t.SecondsToNext( true, 0.0 ), 1.7, "bar@0.3s" );
	expectNear( t.SecondsToNext( false, 0.25 ), 0.7, "beat@0.3s lead 0.25" );
	//An odd tempo: 97 BPM, host phase 0.6.
	t.SetBeatInfo( 97.0f, 0.6f );
	t.Advance( 1.0 / 60.0 );
	const double phase = static_cast< double >( 0.6f );//the host hands a float
	expectNear( t.SecondsToNext( true, 0.0 ), ( 1.0 - phase ) * 4.0 * 60.0 / 97.0, "bar@97bpm phase .6" );
	expectNear( t.SecondsToNext( false, 0.0 ), ( 1.0 - ( phase * 4.0 - std::floor( phase * 4.0 ) ) ) * 60.0 / 97.0, "beat@97bpm phase .6" );
	Check( wrong == 0, fmt( "120 BPM and 97 BPM, with and without a lead, from a host phase and from the running phase: %d wrong%s", wrong, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --timebase-law (no GL): the substep planner covers what it can and drops the rest.
//===========================================================================
int runTimebaseLaw( const Perturb& )
{
	std::printf( "\n=== timebase-law: chemical time = host dt x Time-lapse; the cap drops time, it never banks it\n" );
	double covered = 0.0, dt = 0.0;
	int n   = ChemicalClock::Plan( 0.5, 0.05, 32, covered, dt );
	bool ok = n == 10 && std::fabs( covered - 0.5 ) < 1e-12 && std::fabs( dt - 0.05 ) < 1e-12;
	n       = ChemicalClock::Plan( 5.0, 0.05, 32, covered, dt );
	ok      = ok && n == 32 && std::fabs( covered - 1.6 ) < 1e-12 && std::fabs( dt - 0.05 ) < 1e-12;
	n       = ChemicalClock::Plan( 0.0, 0.05, 32, covered, dt );
	ok      = ok && n == 0 && covered == 0.0;
	Check( ok, "0.5 s at a 0.05 s substep is 10 steps covering all of it; 5 s is capped at 32 steps covering 1.6 s; 0 s is nothing" );
	ChemicalClock clock;
	double sum = 0.0;
	clock.Advance( 100.0 );
	sum += clock.Advance( 100.0 + 1.0 / 60.0 );
	sum += clock.Advance( 100.0 + 2.0 / 60.0 );
	const double back = clock.Advance( 50.0 );//a jump back
	const bool jumped = clock.TakeJump();
	const double big  = clock.Advance( 60.0 );//10 s on: a jump
	Check( std::fabs( sum - 2.0 / 60.0 ) < 1e-12 && back == 0.0 && jumped && big == 0.0, fmt( "two 1/60 s frames sum to %.9f s; a backward step and a 10 s step pass no time", sum ) );
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.Set( PT_DETAIL, 0.0f );
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 3; ++frame )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 7, 5, 300, 170 );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0, drawBuffer = 0;
			GLfloat clear[ 4 ]  = {};
			GLboolean mask[ 4 ] = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			glGetIntegerv( GL_DRAW_BUFFER0, &drawBuffer );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 7 && viewport[ 1 ] == 5 && viewport[ 2 ] == 300 && viewport[ 3 ] == 170, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			expect( drawBuffer == GL_COLOR_ATTACHMENT0, "draw-buffer" );
			for( int u = 0; u < 10; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		Check( problems == 0, fmt( "%s, three frames: viewport, vertex array, array buffer, program, active unit, framebuffer, draw buffer, "
		                           "blend, scissor, clear colour, colour mask, ten texture units (%d wrong:%s)",
		                           effect ? "Over" : "source", problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --prime: no audio event on the first frame after a clip trigger.
//===========================================================================
int runPrime( const Perturb& perturb )
{
	std::printf( "\n=== prime: loud audio already playing when the clip is triggered fires nothing on the trigger frame\n" );
	for( bool effect : { false, true } )
		for( const Raster& raster : kRasters )
		{
			Rig rig( effect );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::IodineClock, 64, 36 );
			rig.plugin.SetUnprimedForTest( perturb.primeOff );
			rig.Set( PT_AUDIO_DROPS, 1.0f );
			rig.feed = AudioFeed::Pulses;
			rig.Render( 100 );
			const unsigned long long before = rig.plugin.Onsets();
			const unsigned long long drops  = rig.plugin.DropsMade();
			rig.clockOffset                 = 0.02 - static_cast< double >( rig.frame ) / rig.fps;
			rig.Render( 1 );
			const unsigned long long atTrigger = rig.plugin.Onsets();
			rig.Render( 35 );
			const unsigned long long after = rig.plugin.Onsets();
			Check( before >= 3 && drops == before && atTrigger == before && after == atTrigger + 1,
			       fmt( "%s %dx%d: %llu onsets from 4 beats before (%llu drops); %llu on the trigger frame; %llu in the next 35 frames (one hit, at 0.5 s)",
			            effect ? "Over  " : "source", raster.w, raster.h, before, drops, atTrigger - before, after - atTrigger ) );
		}
	return Verdict();
}

//===========================================================================
// --resize: the chemistry survives the host's raster changing.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: a resize of the same shape keeps the state to the bit; a new shape resamples it\n" );
	for( bool effect : { false, true } )
	{
		Rig rig( effect );
		if( !rig.Init( 1280, 720 ) )
			return 1;
		rig.plugin.SetClearOnResizeForTest( perturb.resizeClears );
		rig.Set( PT_REACTION, static_cast< float >( Reaction::BZ ) );
		rig.Set( PT_DETAIL, 1.0f );
		rig.Set( PT_SEED_FROM_CLIP, 0.0f );
		if( !rig.Render( 60 ) )
			return 1;
		rig.Set( PT_TIMELAPSE, 0.0f );
		rig.plugin.SetChemTimeForTest( 0.0 );
		rig.Render( 1 );
		const Grid g0      = rig.plugin.CurrentGrid();
		const Floats start = rig.State();
		rig.Resize( 320, 180 );
		rig.plugin.SetChemTimeForTest( 0.0 );
		rig.Render( 1 );
		const Grid g1   = rig.plugin.CurrentGrid();
		const bool same = g1 == g0 && rig.State() == start;
		//4:3: the state resampled onto the new grid; the middle column should
		//still be the middle of the old field (compared cell for cell at the centre line).
		rig.Resize( 960, 720 );
		rig.plugin.SetChemTimeForTest( 0.0 );
		rig.Render( 1 );
		const Grid g2        = rig.plugin.CurrentGrid();
		const Floats after   = rig.State();
		double worst         = 0.0, scale = 0.0;
		for( int y = 0; y < g2.rows; y += 7 )
		{
			//The new grid's cell (x, y) reads the old at the same FRACTION of the extent.
			const int x  = g2.cols / 2;
			const int ox = static_cast< int >( ( x + 0.5 ) / g2.cols * g0.cols ), oy = static_cast< int >( ( y + 0.5 ) / g2.rows * g0.rows );
			const float got = after[ ( static_cast< size_t >( y ) * g2.cols + x ) * 4 + 1 ];
			const float was = start[ ( static_cast< size_t >( oy ) * g0.cols + ox ) * 4 + 1 ];
			worst           = std::max( worst, static_cast< double >( std::fabs( got - was ) ) );
			scale           = std::max( scale, static_cast< double >( std::fabs( was ) ) );
		}
		double maxV = 0.0;
		for( size_t i = 1; i < start.size(); i += 4 )
			maxV = std::max( maxV, static_cast< double >( start[ i ] ) );
		//A bilinear resample of a smooth field lands within a cell's gradient: a tenth of the field's range here.
		Check( same && worst <= 0.1 * std::max( maxV, 1e-6 ) && maxV > 1e-3,
		       fmt( "%s: %dx%d grid: 1280x720 -> 320x180 %s; -> 960x720 (%dx%d) the resampled centre column differs by at most %.3g (field max %.3g)",
		            effect ? "Over  " : "source", g0.cols, g0.rows, same ? "bit-identical" : "CHANGED", g2.cols, g2.rows, worst, maxV ) );
	}
	return Verdict();
}

//===========================================================================
// --timebase: chemical time = host time x Time-lapse at Resolume's ~499
// million ms, any frame rate; the substep cap is counted, never silent.
//===========================================================================
int runTimebase( const Perturb& perturb )
{
	std::printf( "\n=== timebase: a host clock at 499,000,000 ms (a float resolves 32 ms there) advances chemistry by dt x Time-lapse exactly; the cap counts\n" );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h, nullptr, false ) )
			return 1;
		prepare( rig, Reaction::IodineClock, 64, 36 );
		rig.plugin.SetUncappedForTest( false );
		rig.plugin.SetFloatClockForTest( perturb.timebaseFloat );
		rig.Set( PT_TIMELAPSE, ParamFromTimelapse( 20.0 ) );
		rig.hostUnit    = 1000.0;
		rig.clockOffset = 499000.0;
		int paced       = 0;
		while( rig.plugin.ClockScale() == 0.0 && paced < 60 )
		{
			rig.Render( 1 );
			std::this_thread::sleep_for( std::chrono::microseconds( 16667 ) );
			++paced;
		}
		rig.Render( 2 );
		const bool voted      = rig.plugin.ClockScale() == 0.001;
		const double lapse    = TimelapseFromParam( ParamFromTimelapse( 20.0 ) );
		const double chem0    = rig.plugin.ChemicalTimeTotal();
		const double host0    = rig.TimeOf( rig.frame - 1 );
		double worstDt        = 0.0;
		for( int f = 0; f < 240; ++f )
		{
			rig.Render( 1 );
			worstDt = std::max( worstDt, std::fabs( rig.plugin.LastHostDt() - 1.0 / 60.0 ) );
		}
		const double hostElapsed = rig.TimeOf( rig.frame - 1 ) - host0;
		const double chemElapsed = rig.plugin.ChemicalTimeTotal() - chem0;
		const double lost        = rig.plugin.LostChemicalTime();
		//Double at 4.99e8 ms: an ulp is 6e-8 ms. Chemical time is hostDt x lapse summed in double.
		Check( voted && worstDt <= 1e-9 && std::fabs( chemElapsed + lost - hostElapsed * lapse ) < 1e-6 && lost == 0.0,
		       fmt( "%dx%d  the vote settled on ms (%s, %d paced frames); every dt within %.1e s of 1/60 (bound 1e-9); 240 frames: %.6f chemical s = %.6f host s x %.1f (lost %.3g, %lld capped frames)",
		            raster.w, raster.h, voted ? "yes" : "NO", paced, worstDt, chemElapsed, hostElapsed, lapse, lost, rig.plugin.CappedFrames() ) );
		//The cap: at 300x the clock's 0.1 s substep (0.1 / (k' I0) = 100 s!) does not bite; BZ's does. Switch to BZ at 300x.
		rig.Set( PT_REACTION, static_cast< float >( Reaction::BZ ) );
		rig.Set( PT_TIMELAPSE, ParamFromTimelapse( 300.0 ) );
		//A stiffer BZ than the recipe gives (eps 0.002): 5 s a frame at a 5 ms substep wants 1000 substeps.
		rig.plugin.SetParamOverrideForTest( chem::P_BZ_EPS, 0.002f );
		rig.Render( 1 );
		const long long cappedBefore = rig.plugin.CappedFrames();
		const double lostBefore      = rig.plugin.LostChemicalTime();
		rig.Render( 30 );
		const long long capped = rig.plugin.CappedFrames() - cappedBefore;
		const double lostNow   = rig.plugin.LostChemicalTime() - lostBefore;
		Check( capped == 30 && lostNow > 0.0, fmt( "%dx%d  BZ at 300x: the substep cap bit on %lld of 30 frames and %.2f chemical seconds were dropped, counted",
		                                            raster.w, raster.h, capped, lostNow ) );
	}
	return Verdict();
}

//===========================================================================
// --beer: a uniform dish from pixels against the CPU spectral reference; an
// empty dish returns the lightbox/clip; doubling Depth is polychromatic
// Beer-Lambert, not the per-channel square.
//===========================================================================
int runBeer( const Perturb& perturb )
{
	std::printf( "\n=== beer: a uniform layer's pixels are the CPU's Beer-Lambert integral; an empty dish is the lightbox / the clip; depth doubles polychromatically\n" );
	for( const Raster& raster : kRasters )
	{
		//A ferroin dish, fresh (reduced catalyst everywhere, v tiny), still, at 1.5 mm and 3.0 mm.
		for( bool effect : { false, true } )
		{
			Rig rig( effect );
			rig.plugin.SetNaiveDepthForTest( perturb.beerNaive );
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::BZ, 32, 18 );
			rig.Set( PT_SEED_FROM_CLIP, 0.0f );
			rig.Set( PT_LIGHT_COUPLING, 0.0f );
			rig.Set( PT_TIMELAPSE, 0.0f );
			const chem::Recipe recipe = chem::BaseRecipe( Reaction::BZ );
			double worst1 = 0.0, worst2 = 0.0, worstEmpty = 0.0, naiveGap = 0.0;
			for( int pass = 0; pass < 3; ++pass )
			{
				const double depthMm = pass == 0 ? 1.5 : pass == 1 ? 3.0 : 1.5;
				rig.Set( PT_DEPTH, ParamFromDepth( depthMm ) );
				rig.Set( PT_INDICATOR, pass == 2 ? 0.0f : 0.5f );
				rig.Set( PT_RESET, 1.0f );
				rig.plugin.SetChemTimeForTest( 0.0 );
				rig.Render( 1 );
				rig.Set( PT_RESET, 0.0f );
				rig.plugin.SetChemTimeForTest( 0.0 );
				rig.Render( 1 );
				const Floats out = rig.Output();
				//The state's z over the model's peak is the oxidised fraction: read it.
				const Floats st       = rig.State();
				const chem::Params pp = rig.plugin.CurrentParams();
				const double v        = st[ 2 ] / pp[ chem::P_BZ_ZREF ];
				const double ctot     = pass == 2 ? 0.0 : recipe.indicator;
				const double ox       = std::clamp( v, 0.0, 1.0 ) * ctot;
				const std::vector< spectra::Species > sp = { spectra::S_FERROIN, spectra::S_FERRIIN };
				const std::vector< double > conc         = { ctot - ox, ox };
				//Sampled on the card's plain grey (0.85), away from its band and squares.
				double got[ 3 ];
				meanRGB( out, raster.w, raster.h, raster.w * 3 / 10, raster.h / 2, raster.w * 3 / 10 + 8, raster.h / 2 + 8, got );
				double want[ 3 ];
				if( !effect )
					spectra::LayerColour( Lightbox::D65, sp, conc, depthMm * 0.1, want );
				else
				{
					double m[ 9 ];
					spectra::LayerMatrix( sp, conc, depthMm * 0.1, m );
					//The card is 0.85 grey where sampled.
					for( int c = 0; c < 3; ++c )
						want[ c ] = 0.85 * ( m[ c * 3 + 0 ] + m[ c * 3 + 1 ] + m[ c * 3 + 2 ] );
				}
				double diff = 0.0;
				for( int c = 0; c < 3; ++c )
					diff = std::max( diff, std::fabs( got[ c ] - want[ c ] ) );
				if( pass == 0 )
					worst1 = diff;
				else if( pass == 1 )
				{
					worst2 = diff;
					//The naive model: the 1.5 mm colour squared per channel. The gap between it and the integral is the point.
					double one[ 3 ];
					if( !effect )
						spectra::LayerColour( Lightbox::D65, sp, conc, 0.15, one );
					else
					{
						double m1[ 9 ];
						spectra::LayerMatrix( sp, conc, 0.15, m1 );
						for( int c = 0; c < 3; ++c )
							one[ c ] = 0.85 * ( m1[ c * 3 + 0 ] + m1[ c * 3 + 1 ] + m1[ c * 3 + 2 ] );
					}
					for( int c = 0; c < 3; ++c )
						naiveGap = std::max( naiveGap, std::fabs( one[ c ] * one[ c ] / ( effect ? 0.85 : 1.0 ) - want[ c ] ) );
				}
				else
				{
					//Empty: the lightbox's white, or the clip: compare every pixel.
					const Floats card = effect ? buildCard( raster.w, raster.h ) : Floats();
					double white[ 3 ];
					spectra::LayerColour( Lightbox::D65, {}, {}, 0.15, white );
					for( int y = 0; y < raster.h; y += 3 )
						for( int x = 0; x < raster.w; x += 3 )
							for( int c = 0; c < 3; ++c )
							{
								const double o = out[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + c ];
								const double w = effect ? card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + c ] : white[ c ];
								worstEmpty     = std::max( worstEmpty, std::fabs( o - w ) );
							}
				}
			}
			//Tolerances: the GPU sums 41 samples in float (41 x 2^-24 relative) and
			//the sRGB matrix is applied in float: 1e-4 of a unit colour. The
			//naive square differs from the integral by a real amount (printed).
			const double tol = 2e-4;
			//For the Over, an empty dish returns the clip through I exactly; for
			//the source the lightbox white through 41 float terms: an 8-bit level.
			const double emptyTol = effect ? 1.0 / 16777216.0 : 1.0 / 255.0;
			Check( worst1 <= tol && worst2 <= tol && worstEmpty <= emptyTol && naiveGap > 10.0 * tol,
			       fmt( "%s %dx%d  1.5 mm: pixels within %.1e of the double reference; 3.0 mm: %.1e (the per-channel square would be off by %.3f); "
			            "an empty dish returns the %s within %.2e (bound %.1e)",
			            effect ? "Over  " : "source", raster.w, raster.h, worst1, worst2, naiveGap, effect ? "clip" : "lightbox", worstEmpty, emptyTol ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --over-check: Mix 0 returns the clip bit-exact; Light Coupling 0 makes the
// chemistry independent of the clip.
//===========================================================================
int runOverCheck( const Perturb& perturb )
{
	std::printf( "\n=== over: Mix 0 returns the clip bit for bit; with Light Coupling 0 the chemistry does not depend on the clip\n" );
	for( const Raster& raster : kRasters )
	{
		const Floats card = buildCard( raster.w, raster.h );
		{
			Rig rig( true );
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			prepare( rig, Reaction::BZ, 64, 36 );
			rig.Set( PT_SEED_FROM_CLIP, 0.0f );
			rig.Set( PT_MIX, 0.0f );
			rig.Render( 5 );
			const Floats out = rig.Output();
			int differ       = 0;
			for( size_t i = 0; i < out.size(); ++i )
				differ += out[ i ] != card[ i ];
			Check( differ == 0, fmt( "%dx%d  Mix 0 returns the clip bit for bit (%d floats differ)", raster.w, raster.h, differ ) );
		}
		{
			//Two Over rigs, Ru-BZ, Light Coupling 0 (perturbed: coupled anyway), one on
			//the card and one on black: the states must agree exactly.
			Floats black( card.size(), 0.0f );
			for( size_t i = 3; i < black.size(); i += 4 )
				black[ i ] = 1.0f;
			Rig a( true ), b( true );
			if( !a.Init( raster.w, raster.h, &card ) || !b.Init( raster.w, raster.h, &black ) )
				return 1;
			for( Rig* r : { &a, &b } )
			{
				prepare( *r, Reaction::BZ, 64, 36 );
				r->Set( PT_CATALYST, static_cast< float >( Catalyst::Rubpy ) );
				r->Set( PT_SEED_FROM_CLIP, 0.0f );
				r->Set( PT_LIGHT_COUPLING, perturb.overLightOn ? 1.0f : 0.0f );
				r->Set( PT_DROP_POSITION, 1.0f );
			}
			a.Render( 1 );
			b.Render( 1 );
			a.plugin.DropForTest( 32.0, 18.0, 3.0 );
			b.plugin.DropForTest( 32.0, 18.0, 3.0 );
			int differ  = 0;
			double maxU = 0.0;
			for( int i = 0; i < 40; ++i )
			{
				a.Chem( 2.0 );
				b.Chem( 2.0 );
				const Floats sa = a.State(), sb = b.State();
				for( size_t k = 0; k < sa.size(); ++k )
				{
					differ += sa[ k ] != sb[ k ];
					//The wave: HBrO2 (x) excited somewhere away from the drop.
					const size_t cell = k / 4;
					if( k % 4 == 0 && ( cell % 64 ) < 20 )
						maxU = std::max( maxU, static_cast< double >( sa[ k ] ) );
				}
			}
			Check( differ == 0 && maxU > 0.1, fmt( "%dx%d  Ru-BZ with Light Coupling 0: 80 s of a wave on the card and on black agree in every state value at every "
			                                       "frame (%d differ; a wave reached the far third: max x there %.2f)",
			                                       raster.w, raster.h, differ, maxU ) );
		}
	}
	return Verdict();
}

//===========================================================================
// BZ helpers: a still dish with no pacemakers, the three-variable model's
// one-dimensional double reference, and the phase around a point.
//===========================================================================
/// Params with the pacemaker effect off (a homogeneous dish) and, when
/// `fOverride` > 0, the stoichiometric factor set (f = 2.6 is just past the
/// Hopf point at the 1x recipe: excitable, not oscillatory).
void bzHomogeneous( Rig& rig, double fOverride )
{
	rig.plugin.SetParamOverrideForTest( chem::P_BZ_PACE, 0.0f );
	if( fOverride > 0.0 )
		rig.plugin.SetParamOverrideForTest( chem::P_BZ_F, static_cast< float >( fOverride ) );
}

/// The three-variable Oregonator on a line, in double: explicit five-point
/// diffusion (no-flux ends) and a ROS2 reaction step per cell with the
/// analytic Jacobian -- the plugin's scheme, at a finer step, in double.
struct BZLine
{
	chem::BZModel m;
	int n     = 0;
	double dx = 0.1, dt = 0.0, D[ 3 ] = { 0, 0, 0 };
	std::vector< double > x, y, z, phi;
	void Init( const chem::BZModel& model, int cells, double dxMm, double dtS )
	{
		m  = model;
		n  = cells;
		dx = dxMm;
		dt = dtS;
		D[ 0 ] = m.Du;
		D[ 1 ] = chem::kOregonator.DY * chem::kCm2PerS_to_Mm2PerS;
		D[ 2 ] = m.Dv;
		double rest[ 3 ];
		chem::BZRestState( m, rest );
		x.assign( n, rest[ 0 ] );
		y.assign( n, rest[ 1 ] );
		z.assign( n, rest[ 2 ] );
		phi.assign( n, 0.0 );
	}
	void Fire( int from, int to )
	{
		for( int i = std::max( from, 0 ); i < std::min( to, n ); ++i )
		{
			x[ i ] = 1.0;
			y[ i ] = 0.0;
		}
	}
	void Step()
	{
		static std::vector< double > nx, ny, nz;
		nx = x;
		ny = y;
		nz = z;
		const double h2 = dx * dx;
		for( int i = 0; i < n; ++i )
		{
			const int l = std::max( i - 1, 0 ), r = std::min( i + 1, n - 1 );
			nx[ i ] = x[ i ] + dt * D[ 0 ] * ( x[ l ] + x[ r ] - 2.0 * x[ i ] ) / h2;
			ny[ i ] = y[ i ] + dt * D[ 1 ] * ( y[ l ] + y[ r ] - 2.0 * y[ i ] ) / h2;
			nz[ i ] = z[ i ] + dt * D[ 2 ] * ( z[ l ] + z[ r ] - 2.0 * z[ i ] ) / h2;
		}
		const double gamma = 1.0 + 1.0 / std::sqrt( 2.0 );
		const double rate = 1.0 / m.T0, eps = m.eps, epsP = m.epsPrime, q = m.q, f = m.f;
		for( int i = 0; i < n; ++i )
		{
			double s[ 3 ] = { nx[ i ], ny[ i ], nz[ i ] };
			auto F = [ & ]( const double* v, double* d ) {
				d[ 0 ] = rate * ( q * v[ 1 ] - v[ 0 ] * v[ 1 ] + v[ 0 ] * ( 1.0 - v[ 0 ] ) ) / eps;
				d[ 1 ] = rate * ( -q * v[ 1 ] - v[ 0 ] * v[ 1 ] + f * v[ 2 ] + phi[ i ] ) / epsP;
				d[ 2 ] = rate * ( v[ 0 ] - v[ 2 ] );
			};
			//J (row-major) at s.
			const double J[ 9 ] = { rate * ( 1.0 - 2.0 * s[ 0 ] - s[ 1 ] ) / eps, rate * ( q - s[ 0 ] ) / eps, 0.0,
				                    rate * ( -s[ 1 ] ) / epsP, rate * ( -q - s[ 0 ] ) / epsP, rate * f / epsP,
				                    rate, 0.0, -rate };
			double A[ 9 ];
			for( int k = 0; k < 9; ++k )
				A[ k ] = ( k % 4 == 0 ? 1.0 : 0.0 ) - gamma * dt * J[ k ];
			//3x3 inverse.
			const double det = A[ 0 ] * ( A[ 4 ] * A[ 8 ] - A[ 5 ] * A[ 7 ] ) - A[ 1 ] * ( A[ 3 ] * A[ 8 ] - A[ 5 ] * A[ 6 ] ) + A[ 2 ] * ( A[ 3 ] * A[ 7 ] - A[ 4 ] * A[ 6 ] );
			const double inv[ 9 ] = { ( A[ 4 ] * A[ 8 ] - A[ 5 ] * A[ 7 ] ) / det, -( A[ 1 ] * A[ 8 ] - A[ 2 ] * A[ 7 ] ) / det, ( A[ 1 ] * A[ 5 ] - A[ 2 ] * A[ 4 ] ) / det,
				                      -( A[ 3 ] * A[ 8 ] - A[ 5 ] * A[ 6 ] ) / det, ( A[ 0 ] * A[ 8 ] - A[ 2 ] * A[ 6 ] ) / det, -( A[ 0 ] * A[ 5 ] - A[ 2 ] * A[ 3 ] ) / det,
				                      ( A[ 3 ] * A[ 7 ] - A[ 4 ] * A[ 6 ] ) / det, -( A[ 0 ] * A[ 7 ] - A[ 1 ] * A[ 6 ] ) / det, ( A[ 0 ] * A[ 4 ] - A[ 1 ] * A[ 3 ] ) / det };
			auto solve = [ & ]( const double* b, double* out ) {
				for( int r = 0; r < 3; ++r )
					out[ r ] = inv[ r * 3 ] * b[ 0 ] + inv[ r * 3 + 1 ] * b[ 1 ] + inv[ r * 3 + 2 ] * b[ 2 ];
			};
			double f0[ 3 ], k1[ 3 ], st[ 3 ], f1[ 3 ], b2[ 3 ], k2[ 3 ];
			F( s, f0 );
			solve( f0, k1 );
			for( int k = 0; k < 3; ++k )
				st[ k ] = s[ k ] + dt * k1[ k ];
			F( st, f1 );
			for( int k = 0; k < 3; ++k )
				b2[ k ] = f1[ k ] - 2.0 * k1[ k ];
			solve( b2, k2 );
			x[ i ] = std::max( s[ 0 ] + 1.5 * dt * k1[ 0 ] + 0.5 * dt * k2[ 0 ], 0.0 );
			y[ i ] = std::max( s[ 1 ] + 1.5 * dt * k1[ 1 ] + 0.5 * dt * k2[ 1 ], 0.0 );
			z[ i ] = std::max( s[ 2 ] + 1.5 * dt * k1[ 2 ] + 0.5 * dt * k2[ 2 ], 0.0 );
		}
	}
	/// The rightmost cell with x above `level`, or -1.
	int Front( double level ) const
	{
		for( int i = n - 1; i >= 0; --i )
			if( x[ i ] > level )
				return i;
		return -1;
	}
};

/// Speed by least squares of front position (cells) against time, over the
/// samples whose position lies in [lo, hi) cells. Returns mm/s and the count.
double fitSpeed( const std::vector< std::pair< double, double > >& samples, double lo, double hi, double dxMm, int& count )
{
	double st = 0, sp = 0, stt = 0, stp = 0;
	count = 0;
	for( const auto& s : samples )
		if( s.second >= lo && s.second < hi )
		{
			st += s.first;
			sp += s.second;
			stt += s.first * s.first;
			stp += s.first * s.second;
			++count;
		}
	if( count < 3 )
		return 0.0;
	const double slope = ( count * stp - st * sp ) / ( count * stt - st * st );
	return slope * dxMm;
}

/// The front position (cells, rightmost x above level) in a 1-D-ish plugin state.
int frontOf( const Floats& state, int cols, int rows, double level )
{
	for( int i = cols - 1; i >= 0; --i )
		if( state[ ( static_cast< size_t >( rows / 2 ) * cols + i ) * 4 ] > level )
			return i;
	return -1;
}

//===========================================================================
// --oregonator: the stirred dish's period and oxidised duty against the
// three-variable Oregonator in double; the two-variable reduction's error.
//===========================================================================
int runOregonator( const Perturb& perturb )
{
	std::printf( "\n=== oregonator: a stirred BZ dish's period and red/blue duty against the three-variable Oregonator in double\n" );
	const chem::Recipe recipe = chem::BaseRecipe( Reaction::BZ );
	chem::BZModel m           = chem::MakeBZ( recipe );
	if( perturb.oregonatorEps )
		m.eps *= 2.0;
	const double zRef = chem::BZPeakZ( chem::MakeBZ( recipe ) );
	//The references: three variables, and the two-variable reduction.
	auto periodOf = [ & ]( int nv ) {
		double s[ 3 ];
		chem::BZRestState( m, s );
		s[ 0 ] *= 1.01;
		std::vector< double > peaks;
		double lastZ = s[ 2 ], lastDz = 0.0, above = 0.0, total = 0.0, tPrev = 0.0;
		const int zi = nv == 3 ? 2 : 1;
		if( nv == 2 )
		{
			s[ 1 ] = s[ 2 ];
		}
		chem::IntegrateStiff( nv, [ & ]( const double* v, double* d ) {
			if( nv == 3 )
			{
				chem::OregonatorRhs( m, 0.0, v, d );
				for( int k = 0; k < 3; ++k )
					d[ k ] /= m.T0;
			}
			else
			{
				chem::TysonFifeRhs( m, 0.0, v[ 0 ], v[ 1 ], d[ 0 ], d[ 1 ] );
				d[ 0 ] /= m.T0;
				d[ 1 ] /= m.T0;
			}
		}, s, 1600.0, 1e-9, 1e-13, [ & ]( double t, const double* v ) {
			const double dz = v[ zi ] - lastZ;
			if( lastDz > 0.0 && dz <= 0.0 && v[ zi ] > 0.5 * zRef )
				peaks.push_back( t );
			lastDz = dz;
			lastZ  = v[ zi ];
			if( peaks.size() >= 3 )
			{
				total += t - tPrev;
				if( v[ zi ] > 0.5 * zRef )
					above += t - tPrev;
			}
			tPrev = t;
		}, 0.02 );
		double period = 0.0;
		if( peaks.size() >= 4 )
			period = ( peaks.back() - peaks[ 2 ] ) / static_cast< double >( peaks.size() - 3 );
		return std::make_pair( period, total > 0.0 ? above / total : 0.0 );
	};
	const auto ref3 = periodOf( 3 ), ref2 = periodOf( 2 );
	Note( fmt( "three variables: period %.2f s, oxidised (z > peak/2) %.1f%% of the time; the Tyson-Fife two-variable reduction: %.2f s (%.1f%% off: q = %.1e against eps' = %.1e, so the bromide is not fast at rest)",
	           ref3.first, 100.0 * ref3.second, ref2.first, 100.0 * std::fabs( ref2.first - ref3.first ) / ref3.first, m.q, m.epsPrime ) );
	for( const Raster& raster : kRasters )
	{
		double gpuPeriod[ 2 ] = { 0, 0 }, gpuDuty[ 2 ] = { 0, 0 }, pixelPeriod = 0.0;
		for( int pass = 0; pass < 2; ++pass )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::BZ, 48, 27 );
			bzHomogeneous( rig, 0.0 );
			if( perturb.oregonatorEps )
				rig.plugin.SetParamOverrideForTest( chem::P_BZ_EPS, static_cast< float >( m.eps ) );
			rig.Set( PT_STIR, 1.0f );
			rig.plugin.SetSubstepScaleForTest( pass == 0 ? 1.0 : 0.5 );
			rig.Render( 1 );
			const double frameChem = 0.5;
			std::vector< double > peaks, reds;
			double lastZ = 0.0, lastDz = 0.0, above = 0.0, total = 0.0;
			for( int f = 0; f < 3200; ++f )
			{
				rig.Chem( frameChem );
				double a[ 4 ], b[ 4 ];
				rig.plugin.MeanState( a, b );
				const double t = ( f + 1 ) * frameChem;
				const double dz = a[ 2 ] - lastZ;
				if( lastDz > 0.0 && dz <= 0.0 && a[ 2 ] > 0.5 * zRef )
					peaks.push_back( t );
				lastDz = dz;
				lastZ  = a[ 2 ];
				if( peaks.size() >= 3 )
				{
					total += frameChem;
					if( a[ 2 ] > 0.5 * zRef )
						above += frameChem;
				}
				if( pass == 0 )
				{
					const Floats out = rig.Output();
					double rgb[ 3 ];
					meanRGB( out, raster.w, raster.h, raster.w / 2 - 4, raster.h / 2 - 4, raster.w / 2 + 4, raster.h / 2 + 4, rgb );
					reds.push_back( rgb[ 0 ] );
				}
			}
			gpuPeriod[ pass ] = peaks.size() >= 4 ? ( peaks.back() - peaks[ 2 ] ) / static_cast< double >( peaks.size() - 3 ) : 0.0;
			gpuDuty[ pass ]   = total > 0.0 ? above / total : 0.0;
			if( pass == 0 && reds.size() > 100 )
			{
				//The red channel's downward crossings of its mid level: the
				//dish turning blue. (Minima would count float jitter on the
				//long red plateau.)
				double lo = reds[ 100 ], hi = reds[ 100 ];
				for( size_t i = 100; i < reds.size(); ++i )
				{
					lo = std::min( lo, reds[ i ] );
					hi = std::max( hi, reds[ i ] );
				}
				const double mid = 0.5 * ( lo + hi );
				std::vector< double > crossings;
				for( size_t i = 101; i < reds.size(); ++i )
					if( reds[ i - 1 ] >= mid && reds[ i ] < mid )
						crossings.push_back( i * frameChem );
				if( crossings.size() >= 3 )
					pixelPeriod = ( crossings.back() - crossings[ 1 ] ) / static_cast< double >( crossings.size() - 2 );
			}
		}
		//The step's own error, from the two substep sizes (first order: the
		//error at dt is about twice the difference between dt and dt/2), plus
		//one frame's sampling of the peaks and float's share of a period.
		const double stepError = 2.0 * std::fabs( gpuPeriod[ 0 ] - gpuPeriod[ 1 ] );
		const double bound     = stepError + 2.0 * 0.5 + 1e-5 * ref3.first;
		const double dutyBound = 2.0 * std::fabs( gpuDuty[ 0 ] - gpuDuty[ 1 ] ) + 2.0 * 0.5 / std::max( ref3.first, 1.0 ) + 0.01;
		Check( gpuPeriod[ 0 ] > 0.0 && std::fabs( gpuPeriod[ 0 ] - ref3.first ) <= bound && std::fabs( gpuDuty[ 0 ] - ref3.second ) <= dutyBound
		           && std::fabs( pixelPeriod - gpuPeriod[ 0 ] ) <= 1.0,
		       fmt( "%dx%d  stirred: period %.2f s (half the substep: %.2f) against %.2f s in double (bound %.2f from the step's own error and a frame); "
		            "oxidised %.1f%% of the time against %.1f%% (bound %.1f%%); the red channel's minima repeat every %.2f s",
		            raster.w, raster.h, gpuPeriod[ 0 ], gpuPeriod[ 1 ], ref3.first, bound, 100.0 * gpuDuty[ 0 ], 100.0 * ref3.second,
		            100.0 * dutyBound, pixelPeriod ) );
	}
	return Verdict();
}

//===========================================================================
// --fieldnoyes: a plane trigger wave's speed against the one-dimensional
// double solution of the same PDE, the sqrt( [H+][BrO3-] ) law across a 4x
// change of Acid, and the absolute speed against Field & Noyes 1974.
//===========================================================================
struct WaveRun
{
	double speedMmPerS = 0.0;
	int samples        = 0;
	double cellMm      = 0.0;
};

WaveRun planeWave( const Raster& raster, int cols, double dishMm, double acidMultiplier, bool diffusionOff, double seconds, double fOverride, int detailIndex = -1 )
{
	WaveRun run;
	Rig rig;
	if( !rig.Init( raster.w, raster.h ) )
		return run;
	prepare( rig, Reaction::BZ, cols, 4 );
	bzHomogeneous( rig, fOverride );
	rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( dishMm ) );
	rig.Set( PT_ACID_BASE, ParamFromRecipeMultiplier( acidMultiplier ) );
	if( detailIndex >= 0 )
		rig.Set( PT_DETAIL, static_cast< float >( detailIndex ) );
	rig.plugin.SetDiffusionOffForTest( diffusionOff );
	rig.Render( 1 );
	run.cellMm = rig.plugin.CellMm();
	rig.plugin.DropForTest( 3.0, 2.0, 5.0 );
	std::vector< std::pair< double, double > > samples;
	const double frameChem = 1.0;
	for( double t = 0.0; t < seconds; t += frameChem )
	{
		rig.Chem( frameChem );
		const int front = frontOf( rig.State(), cols, 4, 0.3 );
		if( front >= 0 )
			samples.emplace_back( t + frameChem, front );
	}
	run.speedMmPerS = fitSpeed( samples, 0.2 * cols, 0.7 * cols, run.cellMm, run.samples );
	return run;
}

/// The 1-D double reference's speed at a recipe multiplier of Acid, at the
/// plugin's cell size and a step `dtScale` x the plugin's own stable step.
double referenceSpeed( double acidMultiplier, double fWave, double dtScale, double seconds, int& count )
{
	chem::Recipe recipe = chem::ScaledRecipe( Reaction::BZ, 1.0, acidMultiplier, 1.0, 1.0 );
	const chem::BZModel m = chem::MakeBZ( recipe, fWave );
	const double dx   = 0.1;
	const double Dmax = std::max( { m.Du, chem::kOregonator.DY * chem::kCm2PerS_to_Mm2PerS, m.Dv } );
	BZLine line;
	line.Init( m, 512, dx, dtScale * std::min( 0.2 * dx * dx / Dmax, 0.25 * m.eps * m.T0 ) );
	line.Fire( 0, 8 );
	std::vector< std::pair< double, double > > samples;
	double t = 0.0;
	while( t < seconds )
	{
		const double until = t + 1.0;
		while( t < until )
		{
			line.Step();
			t += line.dt;
		}
		const int front = line.Front( 0.3 );
		if( front >= 0 )
			samples.emplace_back( t, front );
	}
	return fitSpeed( samples, 0.2 * 512, 0.7 * 512, dx, count );
}

int runFieldNoyes( const Perturb& perturb )
{
	std::printf( "\n=== fieldnoyes: a plane trigger wave's speed against the 1-D double solution at three acids; the acid exponent and Field & Noyes 1974, reported\n" );
	const double fWave = 2.6;//excitable: just past the Hopf point
	const chem::Recipe recipe = chem::BaseRecipe( Reaction::BZ );
	const chem::BZModel m     = chem::MakeBZ( recipe, fWave );
	struct Acid
	{
		double mult, seconds;
	};
	const Acid acids[] = { { 0.5, 560.0 }, { 1.0, 400.0 }, { 2.0, 300.0 } };
	double ref[ 3 ], refHalf[ 3 ];
	int refCount[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		ref[ i ]     = referenceSpeed( acids[ i ].mult, fWave, 1.0, acids[ i ].seconds, refCount[ i ] );
		int c2       = 0;
		refHalf[ i ] = referenceSpeed( acids[ i ].mult, fWave, 0.5, acids[ i ].seconds, c2 );
	}
	const double exponent = std::log( ref[ 2 ] / ref[ 0 ] ) / std::log( 4.0 );
	Note( fmt( "the 1-D double reference (dx 0.1 mm): %.4f, %.4f, %.4f mm/s at Acid 1/2x, 1x, 2x (at half its step: %.4f, %.4f, %.4f); "
	           "acid exponent %.2f against Field & Noyes 1974's 0.5; at 1x the law gives %.4f mm/s and the pulled-front limit 2 sqrt( D k4 A H ) %.4f",
	           ref[ 0 ], ref[ 1 ], ref[ 2 ], refHalf[ 0 ], refHalf[ 1 ], refHalf[ 2 ], exponent, chem::FieldNoyesSpeedMmPerS( recipe.acidBase, recipe.oxidant ),
	           m.frontSpeedMmPerS ) );
	for( const Raster& raster : kRasters )
	{
		int wrong = 0;
		std::string what;
		double speed1 = 0.0;
		for( int i = 0; i < 3; ++i )
		{
			const WaveRun one  = planeWave( raster, 512, 51.2, acids[ i ].mult, perturb.fieldNoyesNoDiff, acids[ i ].seconds, fWave );
			//The plugin at half its substep, for its own step error.
			WaveRun half;
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::BZ, 512, 4 );
				bzHomogeneous( rig, fWave );
				rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 51.2 ) );
				rig.Set( PT_ACID_BASE, ParamFromRecipeMultiplier( acids[ i ].mult ) );
				rig.plugin.SetDiffusionOffForTest( perturb.fieldNoyesNoDiff );
				rig.plugin.SetSubstepScaleForTest( 0.5 );
				rig.Render( 1 );
				half.cellMm = rig.plugin.CellMm();
				rig.plugin.DropForTest( 3.0, 2.0, 5.0 );
				std::vector< std::pair< double, double > > samples;
				for( double t = 0.0; t < acids[ i ].seconds; t += 1.0 )
				{
					rig.Chem( 1.0 );
					const int front = frontOf( rig.State(), 512, 4, 0.3 );
					if( front >= 0 )
						samples.emplace_back( t + 1.0, front );
				}
				half.speedMmPerS = fitSpeed( samples, 0.2 * 512, 0.7 * 512, half.cellMm, half.samples );
			}
			//One cell over the measurement interval, plus each side's own step
			//error (Richardson: twice the change on halving the step; the
			//splitting is first order).
			const double interval = 0.5 * 512 * one.cellMm / std::max( ref[ i ], 1e-9 );
			const double bound    = one.cellMm / interval + 2.0 * std::fabs( one.speedMmPerS - half.speedMmPerS ) + 2.0 * std::fabs( ref[ i ] - refHalf[ i ] );
			const bool ok         = one.samples >= 10 && std::fabs( one.speedMmPerS - ref[ i ] ) <= bound;
			wrong += !ok;
			what += fmt( " %gx: %.4f (half-step %.4f) vs %.4f, bound %.4f%s;", acids[ i ].mult, one.speedMmPerS, half.speedMmPerS, ref[ i ], bound, ok ? "" : " OUT" );
			if( i == 1 )
				speed1 = one.speedMmPerS;
		}
		Check( wrong == 0, fmt( "%dx%d  mm/s against the double reference:%s %d wrong; at 1x %.4f against Field & Noyes 1974's %.4f (%.0f%% off, reported)",
		                        raster.w, raster.h, what.c_str(), wrong, speed1, chem::FieldNoyesSpeedMmPerS( recipe.acidBase, recipe.oxidant ),
		                        100.0 * std::fabs( speed1 - chem::FieldNoyesSpeedMmPerS( recipe.acidBase, recipe.oxidant ) ) / chem::FieldNoyesSpeedMmPerS( recipe.acidBase, recipe.oxidant ) ) );
	}
	return Verdict();
}

//===========================================================================
// --spiral: Break Wave leaves one +1/-1 pair of phase singularities, which
// persist and rotate with a stable period.
//===========================================================================
struct Singularities
{
	int plus = 0, minus = 0;
	double px = 0, py = 0, mx = 0, my = 0;
};

Singularities windings( const Floats& state, int cols, int rows, double xStar, double zStar )
{
	Singularities s;
	auto phase = [ & ]( int i, int j ) {
		const float* c = &state[ ( static_cast< size_t >( j ) * cols + i ) * 4 ];
		return std::atan2( c[ 2 ] - zStar, c[ 0 ] - xStar );
	};
	for( int j = 2; j < rows - 3; ++j )
		for( int i = 2; i < cols - 3; ++i )
		{
			const double p[ 4 ] = { phase( i, j ), phase( i + 1, j ), phase( i + 1, j + 1 ), phase( i, j + 1 ) };
			double sum          = 0.0;
			for( int k = 0; k < 4; ++k )
			{
				double d = p[ ( k + 1 ) % 4 ] - p[ k ];
				while( d > kPi )
					d -= 2.0 * kPi;
				while( d < -kPi )
					d += 2.0 * kPi;
				sum += d;
			}
			const int w = static_cast< int >( std::lround( sum / ( 2.0 * kPi ) ) );
			if( w == 1 )
			{
				++s.plus;
				s.px = i + 0.5;
				s.py = j + 0.5;
			}
			else if( w == -1 )
			{
				++s.minus;
				s.mx = i + 0.5;
				s.my = j + 0.5;
			}
		}
	return s;
}

int runSpiral( const Perturb& perturb )
{
	std::printf( "\n=== spiral: a wave broken by the pipette leaves exactly one +1/-1 pair of phase singularities that persist and rotate at a steady period\n" );
	for( const Raster& raster : kRasters )
	{
		const int cols = 512, rows = 288;
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		prepare( rig, Reaction::BZ, cols, rows );
		bzHomogeneous( rig, 2.6 );
		rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 51.2 ) );
		rig.Render( 1 );
		rig.plugin.DropForTest( 3.0, rows * 0.5, 7.0 );
		//Let the wave reach the middle, then break it across the middle rows.
		double t = 0.0;
		for( ; t < 600.0; t += 1.0 )
		{
			rig.Chem( 1.0 );
			if( frontOf( rig.State(), cols, rows, 0.3 ) >= cols / 2 )
				break;
		}
		const chem::Params p = rig.plugin.CurrentParams();
		if( !perturb.spiralNoBar )
			rig.plugin.BarForTest( cols * 0.25, rows * 0.5, cols * 0.8, rows * 0.5, 15.0 );
		rig.Chem( 1.0 );
		//Track the singularities every 2 s for 320 s after the ends have curled;
		//the rotation period at eight probes on a ring of 40 cells round the pair.
		double zPeak = 0.0;
		{
			const Floats st = rig.State();
			for( size_t i = 2; i < st.size(); i += 4 )
				zPeak = std::max( zPeak, static_cast< double >( st[ i ] ) );
		}
		const double xStar = 0.2, zStar = 0.4 * zPeak;
		int samples = 0, onePair = 0, none = 0;
		Singularities first {};
		std::vector< std::vector< double > > probeX( 8 );
		for( int k = 0; k < 200; ++k )
		{
			rig.Chem( 2.0 );
			if( k < 40 )
				continue;//the ends curl first
			const Floats st        = rig.State();
			const Singularities sg = windings( st, cols, rows, xStar, zStar );
			++samples;
			if( sg.plus == 1 && sg.minus == 1 )
				++onePair;
			if( sg.plus == 0 && sg.minus == 0 )
				++none;
			if( samples == 1 )
				first = sg;
			const double cx = 0.5 * ( first.px + first.mx ), cy = 0.5 * ( first.py + first.my );
			for( int q = 0; q < 8; ++q )
			{
				const double ang = q * kPi / 4.0;
				const int pi = std::clamp( static_cast< int >( cx + 40.0 * std::cos( ang ) ), 0, cols - 1 );
				const int pj = std::clamp( static_cast< int >( cy + 40.0 * std::sin( ang ) ), 0, rows - 1 );
				probeX[ static_cast< size_t >( q ) ].push_back( st[ ( static_cast< size_t >( pj ) * cols + pi ) * 4 ] );
			}
		}
		std::vector< double > periods;
		for( const auto& series : probeX )
		{
			std::vector< double > times;
			for( size_t i = 1; i + 1 < series.size(); ++i )
				if( series[ i ] > series[ i - 1 ] && series[ i ] >= series[ i + 1 ] && series[ i ] > 0.3 )
					times.push_back( 2.0 * i );
			if( times.size() >= 3 )
				periods.push_back( ( times.back() - times.front() ) / static_cast< double >( times.size() - 1 ) );
		}
		std::sort( periods.begin(), periods.end() );
		const double median = periods.empty() ? 0.0 : periods[ periods.size() / 2 ];
		double spread       = 0.0;
		for( double q : periods )
			spread = std::max( spread, std::fabs( q - median ) );
		//Two seconds of sampling per period reading: the spread allows it.
		Check( samples > 0 && onePair >= static_cast< int >( 0.95 * samples ) && none == 0 && periods.size() >= 6 && spread <= 2.0 + 0.1 * median,
		       fmt( "%dx%d  after the break: %d of %d samples hold exactly one +1 and one -1 singularity (%d with none; first pair at (%.0f,%.0f) and (%.0f,%.0f)); "
		            "the arms sweep %zu of 8 probes every %.1f s (spread %.1f s)",
		            raster.w, raster.h, onePair, samples, none, first.px, first.py, first.mx, first.my, periods.size(), median, spread ) );
		(void)p;
	}
	return Verdict();
}

//===========================================================================
// --photo: in Ru-BZ a lit band above the model's threshold stops a wave and
// one below lets it through; ferroin ignores the same light.
//===========================================================================
bool waveCrosses( Rig& rig, int cols, int rows, double seconds )
{
	for( double t = 0.0; t < seconds; t += 1.0 )
	{
		rig.Chem( 1.0 );
		if( frontOf( rig.State(), cols, rows, 0.3 ) >= static_cast< int >( 0.8 * cols ) )
			return true;
	}
	return false;
}

int runPhoto( const Perturb& perturb )
{
	std::printf( "\n=== photo: Ru-BZ: a lit band above the photo-Oregonator's threshold stops a wave, one below lets it through; ferroin ignores the light\n" );
	const double fWave = 2.6;
	//The model's own threshold on the 1-D line: bisection on phi for a wave
	//crossing a 1 mm band (10 cells) of light.
	const chem::BZModel m = chem::MakeBZ( chem::BaseRecipe( Reaction::BZ ), fWave );
	auto crosses1D = [ & ]( double phi ) {
		BZLine line;
		const double dx   = 0.1;
		const double Dmax = std::max( { m.Du, chem::kOregonator.DY * chem::kCm2PerS_to_Mm2PerS, m.Dv } );
		line.Init( m, 160, dx, std::min( 0.05 * dx * dx / Dmax, 0.1 * m.eps * m.T0 ) );
		for( int i = 72; i < 88; ++i )
			line.phi[ i ] = phi;
		line.Fire( 0, 8 );
		double t = 0.0;
		while( t < 250.0 )
		{
			for( int k = 0; k < 40; ++k )
			{
				line.Step();
				t += line.dt;
			}
			if( line.Front( 0.3 ) >= 130 )
				return true;
		}
		return false;
	};
	double lo = 0.0, hi = chem::kPhiMax;
	const bool loCrosses = crosses1D( lo ), hiCrosses = crosses1D( hi );
	for( int i = 0; i < 7 && loCrosses && !hiCrosses; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( crosses1D( mid ) )
			lo = mid;
		else
			hi = mid;
	}
	Note( fmt( "the model's own threshold on the 1-D line, a 1.6 mm band: a wave crosses at phi %.4f and not at %.4f (full light is phi %.3f)", lo, hi, chem::kPhiMax ) );
	for( const Raster& raster : kRasters )
	{
		const int cols = 256, rows = 32;
		Floats card( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
			{
				const double u = ( x + 0.5 ) / raster.w;
				float* o       = &card[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
				const float lit = ( u >= 0.45 && u < 0.5125 ) ? 1.0f : 0.0f;
				o[ 0 ] = o[ 1 ] = o[ 2 ] = lit;
				o[ 3 ]                   = 1.0f;
			}
		struct Case
		{
			Catalyst catalyst;
			double coupling;
			bool expectCross;
			const char* what;
		};
		const double below = 0.5 * lo / chem::kPhiMax;
		const Case cases[] = { { Catalyst::Rubpy, 1.0, false, "Ru(bpy)3, full light" }, { Catalyst::Rubpy, below, true, "Ru(bpy)3, below the threshold" },
			                   { Catalyst::Ferroin, 1.0, true, "ferroin, full light" } };
		int wrong = 0;
		std::string what;
		for( const Case& c : cases )
		{
			Rig rig( true );
			rig.plugin.SetPhotoOffForTest( perturb.photoOff );
			if( !rig.Init( raster.w, raster.h, &card ) )
				return 1;
			prepare( rig, Reaction::BZ, cols, rows );
			bzHomogeneous( rig, fWave );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 25.6 ) );
			rig.Set( PT_CATALYST, static_cast< float >( c.catalyst ) );
			rig.Set( PT_SEED_FROM_CLIP, 0.0f );
			rig.Set( PT_LIGHT_COUPLING, static_cast< float >( c.coupling ) );
			rig.Render( 1 );
			rig.plugin.DropForTest( 3.0, rows * 0.5, 7.0 );
			const bool crossed = waveCrosses( rig, cols, rows, 320.0 );
			wrong += crossed != c.expectCross;
			what += fmt( " %s: %s", c.what, crossed ? "crossed" : "stopped" );
		}
		Check( wrong == 0, fmt( "%dx%d  a 1.6 mm lit band:%s (%d wrong)", raster.w, raster.h, what.c_str(), wrong ) );
	}
	return Verdict();
}

//===========================================================================
// The registry of checks.
//===========================================================================
struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "names", runNames, true },         { "spectra", runSpectra, true },    { "transport", runTransport, true },
		{ "timebase-law", runTimebaseLaw, true }, { "state", runState, false },  { "prime", runPrime, false },
		{ "resize", runResize, false },      { "timebase", runTimebase, false }, { "beer", runBeer, false },
		{ "over", runOverCheck, false },     { "oregonator", runOregonator, false }, { "fieldnoyes", runFieldNoyes, false },
		{ "spiral", runSpiral, false },      { "photo", runPhoto, false },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "spectra", runSpectra, "the reference observer shifted 40 nm", []( Perturb& p ) { p.spectraShift = true; } );
	add( "transport", runTransport, "a beat counted as a bar", []( Perturb& p ) { p.transportBeat = true; } );
	add( "beer", runBeer, "the plugin squares the half-depth colour per channel instead of integrating", []( Perturb& p ) { p.beerNaive = true; } );
	add( "over", runOverCheck, "Light Coupling 0 still couples the clip", []( Perturb& p ) { p.overLightOn = true; } );
	add( "prime", runPrime, "run the analyser unprimed", []( Perturb& p ) { p.primeOff = true; } );
	add( "resize", runResize, "clear the state on a resize", []( Perturb& p ) { p.resizeClears = true; } );
	add( "timebase", runTimebase, "take elapsed time from the host clock in float", []( Perturb& p ) { p.timebaseFloat = true; } );
	add( "oregonator", runOregonator, "the plugin's eps doubled against the reference", []( Perturb& p ) { p.oregonatorEps = true; } );
	add( "fieldnoyes", runFieldNoyes, "diffusion switched off (no wave)", []( Perturb& p ) { p.fieldNoyesNoDiff = true; } );
	add( "spiral", runSpiral, "no break: a target, not a spiral pair", []( Perturb& p ) { p.spiralNoBar = true; } );
	add( "photo", runPhoto, "the phi term dropped: light does nothing", []( Perturb& p ) { p.photoOff = true; } );
	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );
	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	std::printf( "\n=== bench: after 60 frames of warm-up, each frame timed with glFinish both sides (idle figures only)\n" );
	struct Case
	{
		bool effect;
		Reaction reaction;
		const char* what;
	};
	const Case cases[] = { { false, Reaction::BZ, "BZ, Detail 256, 30x" }, { true, Reaction::BZ, "BZ, Detail 256, 30x" },
		                   { false, Reaction::BriggsRauscher, "Briggs-Rauscher (CPU), 30x" }, { false, Reaction::BlueBottle, "Blue Bottle, Detail 256, 30x" },
		                   { false, Reaction::BZ, "BZ, Detail 1024, 30x" } };
	int n = 0;
	for( const Case& c : cases )
	{
		++n;
		for( const Size& size : sizes )
		{
			Rig rig( c.effect );
			if( !rig.Init( size.w, size.h ) )
				return 1;
			rig.Set( PT_REACTION, static_cast< float >( c.reaction ) );
			rig.Set( PT_DETAIL, n == 5 ? 3.0f : 1.0f );
			if( !rig.Render( 60 ) )
				return 1;
			glFinish();
			constexpr int kTimed = 90;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				if( !rig.Render( 1 ) )
					return 1;
				glFinish();
				times.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::printf( "  %-17s %-30s %-6s median %6.2f ms/frame, worst %6.2f  (median %4.1f%% of a 60 fps frame)\n",
			             c.effect ? "SW Honeydew Over" : "SW Honeydew", c.what, size.name, times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/honeydew.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 300;
	double fps = 60.0;
	bool beat = false, effect = false, sizeGiven = false;
	std::string mode;
	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "hdtest -- render Honeydew offline and measure its chemistry\n\n"
			             "  --out PATH        render and write a PNG (default /tmp/honeydew.png)\n"
			             "  --over            the Over effect, on the harness's card\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 300: 5 s)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --beat            feed a beat every half second into the Audio buffer\n"
			             "  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
			             "  --list            every parameter and its default\n\n"
			             "  checks (GL): --state --prime --resize --timebase --beer --over-check --oregonator --fieldnoyes --spiral --photo\n"
			             "  checks (no GL): --names --spectra --transport --timebase-law\n"
			             "  --negative   --offline   --bench\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument == "--over-check" )
			mode = "over";
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		HoneydewPlugin plugin( effect );
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The chemistry and picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n", failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}
	if( ran )
		;
	else if( mode == "negative" )
		result = runNegative( false );
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig( effect );
		rig.fps = fps;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			if( !rig.Render( std::max( frames, 1 ) ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), %.1f chemical s, %lld substeps, %lld capped frames\n", outPath.c_str(), width, height,
				             frames, fps, frames / fps, rig.plugin.ChemicalTime(), rig.plugin.SubstepsTaken(), rig.plugin.CappedFrames() );
			else
				result = 1;
		}
	}
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
