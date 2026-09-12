#!/usr/bin/env python3
"""Checks that a torch-lit dungeon actually draws something.

Why this exists: an unlit dungeon is a black screen *by design* -- DrawDungeon
clears the view and prints "It's dark!" when gTorch is zero, exactly as the
Apple II original did. That makes a broken dungeon renderer invisible: it would
also show a black screen, and look to a player like a missing torch. This
pins the difference, so a regression in the wall, floor and ceiling drawing
fails here instead of being mistaken for correct behaviour.

Drives the game's own U3_DUNGEON_TEXT_CHECK snapshots, which capture the
dungeon view unlit ("before") and with a torch ("lit") in both the classic and
modern appearances, then compares the view region of each.

Usage: dungeon-render-check.py <ultima3-binary> <assets-dir> <work-dir>
"""

import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

# The dungeon view occupies roughly the upper-left of the 40x24-tile screen.
# Expressed as fractions so the crop follows the canvas in double-size mode
# rather than being pinned to 640x384.
VIEW = (0.047, 0.143, 0.453, 0.482)

# An unlit view is cleared to black, so anything above a rounding-level mean is
# a real drawing. The lit view measures far above this (~16000 of 65535); the
# threshold sits well below that but far above zero, so it catches "renders
# nothing" without being brittle about exact pixel values.
LIT_MIN_MEAN = 2000
UNLIT_MAX_MEAN = 50


def view_mean(path):
    """Mean luminance of the dungeon view region, 0..65535 to match ImageMagick."""
    with Image.open(path) as image:
        grey = image.convert("L")
        w, h = grey.size
        box = (int(VIEW[0] * w), int(VIEW[1] * h), int(VIEW[2] * w), int(VIEW[3] * h))
        region = grey.crop(box)
        pixels = list(region.getdata())
    if not pixels:
        raise SystemExit(f"error: empty crop for {path}")
    return sum(pixels) / len(pixels) * 257.0  # 0..255 -> 0..65535


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    binary, assets, work = sys.argv[1], sys.argv[2], sys.argv[3]

    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    saves = work / "saves"
    if saves.exists():
        for child in saves.iterdir():
            child.unlink()
    saves.mkdir(parents=True, exist_ok=True)
    prefix = work / "dungeon"

    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "U3_ASSETS_DIRECTORY": assets,
        "U3_SAVE_DIRECTORY": str(saves),
        "U3_GAMEPLAY_SCENARIO_CHECK": "1",
        "U3_WORLD_RENDER_CHECK": str(work / "world.bmp"),
        "U3_DUNGEON_TEXT_CHECK": str(prefix),
        "ASAN_OPTIONS": "detect_leaks=0",
    })

    result = subprocess.run([binary], env=env, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"error: the game exited {result.returncode}")

    failures = []
    for mode in ("classic", "modern"):
        # U3CocoaWriteMainBitmap always writes a BMP, whatever the name says.
        unlit = Path(f"{prefix}-{mode}-before.png")
        lit = Path(f"{prefix}-{mode}-lit.png")
        missing = [path.name for path in (unlit, lit) if not path.exists()]
        if missing:
            # Only this mode's snapshots are unusable; the other is still worth
            # reporting, so skip on `missing` rather than on the shared list.
            failures.extend(f"{mode}: {name} was not written" for name in missing)
            continue

        unlit_mean, lit_mean = view_mean(unlit), view_mean(lit)
        print(f"{mode}: unlit view mean {unlit_mean:.1f}, lit view mean {lit_mean:.1f}")

        if unlit_mean > UNLIT_MAX_MEAN:
            failures.append(
                f"{mode}: an unlit dungeon should be black, but the view "
                f"measured {unlit_mean:.1f}")
        if lit_mean < LIT_MIN_MEAN:
            failures.append(
                f"{mode}: a torch-lit dungeon drew nothing (view measured "
                f"{lit_mean:.1f}); walls, floor and ceiling are missing")
        if lit_mean <= unlit_mean:
            failures.append(
                f"{mode}: lighting a torch did not change the view "
                f"({lit_mean:.1f} vs {unlit_mean:.1f})")

    if failures:
        for failure in failures:
            sys.stderr.write(f"FAIL: {failure}\n")
        raise SystemExit(1)
    print("dungeon renders dark without a torch and draws walls with one")


if __name__ == "__main__":
    main()
