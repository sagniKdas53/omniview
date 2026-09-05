# OmniView CPP

OmniView CPP is a native, high-performance C++ implementation of OmniView built with **Qt 5 (C++17)**. It delivers instant native startup (~15ms), minimal memory footprint, zero GIL/Python runtime overhead, and 100% native Linux Wayland and X11 drag-and-drop out-of-the-box with zero helper windows.

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
  - Monitors the active root folder and subfolders with 1.2s debounced rescan to instantly detect added, renamed, or deleted files.
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
- Qt 5 (Widgets, Core, Gui, Sql, Concurrent, Test)

### Build
```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

### Run Tests
```bash
./build/omniview_tests
```

### Run Application
```bash
./build/omniview-cpp [optional_image_directory]
```
