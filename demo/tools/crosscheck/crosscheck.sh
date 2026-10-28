#!/usr/bin/env bash
#
# The browser demo's JavaScript port of the CPU half, against the plugin's C++.
#
#     demo/tools/crosscheck/crosscheck.sh        exit 1 on any difference
#
# printer.cpp drives the plugin's own GL-free model -- Controls.cpp,
# Compander.cpp, Detector.cpp, Chain.cpp's crossoverFor, HostValues and the
# preset rows -- through a fixed scenario: the constructor's defaults, every
# slider position the page can produce through every conversion, the settings
# and both 128-point gain tables for the defaults, every preset and a 384-case
# grid of the controls that shape them, and the detector's pass plan, release
# coefficient, decays, the frame follower's time constant and the band split
# across Time Constant at seven widths. run.mjs runs the PORT block of
# demo/plugin.js through the same and compare.py holds the two together.
#
# What has to agree, and why it is measured rather than assumed:
#
#   - THE ARITHMETIC THE PORT CAN DO. Apple clang fuses a*b+c into one rounding
#     on arm64 (-ffp-contract=on is its default), which JavaScript cannot do, so
#     the plugin's Apple Silicon slice rounds differently from its x86_64 slice,
#     whose baseline has no FMA. The checked comparison is against the C++ built
#     -ffp-contract=off -- the x86_64 slice's arithmetic -- and must agree to 2
#     ulp, the decays aside (compare.py says why they are allowed their power).
#     The native build is compared too, and reported, never failed.
#   - THE LIBM. V8's Math and Apple's libm round some results to the other
#     neighbour; that is the 1 and 2 ulp in the report.
#   - IT CAN FAIL. Two broken ports (the active line as 52.5 us; the gain law's
#     knee 5 dB wide) must both fail the same comparison.
#
# The pictures are checked separately, by pixels.mjs, against the real plugin
# class: see tools/verify.sh.
#
# Run by tools/verify.sh. Needs node and a C++ compiler.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
CXX="${CXX:-c++}"

command -v node >/dev/null 2>&1 || { echo "crosscheck: node is not installed"; exit 2; }
command -v "$CXX" >/dev/null 2>&1 || { echo "crosscheck: no C++ compiler ($CXX)"; exit 2; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

model=( "$REPO/source/Controls.cpp" "$REPO/source/Compander.cpp" "$REPO/source/Detector.cpp" "$REPO/source/Chain.cpp" )
"$CXX" -std=c++17 -O3 -DNDEBUG -ffp-contract=off -I "$REPO/source" "$HERE/printer.cpp" "${model[@]}" -o "$tmp/printer"
"$CXX" -std=c++17 -O3 -DNDEBUG -I "$REPO/source" "$HERE/printer.cpp" "${model[@]}" -o "$tmp/printer-native"

"$tmp/printer" > "$tmp/cpp.txt"
"$tmp/printer-native" > "$tmp/cpp-native.txt"
node "$HERE/run.mjs" > "$tmp/js.txt"

failures=0

echo "the port against the plugin's C++ (-ffp-contract=off, the x86_64 slice's arithmetic):"
if python3 -I "$HERE/compare.py" "$tmp/cpp.txt" "$tmp/js.txt"; then
	echo "ok    the port agrees"
else
	echo "FAIL  the port has drifted from the plugin's C++"
	failures=$(( failures + 1 ))
fi

echo
echo "for information, against the native build (arm64 fuses multiply-adds; never failed):"
python3 -I "$HERE/compare.py" "$tmp/cpp-native.txt" "$tmp/js.txt" --max-ulps 1000000 | grep -v '^FAIL' || true

echo
echo "negative controls (each must FAIL):"
for wrong in active-line knee; do
	node "$HERE/run.mjs" --wrong "$wrong" > "$tmp/wrong.txt"
	if python3 -I "$HERE/compare.py" "$tmp/cpp.txt" "$tmp/wrong.txt" > "$tmp/wrong.log"; then
		echo "FAIL  a port with --wrong $wrong passed: the comparison cannot fail"
		failures=$(( failures + 1 ))
	else
		echo "ok    caught --wrong $wrong ($(grep -m1 'worst (decays aside)' "$tmp/wrong.log" | sed 's/^ *//'))"
	fi
done

echo
if [ "$failures" -eq 0 ]; then
	echo "the port of the CPU half agrees with the plugin's C++, and both broken ports were caught"
	exit 0
fi
echo "$failures failure(s)"
exit 1
