# afterglow

A queue of recent frames laid back over the picture, each one decaying further
as it ages, as an FFGL effect for Resolume Arena/Avenue and an OpenFX plugin for
Resolve. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`.
Public MIT repo.

Read `AGENTS.md` before changing the decay model, the weight schedule or the
capture path.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
- Render a frame offline: `./build/agtest --out /tmp/frame.png`
- List parameters: `./build/agtest --list`
- Just the test card: `./build/agtest --card /tmp/card.png`
- Set a control: `--set "Frames=0.8" --set "Crush=0.7"` (repeatable, by display name)
- Put real footage through the real shaders (for the project video):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/agtest --pipe --width W --height H [--script cues.txt] | ffmpeg …`

## OpenFX build
- `source/ofx/AfterglowOFX.cpp` → `build/Afterglow.ofx.bundle` (target
  `AfterglowOFX`, `-DBUILD_OFX=OFF` to skip) for Resolve/Vegas/Nuke/Natron.
  Links Controls/Decay straight from source; only the per-pixel stage is
  mirrored on the CPU. Change that stage in `Decay.cpp` and `kDecayLibrary`
  together.
- **The queue is not a queue there.** OFX renders frames in any order, so slot
  `k` is the source fetched at `t - k*hold` rather than a remembered texture.
  Exact and deterministic where the FFGL side is merely faithful.
- Smoke test: `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.afterglow --size 640x360 --out /tmp/a.bmp`
- Prove the still-picture invariant there too:
  `ofxprobe … --set halation=0 --set bleach=0` → "0 of N bytes differ".
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- A still picture is untouched: `./build/agtest --still`
- GLSL decay vs C++ decay: `./build/agtest --decay`
- Presets survive every host: `./build/agtest --presets`
- No dead controls: `python3 tools/sweep.py`
- The demo runs this repo's shaders: `python3 demo/tools/check_shaders.py`
- The demo runs this repo's maths: `node demo/tools/check_decay.mjs`
- Render cost: `./build/agtest --bench`
- Universal + exports: `lipo -archs build-universal/Afterglow.bundle/Contents/MacOS/Afterglow`
  and `nm -gU … | grep _plugMain`

## Notes
- **A still picture must come out untouched.** That is the invariant the whole
  weight schedule exists to hold up, and `--still` fails the build if it stops
  being true. Screen is the documented exception: it may lift, never darken.
- **Three weight schedules, not one.** Blend and Screen composite, so they take
  the incremental alphas (`w / running`); Add sums, so it takes proportional
  ones (`w / total`); Lighten takes a max, so it takes the shape weights as
  they are. Handing Add the incremental alphas is a real defect that shipped
  for an afternoon — they sum to about three, and a static frame came out
  194/255 too bright.
- **`GhostAt` exists once**, and both builds call it. Only the per-pixel stage
  is written twice, in `Decay.cpp` and in `kDecayLibrary`. Every mirrored line
  is marked `//= mirrored` in both. Change one, change both, run `--decay`.
- **The live frame is never read out of the queue.** Age 0 comes from the
  full-resolution copy buffer, so Resolution is a memory control and not a
  picture-quality one.
- The test card **moves**, and has to: on a still card every slot holds the
  same picture and the trail is provably invisible.
- `ScopedFBOBinding` does not restore the viewport. Capture the host viewport
  at the top of `ProcessOpenGL` and restore it before the composite.
- This is one of the few plugins in the fleet that uses **GL blending**. Save
  and restore the host's blend state; leaving `GL_BLEND` on is somebody else's
  problem later in the frame.
- `layout` is a GLSL keyword, as are `flat`, `active`, `filter`, `input`,
  `output`, `sample`, `common`. Shader errors surface only at runtime, in the
  diagnostics log, as "the effect does nothing".
- Randomness is an integer PCG hash, never `fract(sin(x)*…)` — a mirrored
  effect cannot use a function that differs per driver.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
- `FFGLScopedFBOBinding.h` is not in `FFGLSDK.h`; include it by hand.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- The harness drives `SetTime` on a synthetic 60fps clock. Without it no time
  passes offline and the warp field is frozen.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the two failures that actually happen:
a shader that will not compile, and a frame queue the driver would not
allocate. The second one logs the size it asked for, because the fix is almost
always Resolution or Frames.

    ~/Library/Logs/afterglow/afterglow.YYYY-MM-DD.log
