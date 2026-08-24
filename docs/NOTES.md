# NOTES — afterglow

Repo-local notes: the things that are true about *this* repo and nowhere else.
Cross-cutting facts live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes); the traps and the
mental model live in [AGENTS.md](../AGENTS.md).

---

## 2026-08-24 — created

Written in one session, from the scaffolding of `tinsel` (structure, harness,
About block, presets, release workflow), `flipbook` (the parameterised
`cmake/InfoOFX.plist.in`) and `vertigo` (the preset-override fix). FFGL plugin
ID **`AG01`**, which was free across the fleet and across the SDK's own
`build/PluginIds.txt`.

The FFGL SDK submodule is pinned at `b1afaf9`, the same revision the rest of
the fleet pins.

---

## Deliberately NOT done from this repo

Three registrations were left alone because they are decisions about the
outside world rather than about this checkout, and making them from here would
publish something:

1. **`stoatworks-website/src/data/projects.json`** — adding an entry there
   publishes a page on stoatworks-labs.com. Until that happens,
   `source/StoatworksAbout.h` is **hand-written in the generated shape** with
   `guide` and `page` deliberately empty, so the About block shows no "User
   guide" or "Project page" button. `StoatworksAboutLinks.h` leaves an absent
   link out of the list rather than offering a button that 404s.
2. **`stoatworks-backend/scripts/sync-about.py`'s `TARGETS`** — `afterglow` is
   not in it, so the next sync will not touch this repo. Once it is, the sync
   will overwrite `source/StoatworksAbout.h` and add the two missing buttons.
   That is the intended end state; the hand-written file is a placeholder.
3. **`stoatworks-backend/resolume-demo/sync.sh`'s `repos`** — `afterglow` is
   not in it either, so `demo/vendor/` was copied from
   `resolume-demo/kit` by hand. It matches the master byte for byte today.

Note also that `sync.sh` computes `projects="$(cd "${here}/../.." && pwd)"`,
which after the 2026-08-17 tree reorg resolves to `~/projects/infrastructure`
and finds none of its targets. Adding `afterglow` to its list without fixing
that path would do nothing, silently. Same defect as
`scripts/sync-attributions.py` — see the fleet note on the repo tree layout.

Still outstanding: `docs/hero.jpg` (the README uses two harness renders of the
test card instead, labelled as such), the user guide PDF, the project video,
the Instagram cover, and the `afterglow-demo.stoatworks-labs.com` Worker route
and its Cloudflare deploy.

The repository itself was created public and pushed on 2026-08-24. The `ci`
workflow went green on the first push, and `release` was dispatched by hand
once (run 32701381985) to exercise the Windows path without cutting anything:
macOS universal bundle, Windows x64 DLL, both OpenFX bundles and the NSIS
installer all built, and the publish job correctly skipped for want of a tag.
Nothing is tagged, so there is no v0.1.0 yet.

---

## Design decisions worth not re-litigating

### The oldest slot is weighted to exactly zero

It is the frame that will be evicted at the next capture. Give it any visible
weight and every capture removes something the eye was looking at, and a trail
that loses its tail once per frame flickers at the capture rate — which reads
as the plugin dropping frames rather than as a design decision.

That is also why **Frames starts at 4 and not 2**: at two, one slot is the live
frame and the other is weighted to zero, i.e. no trail at all.

### Resolution defaults to Full

Half is a quarter of the memory, and it was the default for about an hour. It
is wrong as a default: the live frame is always read at full resolution, but
the *ghosts* are not, and on footage that is not moving the ghosts **are** the
picture — so a dozen soft copies averaged with one sharp one comes out soft.
`agtest --still` measures it: 117/255 worst deviation at Half against 0/255 at
Full, on the test card's fine checkerboard.

"The plugin makes my titles look fuzzy" is not something an operator should
have to work out.

### Halation peaks in the middle of the queue

`halation = amount × age × weight`. Not a compromise between the two terms —
it is what halation is. The newest frame has not aged into anything, and the
oldest is not there any more. What blooms is a ghost on its way out, which is
also the only place a glow can be *seen*: added on top of a frame at full
strength it is invisible.

### Blend is the default, not Screen

Screen is the more exciting default and it lifts every static frame in the
show. Blend — the incremental weighted average, i.e. the queue interpolated
into one picture — is the only mode that provably leaves footage that is not
moving exactly as it was.

### There is no Sync / BPM group, and no audio input

Neither has anything to lock to: the trail's length is measured in *frames*,
not in seconds, and nothing in the effect has a cycle. The only clock-driven
thing here is the warp field's phase.

If audio-reactivity is wanted later, the appended-ID rule applies — declare the
buffer parameter after `PT_PRESET` so no saved composition's parameter ids
shift. See the fleet note on FFGL audio/BPM patterns.

---

## Defects found and fixed during the build

Worth recording because each one was invisible in an obvious way.

### `Add` was handed the incremental alphas

The incremental schedule (`w / running`) reconstructs a weighted average under
a *composite*, not under an *addition*: its alphas sum to about three over a
dozen slots. Under `Add` that is three copies of the picture, and a static
frame came out **194/255 too bright**.

Found by `agtest --still`, in the first run of it, which is the entire
justification for that check existing. There are now three schedules — see
`Schedule` in `Decay.h`.

### The first `--still` check compared the wrong way up

The test card is uploaded to the texture as it stands, so GL treats its first
row as the bottom one and the readback comes back in the same order the card is
in. The first version of the check copied `flipRows(...)` out of the PNG-writing
path, compared the top of the output against the bottom of the card, and
reported a difference of **242/255 on a plugin that was behaving perfectly**.

The lesson generalises: a check that reports a *huge* difference on new code is
more likely to be wrong about the comparison than about the code.

### The live frame was being read out of the reduced queue

See "Resolution defaults to Full" above. The fix — age 0 reads the full-size
copy buffer — is a two-line change with a large comment on it in the
accumulation loop.

---

## Browser-pane verification, and why the demo is unverified

The demo's shaders and its ported maths are both checked mechanically, and it
loads with no console error. It has never been *watched running*.

The Browser pane reports `document.visibilityState === "hidden"`, so
`requestAnimationFrame` never fires. On most of the fleet's demos that only
means a still picture; on this one it means the frame queue never fills past a
single slot, `drawCount` is 0 in Ghosts mode, and the canvas is black — which
looks exactly like a broken port. Two rounds of screenshots went into
establishing that it was the pane and not the page:

- with `Background = Ghosts` and `Frames = 4` the canvas is black, which is the
  correct output for a queue holding one frame;
- with `Background = Black` the live frame renders correctly and shows no decay
  at all, which is also correct for a queue holding one frame, because age 0
  has `t = 0`;
- clicking the page's own **Step** button 90 times changes nothing, because
  Step schedules through `requestAnimationFrame` too;
- a debug write to `document.title` from inside the render loop never lands.

See the fleet note on browser-pane verification traps. The remaining way to
check it is a real browser window with the tab in front.

---

## Numbers on the record

Measured on an M4 Max, macOS 26.4, 2026-08-24, at the defaults unless stated.

| | 720p | 1080p | 1440p | 4K |
| --- | --- | --- | --- | --- |
| defaults (12 frames, Full, halation on) | 0.38 ms | 0.66 ms | 1.18 ms | 2.65 ms |
| 32 frames, Full, halation on | 0.71 ms | 1.50 ms | 2.67 ms | 5.94 ms |

Cost is roughly linear in Frames and quadratic in the picture. The halation
loop is a second pass over the whole queue, at quarter size, plus four blurs —
skipping it when the control is at zero is most of the difference between the
two rows above and a third row that was never worth measuring.

`agtest --decay`: 365,696 comparisons, largest difference 2.38e-7, 64
comparisons on a deliberate discontinuity and skipped.

`check_decay.mjs`: 4,011 comparisons, largest relative difference 1.78e-7.
