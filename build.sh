#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BIN_DIR="$ROOT_DIR/bin"

cd "$ROOT_DIR"

# Configure if not already configured
if [ ! -f build/c4che/_cache.py ]; then
	echo "Configuring build..."

	# Auto-detect SDL2 on macOS via pkg-config (Homebrew)
	SDL2_ARGS=""
	if [ "$(uname)" = "Darwin" ]; then
		if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
			SDL2_ARGS="--sdl-use-pkgconfig"
		fi
	fi

	./waf configure $SDL2_ARGS "$@"
fi

# Build
echo "Building..."
./waf build

# Install to bin directory
echo "Installing to $BIN_DIR..."
./waf install --destdir="$BIN_DIR"

echo "Build complete. Binaries installed to $BIN_DIR"
