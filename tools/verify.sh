#!/usr/bin/env bash
#
# Everything that can fail, checked here rather than after a tag.
#
#   usage: tools/verify.sh [build-dir]
#
# The general lesson this file exists for: a check that only ever runs in CI,
# after a tag, is a check that will catch you after the tag. Anything the
# release job does that can be done locally is done here, where it costs a
# second.
#
#   glslc         every shader compiles, before a host has to find out
#   cmtest        the model and the rendered picture hold their invariants
#   sweep.py      no control is dead
#   ffgltest      the bundle instantiates and actually changes a frame
#   plist         CFBundleExecutable names the binary that is really there
#   lipo          is the macOS build really universal
#   nm            does the bundle export the entry point a host resolves
#
set -uo pipefail

build="${1:-build}"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

failures=0
skipped=0

section() { printf '\n\033[1m%s\033[0m\n' "$1"; }
pass()    { printf 'ok   %s\n' "$1"; }
skip()    { printf '\033[33mskip %s\033[0m\n' "$1"; skipped=$(( skipped + 1 )); }
fail()    { printf '\033[31mFAILED: %s\033[0m\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
section "shaders"
#
# Every fragment shader, through a real GLSL compiler, before a host has to
# discover the problem. A shader that will not compile presents to an operator
# as "the effect does nothing", with the real message only in the log -- so
# catching it here is worth the dependency being optional.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout(location) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#---------------------------------------------------------------------------
if command -v glslc >/dev/null 2>&1; then
	scratch="$(mktemp -d)"
	python3 - "$scratch" <<'PY'
import re, sys, pathlib
out = pathlib.Path(sys.argv[1])
src = {}
for f in ["source/shaders/Vertex.cpp", "source/shaders/Common.cpp", "source/shaders/Passes.cpp"]:
    text = pathlib.Path(f).read_text()
    for m in re.finditer(r'const char\* const (\w+)\s*=\s*R"\((.*?)\)";', text, re.S):
        src[m.group(1)] = m.group(2)
common = src.pop("kCommon")
for name, body in src.items():
    body = body.replace("@COMMON@", common)
    (out / (name + (".vert" if name == "kVertex" else ".frag"))).write_text(body)
PY
	shader_failures=0
	for shader in "$scratch"/*.vert "$scratch"/*.frag; do
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$scratch/err"; then
			fail "$(basename "$shader") does not compile"
			sed 's/^/       /' "$scratch/err"
			shader_failures=$(( shader_failures + 1 ))
		fi
	done
	[ "$shader_failures" -eq 0 ] && pass "all shaders compile"
	rm -rf "$scratch"
else
	skip "glslc not installed (brew install shaderc) -- shaders unchecked until a host loads them"
fi

#---------------------------------------------------------------------------
section "build"
#---------------------------------------------------------------------------
if [ ! -x "$build/cmtest" ]; then
	fail "no harness at $build/cmtest -- cmake --build $build"
else
	pass "harness present"
fi

#---------------------------------------------------------------------------
section "the model and the picture"
#
# The first three need no GL at all and are where the modelling errors get
# caught. The rest render through the real plugin class.
#---------------------------------------------------------------------------
if [ -x "$build/cmtest" ]; then
	for check in --flat --roundtrip --detector --transparent --anisotropy --presets; do
		if "$build/cmtest" "$check" >/dev/null 2>&1; then
			pass "cmtest $check"
		else
			fail "cmtest $check"
			"$build/cmtest" "$check" 2>&1 | sed 's/^/       /' | tail -20
		fi
	done
fi

#---------------------------------------------------------------------------
section "parameter names"
#
# FFGL hands the host a 16-character buffer and the SDK does not enforce it: the
# plugin stores the full name, gives the host a pointer to all of it, and
# Resolume copies sixteen bytes. Six plugins in this fleet shipped a control
# labelled "Background Opaci" before anything noticed.
#---------------------------------------------------------------------------
if [ -x "$build/cmtest" ]; then
	if "$build/cmtest" --list | grep -q "WARNING"; then
		fail "a parameter name is longer than 16 characters"
		"$build/cmtest" --list | grep -B1 "WARNING" | sed 's/^/       /'
	else
		pass "every parameter name fits in 16 characters"
	fi
fi

#---------------------------------------------------------------------------
section "dead controls"
#---------------------------------------------------------------------------
if [ -x "$build/cmtest" ]; then
	if python3 tools/sweep.py >/dev/null 2>&1; then
		pass "no dead controls"
	else
		fail "a control does not reach the picture"
		python3 tools/sweep.py 2>&1 | grep -E "DEAD|dead" | sed 's/^/       /'
	fi
fi

#---------------------------------------------------------------------------
section "the bundle"
#
# ffgltest loads the built bundle the way a host does -- dlopen, plugMain,
# instantiate, push every declared default back through the setters, render --
# and reports how much of the frame changed.
#
# ⚠️ "0 bytes differ" is a FAILURE here, not a pass. It is what a plugin whose
# declared defaults are all zero looks like, and that is exactly what this
# plugin did until ffgltest said so: SetParamInfof reads its default back out of
# GetFloatParameter, so a params[] filled after the declarations tells the host
# every control is zero -- including Mix.
#---------------------------------------------------------------------------
bundle="$build/Compander.bundle"
ffgltest="$root/../resolume-ofx-bridge/build/ffgltest"

if [ ! -d "$bundle" ]; then
	fail "no bundle at $bundle"
elif [ ! -x "$ffgltest" ]; then
	skip "ffgltest not built (../resolume-ofx-bridge) -- the bundle is unchecked as a bundle"
else
	output="$("$ffgltest" "$bundle" 2>&1)"
	if ! grep -q "instantiated ok" <<<"$output"; then
		fail "the bundle does not instantiate"
		sed 's/^/       /' <<<"$output"
	elif grep -qE "^ *0 of [0-9]+ bytes differ" <<<"$output"; then
		fail "the bundle renders its input unchanged -- check the declared defaults"
		sed 's/^/       /' <<<"$output"
	else
		pass "the bundle instantiates and changes the frame ($(grep -oE '[0-9]+ of [0-9]+ bytes differ' <<<"$output"))"
	fi
fi

#---------------------------------------------------------------------------
section "the entry point"
#
# FFGL hosts resolve plugMain by symbol name after dlopen. A bundle that builds,
# links and installs but does not export it loads and reports that it contains
# no plugins.
#---------------------------------------------------------------------------
binary="$bundle/Contents/MacOS/Compander"
if [ -f "$binary" ]; then
	if nm -gU "$binary" 2>/dev/null | grep -q "_plugMain"; then
		pass "exports plugMain"
	else
		fail "does not export plugMain"
	fi
fi

#---------------------------------------------------------------------------
section "universal"
#
# The build log says "success" for an arm64-only binary just as loudly as for a
# universal one, and an Intel Resolume simply will not see it. lipo is the only
# honest answer. Skipped when the developer asked for a single-architecture dev
# build on purpose.
#---------------------------------------------------------------------------
if [ -f "$binary" ]; then
	arches="$(lipo -archs "$binary" 2>/dev/null)"
	if [[ "$arches" == *arm64* && "$arches" == *x86_64* ]]; then
		pass "universal ($arches)"
	else
		skip "$arches only -- a dev build. What ships must be built without -DCMAKE_OSX_ARCHITECTURES"
	fi
fi

#---------------------------------------------------------------------------
section "the OpenFX bundle"
#
# cmake/InfoOFX.plist.in is copied from repo to repo, and the version it is
# usually copied from had the PREVIOUS plugin's name hardcoded into
# CFBundleExecutable. That does not fail the build: the bundle assembles, lipo
# and nm both pass, ofxprobe loads it and renders a correct frame. It fails at
# RELEASE time, in codesign, with "code object is not signed at all / In
# subcomponent: .../Contents/MacOS/<name>.ofx" -- because codesign reads the
# plist, looks for an executable that is not there, and treats the real binary
# as a nested object that should have been signed first. Nothing in that message
# mentions the plist.
#
# So: check the plist against the binary on disk, and run the exact codesign the
# release job runs, against a COPY.
#---------------------------------------------------------------------------
ofx="$build/Compander.ofx.bundle"
if [ ! -d "$ofx" ]; then
	skip "no OpenFX bundle (built with -DBUILD_OFX=OFF, or not yet implemented)"
else
	plist="$ofx/Contents/Info.plist"
	named="$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$plist" 2>/dev/null)"

	if [ ! -f "$ofx/Contents/MacOS/$named" ]; then
		fail "CFBundleExecutable is '$named' but that binary is not in the bundle"
	else
		scratch="$(mktemp -d)/Compander.ofx.bundle"
		cp -R "$ofx" "$scratch"
		if codesign --force --sign - --timestamp=none "$scratch" >/dev/null 2>&1; then
			pass "CFBundleExecutable is $named, and the bundle ad-hoc signs"
		else
			fail "the OpenFX bundle will not codesign"
			codesign --force --sign - --timestamp=none "$scratch" 2>&1 | sed 's/^/       /'
		fi
		rm -rf "$(dirname "$scratch")"
	fi

	# And it must actually render. ofxprobe loads the bundle the way a host
	# does and reports how much of the frame changed; 0 bytes differing here
	# means the same class of failure it means for the FFGL bundle.
	ofxprobe="$root/../resolume-ofx-bridge/build/ofxprobe"
	if [ ! -x "$ofxprobe" ]; then
		skip "ofxprobe not built (../resolume-ofx-bridge) -- the OpenFX render is unchecked"
	else
		out="$(mktemp -d)/ofx.bmp"
		result="$("$ofxprobe" --dir "$build" --render com.stoatworks.compander \
		          --size 640x360 --out "$out" 2>&1)"
		if ! grep -q "rendered" <<<"$result"; then
			fail "the OpenFX bundle does not render"
			sed 's/^/       /' <<<"$result"
		elif grep -qE "^ *0 of [0-9]+ bytes differ" <<<"$result"; then
			fail "the OpenFX bundle renders its input unchanged"
			sed 's/^/       /' <<<"$result"
		else
			pass "the OpenFX bundle renders ($(grep -oE '[0-9]+ of [0-9]+ bytes differ' <<<"$result"))"
		fi
		rm -rf "$(dirname "$out")"
	fi
fi

#---------------------------------------------------------------------------
printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m (%d skipped)\n' "$skipped"
	exit 0
fi
printf '\033[31m%d check(s) failed\033[0m (%d skipped)\n' "$failures" "$skipped"
exit 1
