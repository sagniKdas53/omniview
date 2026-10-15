# OmniView C++ roadmap

Follow-up work deliberately kept out of the full-review PR (see `REVIEW.md`), each
planned as its own focused PR. The C++ app is the maintained implementation and
Linux (X11 and Wayland) is the only supported platform. The Python version is no
longer maintained; the Rust version is on hold, but the shared cache layout
(`~/.cache/omniview`, `{sha256[:24]}.webp`) is kept so it can be picked up again.

Every item below keeps Qt 5 and Qt 6 support. It adds a failing test before the
fix and must pass the Qt 5 / Qt 6 sanitizer CI.

---

## 1. Background zip extraction for drags (REVIEW M38)

**Problem.** Dragging archive entries extracts each one synchronously on the GUI
thread (`FileActions::resolveLocalFiles` → `ZipUtils::ensureExtracted`). That took
about 2.9 s for 150 entries. On Wayland, releasing the mouse button during the
freeze cancels the drag.

**Plan.**
1. Extract ahead of time. When the batch selection changes, or the pointer rests
   on an archive card, queue `ensureExtracted` for the selected archive entries on
   a small dedicated `QThreadPool`. Use one thread, so the disk isn't thrashed.
2. When a drag starts, use the extracted paths that are ready. If any are still
   missing, show a short progress pill on the drag badge and wait. The pill must
   not hold the pointer grab longer than the platform drag threshold allows, so
   the drag never starts with partial data.
3. Extraction results are keyed by archive size+mtime, as they already are, so
   pre-extraction can't serve stale files.

**Tests.** Pre-extraction finishes off the GUI thread. A drag of N entries makes
no `ensureExtracted` call on the GUI thread when they're ready. A changed archive
invalidates them.

## 2. Asynchronous viewer decode with prefetch (REVIEW M46)

**Problem.** Every next/previous step decodes the image on the GUI thread. The
size is bounded (8192 px, reduced-size JPEG decode), but large PNGs still stall
the UI for hundreds of ms.

**Plan.**
1. Move `ColorUtils::loadImage` for the viewer into a `QtConcurrent::run` task
   keyed by `(index, path, generation)`. Results for an old generation are
   dropped.
2. Show the cached thumbnail (from `ThumbnailManager`'s pixmap cache) right away,
   scaled up, and swap in the full decode when it arrives.
3. Prefetch the next and previous image. Keep at most three decoded images in a
   small LRU, with a byte budget and an accounted cost, like the thumbnail cache.

**Tests.** Navigation returns before the decode finishes. A quick ←→← sequence
shows the right final image. Memory stays within the budget.

## 3. Lazy, paged gallery model (REVIEW M50)

**Problem.** `MainWindow::refreshGallery` runs the full query and builds every
`ImageRecord` on the GUI thread, about 400 ms at 100k images, on every filter
change and rescan.

**Plan.**
1. Run the query in a worker (its own `Database` connection) and return the
   result through a queued signal. Discard results for an outdated filter.
2. Make `GalleryModel` page-based: fetch `rowCount` first, then load rows in
   pages of about 500 with `LIMIT/OFFSET` on a stable sort key, using
   `canFetchMore`/`fetchMore`. Remove `rowForPath`'s full hash, or limit it to
   the loaded pages.
3. Keep the selection by path across refreshes (this already works for model
   resets).

**Tests.** Refresh at 100k rows doesn't block the GUI thread for more than one
frame. Selection survives a refresh. Similarity ranking still works. It needs
the full hash list, which `Database` will provide through a dedicated
`(path, dhash)` query.

## 4. Archive handle cache and a decompression budget (REVIEW M59, M15)

**Problem.** Reading each archive entry reopens the zip and walks its central
directory, which is O(N²) for big archives. Workers also have no shared limit on
how much memory concurrent decompressions use.

**Plan.**
1. A per-thread LRU of open `mz_zip_archive` handles, keyed by path, size and
   mtime (thread-local, so no locking around miniz). Capacity 4. Each handle is
   closed when its thread ends.
2. A process-wide decompression budget (for example 256 MB) as a semaphore that
   is acquired before an entry is extracted, with the entry's uncompressed size
   as its weight. Oversized entries are already rejected.

**Tests.** Reading 1000 entries of one archive opens it once per worker. Peak
memory under 8 workers stays within the budget (ASan build, `mallinfo2`).

## 5. Theme and stylesheet extraction

**Problem.** The dark and light colours and stylesheets are spread across
`MainWindow` and `ViewerWindow` as string literals.

**Plan.** One `Theme` struct holding the palette and the derived colours, plus
one stylesheet template. `MainWindow` and `ViewerWindow` apply `Theme::current()`.
This is a pure refactor; screenshots before and after under `QT_QPA_PLATFORM=offscreen`
are compared pixel for pixel.

## 6. Manual checks only (no automation possible here)

- **Drag from the context menu on Wayland (REVIEW M39).** The drag starts after
  the mouse button was released, which Wayland compositors may reject. If it
  fails, change the action to "Drag selected" in the batch toolbar, or start the
  drag from the press that opened the menu.

---

## Decided, not planned

- **Schema versioning (`PRAGMA user_version`).** Not adopted. Schema changes stay
  additive and are detected at start-up instead (`PRAGMA table_info`, `CREATE …
  IF NOT EXISTS`, `DROP INDEX IF EXISTS`). This keeps the database readable by
  older builds and by the Rust version.
- **Canonical dHash across implementations (REVIEW M27).** The C++ definition is
  the reference: Rec.601 integer luma on the stored thumbnail, 9×8 difference
  hash. Nothing else needs to match it for now.
- **macOS and Windows.** Dropped: no CI, no packaging, and no platform-specific
  code. REVIEW M58 (Windows archive share mode) no longer applies.
