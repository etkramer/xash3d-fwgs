# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Xash3D FWGS is a cross-platform game engine providing Half-Life (GoldSrc) compatibility with modern extensions. It supports 14+ platforms including Windows, Linux, macOS, Android, iOS, Nintendo Switch, and PlayStation Vita.

## Build System

The project uses **WAF** (Python-based build system). The `waf` executable is included in the repository.

### Common Build Commands

```bash
# View all options
./waf --help

# Configure (32-bit is default on x86 for GoldSrc compatibility)
./waf configure

# Configure for 64-bit on x86
./waf configure -8

# Build
./waf build

# Install to output directory
./waf install --destdir=/path/to/output

# Build with tests enabled
./waf configure --enable-tests
./waf build
```

### Windows (Visual Studio)
```bash
waf configure --sdl2=c:/path/to/SDL2
waf build
```

### Key Configure Options
- `--dedicated` - Build dedicated server only
- `--enable-tests` - Build standalone tests
- `-8` / `--64bits` - Enable 64-bit build (x86 only)
- `-4` / `--32bits` - Force 32-bit build
- `--enable-bundled-deps` - Use bundled dependencies
- `--enable-all-renderers` - Enable all renderer backends

## Architecture

### Core Components

```
engine/
├── client/     # Client-side: rendering coordination, input, UI, prediction
├── server/     # Server-side: entity physics, game logic, networking
├── common/     # Shared: host loop, console, cvars, networking, model loading
└── platform/   # OS-specific: SDL2/SDL3, Android, iOS, Win32, POSIX
```

### Key Subsystems

- **ref/** - Loadable renderer modules (gl/, soft/, null/)
- **filesystem/** - Virtual filesystem (PAK, ZIP/pk3, WAD, native)
- **public/** - Shared headers and utilities for game DLLs
- **common/** - Math, compression, CRC, string utilities
- **3rdparty/** - External dependencies (mainui, vgui_support, opus, vorbis)

### Platform Abstraction

Platform-specific code lives in `engine/platform/`. When adding platform-specific features, implement for all supported platforms or provide stubs. Check `public/build.h` for platform macros.

## Code Style

Mixed Quake/HLSDK C style:
- **Tabs** for indentation (never spaces)
- **Spaces inside parentheses**: `if( condition )` not `if(condition)`
- **Braces on own lines**:
  ```c
  if( condition )
  {
      code();
  }
  ```
- Short single-line blocks allowed: `if( x ) return;`
- Avoid magic numbers - use named constants

### Code Formatting
```bash
uncrustify -c uncrustify.cfg --no-backup -l C file.c
```
Requires Uncrustify 0.80+.

## Commit Messages

Format: `tag: description` or `tag: subtag: description`

Tags are subsystem names, feature names, or filenames without extension:
```
engine: fix memory leak in host loop
client: cl_parse: handle malformed packets
filesystem: add pk3dir support
```

## Testing

Tests are in `public/tests/`, `common/tests/`, and `filesystem/tests/`.

```bash
./waf configure --enable-tests
./waf build
# Tests run as part of the build
```

## Important Notes

- **Never use GitHub ZIP archives** - they don't include git submodules
- **Clone with `--recursive`**: `git clone --recursive https://github.com/FWGS/xash3d-fwgs`
- 32-bit is default on x86 for Steam Half-Life compatibility
- 64-bit requires recompiling game DLLs from source
- The engine uses its own CRT wrappers in `public/crtlib.h` - prefer these over standard C library functions
