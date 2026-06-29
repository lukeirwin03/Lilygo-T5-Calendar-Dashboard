@echo off
REM Convert all TTF fonts in fonts_tff to LilyGo-EPD47 GFXfont headers.
REM Requires: python + freetype-py (pip install freetype-py)

setlocal enabledelayedexpansion

set CONVERTER=.pio\libdeps\T5-ePaper-S3\LilyGo-EPD47\scripts\fontconvert.py
set OUTDIR=include\fonts

echo Converting fonts...

python %CONVERTER% Genty14 14 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty14pt7b.h
python %CONVERTER% Genty16 16 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty16pt7b.h
python %CONVERTER% Genty20 20 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty20pt7b.h
python %CONVERTER% Genty24 24 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty24pt7b.h
python %CONVERTER% Genty32 32 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty32pt7b.h
python %CONVERTER% Genty48 48 fonts_tff\GentyDemo-Regular.ttf > %OUTDIR%\Genty48pt7b.h

python %CONVERTER% MeltSwashes14 14 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes14pt7b.h
python %CONVERTER% MeltSwashes16 16 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes16pt7b.h
python %CONVERTER% MeltSwashes18 18 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes18pt7b.h
python %CONVERTER% MeltSwashes20 20 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes20pt7b.h
python %CONVERTER% MeltSwashes24 24 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes24pt7b.h
python %CONVERTER% MeltSwashes32 32 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes32pt7b.h
python %CONVERTER% MeltSwashes48 48 fonts_tff\Melt-Swashes.ttf > %OUTDIR%\Melt-Swashes48pt7b.h


echo Done.
pause
