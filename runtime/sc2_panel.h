/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Soulcalibur II's panels over the game, beside the runtime's overlay text (overlay_text.h): training's input display
   and combo readout, one of each on each player's side, its hit boxes and frame meter, and a note for a few
   seconds. Filled once a frame on the
   game's side (sc2_training.cpp) and drawn by the overlay (overlay_imgui_panel.cpp), possibly on another thread. */
#ifndef SC2_PANEL_H
#define SC2_PANEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SC2_PANEL_INPUT_ROWS = 12,
    SC2_PANEL_READOUT_ROWS = 7,
    SC2_PANEL_TEXT = 48,
    SC2_PANEL_SHAPES = 64,
    SC2_PANEL_METER = 80,
    /* The practice's menu (sc2_practice.cpp) */
    SC2_PANEL_MENU = 24,
    SC2_PANEL_HELP = 4,
    SC2_PANEL_MENU_TEXT = 96,
};

/* A frame meter's cell: what a fighter did that frame */
enum { SC2_METER_NONE, SC2_METER_FREE, SC2_METER_STARTUP, SC2_METER_ACTIVE, SC2_METER_RECOVERY, SC2_METER_STUN };

/* A hit box as the picture shows it, in the game's 640x480 frame: a circle (a sphere: where a fighter can be hit,
   or its pushing sphere) or a line (a capsule: a weapon or a limb, which hits) */
enum { SC2_SHAPE_HURT, SC2_SHAPE_PUSH, SC2_SHAPE_ATTACK, SC2_SHAPE_ATTACK_LIVE };
typedef struct Sc2PanelShape {
    float x0, y0, x1, y1, radius;
    unsigned kind;
} Sc2PanelShape;

/* A row of the practice's input display, as Tekken 6 Online's: the frames it was held, the direction (1 up, 2 down, 4 left,
   8 right, as forward is right) and the buttons (1 A, 2 B, 4 K, 8 G) */
typedef struct Sc2PanelInput {
    unsigned frames;
    unsigned char dirs, buttons;
} Sc2PanelInput;

/* A side's frame data box (its last attack, and the advantage of the last exchange from its view) */
typedef struct Sc2PanelFrameData {
    bool shown, attack, exchange, guard, known;
    unsigned startup, active, recovery;
    int damage, advantage;
} Sc2PanelFrameData;

typedef struct Sc2Panel {
    /* The practice: its menu (menu_rows 0: closed), what it does now, the input display and the frame data boxes */
    unsigned menu_rows, help_rows;
    int menu_cursor;
    char menu[SC2_PANEL_MENU][SC2_PANEL_MENU_TEXT];
    char help[SC2_PANEL_HELP][SC2_PANEL_MENU_TEXT];
    char status[64];
    unsigned own_rows;
    Sc2PanelInput own[SC2_PANEL_INPUT_ROWS];
    Sc2PanelFrameData data[2];
    /* Each player's (player 1 left, player 2 right): the input display, newest first, and the readout */
    unsigned input_rows[2];
    char input[2][SC2_PANEL_INPUT_ROWS][SC2_PANEL_TEXT];
    unsigned readout_rows[2];
    char readout[2][SC2_PANEL_READOUT_ROWS][SC2_PANEL_TEXT];
    char note[96];
    /* The FPS counter (top right), "" when off */
    char fps[16];
    unsigned shape_count;
    Sc2PanelShape shapes[SC2_PANEL_SHAPES];
    /* The frame meter: a row for each fighter, oldest cell first */
    unsigned meter_length;
    unsigned char meter[2][SC2_PANEL_METER];
} Sc2Panel;

void sc2_panel_publish(const Sc2Panel *panel);
void sc2_panel_take(Sc2Panel *panel);
bool sc2_panel_empty(const Sc2Panel *panel);

#ifdef __cplusplus
}
#endif

#endif
