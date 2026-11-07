# app_server: `BScreen::ReadBitmap()` stalls the desktop and the mouse pointer

**Report for the Haiku developers**

> **Disclosure.** The analysis and the patch below were written by an AI assistant (Claude, by Anthropic). Dalton
> Singleton (GitHub: Techniix) asked for them while porting OBS Studio to Haiku. Haiku's contribution policy does
> not accept AI-generated code. The patch is offered only as a precise description of the problem and of one
> possible fix. Please rewrite it, adapt it or discard it as you see fit. Nothing here has been built with jam or
> run yet; see [Verification](#verification) for exactly what was checked.

| | |
| --- | --- |
| Component | app_server (`src/servers/app`) |
| Affects | Every client that calls `BScreen::ReadBitmap()` repeatedly (screen recorders, streaming software, VNC-like tools), on any accelerant |
| Symptom | While such a client runs, the mouse pointer moves in bursts and pauses in a regular rhythm, and the whole desktop gets sluggish |
| Base revision | haiku master `b9fdc8a9e6` (2026-11-06) |
| Patch | `0001-app_server-serve-ReadBitmap-from-the-back-buffer-whe.patch` (8 files, +138/−14) |

## 1. Observed behavior

OBS Studio's screen capture on Haiku calls `BScreen::ReadBitmap()` once per frame from a background thread. The
test system has an NVIDIA GeForce RTX 5070 (`nvidia_gsp` accelerant, from the nvidia-haiku driver) at 2560×1080,
32 bpp. While it captures:

- One full-screen `ReadBitmap()` takes about **25–30 ms**. That is inferred from the capture rate OBS sustained when
  limited to spending 25% of wall time in `ReadBitmap()`; there is no direct trace.
- **The pointer stops moving for the length of each read**, then catches up. At 30 reads/s this makes the desktop
  unusable. Even throttled to about 8 reads/s, the stutter is obvious.
- Without throttling (30 reads/s), the system became nearly unresponsive. It later crashed. The crash may have had
  other causes and was not investigated.

As a workaround, OBS now caps the time spent reading and offers an option to read the screen in ~2 ms horizontal
bands. That keeps the pointer smooth, but something moving during a read can then show a seam. Neither is a real
fix. Applications can't avoid the stall, because the cost is in the app_server.

## 2. Root cause

`ServerApp::_DispatchMessage()`, `case AS_READ_BITMAP` (`ServerApp.cpp:3388`):

```cpp
if (fDesktop->GetDrawingEngine()->LockExclusiveAccess()) {
    success = fDesktop->GetDrawingEngine()->ReadBitmap(bitmap, drawCursor, bounds) == B_OK;
    fDesktop->GetDrawingEngine()->UnlockExclusiveAccess();
}
```

`DrawingEngine::ReadBitmap()` (`drawing/DrawingEngine.cpp:1375`) copies from `fGraphicsCard->FrontBuffer()`, the
frame buffer itself.

1. **The read holds the HWInterface write lock for its whole duration.** `HWInterface::MoveCursorTo()`
   (`drawing/HWInterface.cpp:233`) starts with `LockParallelAccess()` (the read side of the same lock), so cursor
   movement waits until the copy is done. So does all drawing, which also takes the parallel lock. A hardware cursor
   doesn't help, because the lock is taken first either way.
2. **The copy reads video memory.** Frame buffers are mapped uncached or write-combined, where CPU reads are an order
   of magnitude or more slower than reads from main memory. 2560×1080×4 bytes (10.5 MiB) in ~25–30 ms is ~400 MB/s.
3. **The same image is already in main memory.** `AccelerantHWInterface` always has a back buffer
   (`AccelerantHWInterface::SetMode()`, "update backbuffer if necessary", `AccelerantHWInterface.cpp:637`; the
   condition recreates it for `B_RGB32` modes as well). The DrawingEngine renders into the back buffer
   (`HWInterface::DrawingBuffer()`), and `_CopyBackToFront()` copies changed areas to the frame buffer. So
   `IsDoubleBuffered()` is true for all accelerants.

Reading 10.5 MiB from main memory takes a few milliseconds, roughly 5–10× less time holding the lock.

## 3. Proposed change

When `IsDoubleBuffered()`, read from the back buffer, except where the screen's current content exists only in the
front buffer:

| Content | Current in the back buffer? | Handling in the patch |
| --- | --- | --- |
| Everything the app_server draws outside update sessions (windows, decorators, desktop) | Yes | Read from the back buffer |
| Windows in an update session (`Window::InUpdate()`) | **No.** `_BeginUpdate()` disables copy-to-front and erases backgrounds in the back buffer. The client then redraws over several messages, and the result reaches the front buffer only in `EndUpdate()`. | Their visible content region is read from the front buffer, which still shows the last complete frame |
| `BDirectWindow` content while directly accessing (`ServerWindow::IsDirectlyAccessing()`) and `kWindowScreenFlag` windows | No, clients draw into the front buffer | Their visible content region is read from the front buffer |
| Whole screen while a `BWindowScreen` holds `Desktop::LockDirectScreen()` | No | New `Desktop::IsDirectScreenLocked()`: the old front-buffer path is used for everything |
| Software cursor | No, it is composited from the back buffer onto the front buffer only | Its area (new `HWInterface::SoftwareCursorFrame()`) is always read from the back buffer, which holds what is under it. `drawCursor = true` composites it afterwards (existing code) |
| Hardware overlays | In neither buffer | Unchanged (they were never captured) |

Where the pieces live:

- **`ServerApp`**, `case AS_READ_BITMAP`, only when `HWInterface()->IsDoubleBuffered()`:
  - Under `Desktop::LockAllWindows()`, collects the visible content regions listed above for the current
    workspace.
  - The write lock is needed because `Window::VisibleContentRegion()` recomputes and caches its result on every
    call. Under the read lock, the window's own thread may be reading it at the same time.
  - Then it takes the engine's exclusive lock and reads. Window lock first, then engine lock: the same order
    `Desktop` uses.
  - Single-buffered and remote interfaces use the unchanged old path and take no window lock (the remote engine
    can block for seconds on the network).
- **`DrawingEngine::ReadBitmap()`** gets an optional `const BRegion* frontBufferRegion`; `NULL` means the old
  behavior. With a region, it:
  - snaps `bounds` to integer coordinates;
  - copies `bounds` from the back buffer, treating it as `B_RGB32` because its alpha bytes are not meaningful,
    exactly as `_CopyToFront()` treats it;
  - copies each rectangle of `(frontBufferRegion ∩ bounds) − software cursor frame` from the front buffer to the
    same position;
  - composites the cursor if requested, as before.
- **`HWInterface::SoftwareCursorFrame()`** is new. It returns the `fCursorAreaBackup` rectangle under
  `fFloatingOverlaysLock`. The engine lock is always taken before that lock (`SetCursor()`, `MoveCursorTo()`,
  `ObscureCursor()`, `AccelerantHWInterface`), and `ReadBitmap()` does the same.
- **`RemoteDrawingEngine::ReadBitmap()`** gets the same parameter and ignores it.

### Behavior changes

- `ReadBitmap(..., drawCursor = false)` no longer contains the software cursor. Before, the front buffer already
  contained it, and `AutoFloatingOverlaysHider` did nothing because `HideFloatingOverlays()` returns early when
  `IsDoubleBuffered()`. Screenshot tools and recorders that ask for no cursor now get none.
- `ReadBitmap(..., drawCursor = true)` no longer composites the software cursor a second time over the copy already
  in the front buffer, which used to double its shadow.
- In 8/15/16-bit modes, pixels outside the front-buffer regions now have the back buffer's 32-bit colors rather than
  the quantized colors actually displayed.
- Right after a mode switch, the back buffer is recreated filled with white (255) until the windows redraw. A read in
  that short window returns white where the old code returned stale front-buffer content.

## 4. Possible follow-ups (not in the patch)

- `MoveCursorTo()` could avoid blocking on the exclusive lock when a hardware cursor is in use. With this patch, the
  lock is held so briefly that it matters much less.
- A mode for recorders that only needs the parallel lock (accepting tearing) or a damage-tracking API (like
  PipeWire's or `CGDisplayStream`) would let capture tools skip unchanged frames entirely.

## 5. Verification

What was done:

- **Syntax-checked (not compiled).** All eight modified files and their dependents (`DrawingEngine.cpp`,
  `HWInterface.cpp`, `ServerApp.cpp`, `RemoteDrawingEngine.cpp`, `AccelerantHWInterface.cpp`, `Desktop.cpp`,
  `ServerWindow.cpp`, `Window.cpp`) were checked with clang 18 (`--target=x86_64-unknown-haiku -fsyntax-only`),
  against the Haiku tree's headers and the app_server's own include paths from its Jamfiles. No errors or new
  warnings.
- **The patch applies to master `b9fdc8a9e6`.**
- **Reviewed against the source.** A second AI pass, which hadn't written the patch, reviewed the first version. It
  found six problems, all fixed in this version:
  1. a data race from calling `VisibleContentRegion()` under the read lock (now the write lock);
  2. half-redrawn windows being captured during update sessions (they are now read from the front buffer);
  3. a garbage alpha channel for `B_RGBA32` bitmaps (the back buffer is now read as `B_RGB32`);
  4. the remote interface being made to take the window lock (it no longer does);
  5. a one-pixel offset with fractional bounds (bounds are now snapped);
  6. the software cursor leaking into direct-window areas (its frame is now excluded).

  The same review checked every site that takes the window lock and the engine lock and found no lock-order
  inversion. It also confirmed that nothing in `src/servers/app` writes to the front buffer except
  `_CopyToFront()` (through `CopyBackToFront`, `_DrawCursor`, `_RestoreCursorArea`) and direct-window clients.

What has **not** been done yet:

- building `app_server` with jam
- running it
- measuring the new read time

The OBS port's author will install it on the RTX 5070 machine; results will be added here.

### Suggested test plan

1. **Screenshot.** Run `screenshot` (the CLI in `/bin`) with and without `--mouse-pointer`. The image should be
   correct and the cursor should be present or absent as requested.
2. **Update sessions.** Capture repeatedly while windows redraw constantly (a clock, a progress bar, `ps` scrolling
   in Terminal). The captures must not flicker or show erased backgrounds.
3. **Pointer smoothness.** Run a client that calls `ReadBitmap()` in a loop (e.g. OBS screen capture at 30 fps with
   the "Tear-free" method). The pointer should stay smooth. Log the duration of each `ReadBitmap()`: it should drop
   from ~25–30 ms to a few ms at 2560×1080.
4. **Direct windows.** Run a `BDirectWindow` application (e.g. the Chart demo) with an app_server window
   overlapping it. Captures should show the direct content and the overlapping window correctly.
5. **BWindowScreen.** Run a `BWindowScreen` application and take a screenshot. Output should match the old behavior.
6. **Mode changes.** Change resolution and color depth (Screen preferences) while a recorder is reading.
7. **Unaffected setups.** Check VESA / framebuffer drivers and the remote desktop interface.
8. **B_RGBA32 target.** Call `ReadBitmap()` into a `B_RGBA32` bitmap; every pixel's alpha should be 255.

## 6. The patch

See `0001-app_server-serve-ReadBitmap-from-the-back-buffer-whe.patch` next to this report (`git am`-able against
master `b9fdc8a9e6`).
