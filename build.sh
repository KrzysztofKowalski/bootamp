#!/usr/bin/env bash
# build.sh — bootamp build (C++23 + SIMD, no -ffast-math).
#
# Installs missing deps (pacman + AUR), configures and builds.
# Usage: ./build.sh [--tsan] [--debug] [--clean]
set -euo pipefail
cd "$(dirname "$0")"

# --- 1. Dependencies ---------------------------------------------------------
PACMAN_DEPS=(miniaudio cli11)
AUR_DEPS=(ftxui)

missing_pacman=()
for p in "${PACMAN_DEPS[@]}"; do
  pacman -Q "$p" &>/dev/null || missing_pacman+=("$p")
done
if ((${#missing_pacman[@]})); then
  echo "==> pacman: installing ${missing_pacman[*]}"
  sudo pacman -S --needed "${missing_pacman[@]}"
fi

missing_aur=()
for p in "${AUR_DEPS[@]}"; do
  pacman -Q "$p" &>/dev/null || missing_aur+=("$p")
done
if ((${#missing_aur[@]})); then
  helper=""
  for h in yay paru; do command -v "$h" &>/dev/null && helper="$h" && break; done
  if [[ -z "$helper" ]]; then
    echo "ERROR: no AUR helper (yay/paru) found — install one first, e.g.:" >&2
    echo "  sudo pacman -S --needed base-devel git && git clone https://aur.archlinux.org/yay.git /tmp/yay && cd /tmp/yay && makepkg -si" >&2
    exit 1
  fi
  echo "==> AUR ($helper): installing ${missing_aur[*]}"
  "$helper" -S --needed "${missing_aur[@]}"
fi

# --- 2. Configure + build ---------------------------------------------------
BUILD_DIR=build
CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
for arg in "$@"; do
  case "$arg" in
    --tsan)  CMAKE_ARGS+=(-DBOOTAMP_ENABLE_TSAN=ON) ;;
    --debug) CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE=Debug) ;;
    --clean) rm -rf "$BUILD_DIR" ;;
    *) echo "unknown flag: $arg" >&2; exit 1 ;;
  esac
done

cmake -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

# No fast-fail: -k keeps the build going through errors so one target doesn't
# hide the rest; full output lands in build.log for triage (fresh each run).
BUILD_LOG=build.log
GEN=$(grep -m1 '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2-)
KEEP=(-k)                    # Unix Makefiles: keep-going
[[ "$GEN" == *Ninja* ]] && KEEP=(-k 0)   # Ninja: -k 0 = keep going forever

set +e
cmake --build "$BUILD_DIR" -j"$(nproc)" -- "${KEEP[@]}" 2>&1 | tee "$BUILD_LOG"
rc=${PIPESTATUS[0]}
set -e

if ((rc != 0)); then
  errs=$(grep -ac 'error:' "$BUILD_LOG" || true)
  warns=$(grep -ac 'warning:' "$BUILD_LOG" || true)
  echo
  echo "==> build FAILED — $errs errors, $warns warnings (full log: $BUILD_LOG)"
  exit "$rc"
fi

echo
echo "==> OK. Run: ./build/bootamp <file|url|radio>"
