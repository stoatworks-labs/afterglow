# Afterglow user guide

Afterglow is **a frame-trail effect for [Resolume](https://resolume.com) Arena and Avenue**, as an
FFGL plugin, and for DaVinci Resolve, Vegas, Nuke and Natron as an OpenFX plugin. It keeps the last
few dozen frames and lays them back over the picture, each one further gone than the one in front
of it.

The idea it is built on is one number. **Everything about a ghost is decided by how old it is** —
how much it is worth, where it has drifted to, how many bits of colour it has left, how far round
the wheel its hue has gone. That is why the trail can be made to *fall apart* as it goes rather
than merely fade, and it is why this is not a motion blur and not a feedback loop.

> **Before you rely on this:** the decay model is verified numerically. Its per-pixel half exists
> twice, once in C++ and once in GLSL, and an offline harness runs the shipped shader against the
> C++ over the whole parameter space — **365,696 comparisons with zero disagreements**. Separately,
> footage that is not moving is proved to come out of the effect **byte-for-byte identical** to
> what went in. All 23 controls are confirmed to change the picture. Both the macOS universal
> bundle and the Windows x64 DLL build in CI.
>
> Still open: it has **never been loaded into Resolume or Resolve**. Everything about how the
> controls *present* in a host is untested. Try it on a spare layer before you put it in a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Drop the plugin into Resolume's FFGL folder and restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
         (or /Users/Shared/Resolume Arena/Extra Effects/)
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. Afterglow then appears in the effects
browser.

**Needs Resolume Arena or Avenue 7.3.1 or newer.**

The macOS builds are **Developer ID-signed and notarised**, so the bundle simply loads — there is
nothing to clear and no `xattr` step. The Windows builds are not code-signed, but plugin files are
not gated the way `.exe` files are, so Resolume loads them normally; only the installer trips
SmartScreen, once: **More info** → **Run anyway**.

### OpenFX hosts (Resolve, Vegas, Nuke, Natron)

Copy `Afterglow.ofx.bundle` from the `-ofx-` download into the OpenFX folder and restart the host:

```
macOS    /Library/OFX/Plugins/
Windows  C:\Program Files\Common Files\OFX\Plugins\
```

Same controls, same conversions, same decay model, so a look set up in one reads the same in the
other. Two differences worth knowing:

- **The OpenFX build fetches its history rather than remembering it.** Slot 3 is the clip at frame
  `t − 3`, not "whatever the third-last frame the host sent happened to be". That makes it exact
  and repeatable: rendering one frame in isolation gives the same picture as rendering the whole
  timeline up to it. It is also slower, because every ghost is a real source fetch.
- **The first frames of a clip are short of history**, for the same reason — there is nothing
  before the start to fetch. The trail builds up over the first Frames × Hold frames.

There is no **Resolution** control in the OpenFX build. It exists in the FFGL build to bound video
memory, and an offline host does not need it.

---

## Start here

Put Afterglow on a layer with something moving on it and leave every control alone. You should see
the picture with a soft, slightly desaturated trail behind whatever moved, and a little warm glow
on the highlights.

If the shot is static, **nothing happens** — and that is correct. A picture that is not moving goes
through this effect untouched, on purpose, so it can be left switched on across a whole show.

Then reach for **Frames**. That is the control the whole effect hangs off.

---

## The trail

**Frames** — how many pictures are in the queue, from 4 to 32. Short is a tight double-image;
long is a smear. It is also the render cost and the memory cost, more or less linearly.

**Hold** — how many of the host's frames each queue slot covers, from 1 to 8. The trail spans
Frames × Hold frames, so a dozen ghosts at Hold 4 covers about two seconds at 25fps. This costs
nothing: holding is not rendering the same thing more often, it is *capturing* less often. Reach
for it before you reach for a longer queue.

**Decay** — how the weight falls away along the trail. Below the middle it hangs on and drops away
late, which is long, even and filmic. Above the middle it falls off immediately and only the last
few frames read at all, which is the tight double-image an interlaced monitor gives.

**Blend** — how the ghosts combine.

| | What it does | Leaves a still picture alone? |
| --- | --- | --- |
| **Blend** | The weighted average: the queue interpolated into one picture. | Yes |
| **Add** | Summed. Identical to Blend on opaque footage; differs where there is alpha. | Yes |
| **Lighten** | The brightest ghost wins at each pixel. Hard-edged trails. | Yes |
| **Screen** | Light accumulating. Brightens wherever the ghosts differ. | No — it lifts |

Blend is the default because it is the one that is invisible on a static shot. Screen is the one to
reach for when you want the trail to *glow* rather than to smear.

**Gain** — the trail's level against the background. It goes to 2, which clips on purpose. Below 1
it fades the trail back towards whatever is behind it.

**Resolution** — what the queue is stored at: Full, Half, Quarter or Eighth. This is a **memory**
control. Thirty-two frames of 4K is about a gigabyte of video memory; at Quarter it is sixty-six
megabytes. The live frame is always full resolution whatever this says, so turning it down costs
sharpness in the *trail* and nowhere else — but on a static shot the trail is most of what you are
looking at, which is why it defaults to Full.

---

## The decay

Every control here describes what has happened to a frame by the time it reaches the **end** of the
queue. Each frame gets that much of it in proportion to its own age, so a long queue spreads the
same setting out over more steps.

**Crush** — the bit depth collapsing, 8 bits down to 1. Watch it on a gradient: the banding walks
up the trail.

**Pixelate** — the grid coarsening, down to six cells across the frame. The cells stay square and
stay put while the ghost drifts underneath them.

**Warp** — a noise field pulling the ghost apart. **Warp Scale** sets how big the cells of that
field are — low is a slow swell that moves the whole ghost, high is boiling grain — and **Warp
Speed** sets how fast it boils. The field has time as a third axis, so it boils rather than sliding
past; a warp that only slides reads as the ghost being *pushed* by something, which is a different
effect.

**Drift** and **Direction** — how far the oldest ghost has travelled, up to a quarter of the frame,
and which way. Both axes are measured in picture *widths*, so the same setting covers the same
distance whichever way it points.

**Zoom** — the middle is off. Below it the trail recedes into the picture; above it the trail grows
out past the edges. A small negative Zoom with a long queue is the classic tunnel.

**Spin** — the middle is off. About the centre of the picture, in a square space, so it is a
rotation on a 16:9 frame and not a shear.

**Hue Shift** — the middle is off. At the ends the oldest ghost is the complement of the picture,
which is the point at which a trail stops reading as the same image and starts reading as a second
one.

**Bleach** — the saturation draining out. On a little by default, because a trail that has visibly
*aged* is the whole point.

---

## Halation

**Halation** — highlights spilling off the ghosts. It is weighted to peak in the *middle* of the
queue rather than at either end: the newest frame has not aged into anything, and the oldest is not
there any more. What blooms is a ghost on its way out — which is also the only place a glow can be
seen, because added on top of a frame at full strength it is invisible.

**Halation Size** — the radius, as a fraction of the picture width. Two Gaussians of different
widths are summed, which is what gives a highlight a tight core and a wide falloff instead of one
soft blob.

**Halation Tint** — neutral to amber. Amber is the colour a real halation ring is: it is red light
scattering back off the film base, and the emulsion has stopped the blue first.

Halation is the most expensive thing in the plugin — it is a second pass over the whole queue plus
four blurs. At zero it is skipped entirely.

---

## Output

**Background** — what is behind the trail.

- **Source** — the clip, with the trail over it. The default.
- **Black** — the trail on black.
- **Transparent** — the trail over nothing, for stacking on a layer below.
- **Ghosts** — the trail with the live frame left *out* of the queue, on black. This is as much a
  tool as a look: it is how to see what the decay controls are actually doing, rather than what
  they look like underneath a sharp copy of the picture. Set the decay with Ghosts, then switch
  back.

**Mix** — dry/wet against the untouched clip. Mix at zero is the null.

**Preset** — eight factory looks. Picking one sets the trail, decay and halation controls; moving
any of them afterwards drops the dropdown back to Custom. Mix and Resolution are never touched by a
preset — the first is your way of pulling any of it back, and the second is a statement about your
machine rather than about the picture.

| Preset | What it is |
| --- | --- |
| Clean Echo | The plain article: a trail with nothing done to it but time. |
| Slow Burn | A long queue held four frames to a slot, falling away late, warm halation. |
| Datamosh | Short queue, bit depth and grid collapsing over half a dozen frames, added. |
| Undertow | The trail sinking backwards into the frame as it goes. |
| Spin Cycle | Each ghost turned and shifted a little further round the wheel. |
| Bad Tape | Warp, crush and bleach — the trail as a tape that has been through the machine too often. |
| Phosphor | Long, soft, entirely halation: the picture on a tube that has not let go of it. |
| Ghost Print | The decay on its own, with the live frame left out. |

---

## If it looks wrong

**Nothing happens.** Is anything moving? A static shot is meant to come out untouched. Try
Background → Ghosts, which shows the trail on its own.

**The picture went bright and washy.** Blend is on Screen. Screen accumulates light by design; use
Blend or Add if you want the effect to be invisible when nothing moves.

**The trail flickers or steps.** Frames is very short, or Hold is high. Both make the queue
coarse in time; a longer queue at a lower Hold covers the same span more smoothly.

**Everything got soft.** Resolution is not on Full. The ghosts are stored reduced, and on a static
shot the ghosts are most of the picture.

**The trail is chunky in a way you did not ask for.** Zoom is pulled a long way negative, which
minifies the ghosts. That is the one place this effect resamples down.

**It runs out of video memory.** Frames × the picture × 4 bytes, divided by the square of the
Resolution divisor. Thirty-two 4K frames at Full is about a gigabyte. Drop Resolution first — it
costs sharpness only in the trail.

---

## Diagnostics

The plugin writes a small log, and it is worth looking at exactly twice: when the effect does
nothing at all, and when the picture goes black on a big canvas.

```
macOS    ~/Library/Logs/afterglow/afterglow.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\afterglow\logs\afterglow.YYYY-MM-DD.log
```

It records the GL vendor and version at load, which shader failed to compile if one did, and the
size of the frame queue whenever it is rebuilt — including the megabytes, which is the number to
look at if a queue could not be allocated.
