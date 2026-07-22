#!/usr/bin/env bash
# Convert all TTF fonts in fonts_tff/ to LilyGo-EPD47 GFXfont headers in
# include/fonts/. Idempotent — safe to re-run.
#
# Requirements:
#   - python3 + freetype-py (pip install freetype-py)
#   - Run `pio run` once first to download LilyGo-EPD47 (provides fontconvert.py)
#
# Usage: ./convert_fonts.sh
set -euo pipefail

# Always run from the script's directory (project root) regardless of CWD.
cd "$(dirname "$0")"

OUTDIR="include/fonts"
FONTS_DIR="fonts_tff"

# Pin the converter to a known env. Try dashboard first, fall back to demo.
CONVERTER=""
for env in dashboard demo; do
    candidate=".pio/libdeps/${env}/LilyGo-EPD47/scripts/fontconvert.py"
    if [ -f "$candidate" ]; then
        CONVERTER="$candidate"
        break
    fi
done

if [ -z "$CONVERTER" ]; then
    echo "Error: fontconvert.py not found under .pio/libdeps/{dashboard,demo}/." >&2
    echo "Run 'pio run' first to download LilyGo-EPD47 dependencies." >&2
    exit 1
fi

# Verify freetype-py is available before doing anything else.
if ! python3 -c "import freetype" 2>/dev/null; then
    echo "Error: Python 'freetype' module not available." >&2
    echo "Install with: pip install freetype-py" >&2
    exit 1
fi

echo "Using converter: $CONVERTER"
echo "Output dir:      $OUTDIR"
echo

# Wrap an auto-generated font header with a localized GCC diagnostic pragma so
# -Wnarrowing/-Woverflow warnings inside the bitmap data do not pollute builds.
# Idempotent: headers already containing the push pragma are left untouched.
wrap_with_pragma() {
    local file="$1"
    python3 - "$file" <<'PY'
import sys
from pathlib import Path

p = Path(sys.argv[1])
text = p.read_text()

if "#pragma GCC diagnostic push" in text:
    sys.exit(0)

text = text.replace(
    '#include "epd_driver.h"',
    '#include "epd_driver.h"\n'
    '\n#pragma GCC diagnostic push\n'
    '#pragma GCC diagnostic ignored "-Wnarrowing"\n'
    '#pragma GCC diagnostic ignored "-Woverflow"'
)

if not text.endswith("\n"):
    text += "\n"
text += "\n#pragma GCC diagnostic pop\n"

p.write_text(text)
PY
}

# convert <C identifier name> <size> <primary ttf> [fallback ttf]
# Writes to $OUTDIR/<name>pt7b.h atomically (tmp file + rename) so a failed
# conversion can never leave a half-written header behind. If a fallback TTF
# is supplied it is appended to the fontconvert.py font stack so missing
# glyphs in the primary are pulled from the fallback.
convert() {
    local name="$1"
    local size="$2"
    local ttf="$3"
    local fallback="${4:-}"

    if [ ! -f "$ttf" ]; then
        echo "  FAIL: '$name' — source TTF not found: $ttf" >&2
        return 1
    fi
    if [ -n "$fallback" ] && [ ! -f "$fallback" ]; then
        echo "  FAIL: '$name' — fallback TTF not found: $fallback" >&2
        return 1
    fi

    local out="${OUTDIR}/${name}pt7b.h"
    local tmp="${out}.tmp"

    if [ -n "$fallback" ]; then
        # fontconvert.py takes the stack as nargs='+' positionals after size.
        if python3 "$CONVERTER" "$name" "$size" "$ttf" "$fallback" > "$tmp"; then
            mv "$tmp" "$out"
            wrap_with_pragma "$out"
            echo "  OK:   $name (${size}pt, fallback $(basename "$fallback"))"
        else
            rm -f "$tmp"
            echo "  FAIL: $name (${size}pt) — converter error" >&2
            return 1
        fi
    else
        if python3 "$CONVERTER" "$name" "$size" "$ttf" > "$tmp"; then
            mv "$tmp" "$out"
            wrap_with_pragma "$out"
            echo "  OK:   $name (${size}pt)"
        else
            rm -f "$tmp"
            echo "  FAIL: $name (${size}pt) — converter error" >&2
            return 1
        fi
    fi
}

echo "Converting fonts..."

# Headings — Genty
convert Genty20 20 "${FONTS_DIR}/GentyDemo-Regular.ttf"
convert Genty24 24 "${FONTS_DIR}/GentyDemo-Regular.ttf"
convert Genty32 32 "${FONTS_DIR}/GentyDemo-Regular.ttf"
convert Genty48 48 "${FONTS_DIR}/GentyDemo-Regular.ttf"

# Body text — MeltSwashes
convert MeltSwashes14 14 "${FONTS_DIR}/Melt-Swashes.ttf"
convert MeltSwashes16 16 "${FONTS_DIR}/Melt-Swashes.ttf"
convert MeltSwashes18 18 "${FONTS_DIR}/Melt-Swashes.ttf"
convert MeltSwashes20 20 "${FONTS_DIR}/Melt-Swashes.ttf"

convert Computer14 14 "${FONTS_DIR}/Computerfont.ttf"
convert Computer16 16 "${FONTS_DIR}/Computerfont.ttf"
convert Computer20 20 "${FONTS_DIR}/Computerfont.ttf"

echo
echo "Done. New/changed headers should be 'git add'ed."
