#pragma once

// Differential-refresh calibration screen (demo build).
//
// Runs before the connection-screen animation and loops test cycles
// until the physical button is pressed. Each row of the screen tests one
// aspect of the flash-free refresh so hardware misbehavior can be
// isolated to a specific primitive:
//
//   ERASE x1   — single-pass masked erase each cycle (reference winner).
//   DIFF SWAP  — the shipping full-swap (1000 ms gap, ink ×8): the
//                quality reference for the speed rows.
//   GAP 0/100/250 — full swaps with shrinking settle gaps; each phase is
//                timed to serial. Judge cleanliness vs DIFF SWAP.
//   RAPID x2   — gap-0, 2-pass-ink swaps back-to-back (6 in a row,
//                total time reported): the floor for a legible swap and
//                a charge-buildup stress test.
//
// The serial log narrates and times every step.
//
// The serial log narrates every step; watch it alongside the panel.

namespace diff_test {
  // Blocks, running test cycles, until the button is pressed.
  void run();
}
