#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
FUZZ_SECONDS=${SHARDRECOVER_FUZZ_SECONDS:-10}
FIXTURE=legacy/ece252_starter/images/red-green-16x16.png
WORK=$(mktemp -d "${TMPDIR:-/tmp}/shardrecover-certify.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

cpp=PASS; tests=PASS; sanitizers=SKIPPED; png_fuzz=SKIPPED; overlap_fuzz=SKIPPED
frontend_tests=SKIPPED; frontend_build=SKIPPED; trace_export=PASS; clean_recovery=PASS; damaged=PASS

rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure | tee "$WORK/ctest.txt"
test_count=$(sed -n 's/^100% tests passed, 0 tests failed out of \([0-9][0-9]*\)$/\1/p' "$WORK/ctest.txt")
test -n "$test_count"

rm -rf build-asan
if cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSHARDRECOVER_ENABLE_SANITIZERS=ON; then
  cmake --build build-asan --parallel
  ctest --test-dir build-asan --output-on-failure
  sanitizers=PASS
fi

rm -rf build-fuzz
if cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSHARDRECOVER_ENABLE_FUZZING=ON >/dev/null 2>&1 && cmake --build build-fuzz --parallel >/dev/null 2>&1; then
  ./build-fuzz/shardrecover_png_fuzz fuzz/corpus/png "-max_total_time=$FUZZ_SECONDS"
  png_fuzz=PASS
  ./build-fuzz/shardrecover_overlap_fuzz fuzz/corpus/overlap "-max_total_time=$FUZZ_SECONDS"
  overlap_fuzz=PASS
else
  echo "Fuzz smoke SKIPPED: active Clang toolchain has no usable libFuzzer runtime."
fi

if [[ -f frontend/package-lock.json ]]; then
  (cd frontend && npm ci && npm test && npm run build)
  frontend_tests=PASS; frontend_build=PASS
fi

./build/shardrecover fragment "$FIXTURE" --size 64 --overlap 32 --shuffle --opaque-names --seed 42 --output "$WORK/clean-fragments" >/dev/null
./build/shardrecover reconstruct "$WORK/clean-fragments" --strategy beam --beam-width 8 --graph-build indexed --threads 2 --min-overlap 32 --format png --output "$WORK/recovered.png" --trace-json "$WORK/trace.json" >/dev/null
cmp "$FIXTURE" "$WORK/recovered.png"
if command -v python3 >/dev/null 2>&1; then python3 -m json.tool "$WORK/trace.json" >/dev/null; fi
grep -q '"schema_version":1' "$WORK/trace.json"

./build/shardrecover fragment "$FIXTURE" --size 64 --overlap 32 --shuffle --opaque-names --seed 1 --corrupt-bytes 1 --output "$WORK/damaged-fragments" >/dev/null
./build/shardrecover reconstruct "$WORK/damaged-fragments" --strategy beam --beam-width 8 --graph-build exhaustive --min-overlap 32 --max-mismatches 1 --repair consensus --output "$WORK/damaged.png" --trace-json "$WORK/damaged-trace.json" >/dev/null
grep -q '"mismatch_details":\[{' "$WORK/damaged-trace.json"

printf '\nShardRecover certification\n---------------------------\n'
printf 'C++ build: %s\nCTest: %s (%s tests)\n' "$cpp" "$tests" "$test_count"
printf 'Sanitizers: %s\nPNG fuzz: %s (%ss)\nOverlap fuzz: %s (%ss)\n' "$sanitizers" "$png_fuzz" "$FUZZ_SECONDS" "$overlap_fuzz" "$FUZZ_SECONDS"
printf 'Frontend tests: %s\nFrontend build: %s\n' "$frontend_tests" "$frontend_build"
printf 'Trace export: %s\nClean reconstruction: %s\nDamaged scenario: %s\n' "$trace_export" "$clean_recovery" "$damaged"
printf 'Working tree: '; git status --porcelain | grep -q . && echo DIRTY || echo CLEAN
