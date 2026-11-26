/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The practice's own menu and tools, as Tekken 6 Online's practice (OFFENSE / DEFENSE, recordings with a frequency, an
   action after a hit or block, position slots, quick select), in the launcher's practice alone (sc2_practice_running).
   The frame data, the hit boxes and the meter stay training's (sc2_training.cpp). */
#ifndef SC2_PRACTICE_H
#define SC2_PRACTICE_H

#include <cstdint>

extern "C" {
#include "input_model.h"
#include "sc2_panel.h"
}

namespace sc2 {

/* Each read of controller 1 or 2 in the practice: the menu and the shortcuts taken out of the player's pad, the dummy's
   pad while it records or plays. Returns whether the game has the controller. */
bool PracticePad(uint32_t port, RecompInputGamepad *pad, bool sampled);
/* The player's pad of the last read as the fight took it (a recording's too, which drives the dummy): the input display */
const RecompInputGamepad &PracticeOwnPad();
/* Once a frame of the fight, at its start. */
void PracticeFrame();
/* At the start of a frame, before the fight's own: a position saved or put back as asked. True if one was put back. */
bool PracticeSlots();
/* (training's start over done: the dummy's option and the settings back) */
void PracticeLoaded();
void PracticePanel(Sc2Panel &panel);
/* The game's own command list (and the menu behind it) is up: nothing of ours over it */
bool PracticeGameMenu();
/* (the practice left, or begun again by the quick select: the recordings kept when keep) */
void PracticeForget(bool keep);

} // namespace sc2

/* (runner.cpp) */
extern "C" int sc2_practice_running(void);
extern "C" int sc2_practice_asking(void); /* (the match question is up: its buttons are its own) */
extern "C" int sc2_practice_pick(int player, int *outfit);
extern "C" int sc2_practice_stage(void);
extern "C" int sc2_practice_again(int char1, int outfit1, int char2, int outfit2, int stage);
extern "C" void sc2_practice_close(void);

#endif
