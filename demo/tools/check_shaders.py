#!/usr/bin/env python3
"""Prove the demo's GLSL is the plugin's GLSL.

The rule the demo pages are built on is that the shader text is *copied*, not
rewritten — `port()` in the kit handles the version line and the precision
qualifiers and nothing else. Nothing enforces that: `plugin.js` cannot include a
C++ file, so the two copies are two files that happen to agree, and a change to
`source/Shaders.cpp` that is not mirrored here is invisible until somebody
notices the demo behaving differently from the plugin.

This reads both and compares them character for character.

    python3 demo/tools/check_shaders.py

Exit status is 0 when every pass matches, 1 otherwise, so it can go in
`tools/verify.sh`. The only edits it allows for are the two JavaScript template
literal escapes — a backslash before a backtick or a `${` — which are required
by the file format and change no GLSL.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHADERS_CPP = ROOT / "source" / "Shaders.cpp"
PLUGIN_JS = ROOT / "demo" / "plugin.js"

# C++ name -> JavaScript name. kGhostPreamble/kDecayLibrarySource/kGhostMain
# are the three pieces GhostShaderSource() concatenates; the demo concatenates
# the same three in the same order, so they are checked individually rather
# than as one assembled string — a drift is then attributed to the piece it is
# in rather than to "the ghost pass".
#
# kDecayLibrarySource is the one that matters most. It is the per-pixel half of
# Decay.cpp transcribed into GLSL, so it is ALREADY a second copy of something;
# this page makes it a third, and only this check keeps all three in step.
PASSES = {
    "kVertexShader": "VERTEX_SHADER",
    "kCopyShader": "COPY_SHADER",
    "kDecayLibrarySource": "DECAY_LIBRARY",
    "kGhostPreamble": "GHOST_PREAMBLE",
    "kGhostMain": "GHOST_MAIN",
    "kBlurShader": "BLUR_SHADER",
    "kCompositeShader": "COMPOSITE_SHADER",
}


def cpp_literals(text):
    """Every `const char* const kName = R"(...)";` in the file."""
    pattern = re.compile(
        r'const char\* const (\w+)\s*=\s*R"\((.*?)\)";', re.DOTALL
    )
    return {m.group(1): m.group(2) for m in pattern.finditer(text)}


def js_literals(text):
    """Every top-level ``const NAME = `...`;`` in the file."""
    pattern = re.compile(r"^const (\w+) = `(.*?)`;$", re.DOTALL | re.MULTILINE)
    return {m.group(1): m.group(2) for m in pattern.finditer(text)}


def unescape(source):
    """Undo the escapes a template literal forces, and nothing else.

    Order matters: the backslash escape has to come last, or unescaping a
    backtick would leave a lone backslash that the next replacement eats.
    """
    return (
        source.replace("\\`", "`")
        .replace("\\${", "${")
        .replace("\\\\", "\\")
    )


def main():
    cpp = cpp_literals(SHADERS_CPP.read_text(encoding="utf-8"))
    js = js_literals(PLUGIN_JS.read_text(encoding="utf-8"))

    failures = 0

    for cpp_name, js_name in PASSES.items():
        if cpp_name not in cpp:
            print(f"MISSING  {cpp_name} not found in source/Shaders.cpp")
            failures += 1
            continue
        if js_name not in js:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue

        want = cpp[cpp_name]
        got = unescape(js[js_name])

        if want == got:
            print(f"ok       {cpp_name} == {js_name}  ({len(want)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {cpp_name} != {js_name}")

        want_lines = want.splitlines()
        got_lines = got.splitlines()
        for i in range(max(len(want_lines), len(got_lines))):
            a = want_lines[i] if i < len(want_lines) else "<end of file>"
            b = got_lines[i] if i < len(got_lines) else "<end of file>"
            if a != b:
                print(f"           line {i + 1}")
                print(f"           Shaders.cpp: {a!r}")
                print(f"           plugin.js:   {b!r}")
                break

    if failures:
        print(f"\n{failures} pass(es) drifted. The demo is no longer running the plugin's shader.")
        return 1

    print(f"\nall {len(PASSES)} pass(es) identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
