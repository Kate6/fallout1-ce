# Fallout 1 Community Edition - Agent Guide

## Project Overview

Fallout 1 CE is a **cross-platform reimplementation** of the original Fallout game engine. It's a C++17 codebase using CMake as its build system, with support for Windows, macOS, Linux, Android, and iOS. The project is a **reverse-engineered recreation** of the original game with bugfixes and modernizations.

## Essential Commands

### Building

```bash
# Desktop (all platforms)
cmake -B build -G <generator>
cmake --build build -j $(nproc)

# Linux x86 (32-bit)
cmake -B build -D CMAKE_TOOLCHAIN_FILE=cmake/toolchain/Linux32.cmake
cmake --build build -j $(nproc)

# Android
cd os/android && ./gradlew assembleDebug

# iOS (requires macOS with Xcode)
cmake -B build -G Xcode -D CMAKE_TOOLCHAIN_FILE=cmake/toolchain/ios.toolchain.cmake -D PLATFORM=OS64
cmake --build build -j $(sysctl -n hw.physicalcpu)
```

### Testing & Quality

```bash
# Static analysis
cppcheck --std=c++17 src/

# Code format check
find src -type f -name '*.cc' -o -name '*.h' | xargs clang-format --dry-run --Werror
```

### Sanitizers (for debugging)
```bash
cmake -B build -D ASAN=ON -D UBSAN=ON
```

## Code Organization

```
src/
├── game/          # Main game logic (actions, combat, scripts, etc.)
├── int/           # Script interpreter (intrp** = "interpret" + 1 char)
├── plib/          # Platform/library code
│   ├── gnw/       # Graphics/window subsystem (GNW = "graphics window")
│   ├── db/        # DAT file database handling
│   ├── assoc/     # Association lists
│   └── color/     # Color utilities
├── audio_engine.* # Platform audio abstraction
├── movie_lib.*    # Movie playback (SMM/MVE files)
├── fps_limiter.*  # Frame rate control
└── platform_compat.* # Cross-platform utilities
```

### Key Architectural Concepts

**Script Interpreter (`src/int/`)**
- The interpreter executes game scripts stored in `.int` files
- Scripts control NPC behavior, dialog, quest logic, etc.
- `Program` struct manages execution: stack, instruction pointer, procedures
- Opcodes range from `0x8000` (control) to `0x804B` (advanced operations)

**Object System (`src/game/object.cc`)**
- All game entities (items, critters, scenery) are `Object` structs
- Objects have prototypes (templates) and instances
- `obj_dude` is the global player character reference
- `obj_egg` is used internally for various operations

**Database System (`src/plib/db/`)**
- Game assets come from `master.dat` and `critter.dat`
- Patches directory allows file overrides without modifying originals
- Uses LZSS compression for some files
- Hash table lookup available for fast file access

**Window/Graphics System (`src/plib/gnw/`)**
- `win_*` functions manage UI windows (buttons, text, drawing)
- `scr_*` functions handle the main game viewport
- Color indices reference `colorTable` (16-bit color mapped to display)
- `intensityColorTable` for lighting effects

## Code Patterns & Conventions

### Naming
- **Files**: Lowercase with underscores (e.g., `game_loop.cc`)
- **Classes/Types**: PascalCase (e.g., `CacheEntry`, `MessageList`)
- **Functions**: snake_case (e.g., `game_handle_input`, `win_add`)
- **Constants**: SCREAMING_SNAKE_CASE or `kPascalCase` for enum values
- **Globals**: Hungarian-style prefix often (e.g., `gnw_`, `game_`, `scr_`)
- **Interpreter**: Uses `field_X` naming for struct members (reverse-engineered)

### Code Style
- Uses `.clang-format` based on WebKit style (AllowShortIfStatementsOnASingleLine: WithoutElse)
- All code lives in `namespace fallout`
- Header guards: `FALLOUT_<PATH>_H_` (e.g., `FALLOUT_GAME_GAME_H_`)
- .cc files include their .h file first

### Comments
- Original assembly offsets often preserved as `// 0xADDR` comments
- `// NOTE:` marks intentional deviations from original behavior
- `// FIXME:` marks known issues
- `// TODO:` marks unimplemented features

### Window System Patterns
```cpp
// Typical window creation pattern
int win = win_add(x, y, width, height, color, flags);
if (win != -1) {
    unsigned char* buf = win_get_buf(win);
    // draw to buffer
    win_draw(win);
}
// When done
win_delete(win);
```

### Database Access Pattern
```cpp
DB_FILE* stream = db_fopen("path\\to\\file", "rb");
// use db_fread*, db_fread*List, or db_fgets
db_fclose(stream);
```

## Critical Gotchas

### Memory Management
- Custom `mem_malloc`, `mem_realloc`, `mem_free` wrappers in `plib/gnw/memory.cc`
- Never use raw `new`/`delete` - use memory system for game allocations
- `gmemory_*` functions for system-level memory

### Path Separators
- **Always use backslash** (`\\`) for in-game paths (Windows-style, e.g., `"data\\sound\\music\\"`)
- The `plib/db/db.cc` handles platform-appropriate conversion internally
- `COMPAT_MAX_PATH` for cross-platform path buffer sizing

### Directory vs. File
- Data access goes through `db_fopen` which searches DAT files AND patches directory
- Patches directory (`patch_file_name` in config) allows file overrides without modifying DATs
- Two-level search: master DAT → patches directory

### Game State Management
- `game_state()` returns enum: `GAME_STATE_0` through `GAME_STATE_5`
- State transitions managed by `game_state_request()` and `game_state_update()`
- `game_user_wants_to_quit` global controls main loop exit (0=running, 1=?, 2=quit)

### Display Coordinates
- `scr_size.ulx` / `scr_size.lrx` define viewport bounds (edge detection)
- `screenGetWidth()` / `screenGetHeight()` for actual screen dimensions
- Original game designed for 640x480; modern builds scale up

### Palette System
- `cmap` global holds current color palette
- `colorTable` maps 16-bit color indices to actual RGB values
- Palette fade functions: `palette_fade_to()`, `palette_set_to()`
- `black_palette`, `white_palette` constants available

### Animation System
- `register_begin()` / `register_end()` for queuing animations
- Animations are processed in background; use `anim_end()` to wait
- `ANIMATION_REQUEST_RESERVED` for system-level animations

### Inter-Script Communication
- Global variables via `game_get_global_var()` / `game_set_global_var()`
- Map-specific variables via `map_get_global_var()` / `map_set_global_var()`
- Scripts can export/import variables via `OPCODE_EXPORT_VARIABLE`

### Rendering Pipeline
1. Game logic updates
2. `scripts_check_state()` processes script execution
3. `map_check_state()` handles tile/map updates
4. `renderPresent()` composites to screen
5. `sharedFpsLimiter.throttle()` maintains frame rate

## Configuration

- **`fallout.cfg`**: Main config (data file paths, language, preferences)
- **`f1_res.ini`**: Display resolution and fullscreen
  ```ini
  [MAIN]
  SCR_WIDTH=1280
  SCR_HEIGHT=720
  WINDOWED=1
  ```
- File naming: game data files may be UPPERCASE or lowercase depending on distribution

## Platform-Specific Notes

### Android
- Uses SDL2 with manual scaling in `os/android/app/src/main/java/`
- Game assets must be copied to device (no bundling due to size)
- Debug and release are separate apps (different app IDs)
- Touch input mapped to mouse: single tap = left click, two-finger tap = right click

### iOS
- Similar to Android but via iOS file sharing feature
- Requires sideloading (AltStore, etc.)

### Windows
- Links against `winmm` for multimedia timing
- Uses `.rc` file for resources, `.ico` for icon

### macOS
- Universal binary (x86_64 + arm64)
- App bundle with `.icns` icon
- Notarization required for distribution

## Testing Observations

- No formal test suite exists
- CI runs `cppcheck` and `clang-format` validation
- Manual testing on each platform is expected
- For behavioral changes, compare against original game if possible

## Common Debug Patterns

```cpp
// Print debug message
debug_printf("value is %d\n", someValue);

// Display on-screen message
display_print("message text");

// Wait for keypress with FPS limiter
while (get_input() == -1 && game_user_wants_to_quit == 0) {
    sharedFpsLimiter.mark();
    renderPresent();
    sharedFpsLimiter.throttle();
}
```

## File Naming Conventions in CMake

Source files are listed explicitly in `CMakeLists.txt` (not globbed). Format:
- `"src/game/foo.cc"` - implementation
- `"src/game/foo.h"` - declaration

Third-party code in `third_party/` uses FetchContent or add_subdirectory.