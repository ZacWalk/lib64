"""Headless checks for the C64 icon's full-circle rainbow and readable vector lettering."""

import sys
import unittest

sys.dont_write_bytecode = True
from generate_icon import draw
from iconlib import Canvas


class C64IconTests(unittest.TestCase):
    def setUp(self) -> None:
        canvas = Canvas(100)
        draw(canvas)
        self.pixels = canvas.rgba()

    def pixel(self, x: int, y: int) -> tuple[int, ...]:
        offset = (y * 100 + x) * 4
        return tuple(self.pixels[offset:offset + 4])

    def test_rainbow_fills_the_circle_across_each_band(self) -> None:
        bands = (
            (13, (30, 50, 70), (238, 35, 40)),
            (30, (15, 50, 85), (247, 137, 0)),
            (50, (10, 32, 90), (239, 215, 0)),
            (72, (15, 50, 85), (74, 188, 39)),
            (86, (30, 50, 70), (39, 107, 222)),
        )
        for y, positions, colour in bands:
            for x in positions:
                with self.subTest(x=x, y=y):
                    self.assertEqual((*colour, 255), self.pixel(x, y))

    def test_letter_counters_show_rainbow_instead_of_a_white_plate(self) -> None:
        self.assertEqual((239, 215, 0, 255), self.pixel(32, 50))
        self.assertEqual((239, 215, 0, 255), self.pixel(71, 50))
        self.assertNotIn(bytes((255, 255, 255, 255)),
                         [self.pixels[i:i + 4] for i in range(0, len(self.pixels), 4)])

    def test_text_outline_and_transparency_are_preserved(self) -> None:
        for x, y in ((23, 50), (44, 45), (77, 50)):
            with self.subTest(x=x, y=y):
                self.assertEqual((12, 17, 26, 255), self.pixel(x, y))
        self.assertEqual((35, 44, 62, 255), self.pixel(50, 3))
        self.assertEqual(0, self.pixel(0, 0)[3])
        self.assertEqual(0, self.pixel(99, 99)[3])


if __name__ == "__main__":
    unittest.main()
