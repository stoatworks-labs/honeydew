#include "PassBuffer.h"

namespace honeydew
{
namespace
{
struct SavedState
{
	GLint framebuffer   = 0;
	GLint texture       = 0;
	GLint viewport[ 4 ] = { 0, 0, 0, 0 };
	SavedState()
	{
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &framebuffer );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
		glGetIntegerv( GL_VIEWPORT, viewport );
	}
	~SavedState()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( framebuffer ) );
		glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( texture ) );
		glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	}
};
} // namespace

bool PassBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint internalFormat, Filter wanted )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;
	if( framebuffer != 0 && width == requestedWidth && height == requestedHeight && format == internalFormat && filter == wanted )
		return true;
	Destroy();
	SavedState saved;
	width  = requestedWidth;
	height = requestedHeight;
	format = internalFormat;
	filter = wanted;

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, GL_FLOAT, nullptr );
	const GLint f = wanted == Filter::Linear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glGenFramebuffers( 1, &framebuffer );
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	if( !complete )
	{
		Destroy();
		return false;
	}
	Clear();
	return true;
}

void PassBuffer::Clear()
{
	if( framebuffer == 0 )
		return;
	SavedState saved;
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
	const GLfloat zero[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glClearBufferfv( GL_COLOR, 0, zero );
}

void PassBuffer::Upload( const void* pixels )
{
	if( texture == 0 )
		return;
	SavedState saved;
	GLint alignment = 4;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels );
	glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
}

void PassBuffer::BindForDrawing() const
{
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	glViewport( 0, 0, width, height );
}

void PassBuffer::Destroy()
{
	if( framebuffer != 0 )
	{
		glDeleteFramebuffers( 1, &framebuffer );
		framebuffer = 0;
	}
	if( texture != 0 )
	{
		glDeleteTextures( 1, &texture );
		texture = 0;
	}
	width = height = 0;
	format         = 0;
}

void PassBuffer::MoveTo( PassBuffer& other )
{
	other.Destroy();
	other.texture     = texture;
	other.framebuffer = framebuffer;
	other.width       = width;
	other.height      = height;
	other.format      = format;
	other.filter      = filter;
	texture = framebuffer = 0;
	width = height = 0;
	format         = 0;
}

} // namespace honeydew
