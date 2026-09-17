#pragma once

// Refresh hygiene policy for the flash-free (differential / ghost) panel
// updates.
//
// Flash-free updates never fully reset the panel, so charge and faint
// ghosting slowly accumulate. Each flash-free update "earns" a credit;
// when the credit cap is hit — or one of the time backstops fire
// (long awake session, or a long absolute span persisted across deep
// sleep via the RTC) — the NEXT user-initiated transition upgrades to a
// full cleared flash instead of another diff, silently paying down the
// debt. The upgrade never happens spontaneously mid-interaction (static
// e-paper frames are stable; only a transition the user already asked
// for gets swapped for the flashing variant), and a full flash resets
// everything.
namespace refresh_hygiene {

void begin();                 // call once at setup: records session start, seeds the RTC epoch if unset
void creditDiff(int n = 1);   // a flash-free differential update drove the panel
void creditGhost(int n = 1);  // an add-ink ghost push drove the panel
bool wantsFlash();            // true when the next transition should upgrade to a full flash
void markFullFlash();         // a full-screen cleared flash happened — everything resets
int  credits();               // current credit count (logging/tests)

} // namespace refresh_hygiene
