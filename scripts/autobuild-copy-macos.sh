#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_TARGET="nf_gnarly"
PLUGIN_DIR_NAME=""
DEST_DIR="/Applications"
WATCH_MODE=0
INTERVAL_SECONDS=2

usage() {
  cat <<'EOF'
Usage: scripts/autobuild-copy-macos.sh [options]

Options:
  --plugin <target>     Plugin CMake target prefix (default: nf_gnarly)
  --plugin-dir <name>   Plugin directory name under plugins/ (default: same as --plugin)
  --dest <directory>    Destination app folder (default: /Applications)
  --watch               Keep watching plugin files and rebuild on every change
  --interval <seconds>  Poll interval in watch mode (default: 2)
  -h, --help            Show this help

Examples:
  scripts/autobuild-copy-macos.sh --plugin nf_gnarly
  scripts/autobuild-copy-macos.sh --plugin nf_gnarly --watch --interval 1
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
    --plugin-dir)
      PLUGIN_DIR_NAME="$2"
      shift 2
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

if [[ -z "$PLUGIN_DIR_NAME" ]]; then
  PLUGIN_DIR_NAME="$PLUGIN_TARGET"
fi

PLUGIN_DIR="$ROOT_DIR/plugins/$PLUGIN_DIR_NAME"
if [[ ! -d "$PLUGIN_DIR" ]]; then
  echo "Plugin directory not found: $PLUGIN_DIR" >&2
  exit 1
fi

build_and_copy() {
  local standalone_target="${PLUGIN_TARGET}_Standalone"
  local standalone_dir="$ROOT_DIR/build/plugins/$PLUGIN_DIR_NAME/${PLUGIN_TARGET}_artefacts/Standalone"
  local app_source=""
  local app_name=""
  local dest_app=""

  echo "[autobuild] Configuring CMake..."
  cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"

  echo "[autobuild] Building target: $standalone_target"
  cmake --build "$ROOT_DIR/build" --target "$standalone_target" --config Release

  app_source="$(find "$standalone_dir" -maxdepth 1 -type d -name "*.app" | head -n 1 || true)"
  if [[ -z "$app_source" ]]; then
    echo "[autobuild] App bundle not found in: $standalone_dir" >&2
    return 1
  fi

  app_name="$(basename "$app_source")"
  dest_app="$DEST_DIR/$app_name"

  echo "[autobuild] Copying $app_name -> $DEST_DIR"
  if ! ditto "$app_source" "$dest_app" 2>/dev/null; then
    local fallback_dir="$HOME/Applications"
    local fallback_app="$fallback_dir/$app_name"
    mkdir -p "$fallback_dir"
    ditto "$app_source" "$fallback_app"
    echo "[autobuild] Could not write to $DEST_DIR. Deployed to $fallback_dir instead."
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
