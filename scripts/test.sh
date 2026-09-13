#!/usr/bin/env bash
# scripts/test.sh — build (if needed) and run the full bootamp test suite.
#
# One command to configure + build + run the suite. Self-sufficient: when
# the build directory has no CMakeCache.txt yet, it configures first.
#
#   ./scripts/test.sh                     # configure (if needed) + build + all tests
#   ./scripts/test.sh --no-build          # skip cmake --build, just run tests
#   ./scripts/test.sh --ctest             # run through ctest (supports -R/-j)
#   ./scripts/test.sh -j N                # up to N tests in parallel (default 8)
#   ./scripts/test.sh -t N                # per-test timeout in seconds (default 90)
#
# Default runs the registered test executables directly, up to -j at a time.
# Each test writes to its OWN temp file (a first parallel attempt streamed
# every test through `sed` pipes into the shared stdout and all eight died of
# SIGPIPE/141 — pipes are gone entirely, a test can only ever fail on its own
# merits). When every job has exited, the logs are replayed in order, each
# line prefixed with its test's name, followed by a [name] PASS/FAIL/TIMEOUT
# line. A per-test `timeout -t` bounds each run (SIGTERM, then SIGKILL after
# 5 s), so a hung test reports TIMEOUT while the rest of the suite keeps going
# instead of wedging the whole run. Temp files are wiped by a trap on exit.
#
# Exit: 0 = all passed; 123 = one or more tests failed or timed out (the
# individual [name] PASS/FAIL/TIMEOUT lines say which, with exit codes).
#
# ctest buffers a whole test's output until process exit (Catch2 #2964), so
# abort banners are lost under it; use --ctest to get ctest's own behavior.
#
# The build directory is "$BOOTAMP_BUILD_DIR" if set, else ./build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BOOTAMP_BUILD_DIR:-$ROOT/build}"
# A relative BOOTAMP_BUILD_DIR resolves against the repo root, not the CWD.
case "$BUILD" in
  /*) ;;
  *)  BUILD="$ROOT/$BUILD" ;;
esac

no_build=false
ctest_mode=false
jobs=8
test_timeout=90
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) no_build=true ;;
    --ctest)    ctest_mode=true ;;
    -j)         shift; jobs="$1" ;;
    -j[0-9]*)   jobs="${1#-j}" ;;
    -t)         shift; test_timeout="$1" ;;
    -t[0-9]*)   test_timeout="${1#-t}" ;;
    *)          args+=("$1") ;;
  esac
  shift
done
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "test.sh: invalid -j value '$jobs' (positive integer expected)" >&2
  exit 1
fi
if [[ ! "$test_timeout" =~ ^[1-9][0-9]*$ ]]; then
  echo "test.sh: invalid -t value '$test_timeout' (positive integer expected)" >&2
  exit 1
fi

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
if [[ "$ctest_mode" == true ]]; then
  ctest --test-dir "$BUILD" --output-on-failure "${args[@]+"${args[@]}"}" || status=$?
else
  # Enumerate the registered test executables via ctest -N (name == binary
  # name in the build dir), then run them directly.
  mapfile -t tests < <(ctest --test-dir "$BUILD" -N 2>/dev/null |
    sed -n 's/^[[:space:]]*Test[[:space:]]*#*[0-9]*:[[:space:]]*//p')
  if [[ ${#tests[@]} -eq 0 ]]; then
    echo "test.sh: could not enumerate tests (ctest -N) — falling back to ctest" >&2
    ctest --test-dir "$BUILD" --output-on-failure "${args[@]+"${args[@]}"}" || status=$?
  else
    tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/bootamp-test.XXXXXX")"
    trap 'rm -rf "$tmpdir"' EXIT
    echo "test.sh: ${#tests[@]} tests, ${jobs} parallel, ${test_timeout}s per test"
    i=0
    pids=()
    for t in "${tests[@]}"; do
      i=$((i + 1))
      # Own temp file, no pipes: a test can SIGPIPE only by its own doing.
      (
        timeout -k 5 "$test_timeout" "$BUILD/$t" > "$tmpdir/$i.log" 2>&1
        echo "$?" > "$tmpdir/$i.status"
      ) &
      pids+=("$!")
    done
    for pid in "${pids[@]}"; do
      # wait returns the job's exit code — a test killed by a signal (e.g. a
      # segfault, 128+11) would trip `set -e`, abort the whole run mid-way and
      # let the EXIT trap rm the temp dir from under the still-running jobs
      # (their "…/N.status: No such file" noise). The per-test code is already
      # captured in the status file — ignore wait's own status.
      wait "$pid" || true
    done
    for i in $(seq 1 "${#tests[@]}"); do
      t="${tests[$((i - 1))]}"
      s="$(cat "$tmpdir/$i.status" 2>/dev/null || echo 1)"
      echo "== $t"
      if [[ -s "$tmpdir/$i.log" ]]; then
        sed "s/^/[$t] /" "$tmpdir/$i.log"
      fi
      if [[ "$s" -eq 0 ]]; then
        echo "[$t] PASS"
      elif [[ "$s" -eq 124 ]]; then
        echo "[$t] TIMEOUT (killed after ${test_timeout}s)"
        status=123
      else
        echo "[$t] FAIL (status $s)"
        status=123
      fi
    done
  fi
fi
echo "test.sh: status ${status} (build dir: $BUILD)"
exit "$status"