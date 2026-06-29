#!/usr/bin/env bash
# Convert all TTF fonts in fonts_tff to LilyGo-EPD47 GFXfont headers.
# Requires: python + freetype-py (pip install freetype-py)
#
# Run from the project root after `pio run` has downloaded dependencies.
set -euo pipefail

OUTDIR="include/fonts"

# Auto-detect fontconvert.py in any PlatformIO libdeps env directory
CONVERTER=$(find .pio/libdeps -path "*/LilyGo-EPD47/scripts/fontconvert.py" -print -quit 2>/dev/null)

if [ -z "$CONVERTER" ]; then
    echo "Error: fontconvert.py not found."
    echo "Run 'pio run' first to download LilyGo-EPD47 dependencies."
    exit 1
fi

echo "Using converter: $CONVERTER"
echo "Converting fonts..."

python "$CONVERTER" Genty16  16 fonts_tff/GentyDemo-Regular.ttf  > "$OUTDIR/Genty16pt7b.h"
python "$CONVERTER" Genty20  20 fonts_tff/GentyDemo-Regular.ttf  > "$OUTDIR/Genty20pt7b.h"
python "$CONVERTER" Genty24  24 fonts_tff/GentyDemo-Regular.ttf  > "$OUTDIR/Genty24pt7b.h"
python "$CONVERTER" Genty32  32 fonts_tff/GentyDemo-Regular.ttf  > "$OUTDIR/Genty32pt7b.h"
python "$CONVERTER" Genty48  48 fonts_tff/GentyDemo-Regular.ttf  > "$OUTDIR/Genty48pt7b.h"

python "$CONVERTER" MeltSwashes14 14 fonts_tff/Melt-Swashes.ttf  > "$OUTDIR/Melt-Swashes14pt7b.h"
python "$CONVERTER" MeltSwashes16 16 fonts_tff/Melt-Swashes.ttf  > "$OUTDIR/Melt-Swashes16pt7b.h"

echo "Done."
