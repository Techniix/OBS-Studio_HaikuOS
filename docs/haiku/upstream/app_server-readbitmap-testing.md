# Testing the app_server `ReadBitmap()` patch on your machine

> **Stock Haiku only.** The `renga` branch of OBS does not use `BScreen::ReadBitmap()`: it captures through Renga's
> `BScreenCapture` sessions (see `ARCHITECTURE.md`, section 9). This document applies to the `main` branch on stock
> Haiku, where the *Capture method* setting and the read throttle it mentions still exist.

This builds Haiku's system package (`haiku.hpkg`) from the same revision you're running, with the patch applied,
and installs it. If anything goes wrong, Haiku's boot menu can return you to the system exactly as it was. Read the
recovery section before you start.

The patch is unofficial and has only been syntax-checked, never built or run. The install step (5) is the one I'm
least sure about; stop and report if it doesn't behave as described.

## 0. Recovery, in case the new app_server doesn't start

Every package change makes Haiku keep the previous package set as a "state".

1. Reboot and hold **Shift** (or Space) until the boot menu appears.
2. **Select boot volume** → your Haiku volume → **Select state**.
3. Pick the state from before the install (the newest one older than today's change) → **Return to main menu** →
   **Continue booting**.

That boots the old `haiku.hpkg`. To make it permanent, open a Terminal and run `pkgman install` on the original
package, or `pkgman full-sync`.

## 1. Find your exact Haiku revision

```sh
uname -v
```

Note the `hrevNNNNN` number. The examples below use `hrevNNNNN`; replace it with yours.

## 2. Get the matching source and apply the patch

```sh
mkdir -p ~/haiku-dev && cd ~/haiku-dev
git clone --depth 1 --branch hrevNNNNN https://review.haiku-os.org/haiku.git
cd haiku
git -C ~/OBS-Studio_HaikuOS pull     # wherever you cloned it; 0002, 0004 and 0005 were added on 2026-11-07
rm -rf ~/haiku-dev/patches && mkdir ~/haiku-dev/patches
cp ~/OBS-Studio_HaikuOS/docs/haiku/upstream/0*.patch ~/haiku-dev/patches/
# Only for the hardware-cursor tests (nvidia-haiku, branch rtx5070-hw-cursor):
# cp ~/nvidia-haiku/docs/haiku-upstream/0003-*.patch ~/haiku-dev/patches/
for f in ~/haiku-dev/patches/0*.patch; do
	git apply --check "$f" && git apply "$f" && echo "applied $(basename $f)" || { echo "FAILED: $f"; break; }
done
git status --short     # 6 modified files under src/servers/app (8 with 0003)
```

The loop applies 0001, 0002, (0003,) 0004 and 0005 in order. They were made against master of 2026-11-06 and still
apply to master of 2026-11-07. If a `--check` fails, your revision differs in these files; send me the error and the
hrev and I'll rebase them.

## 3. Configure the build (on Haiku, no cross-compiler is needed)

```sh
which jam || pkgman install jam
mkdir generated.x86_64 && cd generated.x86_64
../configure --target-arch x86_64
```

## 4. Build: app_server alone first (quick check), then the system package

```sh
jam -q -j$(nproc) app_server
jam -q -j$(nproc) haiku.hpkg
find . -name haiku.hpkg
```

The first command catches compile errors in minutes. The second builds everything in Haiku's base package (kernel,
libbe, servers, apps) and takes much longer. The first run also downloads build packages. Save the output of a
failed step and send it.

## 5. Install

```sh
pkgman install ./objects/haiku/x86_64/packaging/packages/haiku.hpkg
```

(Use the path `find` printed.) pkgman shows what it will change: it should be **one package, `haiku`**, being
replaced. The new one's version ends in `_hrevNNNNN_dirty-1` ("dirty" because the patches are uncommitted), which
sorts after your installed `_hrevNNNNN-1`. If it wants to change anything else, answer **no** and send me the output. Then reboot.

## 6. Test

1. After rebooting, check that the desktop comes up normally and the pointer and windows behave as before.
2. Run `screenshot` and `screenshot -m`. Open both images: the first should have **no** pointer, the second
   exactly one.
3. In OBS (`OBS_HAIKU_GL=osmesa ./obs --verbose`), add a Screen Capture with **Capture method: Tear-free** at 30
   FPS. Move the pointer around: it should be smooth now. In the log, the `[haiku-capture]` line about the read
   time should show a few milliseconds instead of ~25–30. If no such line appears, capture runs at the full rate;
   say so.
4. Turn **Capture Cursor** off in the source: the pointer should disappear from the preview and the recording.
5. Leave a constantly redrawing window on screen (e.g. Terminal running `top`, or a clock) while capturing. The
   capture must not flicker or show blank/erased areas.
6. If you have a `BDirectWindow` application (e.g. the Chart demo, or a game that uses SDL's software renderer),
   capture it with a normal window partly on top of it. Both should look right.
7. Optional: change the resolution in Screen preferences while OBS captures.

Send back the results (and the hrev). I'll add them to the report for the Haiku developers.

## 7. Going back to the official package

```sh
pkgman full-sync
```

Or use the boot menu (section 0).
