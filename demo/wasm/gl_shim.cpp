/*
    The GL entry points emscripten's WebGL2 library cannot take from the plugin
    as they are. Every other GL call the plugin and the FFGL SDK make goes
    straight to emscripten's implementation, on the page's own WebGL2 context.

    glShaderSource
        The plugin hands GL desktop GLSL 4.10 (`#version 410 core`); WebGL2
        compiles GLSL ES 3.00. The page's `honeydewShaderSource` first
        REQUIRES the text to be byte-for-byte one of the programs its own copy
        of the plugin's shaders assembles (demo/shaders.js, which
        demo/tools/check_shaders.py holds to source/Shaders.cpp, with the
        ASSEMBLY table that mirrors InitGL), then applies the demo kit's
        `port()`: the version line and the ES precision defaults, nothing
        else. A shader the plugin assembled differently from the page's copy
        is a refused compile the page reports, not a quiet wrong picture.

    glEnable / glDisable / glIsEnabled
        GL_PROGRAM_POINT_SIZE is desktop-only state (in ES a point is always
        sized by the shader, and WebGL2 rejects the enum with INVALID_ENUM).
        The plugin's GLState.h saves and restores it on every frame, as a
        plugin inside Resolume must. Here it reads as off and enabling it is
        ignored; the plugin draws no points. Every other capability is passed
        through unchanged.

    glTexImage2D / glGetTexImage
        glGetTexImage does not exist in GLES or WebGL2. The plugin uses it in
        two places, both reads of a texture it has just rendered: the dish's
        mean state (readMean: two 1x1 RGBA32F totals, which Clock Sync and the
        page's status line read) and the Over's thumbnail (brightestCell: a
        32x18 luminance copy of the clip, for Drop Position Brightest). WebGL2
        reads a texture back only through a framebuffer, so glGetTexImage here
        attaches the bound texture to a read framebuffer and glReadPixels it:
        the same floats, the only way this GL offers them. That needs the
        texture's size, which WebGL2 cannot be asked for either, so
        glTexImage2D is wrapped to remember each texture's size as it is
        allocated and then forwarded, for exactly the three allocations the
        plugin makes (RGBA32F buffers with no data, the R32F spectra table,
        the 1x1 RGBA8 blank clip); anything else aborts, by design.
*/
#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
constexpr GLenum kProgramPointSize = 0x8642;///< GL_PROGRAM_POINT_SIZE, desktop GL 3.2

struct Size
{
	GLsizei width, height;
};
std::unordered_map< GLuint, Size > g_sizes;///< texture name -> its level-0 size
GLuint g_readFBO = 0;

// clang-format off
EM_JS( void, hdShaderSource, ( GLuint shader, const char* source ), {
	const object = GL.shaders[ shader ];
	const stage  = GLctx.getShaderParameter( object, 0x8B4F /* GL_SHADER_TYPE */ ) === 0x8B31 ? 'vertex' : 'fragment';
	GLctx.shaderSource( object, Module[ 'honeydewShaderSource' ]( stage, UTF8ToString( source ) ) );
} );
EM_JS( void, hdEnable, ( GLenum cap ), { GLctx.enable( cap ); } );
EM_JS( void, hdDisable, ( GLenum cap ), { GLctx.disable( cap ); } );
EM_JS( int, hdIsEnabled, ( GLenum cap ), { return GLctx.isEnabled( cap ) ? 1 : 0; } );
// The pixel view is cut from the heap here rather than by emscripten's own
// glTexImage2D, which this definition replaces; `floats` says which heap.
EM_JS( void, hdTexImage2D, ( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels, int channels, int floats ), {
	let view = null;
	if( pixels )
	{
		const n = width * height * channels;
		view = floats ? HEAPF32.subarray( pixels >> 2, ( pixels >> 2 ) + n ) : HEAPU8.subarray( pixels, pixels + n );
	}
	GLctx.texImage2D( target, level, internalformat, width, height, border, format, type, view );
} );
// clang-format on

int channelsOf( GLenum format )
{
	switch( format )
	{
	case GL_RGBA: return 4;
	case GL_RGB: return 3;
	case GL_RG: return 2;
	case GL_RED: return 1;
	default: return 0;
	}
}
} // namespace

extern "C"
{
void glShaderSource( GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length )
{
	std::string source;
	for( GLsizei i = 0; i < count; ++i )
	{
		if( string[ i ] == nullptr )
			continue;
		const size_t n = ( length != nullptr && length[ i ] >= 0 ) ? static_cast< size_t >( length[ i ] ) : std::strlen( string[ i ] );
		source.append( string[ i ], n );
	}
	hdShaderSource( shader, source.c_str() );
}

void glEnable( GLenum cap )
{
	if( cap != kProgramPointSize )
		hdEnable( cap );
}

void glDisable( GLenum cap )
{
	if( cap != kProgramPointSize )
		hdDisable( cap );
}

GLboolean glIsEnabled( GLenum cap )
{
	if( cap == kProgramPointSize )
		return GL_FALSE;
	return hdIsEnabled( cap ) ? GL_TRUE : GL_FALSE;
}

void glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels )
{
	const int channels = channelsOf( format );
	const bool floats  = type == GL_FLOAT;
	if( target != GL_TEXTURE_2D || channels == 0 || !( floats || type == GL_UNSIGNED_BYTE ) )
	{
		std::fprintf( stderr, "honeydew demo: glTexImage2D( target 0x%x, format 0x%x, type 0x%x ) is not a call this page's GL shim carries\n", target, format, type );
		std::abort();
	}
	GLint bound = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
	if( level == 0 && bound != 0 )
		g_sizes[ static_cast< GLuint >( bound ) ] = Size { width, height };
	hdTexImage2D( target, level, internalformat, width, height, border, format, type, pixels, channels, floats ? 1 : 0 );
}

void glGetTexImage( GLenum target, GLint level, GLenum format, GLenum type, void* pixels )
{
	GLint bound = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
	const auto it = g_sizes.find( static_cast< GLuint >( bound ) );
	if( target != GL_TEXTURE_2D || level != 0 || bound == 0 || it == g_sizes.end() )
	{
		std::fprintf( stderr, "honeydew demo: glGetTexImage of a texture this page's GL shim did not see allocated\n" );
		std::abort();
	}
	GLint previousRead = 0;
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &previousRead );
	if( g_readFBO == 0 )
		glGenFramebuffers( 1, &g_readFBO );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, g_readFBO );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast< GLuint >( bound ), 0 );
	glReadPixels( 0, 0, it->second.width, it->second.height, format, type, pixels );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, static_cast< GLuint >( previousRead ) );
}
} // extern "C"
