#pragma once

// Stylized cold-boot connection screen with flash-free progress.
//
// Shown only on cold boot when there is no cached view to render first
// (no SD payload or the RTC clock isn't valid yet). begin() draws the
// full layout (wordmark + progress bar) with one full refresh; after
// that, stageStart()/stageDone()/tick() redraw just the status band via
// a DIFFERENTIAL refresh — unchanged pixels get no drive, old text is
// erased with a WHITE_ON_WHITE "white ink" frame, new text is drawn
// with a no-clear push. No flashing, no stacking ghosts.
//
// Stage indices match networking::ProgressStage (WiFi=0, Time=1,
// MQTT=2, Data=3); main_dashboard maps between the two.

namespace conn_screen {

// Draw the full connection screen and do a full panel refresh.
// Resets all progress state. Call once before the connect sequence.
void begin();

// A new stage started: shows the stage label, bar stays at the level
// earned by completed stages, and animates "..." while ticking.
void stageStart(int stage);

// A stage finished: the bar jumps to the stage's full fraction. On
// failure the label switches to the stage's failure text.
void stageDone(int stage, bool ok);

// "Still working" heartbeat: creeps the bar partway toward the current
// stage's cap and cycles the label dots. Internally throttled to one
// panel update per ~1.5 s so frequent calls are cheap.
void tick();

} // namespace conn_screen
