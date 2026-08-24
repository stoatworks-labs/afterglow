#pragma once

/**
	The passes, as GLSL source.

	Four fragment shaders and one vertex shader. There are more *passes* than
	shaders because two of them run more than once:

	1. **copy**      run twice. First at picture size, resolving MaxUV and the
	                 half-texel inset into a texture of ours with a mip chain
	                 on it. Then again at queue resolution, reading that mip
	                 chain at the level Resolution asks for -- which is how a
	                 quarter-size queue gets a box-filtered reduction of the
	                 picture instead of an aliased point sample of it.
	2. **ghost**     run once per queue slot, into the trail buffer, and again
	                 once per slot into the quarter-size halation buffer. One
	                 shader for both: the halation source has to be the same
	                 degraded picture the trail is made of, and the surest way
	                 to keep it that way is for there to be one shader that
	                 makes it. `Bloom` switches the bright pass on.
	3. **blur**      quarter size, four times, `Direction` swapped between.
	4. **composite** output size. Background mode, gain, halation, mix.

	`kDecayLibrary` is a transcription of the per-pixel half of `Decay.cpp` --
	`GhostSampleUV`, `DegradeColour` and the noise underneath them. Every
	mirrored line carries a `//= mirrored` marker in both files.

	The per-*slot* half is not here at all: `GhostAt` runs on the CPU in both
	builds and its answers arrive as uniforms. That is the whole reason this
	library is three functions long rather than the whole effect -- see the
	note at the top of `Decay.h`.

	**The ghost shader is assembled, not written out.** `GhostShaderSource()`
	wraps the library in the pass, and `DecayProbeShaderSource()` wraps the
	same string in a one-pixel-per-case probe for `agtest --decay`. So the
	test runs the exact text the plugin runs. A test that compiled its own
	transcription of the library would agree with itself perfectly and prove
	nothing.
*/

#include <string>

namespace afterglow
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kBlurShader;
extern const char* const kCompositeShader;

/// The per-pixel decay stage, as GLSL. Not a complete shader: no `#version`
/// and no `main`.
extern const char* const kDecayLibrary;

/// The ghost pass, assembled around kDecayLibrary.
std::string GhostShaderSource();

/// One case per pixel, writing `GhostSampleUV`'s two outputs and
/// `DegradeColour`'s four to a float target so they can be read straight back
/// and compared against `Decay.cpp`. Built from the same kDecayLibrary the
/// ghost pass uses.
///
/// This exists only for `agtest --decay`, and lives here rather than in the
/// harness so that there is one place where the library is concatenated into
/// something runnable.
std::string DecayProbeShaderSource();

} // namespace afterglow
