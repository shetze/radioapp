#!/usr/bin/env python3
"""Create the reproducible 100x30 / 10 fps / 10 s libcaca UTF-8 ANSI demo.

Only Python's standard library is required. Form-feed plus newline separates
libcaca frames. Rerun after changing the ASCII scenery.
"""
from pathlib import Path

WIDTH, HEIGHT, FRAMES = 100, 30, 100
OUT = Path(__file__).resolve().parent.parent / "animations" / "city-traffic.ansi"


def paint(screen, text, x, y, color):
    if 0 <= y < HEIGHT:
        for i, ch in enumerate(text):
            if 0 <= x + i < WIDTH:
                screen[y][x + i] = (ch, color)


def make_frame(frame):
    screen = [[(" ", "36") for _ in range(WIDTH)] for _ in range(HEIGHT)]
    paint(screen, "CITY TRAFFIC  /  radio-client multimedia demo", 2, 0, "97")
    paint(screen, "10 FPS  |  100 x 30  |  loop", 69, 0, "97")

    for offset in (8, 49, 91):
        cx = (offset + frame // 3) % 130 - 14
        paint(screen, "     .--.    ", cx, 3, "97")
        paint(screen, "  .-(    )-.", cx, 4, "97")
        paint(screen, " (___.__)__)", cx, 5, "97")

    # Houses and a skyline create a stable background for moving cars.
    for left, wide, top in ((0, 17, 9), (16, 15, 7), (32, 20, 11),
                            (54, 12, 8), (68, 15, 10), (84, 16, 6)):
        for y in range(top, 15):
            paint(screen, "#" * wide, left, y, "34")
        for y in range(top + 2, 14, 3):
            for x in range(left + 3, min(left + wide - 2, WIDTH), 5):
                paint(screen, "[]", x, y, "93")
    for y in (15, 16):
        paint(screen, "=" * WIDTH, 0, y, "90")
    for y in range(17, 29):
        paint(screen, "." * WIDTH, 0, y, "90")
    for x in range(-12 + (frame * 2) % 12, WIDTH, 12):
        paint(screen, "------", x, 22, "93")
    paint(screen, "=" * WIDTH, 0, 29, "90")

    right_car = [
        "      _______       ",
        "  ___/ _ | _\\__    ",
        " /___|_o_|_o___\\   ",
        " '---O-------O---'  ",
    ]
    left_car = [
        "      ______       ",
        "   __/|_  _\\___   ",
        "  /___o_||_o___\\  ",
        "  '--O------O--'   ",
    ]
    for x0, y0, speed, color, art in (
        (5, 18, 2, "91", right_car),
        (52, 18, 2, "92", right_car),
        (20, 24, -1, "96", left_car),
        (73, 24, -1, "95", left_car),
    ):
        x = ((x0 + frame * speed) % WIDTH) - 9
        for line, content in enumerate(art):
            paint(screen, content, x, y0 + line, color)

    # Merge equal-colour characters into long runs for compact ANSI files.
    output = []
    for row in screen:
        previous = None
        for char, color in row:
            if color != previous:
                output.append("\x1b[" + color + "m")
                previous = color
            output.append(char)
        output.append("\x1b[0m\n")
    return "".join(output).removesuffix("\n")


def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(("\f\n".join(make_frame(frame) for frame in range(FRAMES))).encode("utf-8"))
    print(f"Wrote {OUT} ({FRAMES} frames, {WIDTH}x{HEIGHT}, 10 fps)")


if __name__ == "__main__":
    main()
