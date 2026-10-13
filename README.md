# OmniView CPP

OmniView CPP is a native, high-performance C++ implementation of OmniView built with **Qt 5 or Qt 6 (C++17)**. It delivers instant native startup (~15ms), minimal memory footprint, zero GIL/Python runtime overhead, and 100% native Linux Wayland and X11 drag-and-drop out-of-the-box with zero helper windows.

---

## Key Features

- **Instant Native Drag-and-Drop (`QDrag`)**:
  - Drag individual image cards, batch selections, or directly from the viewer into Discord, Slack, Chrome, Firefox, Dolphin, Nautilus, or GIMP.
  - Generates rich multi-file drag previews with live thumbnail badges and item count indicators.
  - Directly exposes `text/uri-list`, `text/plain`, and GNOME/KDE file manager formats (`x-special/gnome-copied-files`).
- **Shared High-Efficiency WebP Cache**:
  - Fully compatible with Python and Rust versions of OmniView, utilizing the unified `~/.cache/omniview/thumbnails/{sha256[:24]}.webp` format.
  - Zero thumbnail regeneration if thumbnails were already computed by another OmniView version.
- **SQLite Database Indexing**:
  - High-concurrency WAL-mode SQLite database storing file metadata, aspect ratios, dominant color classifications, and difference hashes (dHash).
  - Background multi-threaded indexing thread pool (`QThreadPool`) with on-screen priority loading.
- **Live Directory Watching (`QFileSystemWatcher`)**:
  - Monitors the active root folder and all of its (non-hidden) subfolders, up to 4096 directories, with a 1.2s debounced rescan (forced after 10s of continuous changes) to detect added, renamed, or deleted files.
  - Automatically prunes missing records from the database on scan.
- **Batch Selection Mode**:
  - Toggle batch mode (`✓ Select`), click cards to toggle selection without resetting others, rubber-band box selection.
  - Bottom floating batch toolbar with:
    - Item counter
    - Select All (`Ctrl+A`) / Invert / Clear (`Esc`)
    - Copy Files (`Ctrl+C`)
    - Copy Paths (`Ctrl+Shift+C`)
    - Dedicated `🖐 Drag Selected to Attach` button for immediate drag without needing any secondary window.
- **Full-Featured Sub-Window Image Viewer**:
  - Non-modal sub-window allowing side-by-side comparison with the gallery.
  - Smooth pan and zoom (`+`, `-`, mouse wheel, `1:1`, and fit-to-screen `⛶ Fit`).
  - Arrow key navigation (`Left` / `Right`).
  - Dedicated `🖐 Drag to Attach` button right on the viewer toolbar.
  - One-click file copying, path copying, and folder opening (`xdg-open`).
- **Dark & Light Mode**:
  - Full theme switching (`🌙` / `☀️`) for cards, toolbar, sidebar, context menu, and viewer.

---

## Building and Testing

### Prerequisites
- CMake 3.16+
- GCC / G++ 11+ (with C++17 support)
- Qt 5 or Qt 6 (Widgets, Core, Gui, Sql, Concurrent, Test)

### Build
```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

To pick a Qt major version explicitly, pass `-DQT_DIR=<prefix>/lib/cmake/Qt5` (or `Qt6`).
WebP thumbnails need the Qt image formats plugin (`qt5-image-formats-plugins` /
`qt6-image-formats-plugins` on Debian/Ubuntu); without it thumbnails are written as JPEG
under the same shared file name.

### Run Tests
```bash
ctest --test-dir build --output-on-failure
```

CTest runs every suite with the offscreen Qt platform and a throw-away cache directory.
The optional private dataset test runs only when `OMNIVIEW_TEST_PIXIV_DIR` points to the
expected fixture.

### Developer options
| CMake option | Default | Effect |
|---|---|---|
| `OMNIVIEW_WARNINGS` | `OFF` | `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` (`/W4` on MSVC) on OmniView's own targets only (never vendored miniz) |
| `OMNIVIEW_WARNINGS_AS_ERRORS` | `OFF` | Adds `-Werror` / `/WX` to the above |
| `OMNIVIEW_SANITIZE` | empty | `address;undefined` or `thread` (GCC/Clang) |

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug "-DOMNIVIEW_SANITIZE=address;undefined"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

Sanitizer suppressions in `tools/sanitizers/` cover uninstrumented Qt libraries only.
Static analysis: `clang-tidy -p <build dir with CMAKE_EXPORT_COMPILE_COMMANDS=ON> src/*.cpp tests/*.cpp`
uses the repository's `.clang-tidy`.

### Run Application
```bash
./build/omniview-cpp [optional_image_directory]
```

Set `OMNIVIEW_CACHE_DIR` to choose a separate cache and database directory.
