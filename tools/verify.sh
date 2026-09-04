#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Seven things get checked, and they fail in different ways:
#
#   --still    that footage which is not moving comes out exactly as it went
#              in. This is the one that matters. It is the invariant the whole
#              weight schedule exists to hold up, it is the one an operator
#              notices first, and it has already caught a real defect: Add was
#              handed the incremental alphas, which sum to about three over a
#              dozen slots, and a static frame came out 194/255 too bright.
#   --decay    the GLSL copy of the per-pixel decay stage against the C++ one,
#              over every control x four aspect ratios x two noise scales x
#              two phases. The two copies exist because the GPU cannot call
#              C++ and the OpenFX build cannot call GLSL; nothing else
#              notices when they drift apart.
#   --presets  that every factory preset survives every host behaviour. No GL:
#              this is the parameter plumbing, and it is the half an external
#              user actually got stuck on (vertigo issue #2).
#   sweep.py   that no control is silently dead. A GLSL uniform whose name
#              does not match the C++ is ignored without a word, so this is
#              the only thing standing between a typo and a shipped slider
#              that does nothing.
#   the demo   TWO checks, because the demo copies half of this repo and ports
#              the other half. check_shaders.py proves demo/plugin.js still
#              holds this repo's shader text character for character;
#              check_decay.mjs proves demo/decay.js still computes what
#              Controls.cpp and Decay.cpp compute. Until the second one
#              existed, a drifted conversion or weight curve produced a page
#              that looked plausible, ran without an error and did not behave
#              like the plugin.
#   the OFX
#   bundle     that Info.plist names the binary that is really there, and that
#              the bundle ad-hoc signs. Both are release-time failures with
#              no local symptom at all -- see below.
#   the binary that the macOS build is universal and still exports plugMain.
#              Checked with lipo and nm rather than by reading the build log,
#              because an arm64-only build logs as a success.
#
#   --bench    the render cost. Not pass/fail -- there is no threshold worth
#              asserting on somebody else's GPU -- but a verify run leaves a
#              timing on the record, which is what turns "it feels slower"
#              into a comparison.
#
# The decay check reports a tolerance rather than demanding equality, and that
# is not a fudge: the GLSL specification allows three units in the last place
# for exp and gives sin no accuracy requirement at all, so the two
# implementations cannot agree bit for bit and a test that insisted would fail
# on every driver. See kDecayTolerance in tools/agtest/main.cpp.
#
# ---------------------------------------------------------------------------
# Why the OpenFX bundle is checked here rather than only in the release job
#
# cmake/InfoOFX.plist.in is one of the files a new plugin repo starts life by
# copying, and the version downpour had spelled the PREVIOUS plugin's name out
# in CFBundleExecutable. NOTHING caught it: the bundle assembles, the binary is
# correct, nm finds _OfxGetPlugin, and ofxprobe loads it and renders a correct
# frame. It fails at RELEASE time, in codesign, with a message about a
# "subcomponent" that never mentions the plist -- which is after the tag.
#
# So the check is the release step itself, run here where it costs a second.
# The general lesson, and it is the reason this file exists at all: anything
# the release job does that can be done locally should be done locally.
# ---------------------------------------------------------------------------
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD=build

if [[ ! -x $BUILD/agtest ]]; then
	echo "$BUILD/agtest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

failures=()

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors GhostShaderSource() and DecayProbeShaderSource() in Shaders.cpp.
ASSEMBLED = {
	"GhostShader":      [ "kGhostPreamble", "kDecayLibrarySource", "kGhostMain" ],
	"DecayProbeShader": [ "#version 410 core\n", "kDecayLibrarySource", "probeMain" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

echo "== shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures+=("shaders")
fi
echo

echo "== still: a picture that does not move is not touched"
if ./$BUILD/agtest --still --width 320 --height 180; then
	:
else
	failures+=("still")
fi

echo
echo "== decay: GLSL against C++"
if ./$BUILD/agtest --decay | tail -2; then
	:
else
	failures+=("decay")
fi

echo
echo "== presets: every factory preset survives every host behaviour"
if ./$BUILD/agtest --presets | tail -1; then
	:
else
	failures+=("presets")
fi

echo
echo "== sweep: no control silently dead"
if python3 tools/sweep.py > /tmp/afterglow-sweep.txt 2>&1; then
	tail -1 /tmp/afterglow-sweep.txt
else
	echo "   *** dead controls, see /tmp/afterglow-sweep.txt"
	tail -4 /tmp/afterglow-sweep.txt
	failures+=("sweep")
fi

echo
echo "== demo: the browser demo's GLSL against this repo's"
if python3 demo/tools/check_shaders.py | tail -1; then
	:
else
	echo "   *** demo/plugin.js is no longer running the plugin's shader"
	failures+=("demo shaders")
fi

echo
echo "== demo: the browser demo's ported maths against this repo's"
if command -v node >/dev/null 2>&1; then
	if node demo/tools/check_decay.mjs | tail -2; then
		:
	else
		echo "   *** demo/decay.js is no longer running the plugin's maths"
		failures+=("demo maths")
	fi
else
	echo "   skipped: no node"
fi

echo
echo "== OpenFX: the bundle names its own binary, and signs"
if [[ -d "$BUILD/Afterglow.ofx.bundle" ]]; then
	plist="$BUILD/Afterglow.ofx.bundle/Contents/Info.plist"
	named=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)
	if [[ ! -f "$BUILD/Afterglow.ofx.bundle/Contents/MacOS/$named" ]]; then
		echo "   *** Info.plist names \"$named\", which is not in Contents/MacOS"
		failures+=("ofx plist")
	else
		# On a COPY, so a verify run never leaves a signature on the build tree
		# that the release job did not put there.
		scratch="${TMPDIR:-/tmp}/afterglow-signcheck.ofx.bundle"
		rm -rf "$scratch"
		cp -R "$BUILD/Afterglow.ofx.bundle" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			echo "   CFBundleExecutable is $named, and the bundle ad-hoc signs"
		else
			echo "   *** the OpenFX bundle will not codesign"
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
			failures+=("ofx codesign")
		fi
		rm -rf "$scratch"
	fi

	symbols=$( nm -gU "$BUILD/Afterglow.ofx.bundle/Contents/MacOS/$named" 2>/dev/null || true )
	if grep -q '_OfxGetPlugin' <<<"$symbols"; then
		echo "   exports _OfxGetPlugin"
	else
		failures+=("no _OfxGetPlugin export")
	fi
else
	echo "   skipped: no OpenFX bundle. Configure without -DBUILD_OFX=OFF."
fi

echo
echo "== bench: the render cost, for the record"
./$BUILD/agtest --bench --frames 30 2>&1 | sed -n '3,8p'

echo
echo "== binary: universal, and exports plugMain"
bundle="build-universal/Afterglow.bundle/Contents/MacOS/Afterglow"
if [[ -f "$bundle" ]]; then
	architectures="$(lipo -archs "$bundle" 2>/dev/null)"
	echo "   architectures: $architectures"
	[[ "$architectures" == *arm64* && "$architectures" == *x86_64* ]] \
		|| failures+=("not universal: $architectures")

	# Captured, then matched from a herestring -- never `nm ... | grep -q`.
	# Under `set -o pipefail` a `grep -q` that finds its match exits
	# immediately, the writer upstream takes SIGPIPE, and the PIPELINE reports
	# failure even though the symbol is there. It is output-size dependent, so
	# it fires on the bigger binary first and looks intermittent. A herestring
	# is not a pipeline, so nothing can SIGPIPE.
	symbols=$( nm -gU "$bundle" 2>/dev/null || true )
	if grep -q '_plugMain' <<<"$symbols"; then
		echo "   exports _plugMain"
	else
		failures+=("no _plugMain export -- the host will load the bundle and find no plugins")
	fi
else
	echo "   skipped: no universal build. Run:"
	echo "     cmake -B build-universal -DCMAKE_BUILD_TYPE=Release && cmake --build build-universal"
fi

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
