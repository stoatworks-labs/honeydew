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
#include <cctype>
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

/// A value for a parameter, from text: a number, or for an option one of its
/// element names (case-insensitive) or its index. Anything else is refused:
/// `strtof( "Fixed" )` silently becomes 0 (polyhedral), and a cue that
/// silently did nothing would film a take that looks deliberate and is wrong.
bool resolveValue( HoneydewPlugin& plugin, const NamedParameter& parameter, const std::string& text, float& out, std::string& error )
{
	auto lower = []( std::string s ) {
		for( char& c : s )
			c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		return s;
	};
	if( parameter.type == FF_TYPE_OPTION )
	{
		const unsigned int count = plugin.GetNumParamElements( parameter.index );
		for( unsigned int k = 0; k < count; ++k )
		{
			const char* const element = plugin.GetParamElementName( parameter.index, k );
			if( element && lower( element ) == lower( text ) )
			{
				out = static_cast< float >( k );
				return true;
			}
		}
	}
	char* end        = nullptr;
	const float v    = std::strtof( text.c_str(), &end );
	const bool whole = end && *end == '\0' && end != text.c_str();
	if( !whole )
	{
		error = "'" + text + "' is not a value for " + parameter.name;
		if( parameter.type == FF_TYPE_OPTION )
		{
			error += " (its options:";
			const unsigned int count = plugin.GetNumParamElements( parameter.index );
			for( unsigned int k = 0; k < count; ++k )
			{
				const char* const element = plugin.GetParamElementName( parameter.index, k );
				error += std::string( " '" ) + ( element ? element : "?" ) + "'";
			}
			error += ")";
		}
		return false;
	}
	out = v;
	return true;
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
			float v = 0.0f;
			if( !resolveValue( plugin, parameter, value, v, error ) )
				return false;
			plugin.SetFloatParameter( parameter.index, v );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

//---------------------------------------------------------------------------
// The cue sheet (the fleet's format): `frame  Parameter Name  value` lines,
// `#` comments. A control whose value is a choice, a switch, a press or a
// count STEPS between keys; only a standard control ramps.
//---------------------------------------------------------------------------
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

using Track    = std::vector< std::pair< int, float > >;
using RawTrack = std::vector< std::pair< int, std::string > >;

/// Each line is `frame Parameter Name value`, where the name is the longest
/// run of words that is one of `names` and the rest is the value, so
/// `0 Drop Position Centre` and `0 Reaction Chemical Chameleon` both read
/// (an option's name may have spaces, and `Drop`, `Drop Size` and `Drop
/// Position` all exist). A line naming no parameter is an error.
std::map< std::string, RawTrack > loadScript( std::istream& in, const std::string& path, const std::vector< std::string >& names, std::string& error )
{
	std::map< std::string, RawTrack > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		auto joined = [ & ]( size_t from, size_t to ) {
			std::string s;
			for( size_t i = from; i < to; ++i )
				s += ( i > from ? " " : "" ) + parts[ i ];
			return s;
		};
		size_t split = 0;
		for( size_t n = parts.size() - 1; n >= 1; --n )
			if( std::find( names.begin(), names.end(), joined( 0, n ) ) != names.end() )
			{
				split = n;
				break;
			}
		if( split == 0 )
		{
			error = "script names '" + joined( 0, parts.size() - 1 ) + "', which is not a parameter (try --list)";
			return {};
		}
		tracks[ joined( 0, split ) ].emplace_back( frame, joined( split, parts.size() ) );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, std::string >& a, const std::pair< int, std::string >& b ) { return a.first < b.first; } );
	return tracks;
}

/// The value at `frame`: held after the last key (and before the first, but
/// runPipe leaves a parameter alone until its first key); between two keys
/// linear if `ramp`, otherwise the earlier key's value until the later key's
/// frame.
float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

struct Cue
{
	Track track;
	bool ramp;
};

/// The cue sheet bound to a plugin's parameters, or an error naming the cue:
/// a name that is not a parameter, or a value that is not one of its.
bool bindScript( HoneydewPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::vector< NamedParameter > known = listParameters( plugin );
	std::vector< std::string > names;
	for( const NamedParameter& parameter : known )
		names.push_back( parameter.name );
	const std::map< std::string, RawTrack > tracks = loadScript( file, path, names, error );
	if( !error.empty() )
		return false;
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				Track track;
				for( const auto& key : entry.second )
				{
					float v = 0.0f;
					if( !resolveValue( plugin, parameter, key.second, v, error ) )
					{
						error = path + ": at frame " + std::to_string( key.first ) + ": " + error;
						return false;
					}
					track.emplace_back( key.first, v );
				}
				out[ parameter.index ] = Cue { track, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock.
//===========================================================================
/// `readStdin`: the Over effect's frames come in on stdin, one out per one in,
/// until a partial frame or EOF. Otherwise frames are made -- `count` of them,
/// or, with `count` 0, until the reader hangs up (so that mode only ever ends
/// with exit 1; use a count for a take that can end cleanly). Without stdin
/// the Over runs on the harness's card.
int runPipe( bool effect, int width, int height, double fps, const std::string& scriptPath, int count, bool readStdin,
             bool beat, const std::vector< std::string >& settings )
{
	Rig rig( effect );
	rig.fps = fps;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;
	//A misspelt cue that silently did nothing would film a take that looks
	//deliberate and is wrong: refuse any name that is not a parameter.
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}
	//"Name=V" now; "Name=V@F" is a cue at frame F.
	for( const std::string& setting : settings )
	{
		std::string error;
		const size_t at = setting.find( '@' );
		if( at == std::string::npos )
		{
			if( !applySetting( rig.plugin, setting, error ) )
			{
				std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
				return 2;
			}
			continue;
		}
		const std::string assignment = setting.substr( 0, at );
		const int frame              = std::atoi( setting.c_str() + at + 1 );
		const size_t equals          = assignment.find( '=' );
		bool found                   = false;
		for( const NamedParameter& parameter : listParameters( rig.plugin ) )
			if( equals != std::string::npos && parameter.name == assignment.substr( 0, equals ) )
			{
				float v = 0.0f;
				if( !resolveValue( rig.plugin, parameter, assignment.substr( equals + 1 ), v, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
				Cue& cue = automation[ parameter.index ];
				cue.ramp = !stepsBetweenCues( parameter.type );
				cue.track.emplace_back( frame, v );
				std::stable_sort( cue.track.begin(), cue.track.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
				found = true;
			}
		if( !found )
		{
			std::fprintf( stderr, "--set %s: no parameter called '%s'\n", setting.c_str(), assignment.substr( 0, equals ).c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || count <= 0 || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			if( effect )
				rig.Upload( picture );
		}

		//Through the plugin's own setter, so a cue moves what a slider would.
		//A parameter is untouched before its first key (its default stands),
		//so `120 Drop 1` presses at frame 120, not at frame 0: a press is the
		//rising edge the plugin sees, and it takes a `0` key to press again.
		for( const auto& cue : automation )
			if( index >= cue.second.track.front().first )
				rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
			{
				//The source is the lightbox: opaque. The Over keeps the clip's alpha.
				const float v = ( !effect && x % 4 == 3 ) ? 1.0f : out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ];
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
			}
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone (`| head -c 1`, ffmpeg dying). SIGPIPE is
			//ignored in main(), so this is EPIPE and not a silent 141: say so
			//and stop, rather than render on into a closed pipe.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
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
	bool excitableNoBar  = false;///< --excitable: the Break Wave never pressed (a ring, not a pair)
	bool excitableDefault = false;///< --excitable: the default Excitability behaving as the excitable one
	bool freshAtRest     = false;///< --freshstir: the fresh dish seeded exactly on the rest state (0.1.0)
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
		//by at most a third; and the cited lambda is a local maximum to 2%
		//(the band has not been pulled off its peak by a shoulder), judged
		//8 nm either side (permanganate's second peak sits 20 nm away).
		const bool height = e >= 0.999 * c.epsMax && e <= 1.35 * c.epsMax;
		bool local        = true;
		for( double d = -8.0; d <= 8.0; d += 1.0 )
			local = local && spectra::Epsilon( sp, c.lambdaMax + d ) <= e * 1.02;
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
			//the card and one on black: the states must agree exactly. A third rig
			//on the card again is the control for the renderer itself: this GPU
			//gives two contexts the same floats, Apple's software renderer does not
			//(it differed by 2e-3 over 80 s of a wave, from frame 5), so where the
			//control differs the card-black difference is held to twice it.
			Floats black( card.size(), 0.0f );
			for( size_t i = 3; i < black.size(); i += 4 )
				black[ i ] = 1.0f;
			Rig a( true ), b( true ), c( true );
			if( !a.Init( raster.w, raster.h, &card ) || !b.Init( raster.w, raster.h, &black ) || !c.Init( raster.w, raster.h, &card ) )
				return 1;
			for( Rig* r : { &a, &b, &c } )
			{
				prepare( *r, Reaction::BZ, 64, 36 );
				r->Set( PT_CATALYST, static_cast< float >( Catalyst::Rubpy ) );
				r->Set( PT_SEED_FROM_CLIP, 0.0f );
				r->Set( PT_LIGHT_COUPLING, perturb.overLightOn ? 1.0f : 0.0f );
				r->Set( PT_DROP_POSITION, 1.0f );
			}
			for( Rig* r : { &a, &b, &c } )
			{
				r->Render( 1 );
				r->plugin.DropForTest( 32.0, 18.0, 3.0 );
			}
			int differ = 0, differControl = 0;
			double maxU = 0.0, maxGap = 0.0, maxGapControl = 0.0;
			for( int i = 0; i < 40; ++i )
			{
				a.Chem( 2.0 );
				b.Chem( 2.0 );
				c.Chem( 2.0 );
				const Floats sa = a.State(), sb = b.State(), sc = c.State();
				for( size_t k = 0; k < sa.size(); ++k )
				{
					if( sa[ k ] != sb[ k ] )
					{
						++differ;
						maxGap = std::max( maxGap, static_cast< double >( std::fabs( sa[ k ] - sb[ k ] ) ) );
					}
					if( sa[ k ] != sc[ k ] )
					{
						++differControl;
						maxGapControl = std::max( maxGapControl, static_cast< double >( std::fabs( sa[ k ] - sc[ k ] ) ) );
					}
					//The wave: HBrO2 (x) excited somewhere away from the drop.
					const size_t cell = k / 4;
					if( k % 4 == 0 && ( cell % 64 ) < 20 )
						maxU = std::max( maxU, static_cast< double >( sa[ k ] ) );
				}
			}
			const std::string what = fmt( "%dx%d  Ru-BZ with Light Coupling 0: 80 s of a wave on the card and on black agree in every state value at every frame "
			                              "(%d differ, by at most %.3g; the same-clip control: %d differ, by at most %.3g; a wave reached the far third: max x there %.2f)",
			                              raster.w, raster.h, differ, maxGap, differControl, maxGapControl, maxU );
			if( differControl > 0 && !perturb.overLightOn )
				//Two contexts on the SAME clip do not agree on this renderer (Apple's
				//software renderer: 5e-4 to 2e-3 after 80 s of a wave, from frame 5),
				//so a bit-exact comparison across contexts proves nothing here. It is
				//the GPU's check; the numbers are printed.
				Skip( what + " -- this renderer gives two contexts different floats, so the exact comparison is the GPU's" );
			else
				Check( differ == 0 && maxU > 0.1, what );
		}
	}
	return Verdict();
}

//===========================================================================
// BZ helpers: a still dish with no pacemakers, the three-variable model's
// one-dimensional double reference, and the phase around a point.
//===========================================================================
/// The excitable setting the wave checks run at: f = 2.6, just past the Hopf
/// point at the 1x recipe (1 + sqrt 2 = 2.414), as the EXCITABILITY CONTROL
/// gives it -- the float the host would send, converted by the plugin's own
/// mapping, so the references and the plugin use the same f to the bit.
const float kExcitableParam = ParamFromExcitability( 2.6 );
const double kFWave         = ExcitabilityFromParam( kExcitableParam );

/// Params with the pacemaker effect off (a homogeneous dish: the pacemakers
/// have no control, so this stays a test hook) and, when `f` > 0, the
/// stoichiometric factor set THROUGH THE EXCITABILITY CONTROL (0.1.1; 0.1.0
/// set it through a hook no user could reach).
void bzHomogeneous( Rig& rig, double f )
{
	rig.plugin.SetParamOverrideForTest( chem::P_BZ_PACE, 0.0f );
	if( f > 0.0 )
		rig.Set( PT_EXCITABILITY, ParamFromExcitability( f ) );
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
	//The reference is the model at the recipe; the negative control doubles
	//the PLUGIN's eps against it (not both: a wrong model shared by the
	//reference and the plugin would pass, and did once).
	const chem::BZModel m = chem::MakeBZ( recipe );
	const double zRef     = chem::BZPeakZ( m );
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
				rig.plugin.SetParamOverrideForTest( chem::P_BZ_EPS, static_cast< float >( 2.0 * m.eps ) );
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
	const double fWave = kFWave;//excitable: just past the Hopf point, through the control
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
		bzHomogeneous( rig, kFWave );
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
// --excitable: Break Wave winds a spiral pair at a setting of the
// Excitability control, from the defaults (the pacemakers on, Flow, a 60 mm
// dish at Detail 256) plus that control, a Drop and the Break Wave button --
// no harness hooks; and at the default Excitability (f = 1.4, the 0.1.0
// dish) the same Break Wave leaves no persistent pair, which is the 0.1.0
// behaviour the release video found, now measured.
//===========================================================================
/// Every +1 and -1 phase singularity's position, for the user-spiral check,
/// which wants to tell a persistent core from a transient defect pair.
void windingsAll( const Floats& state, int cols, int rows, double xStar, double zStar, std::vector< std::pair< double, double > >& plus, std::vector< std::pair< double, double > >& minus )
{
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
				plus.emplace_back( i + 0.5, j + 0.5 );
			else if( w == -1 )
				minus.emplace_back( i + 0.5, j + 0.5 );
		}
}

struct UserSpiral
{
	bool ok = false;      ///< the rig ran
	bool reached = false; ///< the drop's wave reached the pipette's band
	double f = 0.0, tBreak = 0.0;
	int samples = 0, held = 0, none = 0, pairs = 0;///< pairs: +1/-1 pairs in the first sample after the curl
	int extra = 0, odd = 0;                        ///< samples with more pairs than the first, or unequal +1 and -1
	int present = 0;                                ///< samples with at least one +1 and one -1
	int tracked = 0;                                ///< samples in which both of the first pair's cores are found near where they last were
	double maxStep = 0.0;                           ///< the furthest a tracked core moved between samples (2 s), in cells
	double extraGap = 1e9;                          ///< the widest a transient extra pair ever got, in cells
	double coreGap = 0.0;                           ///< the first pair's separation, in cells
	std::string trace;                              ///< the (+,-) counts of every sample that is not held
	double period = 0.0, spread = 0.0;
	size_t probes = 0;
	int cols = 0, rows = 0;
	double px = 0, py = 0, mx = 0, my = 0;
};

UserSpiral userSpiral( const Raster& raster, float excitability, bool pressBar )
{
	UserSpiral r;
	Rig rig;
	if( !rig.Init( raster.w, raster.h ) )
		return r;
	r.ok = true;
	//The defaults, plus the control: no SetGridForTest, no overrides.
	rig.Set( PT_REACTION, static_cast< float >( Reaction::BZ ) );
	rig.Set( PT_EXCITABILITY, excitability );
	rig.plugin.SetUncappedForTest( true );//a chemical second a frame, exactly
	rig.Render( 1 );
	r.f    = ExcitabilityFromParam( excitability );
	r.cols = rig.plugin.GridCols();
	r.rows = rig.plugin.GridRows();
	const int cols = r.cols, rows = r.rows;
	//A Drop (the button), where Drop Position Random puts it.
	rig.Set( PT_DROP, 1.0f );
	rig.Chem( 1.0 );
	rig.Set( PT_DROP, 0.0f );
	//The wave reaches the pipette's band (the middle row, the middle third):
	//then the Break Wave button, which erases the band and leaves two ends.
	auto inBand = [ & ]( const Floats& st ) {
		for( int j = rows / 2 - 1; j <= rows / 2 + 1; ++j )
			for( int i = static_cast< int >( 0.36 * cols ); i < static_cast< int >( 0.64 * cols ); ++i )
				if( st[ ( static_cast< size_t >( j ) * cols + i ) * 4 ] > 0.3f )
					return true;
		return false;
	};
	double t = 1.0;
	for( ; t < 600.0; t += 1.0 )
	{
		rig.Chem( 1.0 );
		if( inBand( rig.State() ) )
		{
			r.reached = true;
			break;
		}
	}
	r.tBreak = t;
	if( pressBar )
	{
		rig.Set( PT_BREAK_WAVE, 1.0f );
		rig.Chem( 1.0 );
		rig.Set( PT_BREAK_WAVE, 0.0f );
	}
	else
		rig.Chem( 1.0 );
	double zPeak = 0.0;
	{
		const Floats st = rig.State();
		for( size_t i = 2; i < st.size(); i += 4 )
			zPeak = std::max( zPeak, static_cast< double >( st[ i ] ) );
	}
	const double xStar = 0.2, zStar = 0.4 * zPeak;
	Singularities first {};
	double trackPx = 0, trackPy = 0, trackMx = 0, trackMy = 0;
	std::vector< std::vector< double > > probeX( 8 );
	for( int k = 0; k < 200; ++k )
	{
		rig.Chem( 2.0 );
		if( k < 40 )
			continue;//the ends curl first
		const Floats st        = rig.State();
		const Singularities sg = windings( st, cols, rows, xStar, zStar );
		++r.samples;
		if( r.samples == 1 )
		{
			first   = sg;
			r.pairs = std::min( sg.plus, sg.minus );
			r.px    = sg.px;
			r.py    = sg.py;
			r.mx    = sg.mx;
			r.my    = sg.my;
			trackPx = sg.px;
			trackPy = sg.py;
			trackMx = sg.mx;
			trackMy = sg.my;
		}
		if( sg.plus >= 1 && sg.minus >= 1 )
			++r.present;
		//Track the first pair's two cores: a spiral core meanders, but a few
		//cells between samples; a core that is not found within 10 cells of
		//where it last was has gone.
		{
			std::vector< std::pair< double, double > > plus, minus;
			windingsAll( st, cols, rows, xStar, zStar, plus, minus );
			auto follow = [ & ]( std::vector< std::pair< double, double > >& found, double& fx, double& fy, double& step ) {
				double best = 1e9;
				std::pair< double, double > at { fx, fy };
				for( const auto& q : found )
				{
					const double d = std::hypot( q.first - fx, q.second - fy );
					if( d < best )
					{
						best = d;
						at   = q;
					}
				}
				if( best <= 10.0 )
				{
					step = std::max( step, best );
					fx   = at.first;
					fy   = at.second;
					return true;
				}
				return false;
			};
			const bool p1 = follow( plus, trackPx, trackPy, r.maxStep );
			const bool m1 = follow( minus, trackMx, trackMy, r.maxStep );
			if( p1 && m1 )
				++r.tracked;
		}
		if( sg.plus == sg.minus && sg.plus == r.pairs && r.pairs >= 1 )
			++r.held;
		else
		{
			if( sg.plus != sg.minus )
				++r.odd;
			else if( sg.plus > r.pairs )
				++r.extra;
			r.trace += fmt( " %d:(%d,%d)", k, sg.plus, sg.minus );
			//How far apart the extra singularities are: a transient defect is a
			//+1 and a -1 a few cells apart; a second spiral pair would be far.
			std::vector< std::pair< double, double > > plus, minus;
			windingsAll( st, cols, rows, xStar, zStar, plus, minus );
			double widest = 0.0;
			for( const auto& a : plus )
			{
				if( std::hypot( a.first - r.px, a.second - r.py ) < 3.0 )
					continue;//the first pair's +1 core
				double nearest = 1e9;
				for( const auto& b : minus )
					nearest = std::min( nearest, std::hypot( a.first - b.first, a.second - b.second ) );
				widest = std::max( widest, nearest );
			}
			r.extraGap = r.extraGap > 1e8 ? widest : std::max( r.extraGap, widest );
		}
		if( r.samples == 1 )
			r.coreGap = std::hypot( r.px - r.mx, r.py - r.my );
		if( sg.plus == 0 && sg.minus == 0 )
			++r.none;
		const double cx = 0.5 * ( first.px + first.mx ), cy = 0.5 * ( first.py + first.my );
		for( int q = 0; q < 8; ++q )
		{
			const double ang = q * kPi / 4.0;
			const int pi = std::clamp( static_cast< int >( cx + 20.0 * std::cos( ang ) ), 0, cols - 1 );
			const int pj = std::clamp( static_cast< int >( cy + 20.0 * std::sin( ang ) ), 0, rows - 1 );
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
	r.probes = periods.size();
	r.period = periods.empty() ? 0.0 : periods[ periods.size() / 2 ];
	for( double q : periods )
		r.spread = std::max( r.spread, std::fabs( q - r.period ) );
	return r;
}

int runExcitable( const Perturb& perturb )
{
	std::printf( "\n=== excitable: from the defaults, Excitability past the Hopf point, a Drop and the Break Wave button wind a spiral pair that persists; at the default Excitability the same press leaves none\n" );
	//A slider position a user reaches: 0.7, f = 4^0.7 = 2.64 (the Hopf point is at 0.636).
	const float reachable = 0.7f;
	for( const Raster& raster : kRasters )
	{
		const UserSpiral e = userSpiral( raster, reachable, !perturb.excitableNoBar );
		if( !e.ok )
			return 1;
		//A pair in at least 95% of the samples (the cores are the first
		//pair's: a transient defect pair elsewhere -- a +1 and a -1 a few cells
		//apart where an arm crosses a pacemaker site -- is counted and shown,
		//and must stay small beside the first pair's separation).
		//The pair the press made persists: both of its cores are followed
		//through at least 95% of the samples (a core meanders a few cells
		//between samples), the dish is never without a singularity, and the
		//arms sweep the probes at one period. A second pair that a wave break
		//makes and that annihilates again (the dish is heterogeneous: the
		//pacemaker sites run 30% faster) is counted and shown, not gated.
		Check( e.reached && e.pairs >= 1 && e.samples > 0 && e.tracked >= static_cast< int >( 0.95 * e.samples ) && e.none == 0 && e.probes >= 6 && e.spread <= 2.0 + 0.1 * e.period,
		       fmt( "%dx%d  Excitability %.2f (f %.2f) on the %dx%d grid of a 60 mm dish: the drop's wave reached the pipette at %.0f s; after the press the pair's two cores are followed through %d of %d samples, moving at most %.1f cells in 2 s (first at (%.0f,%.0f) and (%.0f,%.0f), %.0f cells apart; %d samples with no singularity; %d samples hold a second pair, up to %.0f cells wide, that comes and goes); the arms sweep %zu of 8 probes every %.1f s (spread %.1f s)",
		            raster.w, raster.h, reachable, e.f, e.cols, e.rows, e.tBreak, e.tracked, e.samples, e.maxStep, e.px, e.py, e.mx, e.my, e.coreGap, e.none, e.extra, e.extra ? e.extraGap : 0.0, e.probes, e.period, e.spread ) );
		//The 0.1.0 dish: the default Excitability, the same press.
		const float defaultParam = perturb.excitableDefault ? reachable : ParamFromExcitability( chem::kOregonator.f );
		const UserSpiral d       = userSpiral( raster, defaultParam, true );
		if( !d.ok )
			return 1;
		Check( d.reached && d.present < static_cast< int >( 0.5 * d.samples ),
		       fmt( "%dx%d  at the default Excitability (f %.2f) the same press leaves no persistent pair: a pair is present in %d of %d samples (%d with none): the bulk firing overruns the ends, as 0.1.0 did",
		            raster.w, raster.h, d.f, d.present, d.samples, d.none ) );
	}
	return Verdict();
}

//===========================================================================
// --freshstir: a fresh BZ dish at Stir 1, from Reset, starts on its own.
// 0.1.0 seeded the rest state exactly, and the whole-vessel relaxation of
// hard stirring held the uniform dish on that unstable fixed point for 1500 s
// (the browser demo's build found it). The fresh dish now starts a touch
// above it (kFreshKick); the negative control seeds the exact rest state.
//===========================================================================
int runFreshStir( const Perturb& perturb )
{
	std::printf( "\n=== freshstir: a fresh BZ dish at Stir 1, from Reset, on the host's clock at the default Time-lapse, oscillates on its own within the stated time\n" );
	const chem::BZModel m = chem::MakeBZ( chem::BaseRecipe( Reaction::BZ ) );
	const double peak     = chem::BZPeakZ( m );
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		rig.plugin.SetFreshAtRestForTest( perturb.freshAtRest );
		rig.Set( PT_REACTION, static_cast< float >( Reaction::BZ ) );
		rig.Set( PT_STIR, 1.0f );
		//The user's clock: 60 fps at the default 30x, half a chemical second a
		//frame, uncapped so the chemistry is exact (the cap bites just above
		//32x when stirred, AGENTS.md). A dish seeded exactly on the rest state
		//stays there on this clock (0.1.0's fault, the negative control); it is
		//the plugin's rounding that pins it, and a different frame length can
		//happen to let it go, so the check runs the clock a user runs.
		rig.plugin.SetUncappedForTest( true );
		rig.Render( 1 );
		//Reset: the button, so the dish is the one a user gets.
		rig.Set( PT_RESET, 1.0f );
		rig.Render( 1 );
		rig.Set( PT_RESET, 0.0f );
		double first = -1.0, second = -1.0;
		int rises    = 0;
		bool above   = false;
		double zMin = 1e9, zMax = -1e9;
		const double chemPerFrame = TimelapseFromParam( rig.plugin.GetFloatParameter( static_cast< unsigned int >( rig.plugin.HostIndexOf( PT_TIMELAPSE ) ) ) ) / rig.fps;
		const int frames          = static_cast< int >( std::lround( 300.0 / chemPerFrame ) );
		for( int f = 1; f <= frames; ++f )
		{
			rig.Render( 1 );
			if( f % 2 )
				continue;
			const double t  = f * chemPerFrame;
			const Floats st = rig.State();
			double mean     = 0.0;
			const size_t n  = st.size() / 4;
			for( size_t i = 0; i < n; ++i )
				mean += st[ i * 4 + 2 ] / n;
			zMin = std::min( zMin, mean );
			zMax = std::max( zMax, mean );
			const bool a = mean > 0.5 * peak;
			if( a && !above )
			{
				++rises;
				if( first < 0.0 )
					first = t;
				else if( second < 0.0 )
					second = t;
			}
			above = a;
		}
		//Two rises within 300 chemical seconds, the first within 60 s (the ODE
		//from the same kick peaks at 15 s; a period is 101 s).
		Check( rises >= 2 && first > 0.0 && first <= 60.0,
		       fmt( "%dx%d  a fresh dish at Stir 1 from Reset, %d frames of %.2f chemical s: the catalyst's dish mean rises through half the model's peak %d times in 300 s, first at %.0f s then at %.0f s (z from %.4f to %.4f; the peak in double %.4f)",
		            raster.w, raster.h, frames, chemPerFrame, rises, first, second, zMin, zMax, peak ) );
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
	const double fWave = kFWave;
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
// Colour reading. Every pixel check reads the centre patch of the output,
// converts to hue / saturation / value in linear sRGB, and names the colour
// by the same bands the README uses.
//===========================================================================
enum class Colour
{
	Colourless,
	Red,
	Amber,
	Yellow,
	Green,
	Blue,
	Purple,
	Dark
};
const char* colourName( Colour c )
{
	switch( c )
	{
	case Colour::Colourless: return "colourless";
	case Colour::Red: return "red";
	case Colour::Amber: return "amber";
	case Colour::Yellow: return "yellow";
	case Colour::Green: return "green";
	case Colour::Blue: return "blue";
	case Colour::Purple: return "purple";
	default: return "dark";
	}
}
Colour classify( const double rgb[ 3 ] )
{
	double h, s, v;
	spectra::RGBToHSV( rgb, h, s, v );
	if( v < 0.3 )
		return Colour::Dark;
	if( s < 0.25 && v > 0.5 )
		return Colour::Colourless;
	if( h >= 330.0 || h < 22.0 )
		return Colour::Red;
	if( h < 45.0 )
		return Colour::Amber;
	if( h < 80.0 )
		return Colour::Yellow;
	if( h < 180.0 )
		return Colour::Green;
	if( h < 262.0 )
		return Colour::Blue;
	return Colour::Purple;
}
void centreRGB( const Rig& rig, double rgb[ 3 ] )
{
	const Floats out = rig.Output();
	meanRGB( out, rig.width, rig.height, rig.width / 2 - 3, rig.height / 2 - 3, rig.width / 2 + 3, rig.height / 2 + 3, rgb );
}

/// The reference colour of a well-mixed cell's state through the plugin's
/// own species mapping (the GLSL's, written again here for the dye family,
/// the chameleon and the iodine reactions; a disagreement is the point).
void referenceColour( Reaction r, const chem::Params& p, const chem::Recipe& recipe, const double* a, const double* b, double depthCm, double rgb[ 3 ] )
{
	std::vector< spectra::Species > sp;
	std::vector< double > conc;
	auto add = [ & ]( spectra::Species s, double c ) {
		if( c > 0.0 )
		{
			sp.push_back( s );
			conc.push_back( c );
		}
	};
	switch( r )
	{
	case Reaction::IodineClock:
	case Reaction::BriggsRauscher:
	{
		const double T = r == Reaction::IodineClock ? a[ 2 ] : a[ 0 ], F = a[ 1 ];
		double I2, I3, freeI;
		chem::IodineSpeciation( T, F, chem::kClock.KI3, I2, I3, freeI );
		add( spectra::S_I2, I2 );
		add( spectra::S_I3, I3 );
		add( spectra::S_STARCH_I3, chem::StarchBound( I3, recipe.indicator, chem::kClock.KStarch ) );
		break;
	}
	case Reaction::TrafficLight:
	{
		const double pH = 14.0 + std::log10( std::max( static_cast< double >( p[ chem::P_DY_OH ] ), 1e-14 ) );
		const double yellow = 1.0 / ( 1.0 + std::pow( 10.0, p[ chem::P_DY_PKA ] - pH ) );
		add( spectra::S_IC_BLUE, a[ 2 ] * ( 1.0 - yellow ) );
		add( spectra::S_IC_YELLOW, a[ 2 ] * yellow );
		add( spectra::S_IC_SEMI, a[ 3 ] );
		add( spectra::S_IC_LEUCO, b[ 0 ] );
		break;
	}
	case Reaction::BlueBottle:
	{
		const double K = 2000.0, ox = std::max( a[ 2 ], 0.0 );
		const double mono = ( -1.0 + std::sqrt( 1.0 + 8.0 * K * ox ) ) / ( 4.0 * K );
		add( spectra::S_MB, mono );
		add( spectra::S_MB_DIMER, 0.5 * ( ox - mono ) );
		break;
	}
	case Reaction::Valentine:
		add( spectra::S_RESAZURIN, b[ 2 ] );
		add( spectra::S_RESORUFIN, a[ 2 ] );
		break;
	case Reaction::Chameleon:
		add( spectra::S_MNO4, a[ 0 ] );
		add( spectra::S_MNO4_2, a[ 1 ] );
		add( spectra::S_MNO2, a[ 2 ] );
		break;
	default: break;
	}
	spectra::LayerColour( Lightbox::D65, sp, conc, depthCm, rgb );
}

/// A well-mixed reference run: the colour sequence of a reaction from its
/// fresh state (plus an optional dose into channel `doseChannel` of A) over
/// `seconds`, sampled every `step`, through the reference colour.
struct RefTrace
{
	std::vector< double > t;
	std::vector< Colour > colour;
	std::vector< std::array< double, 8 > > state;
};
RefTrace referenceTrace( Reaction r, const chem::Params& p, const chem::Recipe& recipe, double depthCm, double seconds, double step,
                         int doseChannel = -1, double dose = 0.0, bool valentineAerated = false )
{
	RefTrace out;
	double a[ 4 ], b[ 4 ];
	chem::FreshState( r, p, a, b );
	if( doseChannel >= 0 )
		a[ doseChannel ] = doseChannel == 0 && IsDyeFamily( r ) ? std::max( a[ 0 ], dose ) : a[ doseChannel ] + dose;
	if( valentineAerated )
		a[ 0 ] = p[ chem::P_DY_O2SAT ];
	double y[ 8 ] = { a[ 0 ], a[ 1 ], a[ 2 ], a[ 3 ], b[ 0 ], b[ 1 ], b[ 2 ], b[ 3 ] };
	const double aer = 1.0;
	auto record = [ & ]( double t, const double* s ) {
		double rgb[ 3 ];
		referenceColour( r, p, recipe, s, s + 4, depthCm, rgb );
		out.t.push_back( t );
		out.colour.push_back( classify( rgb ) );
		std::array< double, 8 > st;
		for( int i = 0; i < 8; ++i )
			st[ static_cast< size_t >( i ) ] = s[ i ];
		out.state.push_back( st );
	};
	record( 0.0, y );
	double next = step;
	chem::IntegrateStiff( 8, [ & ]( const double* s, double* d ) { chem::WellMixedRhs( r, p, 0.0, aer, s, s + 4, d, d + 4 ); }, y, seconds, 1e-8, 1e-15,
	                      [ & ]( double t, const double* s ) {
		                      if( t >= next - 1e-9 )
		                      {
			                      record( t, s );
			                      next += step;
		                      }
	                      }, step );
	return out;
}

/// The run-length sequence of colours, repeats collapsed, as text.
std::string sequence( const std::vector< Colour >& colours, std::vector< Colour >* distinct = nullptr )
{
	std::string s;
	Colour last = Colour::Dark;
	bool first  = true;
	for( Colour c : colours )
	{
		if( first || c != last )
		{
			s += std::string( first ? "" : " > " ) + colourName( c );
			if( distinct )
				distinct->push_back( c );
		}
		last  = c;
		first = false;
	}
	return s;
}

/// The first time a trace shows `c`, after `from`; -1 if never.
double firstTime( const RefTrace& tr, Colour c, double from = 0.0 )
{
	for( size_t i = 0; i < tr.t.size(); ++i )
		if( tr.t[ i ] >= from && tr.colour[ i ] == c )
			return tr.t[ i ];
	return -1.0;
}
/// The first time a trace shows either colour, after `from`; -1 if never.
double firstOfAny( const RefTrace& tr, Colour c1, Colour c2, double from = 0.0 )
{
	const double a = firstTime( tr, c1, from ), b = firstTime( tr, c2, from );
	if( a < 0.0 )
		return b;
	if( b < 0.0 )
		return a;
	return std::min( a, b );
}
/// The first time after `from` at which A[channel] of a trace's state crosses
/// `level` downwards, having been above it; -1 if never.
double firstFall( const RefTrace& tr, int channel, double level, double from = 0.0 )
{
	bool above = false;
	for( size_t i = 0; i < tr.t.size(); ++i )
	{
		if( tr.t[ i ] < from )
			continue;
		const double v = tr.state[ i ][ static_cast< size_t >( channel ) ];
		if( v > level )
			above = true;
		else if( above )
			return tr.t[ i ];
	}
	return -1.0;
}

//===========================================================================
// --clock: the switch time read from pixels against the Harcourt-Esson
// closed form at three recipes.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: the iodine clock's switch, read from pixels, lands on the Harcourt-Esson closed form at three recipes, in Batch and in Flow\n" );
	const double frameChem = 0.25;
	for( const Raster& raster : kRasters )
		for( int reactor = 0; reactor < 2; ++reactor )
	{
		int wrong = 0;
		std::string what;
		for( double oxidant : { 0.5, 1.0, 2.0 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::IodineClock, 32, 18 );
			rig.Set( PT_REACTOR, static_cast< float >( reactor ) );
			rig.Set( PT_OXIDANT, ParamFromRecipeMultiplier( oxidant ) );
			if( perturb.clockWrongAcid )
				rig.plugin.SetParamOverrideForTest( chem::P_CK_KP, static_cast< float >( chem::kClock.k1 ) );
			rig.Render( 1 );
			const chem::Recipe recipe = rig.plugin.CurrentRecipe();
			const double k0 = rig.plugin.CurrentParams()[ chem::P_FLOW_K0 ];
			const double H0 = recipe.oxidant, I0 = chem::kClock.iodide, H = recipe.acidBase, S0 = recipe.reductant;
			const double tStar = chem::ClockSwitchTime( H0, H0, I0, H, S0, k0 );
			//The iodine that dims the red channel by 10% (the starch complex absorbs at 620 nm): the colour's own lag.
			double white[ 3 ];
			spectra::LayerColour( Lightbox::D65, {}, {}, 0.15, white );
			double tLag = 0.0;
			{
				double lo = 0.0, hi = 1e-3;
				for( int i = 0; i < 40; ++i )
				{
					const double mid = 0.5 * ( lo + hi );
					double I2, I3, freeI, rgb[ 3 ];
					chem::IodineSpeciation( mid, I0, chem::kClock.KI3, I2, I3, freeI );
					spectra::LayerColour( Lightbox::D65, { spectra::S_I2, spectra::S_I3, spectra::S_STARCH_I3 }, { I2, I3, chem::StarchBound( I3, recipe.indicator, chem::kClock.KStarch ) }, 0.15, rgb );
					if( rgb[ 0 ] < 0.9 * white[ 0 ] )
						hi = mid;
					else
						lo = mid;
				}
				tLag = hi / ( chem::ClockRateConstant( H ) * H0 * I0 );
			}
			double rgb0[ 3 ];
			centreRGB( rig, rgb0 );
			double tPixel = -1.0, tState = -1.0;
			for( int f = 1; f <= 2000 && ( tPixel < 0.0 || tState < 0.0 ); ++f )
			{
				rig.Chem( frameChem );
				double a[ 4 ], b[ 4 ];
				rig.plugin.MeanState( a, b );
				if( tState < 0.0 && a[ 3 ] <= 0.0 )
					tState = f * frameChem;
				double rgb[ 3 ];
				centreRGB( rig, rgb );
				if( tPixel < 0.0 && rgb[ 0 ] < 0.9 * rgb0[ 0 ] )
					tPixel = f * frameChem;
			}
			//One frame, the colour's lag, and the explicit step's drift of the
			//peroxide (dt k' I0 of itself per step over the run).
			const double bound = frameChem + tLag + frameChem * ( chem::ClockRateConstant( H ) * I0 + k0 ) * tStar;
			const bool ok      = tPixel > 0.0 && std::fabs( tPixel - tStar ) <= bound;
			wrong += !ok;
			what += fmt( " %gx H2O2: pixels %.2f s, state %.2f, closed form %.2f (bound %.2f)%s;", oxidant, tPixel, tState, tStar, bound, ok ? "" : " OUT" );
		}
		Check( wrong == 0, fmt( "%dx%d %s:%s %d wrong", raster.w, raster.h, reactor == 0 ? "Batch" : "Flow ", what.c_str(), wrong ) );
	}
	return Verdict();
}

//===========================================================================
// --sync: Clock Sync lands the change on the bar at 120 BPM and an odd tempo:
// the clock's snap, the blue bottle's fade, the chameleon's green peak.
//===========================================================================
int runSync( const Perturb& perturb )
{
	std::printf( "\n=== sync: Bar sync lands the clock's snap, the blue bottle's fade and the chameleon's green on the bar at 120 BPM and at 97 BPM\n" );
	for( const Raster& raster : kRasters )
	{
		for( double bpm : { 120.0, 97.0 } )
		{
			const double bar = 240.0 / bpm;
			//The iodine clock, in both reactors.
			for( int reactor = 0; reactor < 2; ++reactor )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::IodineClock, 32, 18 );
				rig.plugin.SetUncappedForTest( false );
				rig.plugin.SetWrongDoseForTest( perturb.syncWrongDose );
				rig.Set( PT_CLOCK_SYNC, static_cast< float >( ClockSync::Bar ) );
				rig.Set( PT_TIMELAPSE, ParamFromTimelapse( 30.0 ) );
				rig.Set( PT_REACTOR, static_cast< float >( reactor ) );
				rig.bpm = bpm;
				rig.Render( 1 );
				double rgb0[ 3 ];
				centreRGB( rig, rgb0 );
				std::vector< double > snaps;
				bool dark = false;
				for( int f = 0; f < 60 * 14; ++f )
				{
					rig.Render( 1 );
					double rgb[ 3 ];
					centreRGB( rig, rgb );
					const bool now = rgb[ 0 ] < 0.9 * rgb0[ 0 ];
					if( now && !dark )
						snaps.push_back( rig.TimeOf( rig.frame - 1 ) );
					dark = now;
				}
				//The first snap is the fresh dish's own; every later one is a dose's.
				double worst = 0.0;
				int counted  = 0;
				std::string what;
				for( size_t i = 1; i < snaps.size(); ++i )
				{
					const double off = std::fabs( snaps[ i ] / bar - std::round( snaps[ i ] / bar ) ) * bar;
					worst            = std::max( worst, off );
					++counted;
					what += fmt( " %.3f", snaps[ i ] );
				}
				//A frame (the dose lands on a frame; the snap is read on a frame)
				//plus the colour's lag at 30x (a tenth of a chemical second).
				const double bound = 2.0 / 60.0 + 0.1 / 30.0;
				Check( counted >= 3 && worst <= bound, fmt( "%dx%d  clock (%s) at %g BPM (bar %.3f s): %d synced snaps at%s s, worst %.3f s off a bar (bound %.3f); %llu doses",
				                                            raster.w, raster.h, reactor == 0 ? "Batch" : "Flow", bpm, bar, counted, what.c_str(), worst, bound, rig.plugin.DosesMade() ) );
			}
			//The blue bottle: 7 mm, 100x, the fade (half the dye reduced) on the bar.
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::BlueBottle, 32, 18 );
				rig.plugin.SetUncappedForTest( false );
				rig.plugin.SetWrongDoseForTest( perturb.syncWrongDose );
				rig.Set( PT_CLOCK_SYNC, static_cast< float >( ClockSync::Bar ) );
				rig.Set( PT_TIMELAPSE, ParamFromTimelapse( 100.0 ) );
				rig.Set( PT_DEPTH, ParamFromDepth( 7.0 ) );
				rig.Set( PT_REACTOR, static_cast< float >( Reactor::Flow ) );
				rig.bpm = bpm;
				rig.Render( 1 );
				const chem::Params p = rig.plugin.CurrentParams();
				std::vector< double > fades;
				bool blue = true;
				for( int f = 0; f < 60 * 40; ++f )
				{
					rig.Render( 1 );
					double a[ 4 ], b[ 4 ];
					rig.plugin.MeanState( a, b );
					const bool now = a[ 2 ] > 0.5 * p[ chem::P_DY_CTOT ];
					if( !now && blue )
						fades.push_back( rig.TimeOf( rig.frame - 1 ) );
					blue = now;
				}
				double worst = 0.0;
				int counted  = 0;
				std::string what;
				for( size_t i = 1; i < fades.size(); ++i )
				{
					const double off = std::fabs( fades[ i ] / bar - std::round( fades[ i ] / bar ) ) * bar;
					worst            = std::max( worst, off );
					++counted;
					what += fmt( " %.3f", fades[ i ] );
				}
				//A frame each side, plus the fade's gentleness: the half-point of
				//an exponential sampled a frame apart.
				const double bound = 3.0 / 60.0;
				Check( counted >= 2 && worst <= bound, fmt( "%dx%d  blue bottle at %g BPM: %d synced fades at%s s, worst %.3f s off a bar (bound %.3f); %llu shakes",
				                                            raster.w, raster.h, bpm, counted, what.c_str(), worst, bound, rig.plugin.DosesMade() ) );
			}
			//The chameleon: 30x, the manganate peak on the bar.
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::Chameleon, 32, 18 );
				rig.plugin.SetUncappedForTest( false );
				rig.plugin.SetWrongDoseForTest( perturb.syncWrongDose );
				rig.Set( PT_CLOCK_SYNC, static_cast< float >( ClockSync::Bar ) );
				rig.Set( PT_TIMELAPSE, ParamFromTimelapse( 30.0 ) );
				rig.Set( PT_REACTOR, static_cast< float >( Reactor::Flow ) );
				rig.bpm = bpm;
				rig.Render( 1 );
				std::vector< double > peaks;
				double lastM = 0.0, lastDm = 0.0;
				for( int f = 0; f < 60 * 40; ++f )
				{
					rig.Render( 1 );
					double a[ 4 ], b[ 4 ];
					rig.plugin.MeanState( a, b );
					const double dm = a[ 1 ] - lastM;
					if( lastDm > 0.0 && dm <= 0.0 && a[ 1 ] > 1e-4 )
						peaks.push_back( rig.TimeOf( rig.frame - 1 ) );
					lastDm = dm;
					lastM  = a[ 1 ];
				}
				double worst = 0.0;
				int counted  = 0;
				std::string what;
				for( size_t i = 1; i < peaks.size(); ++i )
				{
					const double off = std::fabs( peaks[ i ] / bar - std::round( peaks[ i ] / bar ) ) * bar;
					worst            = std::max( worst, off );
					++counted;
					what += fmt( " %.3f", peaks[ i ] );
				}
				//The peak is flat: a frame of sampling either side of it, plus the
				//half-frame the timing aims at.
				const double bound = 3.0 / 60.0;
				Check( counted >= 2 && worst <= bound, fmt( "%dx%d  chameleon at %g BPM: %d synced green peaks at%s s, worst %.3f s off a bar (bound %.3f); %llu doses",
				                                            raster.w, raster.h, bpm, counted, what.c_str(), worst, bound, rig.plugin.DosesMade() ) );
			}
		}
	}
	return Verdict();
}

//===========================================================================
// --briggs: the stirred BR's period against the mechanism in double, the
// colour order from pixels, Batch running down, Flow carrying on.
//===========================================================================
int runBriggs( const Perturb& perturb )
{
	std::printf( "\n=== briggs: the stirred Briggs-Rauscher's period against De Kepper-Epstein in double; colourless > amber > blue-black from pixels at 7 mm; Batch runs down, Flow goes on\n" );
	const chem::Recipe recipe = chem::BaseRecipe( Reaction::BriggsRauscher );
	//The reference, both reactors, with the same perturbation.
	auto reference = [ & ]( bool flow ) {
		double y[ chem::kBRSpecies ], H, H2O2, feed[ chem::kBRSpecies ];
		chem::BRInitialState( recipe, y, H, H2O2 );
		for( int i = 0; i < chem::kBRSpecies; ++i )
			feed[ i ] = y[ i ];
		chem::BRKinetics k = chem::kBR;
		if( perturb.briggsRate )
			k.r9 *= 0.5;
		std::vector< double > peaks;
		double last = 0.0, lastD = 0.0;
		chem::IntegrateStiff( chem::kBRSpecies, [ & ]( const double* s, double* d ) { chem::BRRhs( k, H, flow ? k.k0CSTR : 0.0, flow ? feed : nullptr, s, d ); }, y, 3000.0, 1e-8, 1e-16,
		                      [ & ]( double t, const double* s ) {
			                      const double d = s[ chem::BR_I2 ] - last;
			                      if( lastD > 0.0 && d <= 0.0 && s[ chem::BR_I2 ] > 3e-4 )
				                      peaks.push_back( t );
			                      lastD = d;
			                      last  = s[ chem::BR_I2 ];
		                      }, 0.05 );
		return peaks;
	};
	const std::vector< double > refBatch = reference( false ), refFlow = reference( true );
	auto meanPeriod = [ & ]( const std::vector< double >& peaks ) {
		return peaks.size() >= 3 ? ( peaks.back() - peaks[ 1 ] ) / static_cast< double >( peaks.size() - 2 ) : 0.0;
	};
	//The mechanism's own colour sequence at 7 mm through the reference colour.
	std::string refSeq;
	{
		double y[ chem::kBRSpecies ], H, H2O2;
		chem::BRInitialState( recipe, y, H, H2O2 );
		std::vector< Colour > colours;
		double next = 0.0;
		chem::IntegrateStiff( chem::kBRSpecies, [ & ]( const double* s, double* d ) { chem::BRRhs( chem::kBR, H, 0.0, nullptr, s, d ); }, y, 400.0, 1e-8, 1e-16,
		                      [ & ]( double t, const double* s ) {
			                      if( t < next )
				                      return;
			                      next = t + 1.0;
			                      const double a[ 4 ] = { s[ chem::BR_I2 ], s[ chem::BR_I ], 0.0, 0.0 }, b[ 4 ] = { 0, 0, 0, 0 };
			                      double rgb[ 3 ];
			                      referenceColour( Reaction::BriggsRauscher, chem::Params {}, recipe, a, b, 0.7, rgb );
			                      colours.push_back( classify( rgb ) );
		                      }, 0.05 );
		refSeq = sequence( colours );
	}
	Note( fmt( "the mechanism in double: Batch %zu iodine peaks in 3000 s (period %.2f s, the last at %.0f s); Flow (residence %.0f s) %zu peaks (period %.2f s, the last at %.0f s); its colour at 7 mm over the first 400 s: %s",
	           refBatch.size(), meanPeriod( refBatch ), refBatch.empty() ? 0.0 : refBatch.back(), 1.0 / chem::kBR.k0CSTR, refFlow.size(), meanPeriod( refFlow ), refFlow.empty() ? 0.0 : refFlow.back(), refSeq.c_str() ) );
	for( const Raster& raster : kRasters )
	{
		for( bool flow : { false, true } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::BriggsRauscher, 32, 18 );
			rig.Set( PT_STIR, 1.0f );
			rig.Set( PT_DEPTH, ParamFromDepth( 7.0 ) );
			rig.Set( PT_REACTOR, static_cast< float >( flow ? Reactor::Flow : Reactor::Batch ) );
			rig.Render( 1 );
			rig.plugin.BRForTest().SetRateScaleForTest( perturb.briggsRate ? 0.5 : 1.0 );
			std::vector< double > peaks;
			std::vector< Colour > colours;
			double last = 0.0, lastD = 0.0;
			const double frameChem = 1.0;
			for( int f = 1; f <= 3000; ++f )
			{
				rig.Chem( frameChem );
				const double I2 = rig.plugin.BR().MeanOf( chem::BR_I2 );
				const double d  = I2 - last;
				if( lastD > 0.0 && d <= 0.0 && I2 > 3e-4 )
					peaks.push_back( f * frameChem );
				lastD = d;
				last  = I2;
				double rgb[ 3 ];
				centreRGB( rig, rgb );
				colours.push_back( classify( rgb ) );
			}
			const std::vector< double >& ref = flow ? refFlow : refBatch;
			const double period = meanPeriod( peaks ), refP = meanPeriod( ref );
			//1% plus the frame's sampling of each peak.
			const double bound = 0.01 * refP + 2.0 * frameChem / std::max( 1.0, static_cast< double >( peaks.size() - 2 ) );
			std::vector< Colour > distinct;
			const std::string seq = sequence( colours, &distinct );
			//The order within each cycle (colourless to colourless): amber comes
			//before the blue-black, never after it. Green and yellow readings
			//between are the mixtures passing (a blue complex over yellow iodine).
			int cycles = 0, wrongOrder = 0;
			{
				size_t i = 0;
				while( i < distinct.size() )
				{
					if( distinct[ i ] != Colour::Colourless )
					{
						++i;
						continue;
					}
					size_t j = i + 1;
					int firstAmber = -1, firstDark = -1, k = 0;
					for( ; j < distinct.size() && distinct[ j ] != Colour::Colourless; ++j, ++k )
					{
						if( distinct[ j ] == Colour::Amber && firstAmber < 0 )
							firstAmber = k;
						if( ( distinct[ j ] == Colour::Dark || distinct[ j ] == Colour::Blue ) && firstDark < 0 )
							firstDark = k;
					}
					if( firstAmber >= 0 && firstDark >= 0 )
					{
						if( firstAmber < firstDark )
							++cycles;
						else
							++wrongOrder;
					}
					i = j;
				}
			}
			bool ok;
			if( !flow )
				//Batch runs down: its last peak (before 3000 s) is where the mechanism puts it.
				ok = peaks.size() >= 4 && std::fabs( period - refP ) <= bound && ref.back() < 2800.0 && std::fabs( peaks.back() - ref.back() ) <= 2.0 * frameChem + 0.01 * ref.back() && cycles >= 3 && wrongOrder == 0;
			else
				ok = peaks.size() >= 10 && std::fabs( period - refP ) <= bound && peaks.back() > 2800.0 && cycles >= 3 && wrongOrder == 0;
			Check( ok, fmt( "%dx%d  %s: %zu peaks, period %.2f s against %.2f in double (bound %.2f), last peak %.0f s (reference %.0f); from pixels %d cycles colourless > amber > blue-black, %d out of order (%s)",
			                raster.w, raster.h, flow ? "Flow " : "Batch", peaks.size(), period, refP, bound, peaks.empty() ? 0.0 : peaks.back(), ref.empty() ? 0.0 : ref.back(),
			                cycles, wrongOrder, seq.size() > 160 ? ( seq.substr( 0, 160 ) + " ..." ).c_str() : seq.c_str() ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --traffic: the traffic light's green > red > yellow after a Shake and back,
// each phase's duration against the double reference, the rate law's trends,
// Batch lengthening, the base moving the oxidised colour.
//===========================================================================
struct DyeRun
{
	std::string sequence;
	std::vector< Colour > distinct;
	double tRed = -1.0, tYellow = -1.0, tColourless = -1.0, tFade = -1.0;
	double tRedHalf = -1.0, tYellowHalf = -1.0, tColourlessHalf = -1.0, tFadeHalf = -1.0;
	int cycles = 0, fades = 0;
	double hueAtFade = 0.0, valAtFade = 0.0;
};

/// A dosed dye dish (air-saturated at t = 0 by the harness's dose, or by the
/// Shake button when `realShake`) watched from pixels and state for
/// `seconds` (chemical), with its substep halved on a second run for the
/// step's own error. The fade is read from state: the oxidised dye crossing
/// half its total downwards.
DyeRun dyeRun( const Raster& raster, Reaction r, double depthMm, double oxidant, double acidBase, double reductant, bool flow, double seconds,
               const Perturb& perturb, int shakes = 1, double between = 0.0, bool realShake = false )
{
	DyeRun run;
	for( int pass = 0; pass < 2; ++pass )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return run;
		prepare( rig, r, 32, 18 );
		rig.Set( PT_DEPTH, ParamFromDepth( depthMm ) );
		rig.Set( PT_OXIDANT, ParamFromRecipeMultiplier( oxidant ) );
		rig.Set( PT_ACID_BASE, ParamFromRecipeMultiplier( acidBase ) );
		rig.Set( PT_REDUCTANT, ParamFromRecipeMultiplier( reductant ) );
		rig.Set( PT_REACTOR, static_cast< float >( flow ? Reactor::Flow : Reactor::Batch ) );
		if( perturb.trafficNoAir )
			rig.plugin.SetParamOverrideForTest( chem::P_DY_KOX, 0.0f );
		if( perturb.bottleDouble )
			rig.plugin.SetParamOverrideForTest( chem::P_DY_K2, static_cast< float >( 2.0 * chem::kDye.k2 ) );
		rig.plugin.SetSubstepScaleForTest( pass == 0 ? 1.0 : 0.5 );
		rig.Render( 1 );
		const chem::Params p = rig.plugin.CurrentParams();
		const double ctot    = p[ chem::P_DY_CTOT ];
		std::vector< Colour > colours;
		const double frameChem = 0.5;
		double tRed = -1.0, tYellow = -1.0, tColourless = -1.0, tFade = -1.0, lastRed = -1.0;
		int shakesDone = 0, fades = 0;
		double nextShake = 0.0;
		bool wasHigh = false;
		for( int f = 0; f * frameChem < seconds; ++f )
		{
			if( shakesDone < shakes && f * frameChem >= nextShake )
			{
				if( realShake )
					rig.Set( PT_SHAKE, 1.0f );
				else
					rig.plugin.DoseForTest( p[ chem::P_DY_O2SAT ] );
				++shakesDone;
				nextShake += between;
			}
			rig.Chem( frameChem );
			rig.Set( PT_SHAKE, 0.0f );
			double rgb[ 3 ], a[ 4 ], b[ 4 ];
			centreRGB( rig, rgb );
			rig.plugin.MeanState( a, b );
			const Colour c = classify( rgb );
			colours.push_back( c );
			const double t = ( f + 1 ) * frameChem;
			//The traffic light's "red" is the semiquinone over the yellow leuco
			//form: red to orange-red, so the red and amber bands both count;
			//its yellow is the yellow band alone, AFTER the red (a continuous
			//colour passes from green to red through the yellows, so a yellow
			//reading before the red is the crossing, not the phase).
			if( tRed < 0.0 && ( c == Colour::Red || c == Colour::Amber ) )
				tRed = t;
			if( tRed > 0.0 && ( c == Colour::Red || c == Colour::Amber ) )
				lastRed = t;
			if( tYellow < 0.0 && c == Colour::Yellow && tRed > 0.0 && t > lastRed + 10.0 )
				tYellow = t;
			if( tColourless < 0.0 && c == Colour::Colourless && f > 2 )
				tColourless = t;
			const double ox = a[ 2 ];
			if( ox > 0.6 * ctot )
				wasHigh = true;
			else if( wasHigh && ox < 0.5 * ctot )
			{
				wasHigh = false;
				++fades;
				if( tFade < 0.0 )
				{
					tFade = t;
					double h, s, v;
					spectra::RGBToHSV( rgb, h, s, v );
					run.hueAtFade = h;
					run.valAtFade = v;
				}
			}
		}
		if( pass == 0 )
		{
			run.sequence    = sequence( colours, &run.distinct );
			run.tRed        = tRed;
			run.tYellow     = tYellow;
			run.tColourless = tColourless;
			run.tFade       = tFade;
			run.fades       = fades;
			//A cycle: a green reading, then a red one, then a yellow one, in that
			//order, whatever passes between (the crossings).
			{
				int stage = 0;
				for( Colour c : run.distinct )
				{
					if( stage == 0 && c == Colour::Green )
						stage = 1;
					else if( stage == 1 && ( c == Colour::Red || c == Colour::Amber ) )
						stage = 2;
					else if( stage == 2 && c == Colour::Yellow )
					{
						++run.cycles;
						stage = 0;
					}
				}
			}
		}
		else
		{
			run.tRedHalf        = tRed;
			run.tYellowHalf     = tYellow;
			run.tColourlessHalf = tColourless;
			run.tFadeHalf       = tFade;
		}
	}
	return run;
}

int runTraffic( const Perturb& perturb )
{
	std::printf( "\n=== traffic: green > red > yellow after a Shake, back to green on the next; phase times against the double reference; the rate law's trends; Batch lengthens; the base moves the oxidised colour\n" );
	const double depth = 7.0;
	for( const Raster& raster : kRasters )
	{
		//The reference trace at the 1x recipe (and the perturbed one, so the
		//negative control fails for the right reason: the plugin disagrees).
		Rig probe;
		if( !probe.Init( 64, 36 ) )
			return 1;
		prepare( probe, Reaction::TrafficLight, 32, 18 );
		probe.Set( PT_DEPTH, ParamFromDepth( depth ) );
		probe.Render( 1 );
		const chem::Params p0     = probe.plugin.CurrentParams();
		const chem::Recipe recipe = probe.plugin.CurrentRecipe();
		const RefTrace ref        = referenceTrace( Reaction::TrafficLight, p0, recipe, depth * 0.1, 1200.0, 0.5, 0, p0[ chem::P_DY_O2SAT ] );
		const double refRed = firstOfAny( ref, Colour::Red, Colour::Amber );
		double refYellow    = -1.0, refLastRed = refRed;
		for( size_t i = 0; i < ref.t.size(); ++i )
		{
			if( ref.t[ i ] < refRed )
				continue;
			if( ref.colour[ i ] == Colour::Red || ref.colour[ i ] == Colour::Amber )
				refLastRed = ref.t[ i ];
			if( refYellow < 0.0 && ref.colour[ i ] == Colour::Yellow && ref.t[ i ] > refLastRed + 10.0 )
				refYellow = ref.t[ i ];
		}
		//Two real Shakes for the order; an air-saturating dose for the timing
		//(a Shake's stirring burst keeps aerating the layer for a while, which
		//the well-mixed reference does not model).
		const DyeRun shaken = dyeRun( raster, Reaction::TrafficLight, depth, 1.0, 1.0, 1.0, false, 1800.0, perturb, 2, 900.0, true );
		const DyeRun one    = dyeRun( raster, Reaction::TrafficLight, depth, 1.0, 1.0, 1.0, false, 1200.0, perturb );
		//Each transition: a frame, plus the plugin's own step error (Richardson),
		//plus a percent of the time: the transitions are a slow hue drift across
		//a class boundary, and the float colour sits ~1e-4 from the double one.
		const double boundRed    = 0.5 + 2.0 * std::fabs( one.tRed - one.tRedHalf ) + 0.01 * refRed;
		const double boundYellow = 0.5 + 2.0 * std::fabs( one.tYellow - one.tYellowHalf ) + 0.01 * refYellow;
		Check( shaken.cycles >= 2 && one.tRed > 0.0 && one.tYellow > 0.0 && std::fabs( one.tRed - refRed ) <= boundRed && std::fabs( one.tYellow - refYellow ) <= boundYellow,
		       fmt( "%dx%d  two Shakes 900 s apart: %s; %d green > red > yellow cycles. Dosed with air (%s): red at %.1f s (reference %.1f, bound %.1f), yellow at %.1f s (reference %.1f, bound %.1f)",
		            raster.w, raster.h, shaken.sequence.c_str(), shaken.cycles, one.sequence.c_str(), one.tRed, refRed, boundRed, one.tYellow, refYellow, boundYellow ) );
		//The trends: more air lengthens the green, more glucose or base shortens it.
		const DyeRun moreAir = dyeRun( raster, Reaction::TrafficLight, depth, 2.0, 1.0, 1.0, false, 900.0, perturb );
		const DyeRun moreGL  = dyeRun( raster, Reaction::TrafficLight, depth, 1.0, 1.0, 2.0, false, 900.0, perturb );
		const DyeRun moreOH  = dyeRun( raster, Reaction::TrafficLight, depth, 1.0, 2.0, 1.0, false, 900.0, perturb );
		Check( moreAir.tRed > one.tRed + 0.5 && moreGL.tRed < one.tRed - 0.5 && moreOH.tRed < one.tRed - 0.5,
		       fmt( "%dx%d  the green lasts %.1f s; %.1f with twice the air, %.1f with twice the glucose, %.1f with twice the base",
		            raster.w, raster.h, one.tRed, moreAir.tRed, moreGL.tRed, moreOH.tRed ) );
		//Batch: with a quarter of the glucose each cycle spends a noticeable share of it.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::TrafficLight, 32, 18 );
			rig.Set( PT_DEPTH, ParamFromDepth( depth ) );
			rig.Set( PT_REDUCTANT, ParamFromRecipeMultiplier( 0.5 ) );
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Batch ) );
			rig.Render( 1 );
			std::vector< double > greens;
			double shakeAt = 0.0, redAt = -1.0;
			bool wasRed    = false;
			const double frameChem = 0.5;
			const double sat = rig.plugin.CurrentParams()[ chem::P_DY_O2SAT ];
			for( int f = 0; f * frameChem < 12000.0 && greens.size() < 4; ++f )
			{
				if( redAt < 0.0 && f * frameChem >= shakeAt )
				{
					rig.plugin.DoseForTest( sat );
					redAt = 0.0;
				}
				rig.Chem( frameChem );
				double rgb[ 3 ];
				centreRGB( rig, rgb );
				const Colour c    = classify( rgb );
				const bool redNow = c == Colour::Red || c == Colour::Amber;
				if( redNow && !wasRed && redAt == 0.0 )
				{
					greens.push_back( ( f + 1 ) * frameChem - shakeAt );
					redAt = ( f + 1 ) * frameChem;
				}
				wasRed = redNow;
				if( redAt > 0.0 && ( f + 1 ) * frameChem >= redAt + 400.0 )
				{
					shakeAt = ( f + 1 ) * frameChem;
					redAt   = -1.0;
				}
			}
			//The first green starts from the fresh, fully oxidised and saturated
			//dish; the cycles after it start from a reduced dish and a dose, and
			//those are compared.
			bool lengthening = greens.size() >= 4;
			std::string what;
			for( size_t i = 0; i < greens.size(); ++i )
			{
				what += fmt( " %.1f", greens[ i ] );
				if( i > 1 && greens[ i ] <= greens[ i - 1 ] )
					lengthening = false;
			}
			Check( lengthening, fmt( "%dx%d  Batch at half the glucose: the green lasts%s s on successive doses of air (the fresh dish's first, then each longer than the last)", raster.w, raster.h, what.c_str() ) );
		}
		//The oxidised colour against the base, from pixels and the pKa.
		{
			std::string what;
			double hues[ 3 ], want[ 3 ];
			int i = 0;
			for( double oh : { 0.25, 1.0, 4.0 } )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::TrafficLight, 32, 18 );
				rig.Set( PT_DEPTH, ParamFromDepth( depth ) );
				rig.Set( PT_ACID_BASE, ParamFromRecipeMultiplier( oh ) );
				rig.Render( 1 );
				rig.Chem( 0.0 );
				double rgb[ 3 ];
				centreRGB( rig, rgb );
				hues[ i ] = hueOf( rgb );
				double a[ 4 ], b[ 4 ], want3[ 3 ];
				rig.plugin.MeanState( a, b );
				referenceColour( Reaction::TrafficLight, rig.plugin.CurrentParams(), rig.plugin.CurrentRecipe(), a, b, depth * 0.1, want3 );
				want[ i ] = hueOf( want3 );
				what += fmt( " %gx: hue %.0f (reference %.0f, %s)", oh, hues[ i ], want[ i ], colourName( classify( rgb ) ) );
				++i;
			}
			Check( hues[ 0 ] > hues[ 1 ] && hues[ 1 ] > hues[ 2 ] && std::fabs( hues[ 0 ] - want[ 0 ] ) < 3.0 && std::fabs( hues[ 2 ] - want[ 2 ] ) < 3.0,
			       fmt( "%dx%d  the fresh oxidised dish's hue falls from blue towards yellow as the base rises:%s", raster.w, raster.h, what.c_str() ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --bluebottle: the blue bottle's blue after a Shake and its duration against
// the double reference, scaling with air; the valentine's blue never returns.
//===========================================================================
int runBlueBottle( const Perturb& perturb )
{
	std::printf( "\n=== bluebottle: colourless > blue on a Shake and back; the blue's duration against the double reference and with half the air; the valentine's blue never returns\n" );
	const double depth = 7.0;
	for( const Raster& raster : kRasters )
	{
		Rig probe;
		if( !probe.Init( 64, 36 ) )
			return 1;
		prepare( probe, Reaction::BlueBottle, 32, 18 );
		probe.Set( PT_DEPTH, ParamFromDepth( depth ) );
		probe.Render( 1 );
		const chem::Params p0     = probe.plugin.CurrentParams();
		const chem::Recipe recipe = probe.plugin.CurrentRecipe();
		const RefTrace ref        = referenceTrace( Reaction::BlueBottle, p0, recipe, depth * 0.1, 1500.0, 0.5, 0, p0[ chem::P_DY_O2SAT ] );
		const double ctot         = p0[ chem::P_DY_CTOT ];
		const double refFade      = firstFall( ref, 2, 0.5 * ctot, 1.0 );
		//The reference's colour at its fade, for the pixels at the plugin's.
		double refHue = 0.0, refVal = 0.0;
		for( size_t i = 0; i < ref.t.size(); ++i )
			if( ref.t[ i ] >= refFade )
			{
				double rgb[ 3 ];
				referenceColour( Reaction::BlueBottle, p0, recipe, ref.state[ i ].data(), ref.state[ i ].data() + 4, depth * 0.1, rgb );
				double h, s, v;
				spectra::RGBToHSV( rgb, h, s, v );
				refHue = h;
				refVal = v;
				break;
			}
		const DyeRun one     = dyeRun( raster, Reaction::BlueBottle, depth, 1.0, 1.0, 1.0, false, 1900.0, perturb, 2, 900.0 );
		const DyeRun halfAir = dyeRun( raster, Reaction::BlueBottle, depth, 0.5, 1.0, 1.0, false, 1900.0, perturb );
		//Two half-second samplings (the plugin's and the reference's crossing),
		//the plugin's measured step error (Richardson), and half a percent of
		//the time for the step's mixed orders (its oxygen terms are exact
		//exponentials, its reduction explicit).
		const double bound   = 1.0 + 2.0 * std::fabs( one.tFade - one.tFadeHalf ) + 0.005 * refFade;
		const bool blueFirst = !one.distinct.empty() && one.distinct[ 0 ] == Colour::Blue;
		Check( blueFirst && one.fades >= 2 && one.tFade > 0.0 && std::fabs( one.tFade - refFade ) <= bound && halfAir.tFade < one.tFade - 1.0 && std::fabs( one.hueAtFade - refHue ) < 3.0
		           && std::fabs( one.valAtFade - refVal ) < 0.02,
		       fmt( "%dx%d  %s; blue first (%s), %d fades (half the dye reduced, from state) on two doses of air; the first at %.1f s (reference %.1f, bound %.1f), %.1f s with half the air; the pixel at the fade: hue %.0f value %.2f (reference %.0f, %.2f)",
		            raster.w, raster.h, one.sequence.c_str(), blueFirst ? "yes" : "NO", one.fades, one.tFade, refFade, bound, halfAir.tFade, one.hueAtFade, one.valAtFade, refHue, refVal ) );
		//The valentine: shaken four times; blue once, then pink and colourless.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::Valentine, 32, 18 );
			rig.Set( PT_DEPTH, ParamFromDepth( depth ) );
			rig.Render( 1 );
			const chem::Params p = rig.plugin.CurrentParams();
			std::vector< Colour > colours;
			double resazurinAfterFirst = -1.0, maxLater = 0.0;
			int blueAfterFirst = 0, firstFadeFrame = -1, fades = 0;
			bool wasHigh           = false;
			const double frameChem = 0.5, ctot = p[ chem::P_DY_CTOT ];
			std::string timeline;
			for( int f = 0; f * frameChem < 4000.0; ++f )
			{
				if( f == 0 || f == 4000 )//two doses of air, 2000 s apart
					rig.plugin.DoseForTest( p[ chem::P_DY_O2SAT ] );
				rig.Chem( frameChem );
				double rgb[ 3 ], a[ 4 ], b[ 4 ];
				centreRGB( rig, rgb );
				rig.plugin.MeanState( a, b );
				const Colour c = classify( rgb );
				colours.push_back( c );
				if( f % 800 == 0 )
					timeline += fmt( " %.0fs: O2 %.1e rz %.2f ox %.2f leuco %.2f;", f * frameChem, a[ 0 ], b[ 2 ] / ctot, a[ 2 ] / ctot, b[ 0 ] / ctot );
				//A fade: the resorufin (the oxidised, pink form) crossing half the dye downwards.
				if( a[ 2 ] > 0.6 * ctot )
					wasHigh = true;
				else if( wasHigh && a[ 2 ] < 0.5 * ctot )
				{
					wasHigh = false;
					++fades;
					if( firstFadeFrame < 0 )
					{
						firstFadeFrame      = f;
						resazurinAfterFirst = b[ 2 ];
					}
				}
				if( firstFadeFrame >= 0 && f > firstFadeFrame )
				{
					maxLater = std::max( maxLater, b[ 2 ] );
					if( c == Colour::Blue )
						++blueAfterFirst;
				}
			}
			std::vector< Colour > distinct;
			const std::string seq = sequence( colours, &distinct );
			const bool blueFirst  = !distinct.empty() && distinct[ 0 ] == Colour::Blue;
			bool pinkSeen         = false;
			for( Colour c : distinct )
				pinkSeen = pinkSeen || c == Colour::Purple || c == Colour::Red;
			Note( fmt( "valentine timeline:%s", timeline.c_str() ) );
			Check( blueFirst && pinkSeen && fades >= 2 && resazurinAfterFirst <= 1e-3 * ctot && maxLater <= 1e-3 * ctot && blueAfterFirst == 0,
			       fmt( "%dx%d  valentine, two doses of air 2000 s apart: %s; blue first (%s), then pink (%s); %d fades (state); resazurin after the first %.1e of the dye and never above %.1e again (state); %d blue readings after it (pixels)",
			            raster.w, raster.h, seq.c_str(), blueFirst ? "yes" : "NO", pinkSeen ? "yes" : "NO", fades, resazurinAfterFirst / ctot, maxLater / ctot, blueAfterFirst ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --chameleon: purple > (blue) > green > yellow-brown from pixels, never
// reversed; transition times against the double reference; the base's
// effect; a drop's rings; MnO2 accumulating in Batch, washed out in Flow.
//===========================================================================
int runChameleon( const Perturb& perturb )
{
	std::printf( "\n=== chameleon: purple > green > yellow-brown from pixels, never reversed; transition times against the double reference; base; rings; MnO2 in Batch and Flow\n" );
	for( const Raster& raster : kRasters )
	{
		Rig probe;
		if( !probe.Init( 64, 36 ) )
			return 1;
		prepare( probe, Reaction::Chameleon, 32, 18 );
		probe.Render( 1 );
		chem::Params p0 = probe.plugin.CurrentParams();
		if( perturb.chameleonSwap )
			std::swap( p0[ chem::P_CH_KA ], p0[ chem::P_CH_KB ] );
		const chem::Recipe recipe = probe.plugin.CurrentRecipe();
		const RefTrace ref        = referenceTrace( Reaction::Chameleon, p0, recipe, 0.15, 400.0, 0.25 );
		const double refGreen = firstTime( ref, Colour::Green ), refBrown = firstOfAny( ref, Colour::Amber, Colour::Yellow, refGreen );
		auto run = [ & ]( double oh, double scale, std::string& seq, double& tGreen, double& tBrown, int& reversed ) {
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return false;
			prepare( rig, Reaction::Chameleon, 32, 18 );
			rig.Set( PT_ACID_BASE, ParamFromRecipeMultiplier( oh ) );
			if( perturb.chameleonSwap )
			{
				rig.plugin.SetParamOverrideForTest( chem::P_CH_KA, static_cast< float >( chem::kChameleon.kB ) );
				rig.plugin.SetParamOverrideForTest( chem::P_CH_KB, static_cast< float >( chem::kChameleon.kA ) );
			}
			rig.plugin.SetSubstepScaleForTest( scale );
			rig.Render( 1 );
			std::vector< Colour > colours;
			tGreen = tBrown = -1.0;
			const double frameChem = 0.25;
			for( int f = 0; f * frameChem < 400.0; ++f )
			{
				rig.Chem( frameChem );
				double rgb[ 3 ];
				centreRGB( rig, rgb );
				const Colour c = classify( rgb );
				colours.push_back( c );
				const double t = ( f + 1 ) * frameChem;
				if( tGreen < 0.0 && c == Colour::Green )
					tGreen = t;
				if( tBrown < 0.0 && tGreen > 0.0 && ( c == Colour::Amber || c == Colour::Yellow ) )
					tBrown = t;
			}
			std::vector< Colour > distinct;
			seq      = sequence( colours, &distinct );
			reversed = 0;
			auto rank = []( Colour c ) {
				switch( c )
				{
				case Colour::Purple: return 0;
				case Colour::Blue: return 1;
				case Colour::Colourless: return 2;
				case Colour::Green: return 3;
				case Colour::Yellow: return 4;
				case Colour::Amber: return 5;
				case Colour::Red: return 6;
				default: return 7;
				}
			};
			for( size_t i = 1; i < distinct.size(); ++i )
				if( rank( distinct[ i ] ) < rank( distinct[ i - 1 ] ) )
					++reversed;
			return true;
		};
		std::string seq, seqHalf, seq2;
		double tGreen, tBrown, tGreenHalf, tBrownHalf, tGreen2, tBrown2;
		int reversed, r2, r3;
		if( !run( 1.0, 1.0, seq, tGreen, tBrown, reversed ) || !run( 1.0, 0.5, seqHalf, tGreenHalf, tBrownHalf, r2 ) || !run( 2.0, 1.0, seq2, tGreen2, tBrown2, r3 ) )
			return 1;
		const double boundG = 0.25 + 2.0 * std::fabs( tGreen - tGreenHalf ), boundB = 0.25 + 2.0 * std::fabs( tBrown - tBrownHalf );
		Check( reversed == 0 && tGreen > 0.0 && tBrown > 0.0 && std::fabs( tGreen - refGreen ) <= boundG && std::fabs( tBrown - refBrown ) <= boundB && tGreen2 < 0.7 * tGreen,
		       fmt( "%dx%d  %s (%d reversals); green at %.2f s (reference %.2f, bound %.2f), yellow-brown at %.2f s (reference %.2f, bound %.2f); with twice the base green at %.2f s",
		            raster.w, raster.h, seq.c_str(), reversed, tGreen, refGreen, boundG, tBrown, refBrown, boundB, tGreen2 ) );
		//A drop in a still dish: it lands half mixed (a tenth of the glucose at
		//its centre, all of it at the rim), so the rim runs ahead and the
		//sequence shows as rings. Each radius's colour is the reference's at
		//its own effective age, g( r ) t, since the chain is first order in
		//the glucose it has.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::Chameleon, 128, 72 );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 60.0 ) );
			rig.Set( PT_INDICATOR, 0.0f );//no permanganate in the dish: only the drop's
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Batch ) );
			rig.Render( 1 );
			const chem::Params pDrop  = rig.plugin.CurrentParams();
			const chem::Recipe rDrop  = rig.plugin.CurrentRecipe();
			rig.plugin.DropForTest( 64.0, 36.0, 12.0 );
			const double t = 120.0;
			for( int f = 0; f < 240; ++f )
				rig.Chem( 0.5 );
			const Floats out = rig.Output();
			const Floats st  = rig.State();
			std::string what;
			int wrong = 0;
			Colour at[ 3 ], want[ 3 ];
			const double radii[ 3 ] = { 0.0, 7.0, 11.0 };
			for( int k = 0; k < 3; ++k )
			{
				const int px = static_cast< int >( ( 64.0 + radii[ k ] ) / 128.0 * raster.w ), py = raster.h / 2;
				double rgb[ 3 ];
				meanRGB( out, raster.w, raster.h, px - 1, py - 1, px + 2, py + 2, rgb );
				at[ k ]              = classify( rgb );
				const double g       = 0.05 + 0.95 * ( radii[ k ] / 12.0 ) * ( radii[ k ] / 12.0 );
				const RefTrace local = referenceTrace( Reaction::Chameleon, pDrop, rDrop, 0.15, g * t + 0.01, g * t, 0, rDrop.oxidant );
				want[ k ]            = local.colour.back();
				double h, s, v;
				spectra::RGBToHSV( rgb, h, s, v );
				const float* cell = &st[ ( static_cast< size_t >( 36 ) * 128 + 64 + static_cast< int >( radii[ k ] ) ) * 4 ];
				const auto& rs    = local.state.back();
				what += fmt( " r%.0f (age %.0f s): %s (hue %.0f sat %.2f val %.2f; P %.2e M %.2e D %.2e GL %.4f), reference %s (P %.2e M %.2e D %.2e);", radii[ k ], g * t, colourName( at[ k ] ), h, s, v,
				             cell[ 0 ], cell[ 1 ], cell[ 2 ], cell[ 3 ], colourName( want[ k ] ), rs[ 0 ], rs[ 1 ], rs[ 2 ] );
				wrong += at[ k ] != want[ k ];
			}
			const bool order = ( at[ 0 ] == Colour::Purple ) && at[ 1 ] == Colour::Green && ( at[ 2 ] == Colour::Amber || at[ 2 ] == Colour::Yellow );
			//The probe times were chosen so the reference itself runs purple >
			//green > yellow-brown along the radius: if it does not, the probe is
			//wrong, not the plugin.
			const bool probeSound = want[ 0 ] == Colour::Purple && want[ 1 ] == Colour::Green && ( want[ 2 ] == Colour::Amber || want[ 2 ] == Colour::Yellow );
			Check( wrong == 0 && order && probeSound, fmt( "%dx%d  a 5.6 mm drop in a still 60 mm dish, 120 s on, along a radius:%s purple > green > brown from the centre (%s; the probe's ages are sound: %s)",
			                                              raster.w, raster.h, what.c_str(), order ? "yes" : "NO", probeSound ? "yes" : "NO" ) );
		}
		//Batch accumulates the colloid dose by dose; Flow washes it out.
		{
			double after[ 2 ][ 4 ];
			for( int reactor = 0; reactor < 2; ++reactor )
			{
				Rig rig;
				if( !rig.Init( raster.w, raster.h ) )
					return 1;
				prepare( rig, Reaction::Chameleon, 32, 18 );
				rig.Set( PT_REACTOR, static_cast< float >( reactor ) );
				rig.Set( PT_INDICATOR, 0.0f );
				rig.Set( PT_DROP_POSITION, static_cast< float >( DropPosition::Centre ) );
				rig.Set( PT_DROP_SIZE, 1.0f );
				rig.Render( 1 );
				for( int dose = 0; dose < 4; ++dose )
				{
					rig.Set( PT_DROP, 1.0f );
					rig.Chem( 0.5 );
					rig.Set( PT_DROP, 0.0f );
					for( int f = 0; f < 1199; ++f )
						rig.Chem( 0.5 );
					double a[ 4 ], b[ 4 ];
					rig.plugin.MeanState( a, b );
					after[ reactor ][ dose ] = a[ 2 ];
				}
			}
			//Each dose adds its colloid to the dish's (a little less each time:
			//the drops land on the same spot and each thins the glucose there).
			const double d0 = after[ 0 ][ 0 ], d1 = after[ 0 ][ 1 ] - after[ 0 ][ 0 ], d2 = after[ 0 ][ 2 ] - after[ 0 ][ 1 ], d3 = after[ 0 ][ 3 ] - after[ 0 ][ 2 ];
			const bool batchUp = d0 > 0.0 && d1 > 0.5 * d0 && d2 > 0.5 * d0 && d3 > 0.5 * d0 && after[ 0 ][ 3 ] > 2.5 * d0;
			//Flow at a 300 s residence: 600 s between doses leaves e^-2 of each.
			const bool flowFlat = after[ 1 ][ 3 ] < 1.3 * after[ 1 ][ 0 ] && after[ 1 ][ 0 ] > 0.0;
			Check( batchUp && flowFlat, fmt( "%dx%d  MnO2 after each of four doses: Batch %.2e %.2e %.2e %.2e (accumulating); Flow %.2e %.2e %.2e %.2e (washed out)",
			                                 raster.w, raster.h, after[ 0 ][ 0 ], after[ 0 ][ 1 ], after[ 0 ][ 2 ], after[ 0 ][ 3 ], after[ 1 ][ 0 ], after[ 1 ][ 1 ], after[ 1 ][ 2 ], after[ 1 ][ 3 ] ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --stir: spatial variance decays faster with Stir; a stirred BZ dish
// oscillates in phase across the vessel, a still one does not.
//===========================================================================
int runStir( const Perturb& perturb )
{
	std::printf( "\n=== stir: a tracer patch's spatial variance decays faster with Stir; stirred BZ oscillates in phase across the vessel, still BZ does not\n" );
	for( const Raster& raster : kRasters )
	{
		//A patch of the chameleon's colloid (a tracer: it is neither made nor
		//consumed once the drop has run, and it barely diffuses), off centre
		//so the vortex carries it: the variance of the MnO2 field.
		double rates[ 3 ];
		int i = 0;
		for( double stir : { 0.0, 0.5, 1.0 } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::Chameleon, 128, 72 );
			rig.plugin.SetStirOffForTest( perturb.stirOff );
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Batch ) );
			rig.Set( PT_INDICATOR, 0.0f );
			rig.Set( PT_VESSEL, static_cast< float >( Vessel::PetriDish ) );
			rig.Render( 1 );
			rig.plugin.DropForTest( 48.0, 36.0, 8.0 );
			for( int f = 0; f < 600; ++f )
				rig.Chem( 0.5 );//300 s: the drop has run to colloid
			rig.Set( PT_STIR, static_cast< float >( stir ) );
			auto variance = [ & ]() {
				const Floats st = rig.State();
				const Grid g    = rig.plugin.CurrentGrid();
				double sum = 0.0, sum2 = 0.0;
				long n = 0;
				for( int y = 0; y < g.rows; ++y )
					for( int xx = 0; xx < g.cols; ++xx )
					{
						const double dx = xx + 0.5 - 0.5 * g.cols, dy = y + 0.5 - 0.5 * g.rows;
						if( dx * dx + dy * dy > 0.9 * ( 0.5 * g.rows - 1 ) * ( 0.5 * g.rows - 1 ) )
							continue;
						const double v = st[ ( static_cast< size_t >( y ) * g.cols + xx ) * 4 + 2 ];//MnO2
						sum += v;
						sum2 += v * v;
						++n;
					}
				return n ? sum2 / n - ( sum / n ) * ( sum / n ) : 0.0;
			};
			const double v0 = variance();
			for( int f = 0; f < 120; ++f )
				rig.Chem( 0.5 );//60 s of stirring
			const double v1 = variance();
			rates[ i++ ]    = v0 > 0.0 && v1 > 0.0 ? std::log( v0 / v1 ) / 60.0 : ( v0 > 0.0 ? 1.0 : 0.0 );
		}
		Check( rates[ 1 ] > 1.5 * rates[ 0 ] && rates[ 2 ] > 1.5 * rates[ 1 ],
		       fmt( "%dx%d  a colloid patch's variance decays at %.4f, %.4f, %.4f per s at Stir 0, 0.5, 1", raster.w, raster.h, rates[ 0 ], rates[ 1 ], rates[ 2 ] ) );
		//BZ: the spread of z across the dish over two periods, still and stirred.
		double spread[ 2 ];
		for( int s = 0; s < 2; ++s )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::BZ, 128, 72 );
			rig.plugin.SetStirOffForTest( perturb.stirOff );
			rig.Set( PT_STIR, s == 0 ? 0.0f : 1.0f );
			rig.Render( 1 );
			for( int f = 0; f < 300; ++f )
				rig.Chem( 1.0 );
			double worst = 0.0;
			for( int f = 0; f < 200; ++f )
			{
				rig.Chem( 1.0 );
				const Floats st = rig.State();
				double lo = 1e9, hi = -1e9;
				for( size_t k = 2; k < st.size(); k += 4 )
				{
					lo = std::min( lo, static_cast< double >( st[ k ] ) );
					hi = std::max( hi, static_cast< double >( st[ k ] ) );
				}
				worst = std::max( worst, hi - lo );
			}
			spread[ s ] = worst;
		}
		const double zRef = chem::BZPeakZ( chem::MakeBZ( chem::BaseRecipe( Reaction::BZ ) ) );
		Check( spread[ 0 ] > 0.5 * zRef && spread[ 1 ] < 0.1 * zRef,
		       fmt( "%dx%d  BZ over 200 s: the catalyst's spread across the dish is %.3f of its peak still and %.3f stirred", raster.w, raster.h, spread[ 0 ] / zRef, spread[ 1 ] / zRef ) );
	}
	return Verdict();
}

//===========================================================================
// --units: a wave's speed in mm/s does not depend on Dish Width, Detail or
// the raster.
//===========================================================================
int runUnits( const Perturb& perturb )
{
	std::printf( "\n=== units: a trigger wave's speed in mm/s depends on the cell's size in mm alone: not on Dish Width or Detail separately, not on the raster\n" );
	const double fWave = kFWave;
	struct Case
	{
		int cols;
		double dishMm;
		const char* what;
	};
	//Group A: 0.1 mm cells three ways; group B: 0.2 mm cells two ways.
	const Case cases[] = { { 512, 51.2, "512 cells, 51.2 mm" }, { 1024, 102.4, "1024 cells, 102.4 mm" }, { 256, 51.2, "256 cells, 51.2 mm" }, { 512, 102.4, "512 cells, 102.4 mm" } };
	std::vector< double > a, b;
	std::string what;
	for( const Raster& raster : kRasters )
		for( int i = 0; i < 4; ++i )
		{
			const Case& c = cases[ i ];
			if( i == 1 && &raster != &kRasters.front() )
				continue;
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::BZ, c.cols, 4 );
			bzHomogeneous( rig, fWave );
			rig.plugin.SetFixedCellForTest( perturb.unitsWrong );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( c.dishMm ) );
			rig.Render( 1 );
			const double cellMm = c.dishMm / c.cols;//the TRUE cell
			rig.plugin.DropForTest( 3.0, 2.0, 5.0 );
			std::vector< std::pair< double, double > > samples;
			const double seconds = 400.0 * c.dishMm / 51.2;
			for( double t = 0.0; t < seconds; t += 1.0 )
			{
				rig.Chem( 1.0 );
				const int front = frontOf( rig.State(), c.cols, 4, 0.3 );
				if( front >= 0 )
					samples.emplace_back( t + 1.0, front );
			}
			int count = 0;
			const double speed = fitSpeed( samples, 0.2 * c.cols, 0.7 * c.cols, cellMm, count );
			( i < 2 ? a : b ).push_back( speed );
			what += fmt( " %dx%d %s (%.1f mm cells): %.4f;", raster.w, raster.h, c.what, cellMm, speed );
		}
	auto spread = []( const std::vector< double >& v, double& lo, double& hi ) {
		lo = 1e9;
		hi = -1e9;
		for( double s : v )
		{
			lo = std::min( lo, s );
			hi = std::max( hi, s );
		}
	};
	double aLo, aHi, bLo, bHi;
	spread( a, aLo, aHi );
	spread( b, bLo, bHi );
	//Within a group the runs are the same cells and the same chemistry: the
	//raster never enters the state, and the dish's width enters only through
	//the cell, so they agree to the front-finding's cell over the interval.
	const double bound = 0.1 / ( 0.5 * 512 * 0.1 / 0.1 ) * 2.0 + 1e-6;
	Check( aLo > 0.0 && bLo > 0.0 && ( aHi - aLo ) <= bound && ( bHi - bLo ) <= bound,
	       fmt( "mm/s:%s 0.1 mm cells agree within %.5f and 0.2 mm cells within %.5f (bound %.5f)", what.c_str(), aHi - aLo, bHi - bLo, bound ) );
	//The cell's own effect: the front is thinner than a cell (sqrt( D eps T0 )
	//is ~0.02 mm at 1x), so the speed converges only as the cell shrinks. The
	//1-D double line at 0.2, 0.1, 0.05 and 0.025 mm says by how much, and the
	//plugin's 0.2 mm / 0.1 mm speed ratio is held to the line's own: a units
	//error (a cell that does not follow Dish Width or Detail) doubles it.
	{
		const chem::BZModel m = chem::MakeBZ( chem::BaseRecipe( Reaction::BZ ), fWave );
		std::string conv;
		double lineAt[ 2 ] = { 0.0, 0.0 };
		for( double dx : { 0.2, 0.1, 0.05, 0.025 } )
		{
			const double Dmax = std::max( { m.Du, chem::kOregonator.DY * chem::kCm2PerS_to_Mm2PerS, m.Dv } );
			BZLine line;
			const int n = static_cast< int >( 51.2 / dx );
			line.Init( m, n, dx, std::min( 0.2 * dx * dx / Dmax, 0.25 * m.eps * m.T0 ) );
			line.Fire( 0, static_cast< int >( 0.8 / dx ) );
			std::vector< std::pair< double, double > > samples;
			double t = 0.0;
			while( t < 400.0 )
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
			int count          = 0;
			const double speed = fitSpeed( samples, 0.2 * n, 0.7 * n, dx, count );
			if( dx == 0.2 )
				lineAt[ 0 ] = speed;
			if( dx == 0.1 )
				lineAt[ 1 ] = speed;
			conv += fmt( " %.3f mm: %.4f;", dx, speed );
		}
		const double ratioPlugin = 0.5 * ( bLo + bHi ) / ( 0.5 * ( aLo + aHi ) );
		const double ratioLine   = lineAt[ 1 ] > 0.0 ? lineAt[ 0 ] / lineAt[ 1 ] : 0.0;
		//A cell of front-finding over the interval, on each of the four speeds.
		const double ratioBound = 0.05;
		Check( std::fabs( ratioPlugin - ratioLine ) <= ratioBound,
		       fmt( "the cell's own effect, on the 1-D double line:%s the plugin's 0.2 mm cells run %.3fx its 0.1 mm cells and the line's %.3fx (bound %.2f on the ratio): the discretisation's, not a units error",
		            conv.c_str(), ratioPlugin, ratioLine, ratioBound ) );
	}
	return Verdict();
}

//===========================================================================
// --turing: with starch (sigma past the Hopf line) the CDIMA layer forms a
// stationary pattern whose dominant wavelength is the Lengyel-Epstein
// model's 2 pi / k_c, in mm, at two Details and two Dish Widths; without
// starch it stays spatially uniform (and oscillates); uniform light erases it.
//===========================================================================
void fft1( std::vector< std::complex< double > >& a )
{
	const size_t n = a.size();
	for( size_t i = 1, j = 0; i < n; ++i )
	{
		size_t bit = n >> 1;
		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
			std::swap( a[ i ], a[ j ] );
	}
	for( size_t len = 2; len <= n; len <<= 1 )
	{
		const double ang = -2.0 * kPi / static_cast< double >( len );
		const std::complex< double > wl( std::cos( ang ), std::sin( ang ) );
		for( size_t i = 0; i < n; i += len )
		{
			std::complex< double > w( 1.0, 0.0 );
			for( size_t j = 0; j < len / 2; ++j )
			{
				const std::complex< double > u = a[ i + j ], v = a[ i + j + len / 2 ] * w;
				a[ i + j ]           = u + v;
				a[ i + j + len / 2 ] = u - v;
				w *= wl;
			}
		}
	}
}

/// The radial power spectrum of a cols x rows field (both powers of two):
/// bin b holds the power at |k| in [b, b+1) cycles per the x extent.
std::vector< double > radialPower( const std::vector< double >& field, int cols, int rows )
{
	std::vector< std::complex< double > > f( static_cast< size_t >( cols ) * rows );
	double mean = 0.0;
	for( double v : field )
		mean += v / field.size();
	for( size_t i = 0; i < f.size(); ++i )
		f[ i ] = std::complex< double >( field[ i ] - mean, 0.0 );
	std::vector< std::complex< double > > line( cols );
	for( int y = 0; y < rows; ++y )
	{
		for( int xx = 0; xx < cols; ++xx )
			line[ xx ] = f[ static_cast< size_t >( y ) * cols + xx ];
		fft1( line );
		for( int xx = 0; xx < cols; ++xx )
			f[ static_cast< size_t >( y ) * cols + xx ] = line[ xx ];
	}
	std::vector< std::complex< double > > col( rows );
	for( int xx = 0; xx < cols; ++xx )
	{
		for( int y = 0; y < rows; ++y )
			col[ y ] = f[ static_cast< size_t >( y ) * cols + xx ];
		fft1( col );
		for( int y = 0; y < rows; ++y )
			f[ static_cast< size_t >( y ) * cols + xx ] = col[ y ];
	}
	std::vector< double > power( static_cast< size_t >( cols / 2 ) + 1, 0.0 );
	for( int y = 0; y < rows; ++y )
		for( int xx = 0; xx < cols; ++xx )
		{
			const double kx = xx <= cols / 2 ? xx : xx - cols;
			const double ky = ( y <= rows / 2 ? y : y - rows ) * static_cast< double >( cols ) / rows;//cycles per x extent
			const double kr = std::sqrt( kx * kx + ky * ky );
			const size_t b = static_cast< size_t >( kr );
			if( b < power.size() )
				power[ b ] += std::norm( f[ static_cast< size_t >( y ) * cols + xx ] );
		}
	return power;
}

int runTuring( const Perturb& perturb )
{
	std::printf( "\n=== turing: CDIMA with starch forms a pattern at the Lengyel-Epstein wavelength, at two Details and two Dish Widths; without starch it stays uniform; uniform light erases it\n" );
	const chem::Recipe recipe = chem::BaseRecipe( Reaction::CDIMA );
	const chem::LEModel m     = chem::MakeLE( recipe );
	const double sigmaHopf    = ( 3.0 * m.a / 5.0 - 25.0 / m.a ) / m.b;
	//The fastest-growing mode from the dispersion relation (the eigenvalue of
	//J - k^2 diag( 1/sigma, d )): what the pattern selects above onset, which
	//is not the onset's k_c.
	double kFast = 0.0, growth = -1e9;
	{
		const double u0 = m.u0, v0 = m.v0, den = 1.0 + u0 * u0, g = ( 1.0 - u0 * u0 ) / ( den * den );
		const double J11 = ( -1.0 - 4.0 * v0 * g ) / m.sigma, J12 = ( -4.0 * u0 / den ) / m.sigma, J21 = m.b * ( 1.0 - v0 * g ), J22 = -m.b * u0 / den;
		for( double k = 0.02; k < 6.0; k += 0.002 )
		{
			const double a11 = J11 - k * k / m.sigma, a22 = J22 - k * k * m.d;
			const double tr = a11 + a22, det = a11 * a22 - J12 * J21;
			const double disc = tr * tr - 4.0 * det;
			const double lam  = disc >= 0.0 ? 0.5 * ( tr + std::sqrt( disc ) ) : 0.5 * tr;
			if( lam > growth )
			{
				growth = lam;
				kFast  = k;
			}
		}
	}
	const double lambdaFast = 2.0 * kPi / kFast * m.xScale;
	const double efold      = m.tScale / growth;
	Note( fmt( "the model at the 1x recipe: a %.2f, b %.3f, sigma %.1f (the Hopf line is at sigma %.1f), d %.3f; the onset k_c %.3f per x' (x' = %.4f mm, %.4f mm); the fastest-growing mode k %.3f, wavelength %.4f mm, e-folding %.0f s",
	           m.a, m.b, m.sigma, sigmaHopf, m.d, m.kc, m.xScale, m.lambdaMm, kFast, lambdaFast, efold ) );
	//Six e-foldings from the seed's 2% noise, then some saturation.
	const double grow = std::min( 8.0 * efold, 1500.0 );
	struct Case
	{
		int cols, rows;
		double dishMm;
	};
	const Case cases[] = { { 512, 256, 10.0 }, { 1024, 512, 10.0 }, { 512, 256, 5.0 } };
	for( const Raster& raster : kRasters )
	{
		double patternContrast = 0.0;
		int wrong = 0;
		std::string what;
		for( const Case& c : cases )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::CDIMA, c.cols, c.rows );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( c.dishMm ) );
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Flow ) );
			if( perturb.turingNoD )
				rig.plugin.SetParamOverrideForTest( chem::P_LE_D, static_cast< float >( 1.0 / m.sigma ) );//the inhibitor as slow as the activator
			rig.Render( 1 );
			for( double t = 0.0; t < grow; t += 1.0 )
				rig.Chem( 1.0 );
			const Floats st = rig.State();
			std::vector< double > u( static_cast< size_t >( c.cols ) * c.rows );
			double mean = 0.0, sq = 0.0;
			for( size_t i = 0; i < u.size(); ++i )
			{
				u[ i ] = st[ i * 4 ];
				mean += u[ i ] / u.size();
			}
			for( double v : u )
				sq += ( v - mean ) * ( v - mean ) / u.size();
			const double contrast = mean > 0.0 ? std::sqrt( sq ) / mean : 0.0;
			patternContrast       = std::max( patternContrast, contrast );
			const std::vector< double > power = radialPower( u, c.cols, c.rows );
			size_t peak = 1;
			for( size_t b = 2; b < power.size(); ++b )
				if( power[ b ] > power[ peak ] )
					peak = b;
			const double cellMm = rig.plugin.CellMm();
			const double extent = c.cols * cellMm;
			const double lambda = extent / static_cast< double >( peak );
			//One FFT bin: the wavenumber 1 / lambda within 1 / extent of the fastest mode's.
			const bool ok = contrast > 0.1 && std::fabs( 1.0 / lambda - 1.0 / lambdaFast ) <= 1.0 / extent;
			wrong += !ok;
			what += fmt( " %d cells over %g mm (%.4f mm cells): contrast %.2f, wavelength %.4f mm (bin %zu of %.1f, model %.4f)%s;", c.cols, c.dishMm, cellMm, contrast, lambda, peak, extent / lambdaFast, lambdaFast, ok ? "" : " OUT" );
		}
		Check( wrong == 0, fmt( "%dx%d after %.0f s:%s %d wrong", raster.w, raster.h, grow, what.c_str(), wrong ) );
		//No starch: sigma 1, below the Hopf line: a relaxation oscillation, no
		//stationary pattern. The 2% seed noise leaves the cells' phases a little
		//apart, so at a spike the dish is far from uniform for a moment (phase
		//waves, as in the dish): what a Turing pattern has and this has not is
		//a TIME-AVERAGE with structure. 300 s is ~43 periods, so a cell's
		//average is within a couple of percent of its neighbours' whatever its
		//phase; the stationary pattern above keeps its full contrast however
		//long the average.
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			prepare( rig, Reaction::CDIMA, 512, 256 );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 10.0 ) );
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Flow ) );
			rig.Set( PT_INDICATOR, 0.0f );
			rig.Render( 1 );
			std::vector< double > sum;
			double worstContrast = 0.0, lo = 1e9, hi = -1e9;
			int frames = 0;
			for( int f = 0; f < 3000; ++f )
			{
				rig.Chem( 0.1 );
				const Floats st = rig.State();
				const size_t n  = st.size() / 4;
				if( sum.empty() )
					sum.assign( n, 0.0 );
				double mean = 0.0, sq = 0.0;
				for( size_t i = 0; i < n; ++i )
				{
					sum[ i ] += st[ i * 4 ];
					mean += st[ i * 4 ] / n;
				}
				for( size_t i = 0; i < n; ++i )
					sq += ( st[ i * 4 ] - mean ) * ( st[ i * 4 ] - mean ) / n;
				worstContrast = std::max( worstContrast, mean > 0.0 ? std::sqrt( sq ) / mean : 0.0 );
				lo            = std::min( lo, mean );
				hi            = std::max( hi, mean );
				++frames;
			}
			double mean = 0.0, sq = 0.0;
			for( double& s : sum )
			{
				s /= frames;
				mean += s / sum.size();
			}
			for( const double s : sum )
				sq += ( s - mean ) * ( s - mean ) / sum.size();
			const double averaged = mean > 0.0 ? std::sqrt( sq ) / mean : 0.0;
			Check( averaged < 0.05 && hi > 1.2 * lo && hi < 100.0, fmt( "%dx%d  without starch (sigma 1): the 300 s time-average has contrast %.4f (a stationary pattern keeps its %.2f); the mean swings from %.3f to %.3f (a relaxation oscillation; the instantaneous contrast reaches %.2f at a spike)",
			                                                      raster.w, raster.h, averaged, patternContrast, lo, hi, worstContrast ) );
		}
		//Uniform light through the Over: the clip white, Light Coupling 1.
		{
			Floats white( static_cast< size_t >( raster.w ) * raster.h * 4, 1.0f );
			Rig rig( true );
			if( !rig.Init( raster.w, raster.h, &white ) )
				return 1;
			prepare( rig, Reaction::CDIMA, 512, 256 );
			rig.Set( PT_DISH_WIDTH, ParamFromDishWidth( 10.0 ) );
			rig.Set( PT_REACTOR, static_cast< float >( Reactor::Flow ) );
			rig.Set( PT_SEED_FROM_CLIP, 0.0f );
			rig.Set( PT_LIGHT_COUPLING, 0.0f );
			rig.Render( 1 );
			for( double t = 0.0; t < grow; t += 1.0 )
				rig.Chem( 1.0 );
			auto contrastNow = [ & ]() {
				const Floats st = rig.State();
				double mean = 0.0, sq = 0.0;
				const size_t n = st.size() / 4;
				for( size_t i = 0; i < n; ++i )
					mean += st[ i * 4 ] / n;
				for( size_t i = 0; i < n; ++i )
					sq += ( st[ i * 4 ] - mean ) * ( st[ i * 4 ] - mean ) / n;
				return mean > 0.0 ? std::sqrt( sq ) / mean : 0.0;
			};
			const double before = contrastNow();
			rig.Set( PT_LIGHT_COUPLING, 1.0f );
			for( double t = 0.0; t < 0.5 * grow; t += 1.0 )
				rig.Chem( 1.0 );
			const double lit = contrastNow();
			Check( before > 0.1 && lit < 0.1 * before, fmt( "%dx%d  the pattern's contrast %.3f in the dark, %.4f after %.0f s under the Over's white clip at Light Coupling 1", raster.w, raster.h, before, lit, 0.5 * grow ) );
		}
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
		{ "spiral", runSpiral, false },      { "photo", runPhoto, false },       { "clock", runClock, false },
		{ "sync", runSync, false },          { "briggs", runBriggs, false },     { "traffic", runTraffic, false },
		{ "bluebottle", runBlueBottle, false }, { "chameleon", runChameleon, false }, { "stir", runStir, false },
		{ "units", runUnits, false },        { "turing", runTuring, false },
		{ "excitable", runExcitable, false }, { "freshstir", runFreshStir, false },
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
std::string g_negativeOnly;///< --only NAME: run that negative control alone

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
	add( "clock", runClock, "the acid term dropped from the plugin's rate", []( Perturb& p ) { p.clockWrongAcid = true; } );
	add( "sync", runSync, "the dose sized from the wrong closed form", []( Perturb& p ) { p.syncWrongDose = true; } );
	add( "briggs", runBriggs, "the plugin's malonic acid step at half its constant", []( Perturb& p ) { p.briggsRate = true; } );
	add( "traffic", runTraffic, "oxygen never re-oxidises the dye (kOx = 0)", []( Perturb& p ) { p.trafficNoAir = true; } );
	add( "bluebottle", runBlueBottle, "the plugin's reduction constant doubled", []( Perturb& p ) { p.bottleDouble = true; } );
	add( "chameleon", runChameleon, "the plugin's two constants swapped", []( Perturb& p ) { p.chameleonSwap = true; } );
	add( "stir", runStir, "Stir ignored", []( Perturb& p ) { p.stirOff = true; } );
	add( "units", runUnits, "the cell 0.1 mm whatever Dish Width says", []( Perturb& p ) { p.unitsWrong = true; } );
	add( "turing", runTuring, "the inhibitor diffusing as slowly as the activator", []( Perturb& p ) { p.turingNoD = true; } );
	add( "excitable", runExcitable, "the Break Wave never pressed: a ring, not a pair", []( Perturb& p ) { p.excitableNoBar = true; } );
	add( "excitable", runExcitable, "the default Excitability behaving as the excitable setting", []( Perturb& p ) { p.excitableDefault = true; } );
	add( "freshstir", runFreshStir, "the fresh dish seeded exactly on the rest state, as 0.1.0 did", []( Perturb& p ) { p.freshAtRest = true; } );
	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );
	if( !g_negativeOnly.empty() )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return g_negativeOnly != c.name; } ), cases.end() );
	if( cases.empty() )
	{
		std::fprintf( stderr, "no negative control named '%s'\n", g_negativeOnly.c_str() );
		return 1;
	}
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
	bool beat = false, effect = false, sizeGiven = false, framesGiven = false;
	int filmFrames = 0;
	std::string mode, scriptPath;
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
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name or index);\n"
			             "                    \"Name=V@F\" sets it at frame F. Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA out, top row first: the source makes --frames N (0/absent: until the\n"
			             "                    reader hangs up); the Over effect (--over) takes frames in on stdin\n"
			             "  --film N          N frames of the Over on its card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value' lines; a wrong name or value is refused\n\n"
			             "  checks (GL): --state --prime --resize --timebase --beer --over-check --oregonator --fieldnoyes --spiral --photo\n"
			             "               --clock --sync --briggs --traffic --bluebottle --chameleon --stir --units --turing\n"
			             "               --excitable --freshstir\n"
			             "  checks (no GL): --names --spectra --transport --timebase-law\n"
			             "  --negative [--only NAME]   --offline   --bench\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
		{
			frames      = std::atoi( argv[ ++i ] );
			framesGiven = true;
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--over" )
			effect = true;
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--only" && hasNext )
			g_negativeOnly = argv[ ++i ];
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

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
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
	else if( mode == "pipe" )
		//The fleet's two shapes: an effect is frames in, frames out; a source
		//makes --frames of them, or runs until the reader hangs up.
		result = runPipe( effect, width, height, fps, scriptPath, framesGiven ? frames : 0, effect, beat, settings );
	else if( mode == "film" )
		//The Over on its card, N frames.
		result = runPipe( true, width, height, fps, scriptPath, filmFrames, false, beat, settings );
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
			//"Name=V" applies before the first frame; "Name=V@F" at frame F (an
			//event that needs something to act on: a Break Wave once a wave exists).
			std::vector< std::pair< int, std::string > > scheduled;
			for( const std::string& setting : settings )
			{
				const size_t at = setting.find( '@' );
				if( at != std::string::npos )
				{
					scheduled.emplace_back( std::atoi( setting.c_str() + at + 1 ), setting.substr( 0, at ) );
					continue;
				}
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			std::stable_sort( scheduled.begin(), scheduled.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
			if( beat )
				rig.feed = AudioFeed::Pulses;
			int rendered = 0;
			bool ok      = true;
			for( const auto& [ frame, setting ] : scheduled )
			{
				if( frame > rendered && !( ok = rig.Render( frame - rendered ) ) )
					break;
				rendered = std::max( rendered, frame );
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s@%d: %s\n", setting.c_str(), frame, error.c_str() );
					return 2;
				}
			}
			if( ok && std::max( frames, 1 ) > rendered )
				ok = rig.Render( std::max( frames, 1 ) - rendered );
			if( !ok )
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
