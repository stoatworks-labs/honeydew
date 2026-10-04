#pragma once

#include <FFGLSDK.h>

namespace honeydew
{
/**
    An off-screen buffer: a colour texture and a framebuffer, and nothing else.

    conway's (stencil's) PassBuffer, with a filter choice: the chemistry's
    state is RGBA32F and the stir pass reads it BETWEEN texels (semi-Lagrangian
    advection), so a state buffer may be LINEAR; the colour buffers are read
    bilinearly by the composite too. `ffglex::FFGLFBO` is not used because its
    `Release()` leaks the colour texture and it always carries a depth buffer.

    A new buffer is cleared (undefined memory in a buffer that feeds back into
    itself is noise that never washes out). Nothing here uses an
    `ffglex::Scoped*` binding, because those clear to 0 on exit instead of
    restoring; the bindings disturbed are saved and put back by hand -- and the
    plugin still allocates everything before it binds anything.
*/
class PassBuffer
{
public:
	enum class Filter
	{
		Nearest,
		Linear
	};

	bool Ensure( GLsizei width, GLsizei height, GLint internalFormat, Filter filter = Filter::Nearest );
	void Clear();
	/// Replace the whole texture with `pixels` (RGBA floats, rows bottom-up).
	void Upload( const void* pixels );
	void Destroy();
	/// Hand everything to `other` (destroyed first), leaving this empty.
	void MoveTo( PassBuffer& other );

	GLuint TextureID() const
	{
		return texture;
	}
	GLuint FramebufferID() const
	{
		return framebuffer;
	}
	GLsizei Width() const
	{
		return width;
	}
	GLsizei Height() const
	{
		return height;
	}
	bool IsValid() const
	{
		return framebuffer != 0;
	}
	void BindForDrawing() const;

private:
	GLuint texture     = 0;
	GLuint framebuffer = 0;
	GLsizei width      = 0;
	GLsizei height     = 0;
	GLint format       = 0;
	Filter filter      = Filter::Nearest;
};

} // namespace honeydew
