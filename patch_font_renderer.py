"""
PlatformIO extra script: patches LilyGo-EPD47 font.c to skip transparent
pixels (bm == 0) in draw_char(), preventing overlapping glyph bounding boxes
from erasing adjacent characters in decorative/script fonts like MeltSwashes.

Without this patch, every pixel in a glyph's bounding box is written to the
framebuffer, including "empty" pixels which get set to bg_color. For fonts
where glyphs extend beyond their advance width, this overwrites visible
pixels of the previous character.
"""
import os
import re

# The exact substitution we need to make
SEARCH = """            if ((xx & 1) == 0)
            {
                buffer[buf_pos] = (old & 0xF0) | color_lut[bm];
            }
            else
            {
                buffer[buf_pos] = (old & 0x0F) | (color_lut[bm] << 4);
            }
            byte_complete = !byte_complete;
            x++;"""

REPLACE = """            if (bm == 0)
            {
                byte_complete = !byte_complete;
                x++;
                continue;
            }

            if ((xx & 1) == 0)
            {
                buffer[buf_pos] = (old & 0xF0) | color_lut[bm];
            }
            else
            {
                buffer[buf_pos] = (old & 0x0F) | (color_lut[bm] << 4);
            }
            byte_complete = !byte_complete;
            x++;"""


def patch_font_c(font_c_path):
    if not os.path.exists(font_c_path):
        print(f"[patch] font.c not found at {font_c_path}, skipping")
        return False

    with open(font_c_path, "r") as f:
        content = f.read()

    if "if (bm == 0)" in content:
        print(f"[patch] font.c already patched, skipping")
        return True

    if SEARCH not in content:
        print(f"[patch] WARNING: expected code block not found in font.c, skipping")
        return False

    content = content.replace(SEARCH, REPLACE)
    with open(font_c_path, "w") as f:
        f.write(content)

    print(f"[patch] Patched font.c: skip transparent pixels (bm==0) in draw_char()")
    return True


def find_and_patch(env):
    project_dir = os.getcwd()
    libdeps = os.path.join(project_dir, ".pio", "libdeps", env)
    for root, dirs, files in os.walk(libdeps):
        for f in files:
            if f == "font.c" and "LilyGo-EPD47" in root:
                patch_font_c(os.path.join(root, f))


Import("env")

# Patch after libraries are installed but before compilation
env.AddPreAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    lambda *args, **kwargs: find_and_patch(env["PIOENV"]),
)
