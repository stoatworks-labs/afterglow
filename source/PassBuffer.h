#pragma once

#include <FFGLSDK.h>

namespace afterglow
{
/**
	An off-screen buffer for one stage of the chain, or one slot of the queue.

	Three things on top of the SDK's FFGLFBO.

	**It reallocates only when it has to.** `Ensure()` is called every frame,
	for every slot, and is a no-op in the overwhelming majority of them. That
	matters more here than in most of the fleet: Frames and Resolution are
	both live controls, and an operator dragging either one is asking for up
	to thirty-two buffers to be reconsidered sixty times a second.

	**It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
	deletes the framebuffer and the depth renderbuffer, then tests
	`depthBufferID` a second time where it plainly meant `colorTextureID` --
	so the colour texture is leaked on every release (SDK b1afaf9,
	`FFGLFBO.cpp`). `Destroy()` deletes it first. A leak of one texture per
	release is a curiosity in a plugin with six fixed buffers; in one that
	reallocates a queue of thirty-two full pictures whenever a slider moves,
	it is a way to exhaust video memory during a show.

	**It owns its filtering**, because this plugin's buffers want two
	different answers:

	- the **copy** buffer is read at a mip level to fill the queue at whatever
	  divisor Resolution asks for, so it needs a full chain. That is not a
	  nicety: point-sampling a picture eight times finer than the target is
	  not a downsample, it is an aliased one, and on footage with any detail
	  in it the result crawls;
	- the **queue slots**, the trail and the halation buffers are all read
	  between texels -- magnified by the ghost pass, filtered by the blur --
	  and want `GL_LINEAR` and nothing else. A mip chain on a queue slot would
	  cost thirty-two `glGenerateMipmap` calls a frame for a level nothing
	  reads.

	Nothing here wants `Nearest`, and it is offered anyway: a buffer that is
	data rather than a picture is the usual reason to add a pass, and the
	trap when that day comes is silent -- `GL_LINEAR` on a data buffer does
	not fail, it returns plausible averages of unrelated numbers.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it
	/// is whatever texture memory the driver handed back -- and a queue slot
	/// is shown to the operator for as many frames as the queue is long, so
	/// on this plugin the garbage would sit on the programme feed for a
	/// second rather than for a frame.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale
	/// chain does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Clear to transparent black.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. With a queue this is up to sixty-four binds a frame.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	GLsizei Width() const
	{
		return width;
	}

	GLsizei Height() const
	{
		return height;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace afterglow
