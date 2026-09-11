#!/usr/bin/env bash
# scripts/test.sh — build (if needed) and run the full bootamp test suite.
#
# One command to configure + build + ctest. Self-sufficient: when the build
# directory has no CMakeCache.txt yet, it configures first. Every extra
# argument is forwarded to ctest, so you can filter or parallelize:
#
#   ./scripts/test.sh                     # configure (if needed) + build + all tests
#   ./scripts/test.sh --no-build          # skip cmake --build, just run ctest
#   ./scripts/test.sh -R test_gieres      # run one test executable by regex
#   ./scripts/test.sh -j4                 # run 4 tests concurrently
#
# The build directory is "$BOOTAMP_BUILD_DIR" if set, else ./build. The exit
# code is ctest's exit code (0 = all tests passed). ctest prints its own
# summary, so this script stays quiet about results.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BOOTAMP_BUILD_DIR:-$ROOT/build}"
# A relative BOOTAMP_BUILD_DIR resolves against the repo root, not the CWD.
case "$BUILD" in
  /*) ;;
  *)  BUILD="$ROOT/$BUILD" ;;
esac

# --no-build is consumed here; everything else goes to ctest verbatim.
no_build=false
args=()
for arg in "$@"; do
  if [[ "$arg" == "--no-build" ]]; then
    no_build=true
  else
    args+=("$arg")
  fi
done

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  if [[ "$no_build" == true ]]; then
    echo "test.sh: $BUILD has no CMakeCache.txt — run without --no-build first" >&2
    exit 1
  fi
  cmake -S "$ROOT" -B "$BUILD"
fi

if [[ "$no_build" == false ]]; then
  cmake --build "$BUILD" -j"$(nproc)"
fi

status=0
ctest --test-dir "$BUILD" --output-on-failure "${args[@]+"${args[@]}"}" || status=$?
echo "test.sh: ctest exit ${status} (build dir: $BUILD)"
exit "$status"