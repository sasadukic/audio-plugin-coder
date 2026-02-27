#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_TARGET="sample_player"
PLUGIN_DIR_NAME="SamplePlayer"
DEST_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
SYSTEM_DEST_DIR="/Library/Audio/Plug-Ins/VST3"
COPY_SYSTEM=1
WATCH_MODE=0
INTERVAL_SECONDS=2

usage() {
  cat <<'EOF'
Usage: scripts/autobuild-copy-macos.sh [options]

Options:
  --plugin <target>     Plugin CMake target prefix (default: sample_player)
  --plugin-dir <name>   Plugin directory name under plugins/ (default: SamplePlayer)
  --dest <directory>    Destination VST3 folder (default: ~/Library/Audio/Plug-Ins/VST3)
  --system-dest <dir>   Optional system VST3 folder (default: /Library/Audio/Plug-Ins/VST3)
  --no-system-copy      Skip attempting system VST3 copy
  --watch               Keep watching plugin files and rebuild on every change
  --interval <seconds>  Poll interval in watch mode (default: 2)
  -h, --help            Show this help

Examples:
  scripts/autobuild-copy-macos.sh
  scripts/autobuild-copy-macos.sh --plugin sample_player --plugin-dir SamplePlayer --watch --interval 1
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --plugin)
      PLUGIN_TARGET="$2"
      shift 2
      ;;
    --dest)
      DEST_DIR="$2"
      shift 2
      ;;
    --system-dest)
      SYSTEM_DEST_DIR="$2"
      shift 2
      ;;
    --plugin-dir)
      PLUGIN_DIR_NAME="$2"
      shift 2
      ;;
    --no-system-copy)
      COPY_SYSTEM=0
      shift
      ;;
    --watch)
      WATCH_MODE=1
      shift
      ;;
    --interval)
      INTERVAL_SECONDS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

PLUGIN_DIR="$ROOT_DIR/plugins/$PLUGIN_DIR_NAME"
if [[ ! -d "$PLUGIN_DIR" ]]; then
  echo "Plugin directory not found: $PLUGIN_DIR" >&2
  exit 1
fi

build_and_copy() {
  local vst3_target="${PLUGIN_TARGET}_VST3"
  local vst3_dir="$ROOT_DIR/build/plugins/$PLUGIN_DIR_NAME/${PLUGIN_TARGET}_artefacts/VST3"
  local vst3_source=""
  local vst3_name=""
  local user_dest=""
  local system_dest=""

  echo "[autobuild] Configuring CMake..."
  cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"

  echo "[autobuild] Building target: $vst3_target"
  cmake --build "$ROOT_DIR/build" --target "$vst3_target" --config Release

  vst3_source="$(find "$vst3_dir" -maxdepth 1 -type d -name "*.vst3" | head -n 1 || true)"
  if [[ -z "$vst3_source" ]]; then
    echo "[autobuild] VST3 bundle not found in: $vst3_dir" >&2
    return 1
  fi

  echo "[autobuild] Re-signing VST3 bundle to keep resource seal valid"
  codesign --force --deep --sign - "$vst3_source"
  if ! codesign --verify --deep --strict --verbose=1 "$vst3_source"; then
    echo "[autobuild] Code signature verification failed for: $vst3_source" >&2
    return 1
  fi

  vst3_name="$(basename "$vst3_source")"
  user_dest="$DEST_DIR/$vst3_name"

  echo "[autobuild] Copying $vst3_name -> $DEST_DIR"
  mkdir -p "$DEST_DIR"
  rm -rf "$user_dest"
  ditto "$vst3_source" "$user_dest"
  if ! codesign --verify --deep --strict --verbose=1 "$user_dest"; then
    echo "[autobuild] Code signature verification failed after copy: $user_dest" >&2
    return 1
  fi
  echo "[autobuild] User VST3 deployed: $user_dest"

  if [[ "$COPY_SYSTEM" -eq 1 ]]; then
    system_dest="$SYSTEM_DEST_DIR/$vst3_name"
    if [[ -d "$SYSTEM_DEST_DIR" && -w "$SYSTEM_DEST_DIR" ]]; then
      echo "[autobuild] Copying $vst3_name -> $SYSTEM_DEST_DIR"
      rm -rf "$system_dest"
      ditto "$vst3_source" "$system_dest"
      if ! codesign --verify --deep --strict --verbose=1 "$system_dest"; then
        echo "[autobuild] Code signature verification failed after system copy: $system_dest" >&2
        return 1
      fi
      echo "[autobuild] System VST3 deployed: $system_dest"
    else
      echo "[autobuild] Skipping system copy (no write access to $SYSTEM_DEST_DIR)"
    fi
  fi
}

snapshot_hash() {
  local digest
  digest="$(find "$PLUGIN_DIR" -type f \( \
      -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.hpp" -o -name "*.mm" -o \
      -name "*.html" -o -name "*.css" -o -name "*.js" -o -name "CMakeLists.txt" -o -name "*.json" \
    \) | LC_ALL=C sort | xargs shasum 2>/dev/null | shasum | awk '{print $1}')"

  if [[ -z "$digest" ]]; then
    digest="no-input-files"
  fi

  echo "$digest"
}

if [[ "$WATCH_MODE" -eq 0 ]]; then
  build_and_copy
  exit 0
fi

echo "[autobuild] Watch mode enabled for $PLUGIN_TARGET"
echo "[autobuild] Polling every ${INTERVAL_SECONDS}s"
last_hash="$(snapshot_hash)"

if build_and_copy; then
  echo "[autobuild] Initial build/deploy complete"
else
  echo "[autobuild] Initial build failed; continuing watch"
fi

while true; do
  sleep "$INTERVAL_SECONDS"
  current_hash="$(snapshot_hash)"

  if [[ "$current_hash" != "$last_hash" ]]; then
    echo "[autobuild] Change detected: rebuilding..."
    if build_and_copy; then
      echo "[autobuild] Rebuild/deploy complete"
    else
      echo "[autobuild] Rebuild failed; waiting for next change"
    fi
    last_hash="$current_hash"
  fi
done
