#!/usr/bin/env bash
# install-omarchy-desktop.sh — register bootamp as an Omarchy launcher app.
#
# Installs bootamp.desktop into ~/.local/share/applications plus the bootamp
# icon into ~/.local/share/icons/hicolor, so the Omarchy Quickshell launcher
# lists bootamp and launches it in the default terminal with launch-or-focus
# semantics (same pattern as the stock Disk Usage / Docker entries).
#
# Usage: ./install-omarchy-desktop.sh [--remove]
# Binary resolution: $BOOTAMP_BIN -> <repo-root>/build/bootamp
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$REPO_ROOT"

DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
DESKTOP_DIR="$DATA_HOME/applications"
DESKTOP_FILE="$DESKTOP_DIR/bootamp.desktop"
ICON_DIR="$DATA_HOME/icons/hicolor/scalable/apps"
# Hyprland personal-override keybinding file (Omarchy). The SUPER+B binding
# is installed there next to the launcher entry; absent file = not Omarchy,
# the binding section is skipped with a note.
BINDINGS_FILE="$HOME/.config/hypr/bindings.lua"
BINDING_MARK="-- bootamp (install-omarchy-desktop.sh)"

if [[ ${1:-} == "--remove" ]]; then
  rm -f "$DESKTOP_FILE" "$ICON_DIR/bootamp.svg"
  update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
  if [[ -f $BINDINGS_FILE ]] && grep -qF "$BINDING_MARK" "$BINDINGS_FILE"; then
    # Marker line + the o.bind line right after it (how install writes them).
    cp "$BINDINGS_FILE" "$BINDINGS_FILE.bak.$(date +%s)"
    mark_re="$(sed 's/[.[\*^$()+?{|]/\\&/g' <<<"$BINDING_MARK")"
    sed -i "/${mark_re}/,+1d" "$BINDINGS_FILE"
    hyprctl reload >/dev/null 2>&1 || true
    echo "Removed the SUPER+B binding from $BINDINGS_FILE"
  fi
  echo "Removed $DESKTOP_FILE and $ICON_DIR/bootamp.svg"
  echo "Alternative: omarchy-remove-launcher-entry bootamp.desktop bootamp"
  exit 0
fi

# --- locate the binary ---------------------------------------------------------
BIN=""
if [[ -n ${BOOTAMP_BIN:-} ]]; then
  BIN=$BOOTAMP_BIN
elif [[ -x build/bootamp ]]; then
  BIN=$REPO_ROOT/build/bootamp
fi
if [[ -z $BIN ]]; then
  echo "ERROR: bootamp binary not found — run ./build.sh first (or set BOOTAMP_BIN)." >&2
  exit 1
fi
[[ $BIN == /* ]] || BIN=$REPO_ROOT/$BIN   # .desktop Exec needs an absolute path

# --- install -------------------------------------------------------------------
mkdir -p "$DESKTOP_DIR" "$ICON_DIR"
cp assets/bootamp.svg "$ICON_DIR/bootamp.svg"

cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Version=1.0
Name=bootamp
GenericName=Music Player
Comment=Terminal music player — local files, radio, web archives
Exec=omarchy-launch-or-focus-tui --app-id=org.omarchy.bootamp "$BIN"
Terminal=false
Type=Application
Icon=bootamp
StartupNotify=true
Categories=AudioVideo;Audio;Music;Player;
Keywords=music;player;radio;
EOF

# Best-effort cache refreshes; neither may fail the install.
update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true

# --- SUPER+B keybinding (Omarchy/Hyprland, best-effort) ------------------------
# Same launch-or-focus command as the .desktop entry, baked with $BIN. The
# marker keeps the install/remove/re-run flows idempotent; a missing
# bindings.lua (not Omarchy, or user removed it) is skipped with a note.
if [[ -f $BINDINGS_FILE ]]; then
  if ! grep -qF "$BINDING_MARK" "$BINDINGS_FILE"; then
    cp "$BINDINGS_FILE" "$BINDINGS_FILE.bak.$(date +%s)"
    {
      echo ""
      echo "$BINDING_MARK SUPER+B launches or focuses the TUI."
      echo "o.bind(\"SUPER + B\", \"bootamp\", \"omarchy-launch-or-focus-tui --app-id=org.omarchy.bootamp $BIN\")"
    } >> "$BINDINGS_FILE"
    hyprctl configerrors >/dev/null 2>&1 || true
    hyprctl reload >/dev/null 2>&1 || true
    echo "Added SUPER+B binding to $BINDINGS_FILE"
  else
    echo "SUPER+B binding already present in $BINDINGS_FILE"
  fi
else
  echo "No $BINDINGS_FILE — skipping the SUPER+B keybinding (not Omarchy?)"
fi

echo "Installed $DESKTOP_FILE"
echo "  binary : $BIN"
echo "  icon   : $ICON_DIR/bootamp.svg"
echo
echo "bootamp now appears in the launcher as \"bootamp\". Re-run this script"
echo "after ./build.sh to refresh the binary path."