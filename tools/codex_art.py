#!/usr/bin/env python3
"""Regenerate firmware/src/codex_art.h from the Codex mark (a 24x24 SVG).

Usage: tools/codex_art.py codex.svg > firmware/src/codex_art.h
Needs rsvg-convert and Pillow."""
import subprocess
import sys
import tempfile

from PIL import Image


def body(svg, w, h, thr=110):
    with tempfile.NamedTemporaryFile(suffix=".png") as f:
        subprocess.run(["rsvg-convert", "-w", str(w * 8), "-h", str(h * 8), "-b", "black", svg, "-o", f.name],
                       check=True)
        im = Image.open(f.name).convert("L").resize((w, h), Image.BOX)
    m = [[1 if im.getpixel((x, y)) > thr else 0 for x in range(w)] for y in range(h)]
    # Close the glyph cut-outs: only what the outside reaches stays empty.
    out = [[0] * w for _ in range(h)]
    st = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while st:
        x, y = st.pop()
        if 0 <= x < w and 0 <= y < h and not out[y][x] and not m[y][x]:
            out[y][x] = 1
            st += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return ["".join("." if out[y][x] else "#" for x in range(w)) for y in range(h)]


def main():
    svg = sys.argv[1]
    variants = [("NORMAL", 18, 16), ("SQUASH", 20, 14), ("STRETCH", 16, 18)]
    print("#pragma once")
    print("#include <stdint.h>")
    print("// The Codex page's corner sprite: the Codex cloud as pixel art, generated")
    print("// from its 24x24 mark (filled, glyph cut-outs closed) at three body sizes for")
    print("// squash and stretch. '#' = body. The \">_\" glyph, shading and every")
    print("// animation are drawn at run time (codex_sprite.cpp).")
    print("// Regenerate: tools/codex_art.py.")
    print()
    print("struct CodexBody { uint8_t w, h; const char* rows; };")
    print()
    for name, w, h in variants:
        print(f"static const char CODEX_BODY_{name}_ROWS[] =")
        rows = body(svg, w, h)
        print("\n".join(f'    "{r}"' for r in rows) + ";")
    print()
    print("static const CodexBody CODEX_BODIES[3] = {")
    for name, w, h in variants:
        print(f"    {{ {w}, {h}, CODEX_BODY_{name}_ROWS }},")
    print("};")
    print("enum { CODEX_BODY_NORMAL, CODEX_BODY_SQUASH, CODEX_BODY_STRETCH };")


if __name__ == "__main__":
    main()
