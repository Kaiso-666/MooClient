"""Regenerate the embedded brand assets from the official logo.

Source of truth: res/logo.png (256x256 RGBA, converted from the official
moomoologo asset). Run this after replacing the logo:

    python tools/make_assets.py

It rewrites res/app.ico (multi-size, PNG-compressed entries) and
res/splash.png (logo + baked drop shadow, 1220x1000 canvas). Touch
res/app.rc afterwards so the resource compiler re-embeds both.
"""

from __future__ import annotations

import io
import struct
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "res"
MASTER = RES / "logo.png"

ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SPLASH_CANVAS = (1220, 1000)
SPLASH_LOGO = 800


def load_master() -> Image.Image:
    image = Image.open(MASTER).convert("RGBA")
    if image.size != (256, 256):
        image = image.resize((256, 256), Image.LANCZOS)
    return image


def upscale(image: Image.Image, size: int) -> Image.Image:
    """Upscale flat art without turning the edges to mush.

    The logo is a handful of flat colours, so a plain LANCZOS upscale is soft.
    Upscaling in two steps and re-applying the alpha silhouette keeps the
    outline tight while leaving the interior colours untouched.
    """
    big = image.resize((size, size), Image.LANCZOS)
    return big.filter(ImageFilter.UnsharpMask(radius=2.2, percent=70, threshold=0))


def icon_entry(image: Image.Image, size: int) -> bytes:
    small = image.resize((size, size), Image.LANCZOS)
    if size <= 48:
        # At tiny sizes the ring needs a touch more edge contrast to survive.
        small = small.filter(ImageFilter.UnsharpMask(radius=0.8, percent=60, threshold=0))
    buffer = io.BytesIO()
    small.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(image: Image.Image, path: Path) -> None:
    blobs = [(size, icon_entry(image, size)) for size in ICON_SIZES]
    header = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = bytearray()
    payload = bytearray()
    for size, blob in blobs:
        entries += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0,
            0,
            1,
            32,
            len(blob),
            offset,
        )
        payload += blob
        offset += len(blob)
    path.write_bytes(header + bytes(entries) + bytes(payload))


def write_splash(image: Image.Image, path: Path) -> None:
    canvas_w, canvas_h = SPLASH_CANVAS
    logo = upscale(image, SPLASH_LOGO)
    left = (canvas_w - SPLASH_LOGO) // 2
    top = (canvas_h - SPLASH_LOGO) // 2 - 40

    canvas = Image.new("RGBA", SPLASH_CANVAS, (0, 0, 0, 0))

    # Two shadows: a wide soft one for depth and a tight one to anchor the
    # shape. Both are built from the logo's own alpha so they follow the
    # outline exactly.
    alpha = logo.split()[3]
    for offset, blur, opacity in ((34, 40, 175), (14, 12, 135), (6, 4, 90)):
        shadow = Image.new("RGBA", SPLASH_CANVAS, (0, 0, 0, 0))
        mask = Image.new("L", SPLASH_CANVAS, 0)
        mask.paste(alpha, (left, top + offset))
        mask = mask.filter(ImageFilter.GaussianBlur(blur))
        mask = mask.point(lambda v, o=opacity: v * o // 255)
        shadow.paste((0, 0, 0, 255), (0, 0, canvas_w, canvas_h), mask)
        canvas = Image.alpha_composite(canvas, shadow)

    layer = Image.new("RGBA", SPLASH_CANVAS, (0, 0, 0, 0))
    layer.paste(logo, (left, top), logo)
    canvas = Image.alpha_composite(canvas, layer)

    canvas.save(path, format="PNG", optimize=True)


def main() -> None:
    image = load_master()
    write_ico(image, RES / "app.ico")
    write_splash(image, RES / "splash.png")
    print("wrote", RES / "app.ico", (RES / "app.ico").stat().st_size)
    print("wrote", RES / "splash.png", (RES / "splash.png").stat().st_size)


if __name__ == "__main__":
    main()
