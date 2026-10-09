"""Rebuild app/icon/scopedeck.ico from app/icon/scopedeck_icon.png.

Dev-time only: the .ico is committed, so building the app never runs this or
needs Python. Re-run it by hand after replacing the source PNG:

    python tools/make_icon.py

Every size is downsampled straight from the full-resolution source. The source
is a palette PNG whose transparent index carries a hidden orange (201, 69, 38);
that colour does not leak into the rim, because Pillow's resize premultiplies
RGBA internally - measured by recolouring every alpha-0 pixel and confirming all
eight sizes come out byte-identical.
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "app" / "icon" / "scopedeck_icon.png"
TARGET = ROOT / "app" / "icon" / "scopedeck.ico"

# Windows picks the nearest size per context: 16/20/24 title bar and small
# lists, 32/40/48 taskbar and Alt-Tab across 100-200% scaling, 256 Explorer.
SIZES = (16, 20, 24, 32, 40, 48, 64, 256)


def downsample(source: Image.Image, size: int) -> Image.Image:
    return source.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    if source.width != source.height:
        raise SystemExit(f"{SOURCE.name} is {source.width}x{source.height}; an icon must be square")

    frames = [downsample(source, size) for size in SIZES]
    # Pillow writes one ICO entry per frame passed via append_images; the 256
    # entry is stored as PNG, the rest as BMP, which is what Windows expects.
    frames[-1].save(TARGET, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print(f"wrote {TARGET} ({', '.join(str(s) for s in SIZES)})")


if __name__ == "__main__":
    main()
