#include "conn_screen.h"
#include "display_manager.h"
#include "epd_driver.h"
#include "fonts/Genty32pt7b.h"
#include "fonts/MeltSwashes16pt7b.h"
#include <Arduino.h>
#include <cstdio>
#include <string.h>

// Layout (960x540 panel):
//
//   ┌──────────────────────────────────────┐
//   │              Calendar                │  Genty32 wordmark, ~y 210
//   │         ──                ──         │  short LTGRAY flanking rules
//   │         Connecting to WiFi...        │  MeltSwashes16 status, y 300
//   │           ████████████░░░░           │  400x16 progress bar, y 326
//   └──────────────────────────────────────┘
//
// Only the status band (label + bar) changes during the connect
// sequence; it is redrawn into the framebuffer and pushed with a
// full-width partial refresh of just those rows.

namespace conn_screen {

// ---------------------------------------------------------------------------
// Palette (4-bit grayscale, two pixels per byte)
// ---------------------------------------------------------------------------
static constexpr uint8_t C_BLACK  = 0;
static constexpr uint8_t C_LTGRAY = 12;
static constexpr uint8_t C_WHITE  = 15;

static constexpr uint8_t EPD_BLACK  = C_BLACK  << 4;
static constexpr uint8_t EPD_LTGRAY = C_LTGRAY << 4;
static constexpr uint8_t EPD_WHITE  = C_WHITE  << 4;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static constexpr int BAR_W = 400;
static constexpr int BAR_H = 16;
static constexpr int BAR_Y = 326;
static constexpr int BAR_X = (960 - BAR_W) / 2;   // panel is 960 wide

// Status band: everything that changes live (label + bar). Partial
// refreshes cover exactly these rows.
static constexpr int BAND_Y = 272;
static constexpr int BAND_H = 96;   // 272..367: label ~300, bar 326..342

static constexpr int LABEL_BASELINE = 300;
static constexpr int WORDMARK_BASELINE = 210;

// ---------------------------------------------------------------------------
// Stages — indices match networking::ProgressStage
// ---------------------------------------------------------------------------
static constexpr int STAGE_COUNT = 4;
static const char* const kLabels[STAGE_COUNT] = {
  "Connecting to WiFi",
  "Syncing clock",
  "Connecting to broker",
  "Fetching calendar",
};
static const char* const kFailLabels[STAGE_COUNT] = {
  "WiFi unavailable",
  "Clock sync failed",
  "Broker unreachable",
  "No payload received",
};
// Fraction of the bar earned when each stage completes. During a stage
// the bar creeps toward (cap - 5%) at most, so completion is always
// visibly honest.
static const float kCaps[STAGE_COUNT] = { 0.35f, 0.55f, 0.75f, 1.00f };

static constexpr unsigned long TICK_MS = 600;    // min panel update interval while waiting
                                                // (full swap ≈ erase pass only — fast)
static constexpr float CREEP = 0.02f;            // bar growth per tick
static constexpr float CREEP_MARGIN = 0.05f;     // never reach the cap by creeping

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static int   s_stage = -1;          // current stage index, -1 = none yet
static bool  s_failed = false;      // current stage failed (label swap)
static float s_completed = 0.0f;    // bar fraction earned by completed stages
static float s_showing = 0.0f;      // fraction currently drawn
static int   s_dots = 1;            // animated 1..3 dots after the label

// Band pixels as last pushed to the panel — the "previous frame" for the
// minimal-drive differential refresh (only changed pixels are driven).
static uint8_t s_prevBand[(960 / 2) * 96];

// Bounds of the currently-drawn stage label (text only, no dots), used
// to exclude an unchanged label from each refresh.
static int s_labelX = 0, s_labelY = 0, s_labelW = 0, s_labelH = 0;

// Last state actually pushed to the panel, used to skip no-op refreshes.
static char  s_drawnLabel[48] = "";
static int   s_drawnDots = -1;
static int   s_drawnFillW = -1;

static unsigned long s_lastPanelMs = 0;

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawBar(uint8_t* fb, float frac) {
  // Track: white fill, black border.
  epd_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, EPD_WHITE, fb);
  epd_draw_rect(BAR_X, BAR_Y, BAR_W, BAR_H, EPD_BLACK, fb);
  // Fill: inset 2 px, width from frac.
  if (frac > 0.001f) {
    int inner = BAR_W - 4;
    int w = (int)(frac * inner + 0.5f);
    if (w < 1) w = 1;
    if (w > inner) w = inner;
    epd_fill_rect(BAR_X + 2, BAR_Y + 2, w, BAR_H - 4, EPD_BLACK, fb);
  }
}

// Current stage label WITHOUT the animated dots. The label must render
// at a FIXED position while its stage runs: centering "label..." with a
// varying dot count would shift every glyph a few px on each tick,
// forcing a whole-label erase+redraw — which reads as garbled text while
// the white-ink erase is mid-flight. The dots render separately at a
// fixed offset past the label's right edge, so a tick only ever changes
// the dots themselves.
static const char* stageLabel() {
  if (s_stage < 0) return "Starting up";
  if (s_failed) return kFailLabels[s_stage];
  return kLabels[s_stage];
}

static void drawBand() {
  uint8_t* fb = display_mgr::framebuffer();

  const char* label = stageLabel();
  int dots = (s_stage >= 0 && !s_failed) ? s_dots : 0;
  bool labelChanged = (strcmp(label, s_drawnLabel) != 0);

  // Skip the panel update when nothing visible changed (e.g. a stage
  // completing instantly, or a tick arriving inside the throttle
  // window).
  int fillW = (int)(s_showing * (BAR_W - 4) + 0.5f);
  if (fillW > BAR_W - 4) fillW = BAR_W - 4;
  if (!labelChanged && dots == s_drawnDots && fillW == s_drawnFillW) {
    return;
  }

  epd_fill_rect(0, BAND_Y, EPD_WIDTH, BAND_H, EPD_WHITE, fb);

  {
    int32_t tw = 0, th = 0, x1 = 0, y1 = 0;
    int32_t cx = EPD_WIDTH / 2, cy = LABEL_BASELINE;
    get_text_bounds((GFXfont*)&MeltSwashes16, label, &cx, &cy, &x1, &y1, &tw, &th, NULL);
    int32_t lx = EPD_WIDTH / 2 - tw / 2;
    int32_t ly = LABEL_BASELINE;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, label, &lx, &ly, fb, BLACK_ON_WHITE, &props);
    // Record the label's bounds (label text only — the dots render
    // outside this rect) so unchanged labels can be excluded from the
    // refresh entirely.
    s_labelX = lx; s_labelW = tw;
    s_labelY = y1; s_labelH = th;

    if (dots > 0) {
      char dotsStr[4];
      snprintf(dotsStr, sizeof(dotsStr), "%.*s", dots, "...");
      int32_t dx = lx + tw + 10;   // fixed spot past the label's right edge
      int32_t dy = LABEL_BASELINE;
      write_mode((GFXfont*)&MeltSwashes16, dotsStr, &dx, &dy, fb, BLACK_ON_WHITE, &props);
    }
  }

  drawBar(fb, s_showing);

  strlcpy(s_drawnLabel, label, sizeof(s_drawnLabel));
  s_drawnDots = dots;
  s_drawnFillW = fillW;

  // Persistent rects — content that must NOT be re-driven this update:
  //   * the progress bar, always (append-only; new fill just drawn in)
  //   * the label, when its text didn't change (ticks only move the dots
  //     and the bar — re-driving identical glyphs is wasted work and
  //     unnecessary panel charge)
  // A label CHANGE deliberately stays outside the rects so it gets the
  // validated full swap.
  display_mgr::PersistRect persist[2];
  int persistCount = 0;
  persist[persistCount++] = { BAR_X, BAR_Y, BAR_W, BAR_H };
  if (!labelChanged && s_labelW > 0) {
    persist[persistCount++] = { s_labelX, s_labelY, s_labelW, s_labelH };
  }
  display_mgr::diffRefresh(BAND_Y, BAND_H, s_prevBand, persist, persistCount);
  s_lastPanelMs = millis();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void begin() {
  s_stage = -1;
  s_failed = false;
  s_completed = 0.0f;
  s_showing = 0.0f;
  s_dots = 1;
  s_drawnLabel[0] = '\0';
  s_drawnDots = -1;
  s_drawnFillW = -1;
  s_labelX = s_labelY = s_labelW = s_labelH = 0;
  s_lastPanelMs = millis();

  uint8_t* fb = display_mgr::framebuffer();
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  // Wordmark (Genty32) centered.
  const char* word = "Calendar";
  int32_t tw = 0, th = 0, x1 = 0, y1 = 0;
  int32_t cx = EPD_WIDTH / 2, cy = WORDMARK_BASELINE;
  get_text_bounds((GFXfont*)&Genty32, word, &cx, &cy, &x1, &y1, &tw, &th, NULL);
  {
    int32_t lx = EPD_WIDTH / 2 - tw / 2;
    int32_t ly = WORDMARK_BASELINE;
    writeln((GFXfont*)&Genty32, word, &lx, &ly, fb);
  }

  // Short LTGRAY flanking rules beside the wordmark.
  {
    int textL = EPD_WIDTH / 2 - tw / 2;
    int textR = EPD_WIDTH / 2 + tw / 2;
    epd_draw_hline(textL - 84, WORDMARK_BASELINE - 12, 56, EPD_LTGRAY, fb);
    epd_draw_hline(textR + 28, WORDMARK_BASELINE - 12, 56, EPD_LTGRAY, fb);
  }

  // Initial status band: white background + empty bar (binary-safe
  // elements only — solid lines, no font antialiasing).
  epd_fill_rect(0, BAND_Y, EPD_WIDTH, BAND_H, EPD_WHITE, fb);
  drawBar(fb, 0.0f);
  display_mgr::powerOn();
  display_mgr::fullRefresh();
  display_mgr::powerOff();

  // Ink the initial label with the 1-bit pass (solid black, NO
  // antialiased fringes). A fullRefresh would render the label's gray
  // edge pixels (nibbles above INK_MAX), which the differential masks
  // classify as "paper" and would never erase — leaving a permanent
  // faint halo of the first label. Inking keeps the panel binary and
  // consistent with the diff model from the very first update.
  {
    const char* label = stageLabel();   // "Starting up" (s_stage == -1)
    int32_t lw = 0, lh = 0, lx1 = 0, ly1 = 0;
    int32_t mcx = EPD_WIDTH / 2, mcy = LABEL_BASELINE;
    get_text_bounds((GFXfont*)&MeltSwashes16, label, &mcx, &mcy, &lx1, &ly1, &lw, &lh, NULL);
    int32_t lx = EPD_WIDTH / 2 - lw / 2;
    int32_t ly = LABEL_BASELINE;
    FontProperties props;
    props.fg_color = C_BLACK;
    props.bg_color = C_WHITE;
    props.flags = 0;
    props.fallback_glyph = 0;
    write_mode((GFXfont*)&MeltSwashes16, label, &lx, &ly, fb, BLACK_ON_WHITE, &props);
    s_labelX = lx; s_labelW = lw;
    s_labelY = ly1; s_labelH = lh;
    strlcpy(s_drawnLabel, label, sizeof(s_drawnLabel));
  }
  display_mgr::inkRefresh(BAND_Y, BAND_H, 8);
  s_drawnDots = 0;
  s_drawnFillW = 0;
  // The band is on the panel exactly as in the framebuffer — seed the
  // differential refresh's "previous frame" from it.
  memcpy(s_prevBand, fb + (BAND_Y * EPD_WIDTH / 2), (EPD_WIDTH / 2) * BAND_H);
  Serial.println("[conn] Connection screen drawn");
}

void stageStart(int stage) {
  if (stage < 0 || stage >= STAGE_COUNT) return;
  s_stage = stage;
  s_failed = false;
  s_showing = s_completed;
  drawBand();
}

void stageDone(int stage, bool ok) {
  if (stage < 0 || stage >= STAGE_COUNT) return;
  s_stage = stage;
  s_failed = !ok;
  s_completed = kCaps[stage];
  s_showing = s_completed;
  drawBand();
}

void tick() {
  if (s_stage < 0 || s_failed) return;
  if (millis() - s_lastPanelMs < TICK_MS) return;
  float cap = kCaps[s_stage] - CREEP_MARGIN;
  if (s_showing < cap) {
    s_showing += CREEP;
    if (s_showing > cap) s_showing = cap;
  }
  s_dots = (s_dots % 3) + 1;
  drawBand();
}

} // namespace conn_screen
