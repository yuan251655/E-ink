"""Create a 192000-byte raw frame compatible with the official PhotoPainter BSP.

This is a verification tool for the product source_plus_bin contract.  It
deliberately mirrors ImgDecode_DitherRgb888(): nearest six-color RGB palette
and Floyd-Steinberg error diffusion.  The generated raw buffer is already in
the controller's 800x480 orientation, so DisplayService uses Rotation=0.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

PORTRAIT_WIDTH, PORTRAIT_HEIGHT = 480, 800
LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT = 800, 480
PALETTE = np.array([
    (0, 0, 0),       # black
    (255, 255, 255), # white
    (255, 0, 0),     # red
    (0, 255, 0),     # green
    (0, 0, 255),     # blue
    (255, 255, 0),   # yellow
], dtype=np.int16)
# ColorBlack, ColorWhite, ColorRed, ColorGreen, ColorBlue, ColorYellow in
# components/port_bsp/display_bsp.h.
PANEL_CODES = np.array([0, 1, 3, 6, 5, 2], dtype=np.uint8)


def official_dither(rgb: np.ndarray) -> np.ndarray:
    work = rgb.astype(np.int16, copy=True)
    height, width, _ = work.shape
    indices = np.empty((height, width), dtype=np.uint8)
    for y in range(height):
        for x in range(width):
            old = np.clip(work[y, x], 0, 255)
            # The official implementation uses 32-bit int for squared RGB
            # distance. Do not let NumPy's int16 intermediate overflow.
            diff = PALETTE.astype(np.int32) - old.astype(np.int32)
            color = int(np.argmin(np.sum(diff * diff, axis=1)))
            new = PALETTE[color]
            indices[y, x] = color
            error = old - new
            if x + 1 < width:
                work[y, x + 1] = np.clip(work[y, x + 1] + error * 7 // 16, 0, 255)
            if y + 1 < height:
                if x > 0:
                    work[y + 1, x - 1] = np.clip(work[y + 1, x - 1] + error * 3 // 16, 0, 255)
                work[y + 1, x] = np.clip(work[y + 1, x] + error * 5 // 16, 0, 255)
                if x + 1 < width:
                    work[y + 1, x + 1] = np.clip(work[y + 1, x + 1] + error // 16, 0, 255)
    return indices


def pack(controller_indices: np.ndarray) -> bytes:
    codes = PANEL_CODES[controller_indices].reshape(-1)
    return bytes(((int(codes[i]) << 4) | int(codes[i + 1])) for i in range(0, len(codes), 2))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output_bin', type=Path)
    parser.add_argument('--preview', type=Path)
    parser.add_argument('--landscape-contain', action='store_true')
    args = parser.parse_args()
    image = ImageOps.exif_transpose(Image.open(args.source)).convert('RGB')
    if args.landscape_contain:
        contained = ImageOps.contain(
            image, (LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT), Image.Resampling.LANCZOS)
        # Use the source corner as the padding color so the added bands blend
        # into paper-textured artwork instead of introducing stark white bars.
        background = image.getpixel((0, 0))
        landscape = Image.new('RGB', (LANDSCAPE_WIDTH, LANDSCAPE_HEIGHT), background)
        landscape.paste(contained, ((LANDSCAPE_WIDTH - contained.width) // 2,
                                    (LANDSCAPE_HEIGHT - contained.height) // 2))
        landscape_indices = official_dither(np.asarray(landscape))
        payload = pack(landscape_indices)
        if len(payload) != 192000:
            raise RuntimeError(f'unexpected frame length: {len(payload)}')
        args.output_bin.parent.mkdir(parents=True, exist_ok=True)
        args.output_bin.write_bytes(payload)
        if args.preview:
            args.preview.parent.mkdir(parents=True, exist_ok=True)
            Image.fromarray(PALETTE[landscape_indices].astype(np.uint8), 'RGB').save(args.preview)
        return
    # Match the official portrait path: resize to 480x800, then the BSP's
    # Rotation=3 path turns it into the physical 800x480 controller frame.
    portrait = image.resize((PORTRAIT_WIDTH, PORTRAIT_HEIGHT), Image.Resampling.BILINEAR)
    portrait_indices = official_dither(np.asarray(portrait))
    controller_indices = np.rot90(portrait_indices, k=1)
    payload = pack(controller_indices)
    if len(payload) != 192000:
        raise RuntimeError(f'unexpected frame length: {len(payload)}')
    args.output_bin.write_bytes(payload)
    if args.preview:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(PALETTE[portrait_indices].astype(np.uint8), 'RGB').save(args.preview)


if __name__ == '__main__':
    main()
