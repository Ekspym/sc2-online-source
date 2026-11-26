/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The practice, as Tekken 6 Online's (its t6_practice.cpp: the same menu, controls and behaviour):
     START                opens / closes the menu (the whole pad goes to it while it is open); stops a recording
     in the menu          up / down a row (held: repeats), left / right its value, A does it, B closes
     BACK held, menu closed  + A the round's start, + left trigger save the position, + right trigger go to it,
                          + X playback, + Y record, + B block after the first hit; BACK tapped stops a recording
   What is held when the menu or a shortcut lets go of the pad stays away from the game until it is let go. START and
   BACK never reach the game: its own practice menu is opened only for its command list (the menu's "Move list").

   The dummy is the game's own (Practice's Free Style): each of its actions is a code in player 2's record (+0x50) set by
   0xB9F50 (thiscall of the practice's object 0x3E0610), the menu's index of it at 0x3E0640 (the setters 0xBA360...).
   While it records or plays a recording it is controller 2's (code 1). COUNTER: the word at +0x1838 of player 2's record
   (1 none, 5 attack; the option at 0x3E0630); INFORMATION TYPE at 0x3E0638. */
#include "sc2_practice.h"
#include "sc2_training.h"
#include "sc2_fighter.h"
#include "sc2_state.h"

extern "C" {
#include "kernel_abi.h"
#include "runtime.h"
#include "sc2_hooks.h"
}

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sc2 {

namespace {

enum : uint32_t {
    kPlayer2 = SC2_FIGHTER_RECORD + SC2_FIGHTER_STRIDE,
    kObject = 0x003e0610u,
    kOption = 0x003e0640u,
    kCounterOption = 0x003e0630u,
    kInformation = 0x003e0638u,
    kCounter = SC2_FIGHTER_FIELD(0x1838u),
    kSetDummy = 0x000b9f50u,
    kHitStun = SC2_FIGHTER_FIELD(0x1594u),
    kReaction = 0xf8u,
    kControllerCode = 1u,
};

struct Action { const char *name; uint32_t option, code; };
/* (the game's Free Style settings, in its menu's order; its CONTROLLER is the recordings') */
constexpr Action kActions[] = {
    {"STAND", 0x0, 0x05}, {"STAND GUARD", 0x1, 0x07}, {"STAND GUARD ALL", 0x2, 0x09}, {"STAND GUARD 2ND ATTACK", 0x3, 0x0b},
    {"CROUCH", 0x4, 0x06}, {"CROUCH GUARD", 0x5, 0x08}, {"CROUCH GUARD ALL", 0x6, 0x0a}, {"CROUCH GUARD 2ND ATTACK", 0x7, 0x0c},
    {"DOWN FACE UP", 0x8, 0x0d}, {"DOWN FACE DOWN", 0x9, 0x0e},
    {"JUMP UP", 0xa, 0x1a}, {"JUMP FORWARD", 0xb, 0x1b}, {"JUMP BACKWARD", 0xc, 0x1c},
    {"RUN RIGHT", 0xd, 0x15}, {"RUN LEFT", 0xe, 0x16}, {"STEP RIGHT", 0xf, 0x17}, {"STEP LEFT", 0x10, 0x18},
    {"RUN RANDOMLY", 0x11, 0x19},
    {"A-ATTACK", 0x12, 0x1f}, {"B-ATTACK", 0x13, 0x20}, {"KICK", 0x14, 0x21}, {"THROW", 0x16, 0x22},
    {"GUARD IMPACT", 0x19, 0x25}, {"GUARD IMPACT ATTACK", 0x1a, 0x26},
};
constexpr int kActionCount = int(sizeof kActions / sizeof kActions[0]);

struct Named { int id; const char *name; };
constexpr Named kChars[] = {
    {32, "ASSASSIN"}, {18, "ASTAROTH"}, {33, "BERSERKER"}, {23, "CASSANDRA"}, {20, "CERVANTES"}, {24, "CHARADE"}, {11, "IVY"},
    {12, "KILIK"}, {31, "LIZARDMAN"}, {4, "MAXI"}, {1, "MITSURUGI"}, {25, "NECRID"}, {17, "NIGHTMARE"}, {21, "RAPHAEL"},
    {2, "SEUNG MINA"}, {6, "SOPHITIA"}, {30, "SPAWN"}, {3, "TAKI"}, {22, "TALIM"}, {5, "VOLDO"}, {13, "XIANGHUA"},
    {15, "YOSHIMITSU"}, {26, "YUNSUNG"}};
constexpr int kCharCount = int(sizeof kChars / sizeof kChars[0]);
constexpr Named kStages[] = {
    {1, "Unwavering Resolve"}, {2, "Guided by Wind"}, {3, "Evil Reborn"}, {4, "Raise Thy Sword"},
    {5, "Brave Sword, Braver Soul"}, {6, "Chasing Death"}, {7, "Destiny Awaits No One"}, {8, "No Turning Back"},
    {9, "Eternal Struggle"}, {10, "Hubris"}, {12, "Sword of the Patriot"}, {13, "Confrontation"}, {14, "Ordinary Pain"}};
constexpr int kStageCount = int(sizeof kStages / sizeof kStages[0]);

constexpr int kRecSlots = 8, kWeightMax = 10, kPositions = 4; /* (position 0: the practice's start) */
constexpr uint32_t kBegun = 0x0039d2acu; /* (Practice's fighting has begun: not 0) */
constexpr size_t kMaxFrames = 30u * 60u;
constexpr const char *kIntervalNames[3] = {"SHORT", "MEDIUM", "LONG"};
constexpr int kIntervalFrames[3] = {20, 60, 120};

enum class Mode { Idle, Record, Play, React };
enum Row {
    RowTrain, RowDummy, RowReact, RowBlock, RowCounter, RowSlot, RowSave, RowLoad, RowInputs, RowFrame1, RowFrame2, RowMeter,
    RowBoxes, RowList, RowHud, RowSelf, RowOpponent, RowStage, RowApply, RowClose, RowQuit, RowInterval, RowAction1,
};
enum : unsigned {
    kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kStart = 0x10, kBack = 0x20,
    kA = 0x100, kB = 0x200, kX = 0x400, kY = 0x800, kLt = 0x1000, kRt = 0x2000,
    kDirections = kUp | kDown | kLeft | kRight,
};
/* (the pad's analog buttons: A, B, X, Y, black, white, the triggers) */
constexpr unsigned kButtons[8] = {kA, kB, kX, kY, 0, 0, kLt, kRt};

Mode mode = Mode::Idle;
std::vector<RecompInputGamepad> recs[kRecSlots], take; /* (as with the dummy on the right: mirrored on the left) */
int weight[kRecSlots] = {10, 10, 10, 10, 10, 10, 10, 10};
int rec_slot, playing, play_gap, interval, react, dummy;
size_t play_at;
bool block_after_hit, defense, counter_on, menu_open, started;
bool reacting;                          /* (React: the dummy is in a hit's or a guard's stun) */
bool dummy_wanted, dummy_controller;    /* (its code set at the next frame: the player's choice, or controller 2) */
int cursor, slot_at, load_slot;
int self_at = -1, opponent_at = -1, stage_at = -1;
bool ask_save, ask_load, ask_start, ask_apply; /* (ask_start: the start kept, once the fighting has begun) */
int information = 2;
SaveState positions[kPositions];
RecompInputGamepad own, source;
unsigned drive;                         /* (the game's menu driven to its command list: the read it is at; 0 none) */
bool game_list;
int running;
uint32_t seed = 0x2545f491u;

void Say(const char *format, ...)
{
    char text[96];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    TrainingNote(text);
}

/* The game's own setting of the dummy (0xB9F50, as its practice menu calls it) */
void SetCode(uint32_t code)
{
    const RecompRegisters saved = recomp_runtime.registers;
    RecompFpuContext fpu;
    recomp_fpu_context_save(&fpu);
    recomp_runtime.registers.ecx = kObject;
    const uint32_t arguments[1] = {code};
    kernel_call_guest(kSetDummy, arguments, 1u);
    recomp_fpu_context_restore(&fpu);
    recomp_runtime.registers = saved;
    std::fprintf(stderr, "practice: dummy code %u, control %u\n", code, *recomp_memory_u32(kPlayer2 + 0x50u));
}

/* The player's choice: its action, or (block after the first hit) the game's guard from the second attack on */
void SetDummy()
{
    const Action &action = kActions[block_after_hit ? (dummy >= 4 && dummy <= 7 ? 7 : 3) : dummy];
    *recomp_memory_u32(kOption) = action.option;
    SetCode(action.code);
}

void ApplySettings()
{
    *recomp_memory_u32(kInformation) = static_cast<uint32_t>(information);
    *recomp_memory_u32(kCounterOption) = counter_on ? 5u : 1u;
    *recomp_memory_u32(kPlayer2 + kCounter) = counter_on ? 5u : 1u;
}

void TakeDummy() { dummy_wanted = dummy_controller = true; }
void ReleaseDummy()
{
    dummy_wanted = true;
    dummy_controller = false;
}

void Mirror(RecompInputGamepad &pad)
{
    const unsigned buttons = pad.buttons;
    pad.buttons = static_cast<uint16_t>((buttons & ~0xcu) | (buttons & 4u) << 1 | (buttons & 8u) >> 1);
    pad.thumb_lx = static_cast<int16_t>(pad.thumb_lx == -32768 ? 32767 : -pad.thumb_lx);
}

unsigned Held(const RecompInputGamepad &pad)
{
    constexpr int kStick = 16000;
    unsigned held = pad.buttons & (kStart | kBack);
    if ((pad.buttons & 1u) || pad.thumb_ly > kStick) held |= kUp;
    if ((pad.buttons & 2u) || pad.thumb_ly < -kStick) held |= kDown;
    if ((pad.buttons & 4u) || pad.thumb_lx < -kStick) held |= kLeft;
    if ((pad.buttons & 8u) || pad.thumb_lx > kStick) held |= kRight;
    for (int b = 0; b < 8; ++b)
        if (pad.analog_buttons[b] > 0x40u) held |= kButtons[b];
    return held;
}

bool Neutral(const RecompInputGamepad &pad)
{
    if (pad.analog_buttons[4] > 0x40u || pad.analog_buttons[5] > 0x40u) return false;
    return (Held(pad) & ~(kStart | kBack)) == 0u;
}

void StopRecording()
{
    size_t a = 0, b = take.size();
    while (a < b && Neutral(take[a])) ++a;
    while (b > a && Neutral(take[b - 1])) --b;
    mode = Mode::Idle;
    ReleaseDummy();
    if (b > a) {
        recs[rec_slot].assign(take.begin() + std::ptrdiff_t(a), take.begin() + std::ptrdiff_t(b));
        Say("Recording %d saved (%.1f s)", rec_slot + 1, double(recs[rec_slot].size()) / 60.0);
    } else {
        Say("Nothing recorded");
    }
    take.clear();
}

/* The slot of the next run: OFFENSE the chosen one, DEFENSE a random recorded one by its frequency; false: none */
bool NextRun()
{
    if (!defense) {
        if (recs[rec_slot].empty()) return false;
        playing = rec_slot;
        return true;
    }
    int total = 0;
    for (int k = 0; k < kRecSlots; ++k)
        if (!recs[k].empty()) total += weight[k];
    if (total == 0) return false;
    seed = seed * 1664525u + 1013904223u;
    int draw = int((seed >> 8) % uint32_t(total));
    for (int k = 0; k < kRecSlots; ++k) {
        if (recs[k].empty() || weight[k] == 0) continue;
        if (draw < weight[k]) {
            playing = k;
            break;
        }
        draw -= weight[k];
    }
    return true;
}

void ToggleRecord()
{
    if (mode == Mode::Record) {
        StopRecording();
        return;
    }
    mode = Mode::Record;
    take.clear();
    TakeDummy();
    Say("Recording - you control the dummy (START or BACK stops)");
}

void TogglePlayback()
{
    if (mode == Mode::Play) {
        mode = Mode::Idle;
        ReleaseDummy();
        Say("Playback off");
    } else if (mode == Mode::Record) {
        Say("Stop the recording first (START)");
    } else if (!NextRun()) {
        Say("Recording %d is empty - record it first", rec_slot + 1);
    } else {
        mode = Mode::Play;
        play_at = 0;
        play_gap = 0;
        TakeDummy();
        Say("Playback on");
    }
}

void ToggleBlock()
{
    block_after_hit = !block_after_hit;
    if (mode == Mode::Idle || (mode == Mode::React && !dummy_controller)) ReleaseDummy();
    Say(block_after_hit ? "Block after first hit: ON" : "Block after first hit: OFF");
}

std::string SlotName(int s) { return s == 0 ? std::string("Start") : "slot " + std::to_string(s); }

void AskSave()
{
    if (slot_at == 0) slot_at = 1; /* (the start stays the start) */
    ask_save = true;
}

void AskLoad(int slot)
{
    load_slot = slot;
    ask_load = true;
}

std::vector<int> Rows()
{
    if (defense) {
        std::vector<int> rows = {RowTrain};
        for (int k = 0; k < kRecSlots; ++k) rows.push_back(RowAction1 + k);
        rows.insert(rows.end(), {RowInterval, RowCounter, RowClose, RowQuit});
        return rows;
    }
    return {RowTrain, RowDummy, RowReact, RowBlock, RowCounter, RowSlot, RowSave, RowLoad, RowInputs, RowFrame1, RowFrame2,
            RowMeter, RowBoxes, RowList, RowHud, RowSelf, RowOpponent, RowStage, RowApply, RowClose, RowQuit};
}

int IndexOf(const Named *list, int count, int id)
{
    for (int i = 0; i < count; ++i)
        if (list[i].id == id) return i;
    return 0;
}
int Self() { return self_at >= 0 ? self_at : IndexOf(kChars, kCharCount, sc2_practice_pick(0, nullptr)); }
int Opponent() { return opponent_at >= 0 ? opponent_at : IndexOf(kChars, kCharCount, sc2_practice_pick(1, nullptr)); }
int Stage() { return stage_at >= 0 ? stage_at : IndexOf(kStages, kStageCount, sc2_practice_stage()); }

std::string Text(int row)
{
    const Training &t = TrainingState();
    const auto on = [](bool value) { return value ? "< ON >" : "< OFF >"; };
    const auto line = [](const char *format, auto... values) {
        char s[SC2_PANEL_MENU_TEXT];
        std::snprintf(s, sizeof s, format, values...);
        return std::string(s);
    };
    switch (row) {
    case RowTrain: return line("Training mode     < %s >", defense ? "DEFENSE" : "OFFENSE");
    case RowDummy: return line("Opponent actions  < %s >", kActions[dummy].name);
    case RowReact:
        return react ? line("Action after a hit or block < Opponent action %d >", react)
                     : std::string("Action after a hit or block < NOT SET >");
    case RowBlock: return line("Block after first hit  %s", on(block_after_hit));
    case RowCounter: return line("Counter hit       %s", on(counter_on));
    case RowSlot: return line("Position slot     < %s >", SlotName(slot_at).c_str());
    case RowSave: return line("Save position to %s", SlotName(slot_at == 0 ? 1 : slot_at).c_str());
    case RowLoad:
        return line("Go to position: %s%s", SlotName(slot_at).c_str(),
                    !positions[slot_at].valid ? " (empty)" : "");
    case RowInputs: return line("Input display     %s", on(t.input_display));
    case RowFrame1: return line("Frame data: you      %s", on(t.frame_data_side[0]));
    case RowFrame2: return line("Frame data: opponent %s", on(t.frame_data_side[1]));
    case RowMeter: return line("Frame meter       %s", on(t.frame_meter));
    case RowBoxes: return line("Hit boxes         %s", on(t.hit_boxes));
    case RowList: return "Move list";
    case RowHud:
        return line("HUD (attack data) < %s >", information == 2 ? "FULL" : "NONE");
    case RowSelf: return line("Quick select: you      < %s >", kChars[Self()].name);
    case RowOpponent: return line("Quick select: opponent < %s >", kChars[Opponent()].name);
    case RowStage: return line("Quick select: stage    < %s >", kStages[Stage()].name);
    case RowApply: return "Quick select: APPLY";
    case RowClose: return "Close menu";
    case RowQuit: return "Close practice";
    case RowInterval: return line("Action intervals  < %s >", kIntervalNames[interval]);
    default: break;
    }
    const int k = row - RowAction1;
    const std::string often = weight[k] ? std::to_string(weight[k]) : std::string("OFF");
    if (recs[k].empty()) return line("Opponent action %d   frequency < %s >   (empty - A: record)", k + 1, often.c_str());
    return line("Opponent action %d   frequency < %s >   %.1f s  (A: record again)", k + 1, often.c_str(),
                double(recs[k].size()) / 60.0);
}

void Apply()
{
    if (mode == Mode::Record) {
        Say("Stop the recording first");
        return;
    }
    if (kChars[Self()].id == sc2_practice_pick(0, nullptr) && kChars[Opponent()].id == sc2_practice_pick(1, nullptr) &&
        kStages[Stage()].id == sc2_practice_stage()) {
        Say("Already loaded - choose another character or stage");
        return;
    }
    ask_apply = true;
    menu_open = false;
}

/* The menu open: up / down a row, left / right its value, A does it, B closes */
void Menu(unsigned pressed)
{
    const std::vector<int> rows = Rows();
    const int n = int(rows.size());
    cursor = std::clamp(cursor, 0, n - 1);
    if (pressed & kUp) cursor = (cursor + n - 1) % n;
    if (pressed & kDown) cursor = (cursor + 1) % n;
    if (pressed & kB) {
        menu_open = false;
        return;
    }
    const int lr = pressed & kLeft ? -1 : pressed & kRight ? 1 : 0;
    const bool act = (pressed & kA) != 0u;
    if (lr == 0 && !act) return;
    Training &t = TrainingState();
    const auto toggle = [lr](bool &value) {
        if (lr != 0) value = !value;
    };
    const int row = rows[size_t(cursor)];
    switch (row) {
    case RowTrain:
        if (lr == 0) break;
        defense = !defense;
        if (mode == Mode::Play || mode == Mode::React) {
            mode = Mode::Idle;
            ReleaseDummy();
        }
        break;
    case RowDummy:
        if (lr == 0) break;
        dummy = (dummy + lr + kActionCount) % kActionCount;
        if (mode == Mode::Idle || (mode == Mode::React && !dummy_controller)) ReleaseDummy();
        break;
    case RowReact:
        if (lr == 0) break;
        react = (react + lr + kRecSlots + 1) % (kRecSlots + 1);
        if (mode == Mode::React) {
            mode = Mode::Idle;
            ReleaseDummy();
        }
        break;
    case RowBlock:
        if (lr != 0 && (lr > 0) != block_after_hit) ToggleBlock();
        break;
    case RowCounter: toggle(counter_on); break;
    case RowSlot: slot_at = (slot_at + lr + kPositions) % kPositions; break;
    case RowSave:
        if (act) AskSave();
        break;
    case RowLoad:
        if (!act) break;
        AskLoad(slot_at);
        menu_open = false;
        break;
    case RowInputs: toggle(t.input_display); break;
    case RowFrame1: toggle(t.frame_data_side[0]); break;
    case RowFrame2: toggle(t.frame_data_side[1]); break;
    case RowMeter: toggle(t.frame_meter); break;
    case RowBoxes: toggle(t.hit_boxes); break;
    case RowList:
        if (!act) break;
        if (mode != Mode::Idle) {
            Say("Stop the recording / playback first");
            break;
        }
        menu_open = false;
        drive = 1;
        break;
    case RowHud:
        if (lr != 0) information = 2 - information;
        break;
    case RowSelf:
        if (lr != 0) self_at = (Self() + lr + kCharCount) % kCharCount;
        else Apply();
        break;
    case RowOpponent:
        if (lr != 0) opponent_at = (Opponent() + lr + kCharCount) % kCharCount;
        else Apply();
        break;
    case RowStage:
        if (lr != 0) stage_at = (Stage() + lr + kStageCount) % kStageCount;
        else Apply();
        break;
    case RowApply:
        if (act) Apply();
        break;
    case RowClose:
        if (act) menu_open = false;
        break;
    case RowQuit:
        if (!act) break;
        menu_open = false;
        sc2_practice_close();
        break;
    case RowInterval: interval = (interval + lr + 3) % 3; break;
    default: {
        const int k = row - RowAction1;
        weight[k] = std::clamp(weight[k] + lr, 0, kWeightMax);
        if (act && mode != Mode::Record) {
            rec_slot = k;
            menu_open = false;
            ToggleRecord();
        }
        break;
    }
    }
}

/* The player's pad: the menu and the shortcuts taken out of it */
void Player(RecompInputGamepad *pad)
{
    static unsigned before, swallow, repeat_held;
    static int repeat;
    static bool eaten_before, back_used;
    const unsigned held = Held(*pad), pressed = held & ~before, released = before & ~held;
    before = held;
    if (drive != 0u) {
        /* (the game's pause menu to its COMMAND LIST: START, three down, A) */
        *pad = RecompInputGamepad{};
        const unsigned at = drive++;
        if (at <= 4u) pad->buttons = kStart;
        for (unsigned k = 0; k < 3u; ++k)
            if (at >= 50u + 20u * k && at < 54u + 20u * k) pad->buttons = kDown;
        if (at >= 115u && at < 119u) pad->analog_buttons[0] = 0xffu;
        if (at >= 130u) {
            drive = 0;
            game_list = true;
            running = 0;
        }
        eaten_before = true;
        return;
    }
    if (game_list) { /* (the game's list, and its menu behind it, are open: its own controls, START too) */
        eaten_before = true;
        return;
    }
    bool eat = menu_open;
    if ((pressed & kStart) && !(held & kBack)) {
        if (mode == Mode::Record) StopRecording();
        else menu_open = !menu_open;
        eat = true;
    } else if (held & kStart) {
        eat = true;
    } else if (held & kBack) {
        eat = true;
        if (!menu_open) {
            if (pressed & (kLt | kRt | kA | kY | kX | kB)) back_used = true;
            if ((pressed & kLt) && !(held & kRt)) AskSave();
            if ((pressed & kRt) && !(held & kLt)) {
                if (slot_at == 0 && !positions[1].valid) {
                    Say("No saved position");
                } else {
                    if (slot_at == 0) slot_at = 1;
                    AskLoad(slot_at);
                }
            }
            if (pressed & kA) AskLoad(0);
            if (pressed & kY) ToggleRecord();
            if (pressed & kX) TogglePlayback();
            if (pressed & kB) ToggleBlock();
        }
    } else if (released & kBack) {
        if (!back_used && mode == Mode::Record) StopRecording();
        back_used = false;
        eat = true;
    } else if (menu_open) {
        /* (a held direction repeats: after 18 reads, then every 4th) */
        const unsigned now = held & kDirections;
        unsigned again = 0;
        if (now != 0u && now == repeat_held) {
            if (++repeat >= 18 && (repeat - 18) % 4 == 0) again = now;
        } else {
            repeat_held = now;
            repeat = 0;
        }
        Menu(pressed | again);
    }
    /* (what was held as the pad was let go stays away from the game until it is let go) */
    if (eaten_before && !eat) swallow = held;
    eaten_before = eat;
    swallow &= held;
    if (eat) {
        *pad = RecompInputGamepad{};
        return;
    }
    pad->buttons = static_cast<uint16_t>(pad->buttons & ~(kStart | kBack));
    if (swallow & kDirections) {
        pad->buttons = static_cast<uint16_t>(pad->buttons & ~(swallow & kDirections));
        pad->thumb_lx = pad->thumb_ly = 0;
    }
    for (int b = 0; b < 8; ++b)
        if (kButtons[b] & swallow) pad->analog_buttons[b] = 0u;
}

/* (a change of either fighter's record: the fight runs, the game's menu is closed) */
bool Moving()
{
    static uint32_t last;
    uint32_t now = 2166136261u;
    for (uint32_t p = 0; p < 2u; ++p) {
        const uint8_t *record = recomp_memory(SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE, 0x3000u);
        for (uint32_t i = 0; i < 0x3000u; i += 4u) {
            uint32_t word;
            std::memcpy(&word, record + i, 4u);
            now = (now ^ word) * 16777619u;
        }
    }
    const bool moved = now != last;
    last = now;
    return moved;
}

} // namespace

const RecompInputGamepad &PracticeOwnPad() { return own; }

bool PracticePad(uint32_t port, RecompInputGamepad *pad, bool sampled)
{
    if (port == 0u) {
        if (!sampled) *pad = RecompInputGamepad{};
        if (!sc2_practice_asking()) Player(pad);
        else if (menu_open) *pad = RecompInputGamepad{};
        else pad->buttons = static_cast<uint16_t>(pad->buttons & ~(kStart | kBack));
        own = *pad;
        if (mode == Mode::Record && !game_list && drive == 0u) {
            source = *pad;
            *pad = RecompInputGamepad{};
        }
        return true;
    }
    if (mode == Mode::Record) {
        *pad = source;
        return true;
    }
    if (mode == Mode::Play || (mode == Mode::React && dummy_controller)) {
        *pad = RecompInputGamepad{};
        const auto &rec = recs[playing];
        if ((mode == Mode::React || play_gap == 0) && play_at < rec.size()) {
            *pad = rec[play_at];
            if (!TrainingDummyRight()) Mirror(*pad);
        }
        return true;
    }
    pad->buttons = static_cast<uint16_t>(pad->buttons & ~(kStart | kBack));
    return sampled;
}

void PracticeFrame()
{
    if (game_list) {
        running = Moving() ? running + 1 : 0;
        if (running >= 10) {
            game_list = false;
            std::fprintf(stderr, "practice: the game's command list / menu closed\n");
        }
        return;
    }
    if (drive != 0u) return;
    if (!started) {
        if (*recomp_memory_u32(kBegun) == 0u) return;
        started = ask_start = true;
        ReleaseDummy();
    }
    if (ask_apply) {
        ask_apply = false;
        int outfit[2] = {0, 0};
        const int before[2] = {sc2_practice_pick(0, &outfit[0]), sc2_practice_pick(1, &outfit[1])};
        const int chosen[2] = {kChars[Self()].id, kChars[Opponent()].id};
        for (int p = 0; p < 2; ++p)
            if (chosen[p] != before[p]) outfit[p] = 0;
        if (!sc2_practice_again(chosen[0], outfit[0], chosen[1], outfit[1], kStages[Stage()].id)) {
            Say("The quick select needs the launcher");
        } else if (chosen[1] != before[1]) {
            for (auto &rec : recs) rec.clear(); /* (another opponent: its recordings would be the last one's) */
        }
        return;
    }
    /* DEFENSE: the dummy plays its actions by their frequency; OFFENSE: the action after a hit or block */
    if (mode == Mode::Idle && defense && !menu_open && NextRun()) {
        mode = Mode::Play;
        play_at = 0;
        play_gap = kIntervalFrames[interval];
        TakeDummy();
    } else if (mode == Mode::Idle && !defense && react != 0 && !recs[react - 1].empty()) {
        mode = Mode::React;
        reacting = false;
        ReleaseDummy();
    }
    if (mode == Mode::Record) {
        RecompInputGamepad entry = source;
        if (!TrainingDummyRight()) Mirror(entry);
        take.push_back(entry);
        if (take.size() >= kMaxFrames) StopRecording();
    } else if (mode == Mode::Play) {
        if (play_gap > 0) {
            --play_gap;
        } else if (++play_at >= recs[playing].size()) {
            play_at = 0;
            play_gap = kIntervalFrames[interval];
            if (!NextRun()) {
                mode = Mode::Idle;
                ReleaseDummy();
            }
        }
    } else if (mode == Mode::React) {
        if (dummy_controller) {
            if (++play_at >= recs[playing].size()) {
                ReleaseDummy();
                reacting = false;
            }
        } else {
            const bool stunned = *reinterpret_cast<const int32_t *>(recomp_memory(kPlayer2 + kHitStun, 4u)) > 0 ||
                *recomp_memory_u32(kPlayer2 + kReaction) != 0u;
            if (reacting && !stunned) {
                playing = react - 1;
                play_at = 0;
                TakeDummy();
            }
            reacting = stunned;
        }
    }
    if (dummy_wanted) {
        dummy_wanted = false;
        if (dummy_controller) SetCode(kControllerCode);
        else SetDummy();
    }
    ApplySettings();
}

void PracticeLoaded()
{
    play_at = 0;
    play_gap = 0;
    take.clear(); /* (a recording starts again from the new position) */
    reacting = false;
    if (mode == Mode::Play || mode == Mode::Record) TakeDummy();
    else ReleaseDummy();
}

bool PracticeSlots()
{
    if (!ask_save && !ask_load && !ask_start) return false;
    const bool save = ask_save, first = ask_start;
    ask_save = ask_load = ask_start = false;
    if (!sc2_practice_running() || !TrainingFightReady()) return false;
    recomp_kernel_drain_others();
    if (first) {
        SaveFrame(positions[0]);
        std::fprintf(stderr, "practice: the start kept\n");
        return false;
    }
    SaveState &state = positions[save ? slot_at : load_slot];
    if (save) {
        if (mode == Mode::Record || mode == Mode::Play) {
            Say("Stop the recording / playback first, then save the position");
            return false;
        }
        SaveFrame(state);
        Say("Position saved (%s)", SlotName(slot_at).c_str());
        return false;
    }
    if (!state.valid) {
        Say("Nothing saved in %s yet - menu: Save position", SlotName(load_slot).c_str());
        return false;
    }
    LoadFrame(state);
    TrainingPositionLoaded();
    Say("Reset (%s)", SlotName(load_slot).c_str());
    return true;
}

bool PracticeGameMenu() { return game_list || drive != 0u; }

void PracticePanel(Sc2Panel &panel)
{
    if (mode == Mode::Record)
        std::snprintf(panel.status, sizeof panel.status, "REC  %.1f / 30 s  (START: stop)", double(take.size()) / 60.0);
    if (!menu_open) return;
    const std::vector<int> rows = Rows();
    panel.menu_cursor = std::clamp(cursor, 0, int(rows.size()) - 1);
    for (const int row : rows)
        if (panel.menu_rows < SC2_PANEL_MENU)
            std::snprintf(panel.menu[panel.menu_rows++], SC2_PANEL_MENU_TEXT, "%s", Text(row).c_str());
    static const char *const kHelp[SC2_PANEL_HELP] = {
        "Left / right: change   A: do it   B / START: close", "Quick, menu closed (BACK +):",
        "  A: round start   LT: save position   RT: go to position", "  X: playback    Y: record    B: block after hit"};
    for (const char *help : kHelp) std::snprintf(panel.help[panel.help_rows++], SC2_PANEL_MENU_TEXT, "%s", help);
}

void PracticeForget(bool keep)
{
    mode = Mode::Idle;
    menu_open = started = game_list = ask_save = ask_load = ask_start = ask_apply = reacting = false;
    dummy_wanted = dummy_controller = false;
    drive = 0;
    play_at = 0;
    play_gap = 0;
    slot_at = 0;
    self_at = opponent_at = stage_at = -1;
    take.clear();
    for (SaveState &state : positions) state = SaveState{};
    if (keep) return;
    for (auto &rec : recs) rec.clear();
    defense = block_after_hit = counter_on = false;
    dummy = react = interval = rec_slot = cursor = 0;
    information = 2;
    std::fill(std::begin(weight), std::end(weight), 10);
}

} // namespace sc2
