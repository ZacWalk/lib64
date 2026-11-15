"""Vector source for the lib64 host's C64 wordmark and striped rainbow circle."""

import sys
from pathlib import Path

sys.dont_write_bytecode = True
from iconlib import Canvas, Colour, generate

ROOT = Path(__file__).resolve().parents[1]


def draw(canvas: Canvas) -> None:
    """Bold C64 lettering over five horizontal rainbow bands filling the entire circle."""
    ink = (12, 17, 26)
    canvas.circle(50, 50, 48, (35, 44, 62))
    colours = ((238, 35, 40), (247, 137, 0), (239, 215, 0), (74, 188, 39), (39, 107, 222))

    def rainbow(_: float, y: float) -> Colour:
        band = int((y - 4) * len(colours) / 92)
        return colours[min(len(colours) - 1, max(0, band))]

    canvas.circle(50, 50, 46, rainbow)
    canvas.polygon(((38, 33), (28, 33), (23, 36), (20, 42), (20, 58), (23, 64),
                    (28, 67), (38, 67), (38, 60), (30, 60), (27, 57), (27, 43),
                    (30, 40), (38, 40)), ink)
    canvas.line(((56, 38), (52, 36), (47, 36), (44, 40), (44, 61), (48, 64),
                 (54, 64), (58, 60), (58, 54), (54, 51), (45, 51)), 6.5, ink)
    canvas.polygon(((73, 33), (80, 33), (80, 54), (84, 54), (81, 61), (80, 61),
                    (80, 67), (73, 67), (73, 61), (62, 61), (62, 54)), ink)
    canvas.polygon(((73, 44), (73, 54), (67, 54)), rainbow)


if __name__ == "__main__":
    raise SystemExit(generate(draw, ROOT / "app/res/c64.ico"))
