#!/usr/bin/env bash
# scripts/test.sh — build (if needed) and run the full bootamp test suite.
#
# One command to configure + build + run the suite. Self-sufficient: when
# the build directory has no CMakeCache.txt yet, it configures first.
#
#   ./scripts/test.sh                     # configure (if needed) + build + all tests
#   ./scripts/test.sh --no-build         # skip cmake --build, just run tests
#   ./scripts/test.sh --ctest            # run through ctest (supports -R/-j)
#
# Default runs each test executable directly, one after another, with its
# output streaming to the console LIVE. ctest buffers a whole test's output
# until the process exits (Catch2 #2964) — a malloc/SIGABRT abort then
# swallows everything printed since the last flush, and a hanging test
# produces no output at all. Direct execution makes stdout the terminal
# (line-buffered): you see where the suite is, and nothing is lost on abort.
# Use --ctest to get ctest's own behavior (its -R regex / -j flags).
# The build directory is "$BOOTAMP_BUILD_DIR" if set, else ./build. exit 0
# = all tests passed (non-zero = the last failing test's exit code).
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
if [[ " ${args[*]:-} " == *" --ctest "* ]]; then
  ctest --test-dir "$BUILD" --output-on-failure "${args[@]+"${args[@]}"}" || status=$?
else
  # Enumerate the registered test executables via ctest -N (name == binary
  # name in the build dir), then run each directly, streaming its output.
  mapfile -t tests < <(ctest --test-dir "$BUILD" -N 2>/dev/null |
    sed -n 's/^[[:space:]]*Test[[:space:]]*#*[0-9]*:[[:space:]]*//p')
  if [[ ${#tests[@]} -eq 0 ]]; then
    echo "test.sh: could not enumerate tests (ctest -N) — falling back to ctest" >&2
    ctest --test-dir "$BUILD" --output-on-failure "${args[@]+"${args[@]}"}" || status=$?
  else
    for t in "${tests[@]}"; do
      echo "== $t"
      "$BUILD/$t" || status=$?
    done
  fi
fi
echo "test.sh: status ${status} (build dir: $BUILD)"
exit "$status"
