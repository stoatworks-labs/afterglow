# AGENTS.md — Afterglow

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the
short command reference; this is the *why*. Read the "What is actually
verified" section before you tell anybody this works.

---

## What the plugin is

A queue of recent frames, composited back over the picture with each one
degraded further as it ages.

Every frame goes into a ring of textures. Every frame, the whole ring is drawn
back over the picture, oldest first: each ghost dimmer than the one in front of
it, and each one crushed, pixelated, warped, drifted, spun and hue-shifted in
proportion to how old it is, until it reaches the end of the queue and drops
off. What the operator sets is how long the queue is, how fast it falls away,
and how much of each kind of damage a frame has taken by the time it goes.

It ships twice: as an FFGL effect (Resolume Arena/Avenue) and as an OpenFX
plugin (Resolve, Vegas, Nuke, Natron).

---

## The three ideas the code is built on

### 1. Everything about a ghost is a function of its age

`GhostAt( slot, params )` in `Decay.cpp` takes one number — `t`, 0 for the
frame that just arrived and 1 for the one about to leave — and returns
everything about that slot: its weight, where it has drifted to, how many
quantisation levels it has left, how coarse its grid is, how far round the
wheel its hue has gone. Nothing in it looks at a pixel.

That is what makes the decay model checkable at all. It is arithmetic, it has
one right answer, and the answer does not depend on a picture.

It also means the model can exist **once**. `GhostAt` runs per slot, not per
pixel — a couple of dozen floating-point operations, at most thirty-two times a
frame — so both builds simply call it and the GPU is handed the answers as
uniforms. A model that cannot drift is worth more than one that is checked.

### 2. A still picture comes out untouched

Twelve copies of the same frame average to that frame, **exactly**, whatever
the queue length or the falloff curve is. `agtest --still` proves it to
0/255 and fails the build if it stops being true.

This is the invariant the whole weight schedule is built to hold up, and it is
worth defending hard: an effect that quietly lifts, dims or softens footage
that is not moving is an effect that has to be switched off between cues, which
in practice means an effect nobody uses.

Screen is the deliberate exception. It brightens where the ghosts genuinely
differ, which is the whole reason to pick it, and it is checked for never
*darkening*.

### 3. A queue, not a feedback buffer

The obvious build is recursive: one buffer, blend each new frame in, show the
result. One texture instead of thirty-two, and the trail is genuinely similar —
an exponential falloff is exactly what a recursive blend makes, and Decay at a
curve of 1 over a long queue is its truncated impulse response.

It cannot make an old frame look *older*. Fold the frames together and every
degradation compounds on the accumulator instead of on the frame it belongs to:
yesterday's crush gets crushed again today, the drift integrates into a smear,
and there is no way back to the picture.

If somebody asks for a "feedback" mode later, it is not a mode — it is this
plugin with the decay controls at zero and a long queue.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider position means, in physical units. One copy, used by both builds and the harness. |
| `source/Decay.{h,cpp}` | The decay model. `GhostAt` (per slot, single copy) and the per-pixel stage (`GhostSampleUV`, `DegradeColour`, the noise) which is mirrored in GLSL. |
| `source/Shaders.{h,cpp}` | The GLSL. Four fragment shaders; `kDecayLibrary` is the mirrored per-pixel stage. |
| `source/PassBuffer.{h,cpp}` | An FBO that reallocates only when it has to and actually frees its colour texture. |
| `source/Afterglow.{h,cpp}` | The FFGL plugin: parameters, the ring, the passes, presets. |
| `source/Presets.h` | The factory presets, in host-facing 0..1. Both builds read it. |
| `source/ofx/AfterglowOFX.cpp` | The OpenFX plugin. Links Controls and Decay; mirrors the per-pixel stage on the CPU. |
| `tools/agtest/main.cpp` | The offline harness: renders, checks, benchmarks, dumps. |
| `demo/` | The browser demo. Copies the GLSL, ports the C++, and both halves are checked. |

### The passes

1. **copy** — picture size, mipmapped. Resolves MaxUV and the half-texel inset.
2. **copy again** — into the queue slot the head is on, reading the mip chain
   at `log2(divisor)`. That second use is the whole reason `copy` exists as its
   own buffer: point-sampling a picture eight times finer than the slot is an
   aliased downsample, not a downsample.
3. **ghost × N** — one slot of the queue, degraded, weighted, blended into the
   trail.
4. **ghost × N again** — the same slots into a quarter-size halation buffer,
   bright-passed. Skipped entirely when Halation is zero, which is most of the
   frame's cost.
5. **blur × 4** — separable, two widths, on the halation buffer.
6. **composite** — the trail *over* the background, the halation *added*.

---

## Traps

Roughly in the order they will bite.

### ☠️ The three weight schedules are not interchangeable

`Ghost::weight` is a **shape** — 1 at the head, 0 at the tail — and it is not
what any blend wants handed to it directly. A dozen ghosts at an average weight
of half do not add up to a picture, they add up to five or six pictures.

- **Blend** and **Screen** composite (`dst*(1-a)+src`, `src+dst*(1-src)`), so
  they take the **incremental** alphas: drawn oldest first, each ghost gets
  `w / (the weights so far, including its own)`. That is the running weighted
  average written as a series of composites.
- **Add** sums, so it takes **proportional** alphas: `w / total`, which add to
  exactly one.
- **Lighten** takes a `max`, which does not accumulate at all, so it takes the
  shape weights unchanged. Hand it either of the others and the trail is
  brightest at its *tail*, because both of those give their largest alpha to
  the oldest ghost.

This is not theoretical. Add shipped for an afternoon with the incremental
alphas — they sum to about three over a dozen slots — and a static frame came
out **194/255 too bright**. `--still` is what found it, and it is why that
check runs first in `verify.sh`.

### ☠️ The incremental schedule *is* the draw order

`ResolveDrawAlphas` computes the incremental alphas oldest-first, and the
accumulation loop draws oldest-first. If one of those is ever reversed, the
result is a weighted average of the wrong weights — which still looks like a
trail, just the wrong one.

### The live frame is never read out of the queue

Age 0 comes from the full-resolution copy buffer, not from the slot the capture
just wrote. Read it from the queue and Resolution stops being a memory control
and becomes a picture-quality one: at Half the live frame is a bilinear upscale
of a half-size copy of itself, so in the default Background mode every static
frame in the show comes out softer than it went in.

The ghosts do not care — they are being crushed and warped and dimmed anyway.
The live frame is the one the operator is looking at.

### The head advances every Hold frames; the picture is written every frame

Those are deliberately not the same thing. Advance and write together and the
newest slot is up to Hold−1 frames stale, so at Hold = 8 the live picture in
Black and Transparent modes judders at one eighth of the frame rate — a defect
that reads as dropped frames rather than as the control that was moved.

### Changing Frames empties the queue, and has to

The ring's indexing is modulo its own length, so changing the length does not
shuffle the contents, it *reinterprets* them: every slot still holds a real
frame and every one is filed under the wrong age. Starting empty costs a
queue's worth of frames and is the only answer that is not wrong.

### The queue is a fixed array, not a `std::vector`

`ffglex::FFGLFBO` has a user-declared destructor and raw GL ids, so its
implicit copy constructor duplicates the ids without duplicating the objects. A
vector reallocation would hand two `PassBuffer`s the same framebuffer and
delete it twice.

### This plugin blends, and most of the fleet does not

`ProcessOpenGL` captures `GL_BLEND`, the blend equation and the blend function
at the top and puts them back at the bottom. Leaving `GL_BLEND` enabled is
somebody else's problem later in the host's frame, and it will not look like
this plugin's fault.

`GL_MAX` (Lighten) ignores the blend *function* entirely, so leaving that case
without setting one would make the next pass depend on what the previous one
happened to set.

### `ScopedFBOBinding` restores the framebuffer and not the viewport

SDK b1afaf9. Every pass's `ResizeViewPort()` leaks into the next one, and the
composite — which draws to the host's own framebuffer and so has no buffer to
size itself from — inherits whatever the last pass left. Here that would be the
quarter-size halation buffer, and the effect renders into the bottom-left
quarter with the rest transparent. Capture `GL_VIEWPORT` at the top, restore it
before the composite.

### Every `ffglex::Scoped*` binding CLEARS to 0 on scope exit

It does not restore. `FFGLFBO::Initialise` sizes its new colour texture under
one of those, so **allocating a buffer unbinds the input texture from the
active unit**. Every `Ensure()` in `ProcessOpenGL` happens before anything
binds a texture, and it has to stay that way. The symptom is the dangerous
part: correct on every frame except the one that allocates.

### `FFGLFBO::Release()` leaks the colour texture

It deletes the framebuffer and the depth renderbuffer, then tests
`depthBufferID` a second time where it plainly meant `colorTextureID`.
`PassBuffer::Destroy()` deletes it first. A leak of one texture per release is
a curiosity in a plugin with six fixed buffers; in one that reallocates
thirty-two full pictures whenever a slider moves, it is a way to exhaust video
memory during a show.

### A TEXT parameter without `SetTextParameter` kills the whole plugin

`instantiateGL` pushes every declared default back through the setters and
deletes the instance the moment one returns `FF_FAIL` — which is exactly what
`CFFGLPlugin::SetTextParameter` does. The About block is display-only text, so
there is nothing to store, but it has to say so *successfully*. Invisible in
every in-repo harness, because they call the plugin class directly.

### A GLSL uniform name that does not match the C++ is silently ignored

`glGetUniformLocation` returns −1 and `glUniform(-1)` is a documented no-op, so
a control can be stone dead while everything compiles, links, loads and
renders. `tools/sweep.py` is the only thing that catches it.

### `FFGLShader::Set` has no integer-vector overload

The overloads are `float`, `vec2`, `vec3`, `vec4` and `int` — nothing else.
`Set( name, someInt, someInt )` resolves to `(float,float)` and issues a
`glUniform2f` against an `ivec2`, which is a `GL_INVALID_OPERATION` that leaves
the uniform at zero with nothing anywhere the plugin can see.

### A ranged STANDARD parameter cannot have a ranged default

`SetParamInfo` clamps a standard default into 0..1 *before* returning, and
`SetParamRange` can only be called afterwards. So every numeric parameter here
is a plain 0..1 float and the conversions live in `Controls.cpp`.

### Option lists are sorted; option VALUES are not

`SetParamElementInfo` takes an element's display slot and its stored value as
different arguments, and the spec is explicit that picking an option stores the
option's *value*. So the lists can be re-sorted for whoever has to read them
without a saved composition, a preset or the harness changing meaning.

Resolution is deliberately **not** sorted: it is a divisor, the position is the
meaning, and alphabetically it reads Eighth, Full, Half, Quarter.

### Presets: the host owns parameter state and does not consume value events

Reported against vertigo as its issue #2 and copied into seven plugins before
anybody noticed. Resolume does not act on the `FF_EVENT_FLAG_VALUE` events a
plugin raises; it carries on pushing the values it still believes in, which are
the ones from *before* the preset. So "a covered parameter changed, therefore
the operator has taken over" fires on the host's own echo, immediately, and the
dropdown snaps back to Custom.

The fix here is the simpler of the two shapes in the fleet: keep `hostValues[]`
— what the host last *sent* — separately from what the plugin renders with, and
do not write the host's restatement into `params[]` at all.

Two things in that mechanism will bite again:

- **`seedHostValues()` must run BEFORE `applyPreset` can.** Seeding lazily
  inside the guard would record the preset's own values as the host's opening
  position, and the host's very next restatement would look like an edit.
- **The two tolerances are different numbers on purpose.** The
  host-restatement test uses 1e-3, a quantisation allowance; the "did a covered
  parameter move?" test uses 1e-4. A value that *matches* the preset must be
  ignored, not written — writing a host's rounded copy of our own value trips
  the tighter test.

`agtest --presets` drives three hosts (honours the events / ignores them /
honours-but-quantises) across every preset with no GL.

### A float `mod` is not safe for an index

GLSL defines `mod` as `x - y*floor(x/y)`; on an exact multiple the division can
round a hair below the integer, `floor` drops a whole step, and the result is
`y` instead of `0` — **on the GPU only**, so a CPU mirror will not reproduce
it. Nothing here uses one; keep it that way.

### Never `fract( sin( x ) * 43758.5453 )`

That is a different function on every driver, which for a *mirrored* effect
means the CPU and GPU builds cannot be made to agree even in principle. The
noise here is an integer PCG hash on both sides, which is exact.

### `layout` is a GLSL keyword

So are `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`. A
shader that fails to compile surfaces only at runtime, as "the effect does
nothing" — and in a shader assembled from several strings, the reported line
number is in a file that does not exist. That is what `Diag` is for.

### `FFGLScopedFBOBinding.h` is not in the umbrella header

`FFGLSDK.h` includes every other scoped binding and omits that one.

### macOS must build universal, and the log will lie about it

CMake latches `CMAKE_OSX_ARCHITECTURES` when the first target is created, so
setting it late is silently ignored and the build still logs a success. Only
`lipo` is honest.

### `cmake/InfoOFX.plist.in` is the file that catches new repos

The version this repo would have been copied from spelled the *previous*
plugin's name into `CFBundleExecutable`. Nothing catches that locally: the
bundle assembles, the binary is correct, `nm` finds `_OfxGetPlugin`, and
`ofxprobe` renders through it. It fails at release time, in `codesign`, with a
message about a "subcomponent" that never mentions the plist. This repo's copy
is parameterised on `@PROJECT_NAME@`, and `tools/verify.sh` runs the release
step locally.

### `vcpkg.json` is invisible from the CMakeLists

GLEW arrives through the vcpkg manifest, and the CMakeLists never mentions it —
so every local build and every macOS CI job passes while the Windows job fails
at *configure*.

---

## The one real difference between the two builds

The FFGL build **remembers** the last N frames. The OpenFX build **fetches**
them.

FFGL hands a plugin one frame at a time, in order, and there is no way to ask
for a frame it was not given — so the queue is a ring of textures, and it holds
whatever the host happened to ask for. Scrub the composition, retrigger the
clip, or drop a frame under load and the trail holds pictures that were never
adjacent.

OFX is the opposite: frames render in any order, alone and concurrently, and
the host will fetch any frame of the source clip on request. So slot `k` is
simply the source at `t − k × hold`, through temporal clip access. That is
exact, and it is deterministic — rendering frame 500 alone gives the same
picture as rendering the whole timeline up to it, which the FFGL build cannot
promise and does not claim to.

The consequence to know about on the OFX side: **the first frames of a clip are
short of history**, because `t − k` is before the clip starts and the host
returns nothing. The trail builds over the first Frames × Hold frames rather
than being wrong.

The other departure is the warp phase: FFGL integrates the rate so a live nudge
of Warp Speed does not rescale the field's history; OFX uses `time × speed`,
because there is no previous frame to have integrated from and a deterministic
frame matters more in a host that renders them out of order.

---

## The browser demo

`demo/` is a WebGL2 page that runs the plugin's own GLSL over clips generated
in the page. It is not the plugin, and it says so in a banner.

It **copies** the shader text and **ports** the C++, and both halves are
checked:

- `demo/tools/check_shaders.py` compares every shader literal in
  `source/Shaders.cpp` against the template literal in `demo/plugin.js`,
  character for character.
- `demo/tools/check_decay.mjs` runs `agtest --ghosts` — a JSON dump of every
  control mapping, every ghost and every weight schedule — and compares it
  against `demo/decay.js`.

That second one is new in this repo and worth keeping. Until it existed nothing
checked the ported half at all, and on *this* plugin the gap is wide:
everything about a ghost comes out of `GhostAt`, so a mistake in the port is
not a slider reading 0.47 instead of 0.5, it is the whole trail being a
different shape.

The pure maths therefore lives in `demo/decay.js`, which touches no DOM and no
WebGL. Keep it that way, or the checker cannot import it.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4

- **A still picture is not touched.** Blend, Add and Lighten: 0/255 deviation
  across the whole frame, on the FFGL build. Screen: never darker. The OpenFX
  build gives *byte-identical* output through `ofxprobe` for the same three
  blends ("0 of 921600 bytes differ from the input").
- **The GLSL decay stage against the C++ one**: 365,696 comparisons, 0
  disagreements past 5e-4, largest observed difference 2.4e-7. 64 comparisons
  landed on a deliberate discontinuity (a quantiser or pixelation boundary) and
  were counted and skipped rather than failed.
- **No dead controls**: all 23 parameters measurably change the picture.
- **Factory presets**: all 8, against all 3 host behaviours, in both builds.
- **The demo's shaders** are this repo's, character for character; **the
  demo's maths** agrees to 1.8e-7 relative.
- **The macOS bundle** is universal (`x86_64 arm64`) and exports `plugMain`.
- **The OpenFX bundle** loads and renders through `ofxprobe`, exports
  `_OfxGetPlugin`, names its own binary in its plist and ad-hoc signs.
- **Render cost**: 0.66 ms/frame at 1080p and 2.6 ms at 4K with the defaults;
  1.5 ms and 5.9 ms with 32 frames at Full resolution and halation on.

### Assumed, not measured

- ☠️ **It has never been loaded into Resolume.** Everything above runs the
  plugin class directly in a headless GL context. The things that only a real
  host exercises are: `plugMain` and `instantiateGL` (the `SetTextParameter`
  trap lives there), whether Resolume honours the parameter groups, and what
  the host's clock and blend state actually look like on the way in.
- ☠️ **It has never been loaded into DaVinci Resolve.** `ofxprobe` is a
  faithful harness but it is not Resolve, and in particular it is not a host
  that renders frames out of order across several threads — which is the
  condition the OpenFX build's whole design assumes.
- **Windows has never been built.** The release workflow does it; nothing has
  run it yet.
- **Linux has no path at all.** The CMakeLists branches on APPLE and WIN32.
- **The browser demo has never been watched running.** Its shaders and its
  maths are proved to be this repo's, and the page loads without a console
  error — but the Browser pane used to check it reports
  `document.visibilityState === "hidden"`, so `requestAnimationFrame` never
  fires, the queue never fills past one frame and no trail can appear. The
  render loop itself (the ring indexing, the blend state, the buffer wiring) is
  a straight port of the C++ loop that *is* verified, and that is the whole of
  the argument for it.
- **The memory table in the README is arithmetic**, not a measurement. Nobody
  has watched a 4K show hold thirty-two full frames.
- **Nothing has been through a real show.**

---

## Sibling projects

- **`resolume-ofx-bridge`** — `build/ofxprobe` is what loads and renders the
  OpenFX build here. `--edit name=value` delivers a real user edit and fires
  `instanceChanged`, which is the only way preset logic runs headless.
- **`tinsel`, `downpour`, `orrery`, `vertigo`, `porthole`, `old-cathode`** —
  the same scaffolding, the same About block, the same preset mechanism and the
  same release workflow. A fix to any of those shapes belongs in all of them.
- **`stoatworks-backend`** — the masters for `source/StoatworksAbout*.h`,
  `demo/vendor/`, `scripts/release-lib.sh` and `ATTRIBUTIONS.md`. Edit them
  there and re-run the sync; the copies here are build inputs.

## What is still to do

See `docs/NOTES.md` for the list, including the registrations in
`stoatworks-backend` and on the website that have deliberately not been made
from here.
