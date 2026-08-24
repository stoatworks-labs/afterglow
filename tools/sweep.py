#!/usr/bin/env python3
"""
No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is
indistinguishable from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

The awkward part is that many controls are conditional: Direction does nothing
unless Drift is up, Halation Size does nothing unless Halation is, Warp Speed
does nothing unless Warp is up AND enough frames pass for the field to move. A
naive sweep reports those as dead and buries the one real failure in five false
ones, so each such parameter carries the context it needs to mean anything.
Those contexts are the interesting content of this file -- if a parameter is
added without one and it turns out to be conditional, this will say so loudly.

    python3 tools/sweep.py [--binary build/agtest]
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all. Without
# these the sweep reports a working control as dead.
#
# An entry beginning with "@" is a HARNESS setting, not a plugin parameter.
# That prefix is not decoration: this plugin has a parameter genuinely called
# "Frames", and the fleet's other sweeps spell the harness's frame count
# "Frames=" with no prefix at all. Without the marker, asking for a longer run
# and asking for a longer queue would be the same string.
CONTEXT = {
    # A held queue only differs from an unheld one once enough host frames have
    # gone by for the holding to have skipped a capture.
    "Hold": ["@frames=60"],
    "Warp Scale": ["Warp=0.6"],
    # The warp field is driven by the clock, so it needs Warp up AND time to
    # pass. The harness drives a synthetic 60fps clock, so 60 frames is one
    # second -- at the bottom of Warp Speed's range that is still nothing,
    # which is the point.
    "Warp Speed": ["Warp=0.6", "@frames=60"],
    "Direction": ["Drift=0.6"],
    "Halation Size": ["Halation=0.6"],
    "Halation Tint": ["Halation=0.6"],
    # The sweep's positions only ever reach preset 0 and preset 1. Preset 1 is
    # "Clean Echo", which is deliberately NOT the plugin's defaults -- see the
    # comment on it in Presets.h. Applied over the defaults it changes Bleach
    # and Halation, so it is visible without a context of its own; this entry
    # is here to make it visible by more than that.
    "Preset": ["Crush=0.6"],
}

# Positions to try. Three rather than two: a control that is a no-op at both
# ends but not in the middle is rare, but the bipolar ones -- Zoom, Spin, Hue
# Shift -- are all NULL in the middle, so 0.0 and 1.0 are the two that have to
# differ and 0.5 is the one that must not be the only sample.
POSITIONS = ["0.0", "0.5", "1.0"]

# Parameters with no scalar float to sweep.
SKIP = set()


def render(binary, out, settings, frames):
    command = [binary, "--out", str(out), "--width", "320", "--height", "180",
               "--frames", str(frames)]
    for setting in settings:
        command += ["--set", setting]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"render failed: {result.stderr.strip()}")
    return hashlib.sha256(out.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/agtest")
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DAFTERGLOW_BUILD_TOOLS=ON first")
        return 2

    listing = subprocess.run([str(binary), "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr.strip())
        return 2

    names = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split(None, 1)
        if len(parts) == 2:
            names.append(parts[1].rsplit(None, 1)[0].strip())

    # The About block is a text field and browser buttons, declared last. They
    # never touch a pixel, so sweeping them only buries a real dead control.
    if "About" in names:
        names = names[: names.index("About")]

    if not names:
        print("no parameters found")
        return 2

    dead = []
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / "sweep.png"

        for name in names:
            if name in SKIP:
                continue
            context = list(CONTEXT.get(name, []))

            # Long enough for a full queue to have been filled with DIFFERENT
            # pictures. The harness's card moves; a run shorter than the queue
            # is a picture of the buffers still warming up, and several
            # controls provably do nothing to a half-filled queue.
            frames = 40
            for entry in list(context):
                if entry.startswith("@frames="):
                    frames = int(entry.split("=", 1)[1])
                    context.remove(entry)

            digests = set()
            for position in POSITIONS:
                try:
                    digests.add(render(binary, out, context + [f"{name}={position}"], frames))
                except RuntimeError as error:
                    print(f"  {name}: {error}")
                    dead.append(name)
                    break
            else:
                mark = "ok" if len(digests) > 1 else "DEAD"
                if len(digests) == 1:
                    dead.append(name)
                print(f"  {mark:4}  {name}")

    print()
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    swept = len([n for n in names if n not in SKIP])
    print(f"all {swept} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
