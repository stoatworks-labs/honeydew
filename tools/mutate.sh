#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# 320x180.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
MUTANTS=(
	"source/Shaders.cpp|return rate * vec3( ( q * s.y - s.x * s.y + s.x * ( 1.0 - s.x ) ) / eps,|return rate * vec3( ( q * s.y - s.x * s.y + s.x * ( 1.0 + s.x ) ) / eps,|--oregonator|GLSL: the Oregonator's autocatalysis unbounded, 1.0 - x -> 1.0 + x"
	"source/Shaders.cpp|float T = exp( -2.302585092994046 * A * depth );|float T = exp( -2.302585092994046 * A + depth );|--beer|GLSL: Beer-Lambert's depth added instead of multiplied, * -> +"
	"source/Shaders.cpp|I2 -= x;|I2 += x;|--clock|GLSL: the thiosulfate makes iodine instead of taking it, -= -> +="
	"source/Transport.h|while( wait < lead )|while( wait > lead )|--transport|C++: the beat lead skips every boundary but the first, < -> >"
	"source/Chemistry.cpp|return -std::log( 1.0 - S0 / ( 2.0 * H0 ) ) / ( ClockRateConstant( H ) * I0 );|return -std::log( 1.0 - S0 / ( 2.0 + H0 ) ) / ( ClockRateConstant( H ) * I0 );|--clock|C++: the closed form the check holds the plugin to, * -> + (a wrong reference is caught too)"
)

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target hdtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	if "$tree/build/hdtest" "$check" --size 320x180 >"$WORK/log" 2>&1; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		grep -E '^  FAIL' "$WORK/log" | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
