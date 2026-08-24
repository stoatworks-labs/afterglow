# demo/ — the browser demo

Intended for **https://afterglow-demo.stoatworks-labs.com**, to be linked from
the project page and from the [video plugins
page](https://stoatworks-labs.com/video-plugins/) once this repo is published.
Neither the hostname nor the project page exists yet — see `docs/NOTES.md`.

**This is not the plugin.** It is the GLSL from
[`source/Shaders.cpp`](../source/Shaders.cpp), copied across unedited and run in
WebGL2 over clips generated in the page, with the parameters the plugin's
constructor declares. The page says so in a banner, and lists what it does not
reproduce at the foot.

All of the chain is here in the order `ProcessOpenGL` runs it — copy, capture,
the ghost pass once per queue slot, the same pass again into the halation
buffer, four blur passes, composite — including the ring of frame buffers, so
Frames and Hold behave as they do in the host rather than being flattened out.

## Editing it

- `plugin.js` — this plugin's parameters and its shaders. **When a shader in
  `source/Shaders.cpp` changes, change it here too.** The two copies exist
  because the demo cannot include a C++ file; `tools/check_shaders.py` is what
  enforces that they agree, and it runs in `tools/verify.sh`.
- `vendor/` — the shared kit, vendored from
  `stoatworks-backend/resolume-demo/kit`. **Do not edit these.** Fix the master
  and re-run `./sync.sh`; `./sync.sh --check` reports drift. Note that
  `sync.sh`'s repo list does not include `afterglow` yet, and that its
  `projects=` path has not been updated for the 2026-08 tree reorg — these
  files were copied from the master by hand.

`Controls.cpp`, the per-slot half of `Decay.cpp` and `Presets.h` are ported into
`plugin.js` rather than re-derived, so the number beside each slider is the
plugin's own conversion and a preset is the plugin's own table. The per-**pixel**
half of `Decay.cpp` is not ported at all: the page runs the plugin's GLSL for
that, which is the point of copying the shader text rather than rewriting it.

## What the page cannot have

- **Frame rate matters here.** The queue holds frames, not seconds, so a
  throttled background tab does not change what the trail looks like — but it
  does change how much real time Frames × Hold covers.
- **No transport and no host FFT.** Neither is used by this plugin, so nothing
  is missing on that account; it is worth saying because the rest of the fleet's
  demos have to.
- **Colour bars does not move**, and that is deliberate. With Blend, Add or
  Lighten it comes out identical to the input, which is the invariant the whole
  weight schedule is built on and the one `agtest --still` checks.

## Deploying

From the repo root:

```bash
cf-run npx wrangler deploy
```

There is no build to run first — but check `git status` before deploying,
because a parallel session sharing this checkout can have staged its own work
into `demo/`.

Verify **by content, never by status code**. A wrong page returns a cheerful
200; only the title and the banner tell you which page is live:

```bash
curl -s 'https://afterglow-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```
