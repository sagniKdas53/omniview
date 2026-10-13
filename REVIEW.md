# OmniView C++ — full quality review

Branch `review/full-pass`, baseline `61a2d90` (main). Scope: every project source, test, `CMakeLists.txt`
and the CI workflow. `src/miniz.c` / `src/miniz.h` are vendored and were only checked for version/CVEs.

## 1. Baseline (Phase 1)

Environment: Ubuntu 24.04, GCC 13.2, Clang 18.1 (clang-tidy), cppcheck 2.13, Qt 5.15.13 and Qt 6.4.2.
Both Qt versions were built and tested for every step below.

### Commands

```bash
# deps (as CI + Qt 6 + image formats + cppcheck)
apt-get install qtbase5-dev libqt5sql5-sqlite qttools5-dev-tools qt5-image-formats-plugins \
                qt6-base-dev libqt6sql6-sqlite qt6-image-formats-plugins cppcheck

# normal build + tests (per Qt version)
cmake -B build-qt5 -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5
cmake --build build-qt5 && QT_QPA_PLATFORM=offscreen ctest --test-dir build-qt5 --output-on-failure
#   (same with Qt6)

# warnings build: project C++ only (miniz is C, so CXX flags never reach it)
cmake -B build-warn -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wconversion" ...
# ASan+UBSan / TSan (baseline: flags by hand; after: -DOMNIVIEW_SANITIZE=...)
cmake -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ...
cmake -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" ...
# clang-tidy with the repository .clang-tidy, cppcheck excluding miniz
clang-tidy -p build-warn src/*.cpp tests/*.cpp
cppcheck --enable=warning,style,performance,portability --std=c++17 --library=qt -I src \
         -i src/miniz.c --suppress=missingIncludeSystem src/*.cpp tests/*.cpp
```

Clang's sanitizer runtime is not installed in this container, so sanitizer builds use GCC (libasan/libtsan).

### Results: before → after

| Check | Baseline (61a2d90) | After this PR |
|---|---|---|
| Build Qt 5 / Qt 6 | ✅ / ✅ (Qt 6: 10 deprecation warnings) | ✅ / ✅ (0 warnings) |
| ctest Qt 5 / Qt 6 | 2/2 suites pass (23 test functions) | 7/7 ctest entries pass (6 suites + HiDPI viewer run; 106 test functions) |
| Warnings build (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, unique) | Qt 5: **6**, Qt 6: **17** | Qt 5: **0**, Qt 6: **0** |
| ASan+UBSan Qt 6 | 0 reports | 0 reports |
| ASan+UBSan Qt 5 | **1** LeakSanitizer report (7 allocations, all in Qt's `libqoffscreen`) → suite fails | 0 (leak suppressed by library name, third-party only) |
| TSan Qt 6 | **12** reports, suite fails | **0** |
| TSan Qt 5 | **19** reports, suite fails | **0** |
| clang-tidy (final `.clang-tidy`, project files) | **32** | **0** |
| cppcheck (warning/style/performance/portability) | **37** | **25** (20 unchecked `prepare()`/`rollback()` whose failure surfaces via the checked `exec()`/commit, 4 `useStlAlgorithm` style hints, 1 info) |

TSan triage (baseline): 1 real test-harness race (QSignalSpy written from the Scanner thread while
`wait()` reads it on the main thread); everything else was new/delete/memmove inside Qt's own
uninstrumented image-scaling thread pool and QtTest's watchdog. During the fixes TSan also found that
Qt 5's `QMutex` is invisible to TSan (not annotated, unlike Qt 6) — the thumbnail manager now uses
`std::mutex`, and the runnable hand-off uses an explicit release/acquire atomic. The suppression file
(`tools/sanitizers/tsan.supp`) only contains `called_from_lib:` entries for Qt libraries, so any race in
OmniView's instrumented code is still reported.

### Vendored miniz

`src/miniz.{c,h}` is **3.1.2**, byte-identical to the upstream 3.1.2 release archive
(`miniz-3.1.2.zip`, verified with `diff`), which is the newest upstream tag (`git ls-remote --tags`).
The only CVE found for the standalone miniz library (CVE-2018-12913, `tinfl` infinite loop) was fixed in
2.0.8. No update needed; miniz was not edited.

## 2. Findings

Each finding was verified by one of: **T** a regression test added in this PR (failing-first against the
baseline behaviour where the API allowed), **H** a reproduction harness run against the baseline code on
Qt 5.15 and Qt 6.4 during review, **S** sanitizer output, **R** a code-level argument (stated). False
positives found during review were dropped (e.g. "QPixmap used on pool threads": it is not; "missing
Windows rename fix": `QSaveFile` now handles it).

Line numbers refer to the baseline commit.

### Critical

| ID | Where | Failure scenario | Fix | Status |
|---|---|---|---|---|
| C1 | `ColorUtils.cpp:81` | On Qt 5 (the Linux release build) `convertToFormat(Grayscale8).scaled(9,8,Smooth)` returns RGB32, but `computeDHash` read it as 1 byte/pixel: **every image hashed to `2222222222222222`** (H, reproduced). "Find similar" returned the whole library and the garbage was persisted in the DB shared with the Rust app. | Scale a normalised RGB32 image and compute Rec.601 integer luma explicitly; on startup, clear stored `2222222222222222` hashes (data fix, no schema change) and recompute them from the cached thumbnail without re-decoding sources. | Fixed, T (`dhashDistinguishesImages`, `dhashIndependentOfSourceFormat`, `brokenQt5HashesAreClearedOnStartup`, `clearedHashIsRecomputedFromThumbnail`) |

### High

| ID | Where | Failure scenario | Fix | Status |
|---|---|---|---|---|
| H1 | `Database.cpp:355`, `MainWindow.cpp:1004` | Clicking "⭐ Favorites (2)" sent `subfolder='__fav__'` and the query became `subfolder='__fav__' AND is_favorite=1`: **Favorites view always empty** (H). | New `FilterState` with a typed scope; `QueryFilter` gets `FolderScope` instead of marker strings. | Fixed, T |
| H2 | `ThumbnailWorker.cpp:68` | `~ThumbnailManager` called `waitForDone()` without clearing the queue: closing after opening a large folder drained every queued thumbnail (2.75 s for 160, minutes for 50k) with an invisible process still writing the DB/cache. | `cancelPending()` (clear queue, generation counter) before `waitForDone()`; also called on root change and in `~MainWindow`. | Fixed, T (`destructorDropsQueuedWork`) |
| H3 | `Scanner.cpp:117` | A NAS/USB root disconnecting mid-scan → walk ends early → prune deletes every row, **favourites lost for good** (H: 20k rows → 0). | Prune only after a complete walk of a root that still exists; prune also checks the root and is cancellable. | Fixed, T (`pruneKeepsIndexWhenRootIsMissing`) |
| H4 | `ZipUtils.cpp:55` | `isZipPath` split at the first `#`: archives under `C#/` or named `best#1.zip` were misparsed and **their entries deleted on every scan**; open/copy/drag silently did nothing. | Split at the first case-insensitive `.zip#`. | Fixed, T (`compositePathSplitsAtZipMarker`, `scanKeepsArchivesWithHashInPath`) |
| H5 | `ThumbnailWorker.cpp:26`, `GalleryDelegate.cpp:100` | A truncated/undecodable cached WebP (e.g. written non-atomically by another implementation) took the "already indexed" fast path, failed to decode on the GUI thread, emitted `dataChanged`, repainted, re-requested: **self-sustaining loop**, ~4000 cycles/2 s per visible card (H). | Worker decodes the cached thumbnail; if it does not decode it is regenerated; the decoded `QImage` travels with the result. | Fixed, T (`corruptCachedThumbnailIsRegenerated`) |
| H6 | `ThumbnailWorker.cpp:47` | Decode failures were never recorded: every repaint and every rescan re-decoded broken/oversized images forever. | Failures remembered per (path, mtime, size) in `ThumbnailManager`; retried only when the file changes. (No DB change, so other implementations are unaffected.) | Fixed, T (`failedImagesAreNotRetriedUntilTheyChange`) |
| H7 | `Database.cpp:318` | `toggleFavorite` kept its SELECT open while running the UPDATE; SQLite can't wait on a read→write upgrade, so **star clicks failed instantly with "database is locked"** while scanning/indexing (H: 0 ms failure vs 830 ms wait). | Single UPDATE first inside a transaction, then read back; explicit 5 s busy timeout. | Fixed, T (`toggleFavoriteWaitsForOtherWriters`) |
| H8 | `GalleryView.cpp:141`, `MainWindow.cpp:898` | Every refresh (search keystroke, watcher rescan, card resize) reset the model, clearing the selection **without any signal**: batch bar still said "5 selected" and the drag button still carried the old paths (H). | View remembers selected paths on `modelAboutToBeReset`, restores them on `modelReset` and always re-emits the count; card resize uses `sizeHintChanged` instead of `reset()`. | Fixed, T (`selectionSurvivesModelRefresh`, `cardResizeKeepsSelection`) |
| H9 | `GalleryView.cpp:207` | After a press the view handled itself, sub-threshold mouse moves reached `QListView` and started a rubber band from a stale position: a multi-file drag carried **files the user never selected** (H). | Track press ownership; consumed presses never forward moves; the dragged card is a `QPersistentModelIndex` captured at press time. | Fixed, T (`smallMovesInBatchModeDoNotRubberBand`) |
| H10 | `GalleryView.cpp:595`, `MainWindow.cpp:1069` | Context-menu actions captured a plain `QModelIndex`/row; a refresh while the menu was open made "Favorite" star (and "Open" show) a **different image** (H). | Actions capture the path + `QPersistentModelIndex`; `MainWindow` resolves rows by path. | Fixed, R + T (path-based API covered by UI tests) |
| H11 | `Database.cpp:53` | Connections were named by native thread id and never removed: reused ids made Qt close another thread's connection ("does not belong to the calling thread", "duplicate connection name"), and open connections (64 MB cache each) piled up (H). | One uniquely named connection per `Database` instance, removed in the destructor; class made non-copyable. | Fixed, T (`connectionsAreRemovedWithTheirOwner`) |
| H12 | `ViewerWindow.cpp:529` | The viewer's first toolbar button took focus: **Left/Right moved focus between buttons and Space clicked "Previous"** instead of navigating/toggling zoom (H). | Toolbar buttons and scroll area use `Qt::NoFocus`; the window takes focus on show. | Fixed, T (`viewerArrowKeysNavigateWhateverHasFocus`) |
| H13 | `ViewerWindow.cpp:341`, `ColorUtils.cpp:153` | Full-resolution decodes with no size handling: Qt 6.0–6.4's 128 MB reader limit **rejects ordinary 35+ MP photos** (viewer blank, never thumbnailed); Qt 5 decoded a 16000² PNG to 1 GB on the GUI thread. | Shared `ColorUtils::loadImage`: pixel cap, reduced-size decode (`setScaledSize`) for thumbnails and for the viewer (bounded to the 8192 px max view), Qt 6 allocation limit set once at startup. | Fixed, T (`reducedDecodeKeepsOrientationAndAspect`) |
| H14 | `Scanner.cpp:99` | The shared DB holds fractional mtimes from the Rust app; this scanner stores whole seconds, so `mtime != excluded.mtime` reset **every row on every app switch**, re-thumbnailing the whole shared cache. | Content-change test uses a 1 s tolerance; sub-second differences keep the stored value. | Fixed, T (`subSecondMtimeDifferencesDoNotInvalidate`) |
| H16 | `GalleryView.cpp:207,165` | **Reported by the user:** in batch mode a stray click or small drag selected hundreds of cards. Root cause: card presses in batch mode are handled by `GalleryView` and never reach `QListView`, but every move with the button held still did; `QAbstractItemView` builds its rubber band from the last press *it* saw (`d->pressedPosition`, content coordinates) and applies it with `ClearAndSelect`. After a double-click (second click arrives as `MouseButtonDblClick`, consumed) the stored press can be hundreds of rows away: a 3 px wobble selected **216 cards** and wiped the batch (T, reproduced on `main` with Qt 5 and Qt 6). Separately, empty-space presses were made additive but the moves were not, so any wobble starting between cards cleared the batch. | Moves/releases reach `QListView` only if it saw the gesture's press; in batch mode an empty-space press starts a rubber band only past the drag distance, and press, moves and release are all additive (Ctrl). Normal mode unchanged. | Fixed, T (`dragAfterDoubleClickDoesNotUseStalePressPosition`, `strayDragOnEmptySpaceKeepsBatch`, `deliberateRubberBandInBatchModeAdds`, plus normal-mode guards) |
| H15 | `MainWindow.cpp:1062` | The viewer was fed a stale copy (`m_currentItems`) by row; after pressing F the viewer showed "⭐ Favorite" and **clicking it un-favourited the image** (H). | `m_currentItems` removed; viewer gets the model's records, rows resolved by path. | Fixed, R + T |

### Medium

| ID | Where | Failure scenario | Fix | Status |
|---|---|---|---|---|
| M1 | `Database.cpp:173` | `subfolder` (relative to the scanned root) was never updated: opening a parent/child of an earlier root showed phantom folders and an empty "Root Folder". | Re-home `subfolder`/`filename` when they differ, keeping features. | Fixed, T |
| M2 | `Database.cpp:216`, `Scanner.cpp:63` | Entries removed/renamed inside a still-existing zip were never pruned (dead tiles retried forever). | Scanner records archives it listed and paths it saw; prune removes unseen entries of listed archives only (unreadable archives keep their rows). | Fixed, T |
| M3 | `Database.cpp:294` | A worker that decoded the old file overwrote the rescan's reset (`indexed=1` with stale features). | `updateImageFeaturesIfUnchanged` matches mtime/size; stale results are dropped. | Fixed, T |
| M4 | `Database.cpp:18` | Root filter used `LIKE` (ASCII case-insensitive, full table scan): `/data/Photos` also showed `/data/photos`; ~39 ms vs ~1 ms per query at 200k rows. | Indexed, case-sensitive range `path >= root/ AND path < root0`. | Fixed, T |
| M5 | `Database.cpp:262` | `getUnindexedPaths` was not scoped to the root: other (e.g. unmounted) roots were re-queued after every scan. | Optional root range. | Fixed, T |
| M6 | `Database.cpp:150` | Corruption recovery deleted `-wal`/`-shm`, losing un-checkpointed commits from the backup. | WAL/SHM copied next to the backup before the connection closes (SQLite deletes the WAL on close). | Fixed, T |
| M7 | `Config.cpp:65` | Legacy DB migration used `QFile::copy`: misses the legacy WAL, can be torn while the old app writes, never retried. | `VACUUM INTO` a temp file, then rename. | Fixed, T |
| M8 | `Config.cpp:106`, `ThumbnailWorker.cpp:43` | Regeneration wrote to whatever `getThumbPath` returned, including `~/.cache/pixiv_gallery` (even with `OMNIVIEW_CACHE_DIR` set) and legacy MD5 JPEGs; the canonical WebP never appeared. | `canonicalThumbPath()` for writing; legacy paths read-only; legacy lookup skipped when `OMNIVIEW_CACHE_DIR` is set. Cache key/path unchanged. | Fixed, T |
| M9 | `ZipUtils.cpp:112` | Entry names without the UTF-8 flag (CP437, Explorer/7-Zip) were mangled and could never be read back. | Decode by flag; unflagged valid UTF-8 stays UTF-8, otherwise CP437. Lookup compares decoded names. | Fixed, T |
| M10 | `ZipUtils.cpp:108` | Encrypted, bzip2/LZMA, empty and symlink entries were indexed as images that never load (a symlink entry returned `../../etc/passwd`). | Skipped in listing and refused in reads. | Fixed, T |
| M11 | `ZipUtils.cpp:159` | `mz_zip_reader_locate_file(…, 0)` is case-insensitive: `A.png` returned `a.png`'s bytes. | Exact decoded-name lookup. | Fixed, T |
| M12 | `ZipUtils.cpp:85` | Any same-named file beside the archive hid the archive entry (vol2's pages hidden by vol1's extracted pages). | Duplicate requires matching size; backslash names normalised. | Fixed, T |
| M13 | `ZipUtils.cpp:233` | `ensureExtracted` ignored write errors; a truncated file (full disk) was cached and served forever. | `QSaveFile`. | Fixed, R |
| M14 | `ZipUtils.cpp:215` | Extraction cache key ignored archive changes (old bytes served after the zip was replaced). | Key includes archive size + mtime; files keep their real names (no more hash-named attachments). | Fixed, T |
| M15 | `ZipUtils.cpp:196` | Each read used 2× the entry size and no ratio check: a 1 MB zip of 127 MB zero "PNGs" pushed 8 workers to ~1.45 GB. | Extract straight into one presized buffer; entries >32 MB with >200:1 ratio rejected (list + read). | Fixed (global in-flight budget left as follow-up), T |
| M16 | `Scanner.cpp:84` | `batchSyncFiles` failures were ignored; the UI reported images the DB never got. | Scan stops, reports the error, skips pruning. | Fixed, T |
| M17 | `Scanner.cpp:121`, `MainWindow.cpp:748` | A cancelled scanner still emitted `finished`, delivered after deletion: old totals shown, indexing/watchers started mid-scan. `Scanner::finished` also shadowed `QThread::finished`. | `scanFinished(count, stats, error)` never emitted after cancel; MainWindow ignores non-current scanners; old scanners `deleteLater` (no more leaks, no blocking `wait()` on the GUI thread). | Fixed, T |
| M18 | `ZipUtils.cpp:116` | Regression from 787fab1: images inside hidden folders of archives were indexed again. | `isHiddenInnerPath`. | Fixed, T |
| M19 | `ThumbnailWorker.cpp:112` | Priority requests for paths already queued by background indexing were dropped: the on-screen card finished 159th of 160. | Queued task is promoted (`QThreadPool::tryTake` + restart at priority 10). | Fixed, T |
| M20 | `ThumbnailWorker.cpp:121` | Folder switch kept the previous root's backlog queued ahead of the new one. | `cancelPending()` on root change; indexing scoped to the root. | Fixed, R |
| M21 | `ThumbnailWorker.cpp:89,63` | Every finished thumbnail was re-read and decoded on the GUI thread; the cache held 1500 pixmaps (~0.5–0.8 GB). | Worker hands over the decoded `QImage`; cache costed in KiB (96 MiB budget). | Fixed, R |
| M22 | `ColorUtils.cpp:218` | `QFile::rename` never overwrites: replacements went through remove-then-rename (window with no file; orphan temp files). | `QSaveFile` (atomic replace on all platforms). | Fixed, T |
| M23 | `ColorUtils.cpp:145` | Width/height/aspect came from the pre-EXIF size: phone portraits stored as landscape. | Orientation-corrected size. | Fixed, T |
| M24 | `ColorUtils.cpp:176` | A null scaled image (OOM, empty size) was dereferenced → segfault on a worker thread. | Null checks. | Fixed, T |
| M25 | `ColorUtils.cpp:171` | Transparent pixels counted as black (logos/stickers classified "black"). | Alpha-weighted mean; fully transparent → gray. | Fixed, T |
| M26 | `ColorUtils.cpp:209` | WebP→JPEG fallback reused the failed `QImageWriter` (still WebP), so it never worked when a WebP handler existed but failed. | Fresh writer; one warning when WebP is unavailable. JPEG bytes keep the shared `.webp` name for cache compatibility; CI now installs the image-formats plugins. | Fixed, R |
| M27 | `ColorUtils.cpp:191` | dHash is not value-compatible with the Rust/Python apps sharing the DB (luma formula, orientation, source resolution): near-duplicates indexed by different apps never match. | **Needs your decision** (canonical definition across implementations). This PR uses Rec.601 integer luma on the thumbnail (PIL `convert("L")` semantics). | Open |
| M28 | `Config.cpp:34` | `GenericCacheLocation` ≠ the Rust app's hard-coded `~/.cache/omniview` whenever `XDG_CACHE_HOME` is set, and always on macOS/Windows: no sharing there. | **Needs your decision** (changing it moves existing C++ caches). | Open |
| M29 | `MainWindow.cpp:801` | File events during a scan were dropped. | Pending-rescan flag re-runs the scan when the current one finishes. | Fixed, R |
| M30 | `MainWindow.cpp:789` | Only the root and its first 199 top-level folders were watched. | `LibraryWatcher`: recursive (non-hidden, no symlinks), capped at 4096 dirs, failures surfaced in the status bar. | Fixed, T |
| M31 | `MainWindow.cpp:779` | Removing and re-adding all watches after each scan discarded queued inotify events. | Diffed watch set. | Fixed, T |
| M32 | `MainWindow.cpp:782` | Root deleted/unmounted → every watch dropped, never re-armed. | Nearest existing ancestor watched until the root returns. | Fixed, T |
| M33 | `MainWindow.cpp:797` | Debounce restarted on every event (no max wait) and survived manual rescans/root changes. | 1.2 s debounce with 10 s max delay; cancelled on rescan. | Fixed, T |
| M34 | `MainWindow.cpp:785` | Symlinked subdirectories were watched (rescans for files never indexed; `up -> ..` watched the parent). | `QDir::NoSymLinks`, matching the scanner. | Fixed, T |
| M35 | `MainWindow.cpp:928` | Changing root kept favourites-only/similarity: the new folder showed 0 of 3 images. | `FilterState::resetForNewRoot()`; new root's indexed rows shown immediately. | Fixed, T |
| M36 | `MainWindow.cpp:849` | When the selected folder vanished, the sidebar highlighted "All" while the gallery stayed filtered (empty). | Filter falls back to All Images. | Fixed, T |
| M37 | `MainWindow.cpp:747` | Rescan/folder change did `cancel(); wait()` on the GUI thread while prune (no cancel checks) ran: 249 ms local, seconds on NAS. | Asynchronous cancel + cancellable prune. | Fixed, T |
| M38 | `GalleryView.cpp:261` | Dragging many zip entries extracts them synchronously on the GUI thread (2.9 s for 150); on Wayland releasing the button during that freeze cancels the drag. | — | Open (follow-up: pre-extract on selection or show progress) |
| M39 | `GalleryView.cpp:521` | "Drag to Share" from the context menu starts a drag after the button was released; not supported on Wayland. | — | Open (needs manual Wayland test; see PR checklist) |
| M40 | `GalleryView.cpp:178` | Right-click in batch mode replaced the batch with one card. | Right press only sets the current index in batch mode. | Fixed, T |
| M41 | `GalleryView.cpp:165` | A click in the gap between cards wiped the batch. | Empty-space presses in batch mode are additive. | Fixed, T |
| M42 | `GalleryView.cpp:64,106` | Quadratic `getSelectedRows`; invert emitted one change per row (1.5 s for 2000 items). | Linear rows; one `Toggle` over the range. | Fixed, T |
| M43 | `GalleryView.cpp:567` | Single-item menu "Copy File/Path" copied the *selection*, not the clicked card. | Act on the clicked card. | Fixed, R |
| M44 | `GalleryDelegate.cpp:17` | See H8 (card size used `reset()`). | `sizeHintChanged`. | Fixed, T |
| M45 | `ViewerWindow.cpp:354` | Navigating to an undecodable image kept the previous title/favourite state while F acted on the new file. | Title/favourite always updated; error shown. | Fixed, T |
| M46 | `ViewerWindow.cpp:333` | Each navigation decodes synchronously on the GUI thread. | Decode is now bounded (≤8192 px, JPEG reduced decode). | Partly fixed; async/prefetch follow-up |
| M47 | `ViewerWindow.cpp:575` | Wheel zoom only worked while scrollbars could not move. | Viewport event filter, delta accumulation. | Fixed, T |
| M48 | `ViewerWindow.cpp:522`, `GalleryView.cpp:620` | "Reveal in folder" only tried dolphin/nautilus/xdg-open: nothing on Windows/macOS. | `FileActions::revealInFileManager`: Explorer `/select,`, `open -R`, Linux chain. | Fixed, R (manual test listed) |
| M49 | `ViewerWindow.cpp:404` | Pixmaps rendered at DPR 1: blurry on HiDPI, "1:1" really 200 %. | Render at `devicePixelRatioF()`. | Fixed, R |
| M50 | `MainWindow.cpp:862` | `refreshGallery` runs the whole query and builds all records on the GUI thread (~400 ms at 100k). | — | Open (follow-up: lazy/paged model) |
| M51 | `ViewerWindow.cpp:140` | Toolbar forces a ~1.4–1.6k px minimum width (off-screen on 1366/1440 px displays). | — | Open (follow-up: QToolBar overflow) |
| M52 | `MainWindow.h:93` | `m_currentItems` duplicated the model (deep copy on every refresh) and was the root of H15. | Removed. | Fixed |
| M53 | `MainWindow.cpp:20` | `ensureDirectories`/`initSchema` failures ignored: empty gallery, no message. | Error shown in the status label. | Fixed, R |
| M54 | `Database.cpp:225` | Pruning hard-deletes rows (and favourites) for files under a temporarily missing *sub*-mount of the root. | — | Open: proper fix ("mark missing" instead of delete) needs a schema change → your decision |
| M55 | `ColorUtils.cpp:168` | Qt 6.4 re-quantises palette/mono images when smooth-scaling (dithered thumbnails). | Normalise to (A)RGB32 after decode. | Fixed, T |
| M56 | `ColorUtils.cpp:188` | Qt 6 float HSV moved boundary colours into the wrong bucket (20k colours differ Qt5↔Qt6). | Double-precision HSV from 8-bit values. | Fixed, T |
| M57 | `ColorUtils.cpp:147` | Pixel cap ignores bytes/pixel (64 MP 16-bit PNG ≈1 GB transient on Qt 5). | Qt 6 allocation limit + reduced decode where supported. | Partly fixed (Qt 5 16-bit PNG worst case remains) |
| M58 | `ZipUtils.cpp:95` | (Windows, untested) miniz opens archives without share-delete, so Explorer can't move/delete a zip while it's being indexed. | — | Open (follow-up; manual Windows test listed) |
| M59 | `ZipUtils.cpp:154` | Each entry read re-opens the archive and walks the directory (O(N²) for big archives). | — | Open (follow-up: per-worker archive cache) |
| M60 | `main.cpp:88` | A typo'd/non-directory argument silently indexed the entire `$HOME`. | Warn and open the default Pictures folder. | Fixed, R |
| M61 | `tests/test_omniview.cpp:296` | QSignalSpy on a signal emitted from the Scanner thread: TSan data race, flaky test. | Tests wait on the thread / use queued connections. | Fixed, S |
| M62 | `ColorUtils.cpp` (this PR, from CodeRabbit) | The pixel cap rejected >64 MP JPEGs (108/200 MP phone photos) even though libjpeg decodes them at up to 1/8 per side: no thumbnail, "too large" in the viewer. | JPEG/WebP may be 64× larger when a bounding box applies; others (PNG decodes in full despite advertising `ScaledSize`) keep the cap. | Fixed, T |
| M63 | `ViewerWindow.cpp` (this PR, from CodeRabbit) | `MAX_VIEW_DIMENSION` capped logical pixels; at a device pixel ratio of 2 the backing pixmap reached 16384² (~1 GiB) on every zoom step. | Cap applied in device pixels. | Fixed, T (`ui_tests_hidpi`) |

### Low

| ID | Where | Issue | Status |
|---|---|---|---|
| L1 | `GalleryView.cpp:588` | `&` in folder names became a mnemonic in menu text | Fixed |
| L2 | `ViewerWindow.cpp:411`, `MainWindow.cpp:830,885` | Chained `QString::arg` substituted `%1`/`%2` inside file/folder names | Fixed, T |
| L3 | `ViewerWindow.cpp:405` | `*@2x.png` files drawn at half size (reader DPR kept) | Fixed, T |
| L4 | `ViewerWindow.cpp:375` | Fit used a hard-coded 1000×700 in short windows | Fixed |
| L5 | `ViewerWindow.cpp:566` | "Copy Image" tooltip advertised Ctrl+C (bound to Copy File) | Fixed |
| L6 | `ViewerWindow.cpp:347` | Minimised viewer not restored when opening another image | Fixed |
| L7 | `ViewerWindow.cpp:44` | DragButton ignored presses at (0,0) | Fixed |
| L8 | `ViewerWindow.cpp:328` | Non-const access detached (deep-copied) the item list | Fixed |
| L9 | `ViewerWindow.cpp:433` | Zoom-out had a 5 % floor while fit allowed 1 % (zoom-out enlarged tall images) | Fixed, T |
| L10 | `ViewerWindow.cpp:576` | Wheel ignored delta size (touchpads jumped to 10×) | Fixed |
| L11 | `ViewerWindow.h:87` | Closed viewer kept the decoded image in memory | Fixed |
| L12 | `GalleryView.cpp:396` | Ctrl+F / Ctrl+S / Alt+F toggled favourites | Fixed, T |
| L13 | `GalleryView.cpp:295`, `ViewerWindow.cpp:79` | Drag badge missed the cache for zip items and decoded full images (EXIF ignored) | Fixed |
| L14 | `ZipUtils.cpp:218` | Extracted files were named by hash in drag targets | Fixed, T |
| L15 | various | Dead code: `getAllHashesForPaths`, `storeCachedPixmap`, `copySelectedImage` | Removed |
| L16 | `GalleryView`/`ViewerWindow` | Drag/clipboard/reveal code copied 4× and diverging | `FileActions` |
| L17 | `ColorUtils.cpp:78` | `0000…` was both "no hash" and a real hash (flat images excluded from similarity) | Fixed, T |
| L18 | `ColorUtils.cpp:107` | Hand-rolled popcount | `qPopulationCount` |
| L19 | `MainWindow.cpp:875` | Unstable similarity sort | `std::stable_sort`, T |
| L20 | `main.cpp:80` | `--version` printed nothing | Fixed |
| L21 | `main.cpp:10`, `GalleryView.cpp:198`, `ViewerWindow.cpp:618` | Qt 6 deprecations | Fixed |
| L22 | `ZipUtils.cpp:42,47` | `./img.png` and `1:30.png` rejected; names >511 bytes truncated | Fixed, T |
| L23 | `ZipUtils.cpp:83` | Duplicate check used un-normalised backslash names | Fixed, T |
| L24 | `Database.cpp:398` | Name sort put all uppercase before lowercase | `COLLATE NOCASE`, T |
| L25 | `Database.cpp:400` | Random sort reshuffles on every refresh | Open |
| L26 | `Database.cpp:379` | Search case-insensitive for ASCII only | Open |
| L27 | `ZipUtils.cpp:137` | Zip entry mtimes depend on local time zone | Open |
| L28 | `ZipUtils.cpp:230` | Extraction cache never evicted | Partly (no temp orphans now); eviction open |
| L29 | `Database.cpp:108` | Old DBs keep a redundant `idx_path` | Open (schema change → your call) |
| L30 | `ViewerWindow.cpp:343` | Animated GIF/WebP show the first frame only | Open |
| L31 | `ViewerWindow.cpp:427` | Zoom does not keep the view centre | Open |
| L32 | `ViewerWindow.cpp:399` | No true 1:1 above 8192 px | Open (by design: bounded decode) |
| L33 | `MainWindow.cpp:490,556,145` | Duplicated stylesheets; bare `QFrame` rule borders every label; low light-mode contrast | Open (cosmetic) |
| L34 | `GalleryModel.cpp:118` | Square filter shows unindexed images until the next refresh | Open |
| L35 | `MainWindow.cpp:57` | Emoji font fallback list in three places | Open (cosmetic) |
| L36 | `Database.cpp:486` | `getStats` ran 3 queries | One query |
| L37 | `Database.cpp:64` | PRAGMA failures unchecked | Warned |
| L38 | `MainWindow.h:74` | View/delegate hold raw pointers to a `ThumbnailManager` member destroyed before them (latent; no use during destruction) | Open (documented) |
| L39 | Qt 5 offscreen plugin | LSan leak in `libqoffscreen` | Suppressed (third-party) |
| L40 | `ColorUtils.cpp` (this PR) | One-time WebP warning flag was a plain `static bool` written from pool threads | `std::atomic<bool>` |
| L41 | `GalleryView.cpp` (this PR) | Card context-menu actions kept a persistent index, which a model reset during `menu.exec()` invalidates ("Select" became a no-op) | Row resolved by path when the action runs |
| L42 | `ci.yml` | Workflow token used repository-default permissions | `permissions: contents: read` |
| L43 | `CMakeLists.txt` | `OMNIVIEW_SANITIZE=thread,address` bypassed the TSan+ASan conflict check | Commas normalised |

## 3. Design changes

- **Testable logic out of `MainWindow`** (1.1k lines): `FilterState` (all filter state + transitions +
  similarity ranking), `LibraryWatcher` (watch set, debounce, re-arming), `FileActions` (drag/clipboard/
  reveal/open). Behaviour-preserving except for the bugs listed above; all three have unit tests.
- **Ownership:** `Database` owns exactly one connection (RAII, non-copyable); scanners are parented and
  `deleteLater`ed; runnables never outlive `ThumbnailManager` (queue cleared + waited in its destructor).
- **Threading contract** documented in `ThumbnailWorker.h`: only `taskStarting/taskFinished` cross threads;
  results come back with a queued call; QPixmaps exist only on the GUI thread.
- **CMake:** `omniview_core` static library shared by the app and tests (sources were compiled 2–3×);
  vendored miniz built as its own target; `OMNIVIEW_WARNINGS`, `OMNIVIEW_WARNINGS_AS_ERRORS`,
  `OMNIVIEW_SANITIZE` (all off by default, warnings on project targets only); install rule; version macro.
- **CI:** Linux/Windows get the WebP image-format plugins; new Linux sanitizer matrix (ASan+UBSan, TSan ×
  Qt 5, Qt 6) with project warnings enabled.
- **Review tooling:** `.clang-tidy` (with rationale for exclusions), `.coderabbit.yaml`.

## 4. Deliberately not done (and why)

| Item | Why |
|---|---|
| M27 canonical dHash across implementations | Cross-repository decision; needs agreement with the Python/Rust versions. |
| M28 cache directory on macOS/Windows/`XDG_CACHE_HOME` | Changes the shared cache path — you asked to be consulted. |
| M54 "mark missing" instead of deleting rows; persisting decode failures; dropping `idx_path`; schema versioning (`PRAGMA user_version`) | Schema changes without an agreed migration — you asked to be consulted. |
| M38/M46/M50/M59 async extraction, async viewer decode, lazy gallery model, archive handle cache | Larger refactors with their own risk; better as focused follow-up PRs. |
| M39 Wayland drag from menu, M58 Windows share mode | Cannot be verified in this headless Linux container; listed for manual testing. |
| M51, L25–L35 | Low impact or cosmetic. |
| Extracting the theme/stylesheet code from `MainWindow` | Pure refactor with no test coverage possible offscreen; listed as follow-up. |

## 5. Suggested follow-ups

1. Agree a canonical dHash/colour definition and cache directory with the Python/Rust implementations (M27, M28).
2. Schema v2 with `PRAGMA user_version` migrations: `missing` flag instead of deletes, persisted decode failures, drop `idx_path`.
3. Move gallery queries off the GUI thread and page the model (M50); async viewer decode with prefetch (M46).
4. Per-worker archive handle cache (M59) and a global in-flight decompression budget (M15).
5. Extract theming (one palette struct + stylesheet template) out of `MainWindow` and `ViewerWindow`.
