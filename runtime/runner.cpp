/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Soulcalibur II runner: DOA2U's runner without its game hooks (profile
   unlocks, Story finale, dedication card), with the options the launcher
   starts the game with:

     --disc IMAGE      the player's Xbox disc image, read in place (instead of
                       --xbe and --disc-root, a folder of the disc's files)
     --settings FILE   settings.ini: the features and the key and button layout
     --storage DIR     the hard disk (saves) and the save journal; without it,
                       <disc root>/.recomp-storage
     --disc-file NAME=FILE  the game reads FILE for the disc's NAME
     --guest-link FILE Link in the guest's place: FILE is the root.olk built
                       from the player's discs (tools/guests/build_guest.py)
     --guest-heihachi FILE  Heihachi in the guest's place, the same way
     --guests FILE     Link and Heihachi beside Spawn: FILE is the root.olk
                       built by tools/guests/roster.py
     --fullscreen  --widescreen (the game's own 16:9 mode)  --mute
     --free-camera     ` or the right stick's click flies the camera
     --quick-match P1:C1:P2:C2[:STAGE]  the first attract demo fights these
                       (ids as in the select grid: Link 27, Heihachi 28)
     --input-scene SCENE[#N]:POLL:KEY  presses KEY POLL polls into scene SCENE
     --host [PORT]  --join CODE|ADDRESS[:PORT]  online play (sc2_netplay.h), with
                       --net-name NAME --net-delay FRAMES --net-list --net-service URL|off
                     --net-invite TO:FROM --net-invite-name NAME (a player in the launcher's waiting room invited)
     --list-features   prints the features for the launcher and ends
     --menu-shot PRESENT:KEYS:FILE.bmp  tests the in-game menu (sc2_menu.h)
     --export-pictures PRESENT[:FOLDER]  at that Swap, the menu's Export pictures (to FOLDER, or the pictures
                       folder beside settings.ini), and the game ends (sc2_pictures.h) */
#include "host_diagnostics.h"
#include "d3d8_adapter.h"
#include "dsound_api_adapter.h"
#include "free_camera_adapter.h"
#include "input_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "runner_load.h"
#include "save_transaction.h"
#include "sc2_settings.h"
#include "sc2_menu.h"
#include "sc2_training.h"
#include "sc2_guest.h"
#include "sc2_hooks.h"
#include "sc2_netplay.h"
#include "sc2_version.h"
#include "sc2_xbe.h"
#include "sc2_fighter.h"
#include "../recomp-common/folders.h"
#include "../recomp-common/launcher/src/online.h"
#include "sc2_online.h"
#include "sc2_pictures.h"
#include "input_keyboard_win32.h"
#include "sc2_state.h"
#include "sc2_dirty.h"
#include "ggpo/ggponet.h"

extern "C" {
uint32_t recomp_d3d8_swap_counter(void);
void sc2_d3d8_set_swap_counter(uint32_t counter);
}
#include "overlay_text.h"
#include "audio_output.h"
#include "disc_image.h"
#include "d3d_vblank.h"
#include "d3d_presenter.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "program_manual.h"
#include "stop_report.h"
RecompFunction recomp_program_lookup(std::uint32_t address);
}

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h> /* (main is the library's SDL_main, which the activity calls) */
#include "sc2_android.h"
#endif
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <cctype>
#include <string>
#include <vector>
#include <charconv>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif
#ifdef _WIN32
static int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && std::getenv(name) != nullptr) return 0;
    return _putenv_s(name, value);
}
#endif

static RecompInputPulseSource inputPulses;
static bool scriptedInputOnly;

/* --input-scene SCENE[#N]:POLL:KEY presses KEY (A, B, X, Y, start, back, up,
   down, left, right) for six polls, POLL polls after the game switched to
   SCENE for the Nth time (sc2_hooks.h). Timed from the scene, not from boot,
   a menu walk does not drift with load times. */
struct ScenePress {
    std::uint32_t scene, occurrence;
    std::uint64_t poll;
    std::uint16_t buttons;
    int analog;   // A, B, X or Y (RECOMP_INPUT_ANALOG_*), or -1
};
static std::vector<ScenePress> scenePresses;

static bool parseScenePress(const std::string &text, ScenePress &press)
{
    unsigned scene = 0, occurrence = 1;
    unsigned long long poll = 0;
    char key[16] = {};
    if (std::sscanf(text.c_str(), "%u#%u:%llu:%15s", &scene, &occurrence, &poll, key) != 4) {
        occurrence = 1;
        if (std::sscanf(text.c_str(), "%u:%llu:%15s", &scene, &poll, key) != 3) return false;
    }
    static const struct { const char *name; std::uint16_t buttons; int analog; } keys[] = {
        {"up", 0x1, -1}, {"down", 0x2, -1}, {"left", 0x4, -1}, {"right", 0x8, -1}, {"start", 0x10, -1},
        {"back", 0x20, -1}, {"A", 0, RECOMP_INPUT_ANALOG_A}, {"B", 0, RECOMP_INPUT_ANALOG_B},
        {"X", 0, RECOMP_INPUT_ANALOG_X}, {"Y", 0, RECOMP_INPUT_ANALOG_Y}};
    for (const auto &k : keys) {
        if (_stricmp(k.name, key) == 0) {
            press = {scene, occurrence, poll, k.buttons, k.analog};
            return true;
        }
    }
    return false;
}

static void applyScenePresses(RecompInputGamepad *gamepad)
{
    static std::uint32_t seenSwitches;
    /* Timed from when the scene became current ([0x398700], after its fade and load): a scene the game asks for
       (0x502A0) can turn out another (VS Battle's character select asks for 6 and is 10). */
    static std::uint32_t scene = ~0u;
    static std::uint64_t scenePoll;
    static std::map<std::uint32_t, std::uint32_t> entries;
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    if (current != scene || sc2_scene_switches != seenSwitches) {
        if (current != scene || (sc2_scene_switches != seenSwitches && sc2_scene == current)) {
            scene = current;
            scenePoll = 0;
            ++entries[scene];
            std::fprintf(stderr, "recomp input-scene: scene %u#%u\n", scene, entries[scene]);
        }
        seenSwitches = sc2_scene_switches;
    }
    for (const ScenePress &press : scenePresses) {
        if (press.scene != scene || press.occurrence != entries[scene] || scenePoll < press.poll ||
            scenePoll >= press.poll + 6u)
            continue;
        gamepad->buttons |= press.buttons;
        if (press.analog >= 0) gamepad->analog_buttons[press.analog] = 0xff;
    }
    ++scenePoll;
}

/* Online play (sc2_netplay.h): each read of controller 1 is a read of the match, whose controls are swapped with
   the other side's; controller 2 is the other player, as the same read had them. */
static std::unique_ptr<sc2::Netplay> netplay;
static std::uint64_t netReads;
static RecompInputGamepad netPlayers[2];

/* What the two games are compared by every Netplay::kHashEvery reads: the scene, both fighters' places, and the
   state of the game's two random number generators (rand at 0x3706F0; the Mersenne Twister at 0x372DF8, its
   place at 0x193C60). */
static std::uint32_t matchHash()
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](const std::uint8_t *p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    };
    mix(reinterpret_cast<const std::uint8_t *>(&sc2_scene), 4u);
    /* (the fighters' places in a fight only: elsewhere they are what the last fight, the attract demo's say, left,
       which a host waiting in the attract mode has and the other does not; a fight sets them afresh) */
    if (*recomp_memory_u32(0x398700u) == 7u) {
        mix(recomp_memory(SC2_FIGHTER_RECORD + SC2_FIGHTER_FIELD(0x2920u), 12u), 12u);
        mix(recomp_memory(SC2_FIGHTER_RECORD + SC2_FIGHTER_FIELD(0x2920u) + SC2_FIGHTER_STRIDE, 12u), 12u);
    }
    mix(recomp_memory(0x3706F0u, 4u), 4u);
    mix(recomp_memory(0x193C60u, 4u), 4u);
    mix(recomp_memory(0x372DF8u, 624u * 4u), 624u * 4u);
    return h;
}

/* The match begins at the first read with the title screen up (scene 5 current): before that each game boots on its
   own, which takes its own time. There both seed the game's random numbers from the match's seed (rand's state,
   and the Mersenne Twister through its seeding routine 0x1B460), so the two are alike from that read on. */
extern "C" {
extern const std::uint32_t sc2_direct_targets[], sc2_direct_hooked[];
extern const unsigned sc2_direct_target_count, sc2_direct_hooked_count;
RecompFunction recomp_lookup_manual(uint32_t guest_address);
}
static bool netLive;      // the match has begun (both games at the title screen, the state taken)
static bool netConnected; // the two games have met (the host waits in the background, its game playing)

/* The game's state that boot leaves different on each machine, by how long loading took there: the host's is
   taken by the other side as the match begins (Netplay::SyncState). rand's state; the Mersenne Twister's index;
   its state and the block after it (0x3737DC-0x373F74: the next seed, an order of three); the attract demo's
   counters; the scene's; the frame routine's frame counts (0x38FAC4-0x38FAEF, 0x38FB40-0x38FB57, 0x3CB418: the
   menus' timing follows them). With them the count of frames shown (the D3D adapter's, which the game copies). */
static const struct { std::uint32_t address, size; } kMatchState[] = {
    {0x3706F0u, 4u}, {0x193C60u, 4u}, {0x372DF8u, 0x373F80u - 0x372DF8u}, {0x398BF0u, 0x20u}, {0x398700u, 0x20u},
    {0x38FAC4u, 0x2Cu}, {0x38FB40u, 0x18u}, {0x3CB418u, 4u},
};

/* Online play from the in-game menu (sc2_online.h): the settings every match starts from (the player's storage, the
   build, the launcher's name), whether this hosting came from the menu (its game the player's own until someone
   joins: then to the title screen, once), and a note for the menu. What a match shows, and the chat and Esc's
   question while it goes on. */
static sc2::Netplay::Settings netBase;
static bool netFromMenu, netTitleAsked;
static std::string netNote;
static sc2::OnlineShown netShown;
static bool netTyping, netLeaving;
static std::string netTyped;
struct ChatLine { std::string text; std::uint64_t until; };
static std::vector<ChatLine> netChat;
constexpr std::uint64_t kChatMs = 8000;
constexpr std::size_t kChatLines = 5;

static std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/* Why the match ended, shown for a while after it (the reads left to show it) */
static std::string netEnded;
static unsigned netEndedShown;

/* Coming back after a drop (cps2recomp's way): the host, whose other player left or went quiet, hosts again with
   the same code while its game plays on (when they come, it starts over for the match, as hosting from the menu
   does); a joining player whose connection was lost starts again joining the same match (--net-again). */
static bool netAgain;          // (--net-again: this copy is one started again to come back)
static bool startedOver;       // (--started-over: this copy is one started in place of another, sc2_dashboard)
static bool netFresh;          // (--net-fresh: this copy is the one started over for the match: it takes them in)
static bool netWaitingReturn;  // (hosting again, for the other player to come back)
[[noreturn]] static void rejoin();
/* The game leaves for the Xbox Dashboard (XLaunchNewImage with no program, program_manual.c): the one case is its
   "game data is unusable: delete it?" (a save it cannot read, another release's: the European program with the US
   disc's save, issue #77), where the player would delete the save in the Dashboard and start the game again. There
   is no Dashboard: the save is set aside (kept, in "saves set aside" in the storage folder) and the game starts
   over, as it would after. The folder is marked here and the saves moved as the new copy starts, before it opens
   them (this copy still holds them). */
[[noreturn]] static void startAgain(std::vector<std::string> args);
static std::vector<std::string> argsAgain();
static std::string dashboardStorage;
static const char *const kSetAsideMark = "set-saves-aside";

extern "C" void sc2_dashboard(void)
{
    if (dashboardStorage.empty() || netplay) return;   // (nowhere of its own to set them aside in: the caller stops)
    std::error_code error;
    const std::filesystem::path mark = std::filesystem::u8path(dashboardStorage) / kSetAsideMark;
    if (std::FILE *file = std::fopen(mark.u8string().c_str(), "wb")) std::fclose(file);
    else return;
    std::fprintf(stderr, "recomp sc2: the game asks for the Dashboard to delete its save: set aside, starting over\n");
    std::vector<std::string> args = argsAgain();
    args.push_back("--started-over");
    startAgain(args);
}

/* A storage folder marked by sc2_dashboard: each saved game in it (partition1/UDATA/<title>/<save>/) moved to
   "saves set aside/<n>/<title>/<save>" */
static void setSavesAside(const std::filesystem::path &storage)
{
    std::error_code error;
    const std::filesystem::path mark = storage / kSetAsideMark;
    if (!std::filesystem::exists(mark, error)) return;
    std::filesystem::remove(mark, error);
    const std::filesystem::path aside = storage / "saves set aside";
    std::filesystem::path into;
    for (int n = 1; n < 1000; ++n) {
        into = aside / std::to_string(n);
        if (!std::filesystem::exists(into, error)) break;
    }
    unsigned moved = 0;
    for (const auto &title : std::filesystem::directory_iterator(storage / "partition1" / "UDATA", error)) {
        if (!title.is_directory(error)) continue;
        std::vector<std::filesystem::path> saves;
        for (const auto &save : std::filesystem::directory_iterator(title.path(), error))
            if (save.is_directory(error)) saves.push_back(save.path());
        for (const std::filesystem::path &save : saves) {
            const std::filesystem::path to = into / title.path().filename() / save.filename();
            std::filesystem::create_directories(to.parent_path(), error);
            std::filesystem::rename(save, to, error);
            if (!error) ++moved;
        }
    }
    std::fprintf(stderr, "recomp sc2: %u saved game(s) set aside in %s\n", moved, into.u8string().c_str());
}

static void hostAgain(const std::string &code, const std::string &secret);
[[noreturn]] static void startAgain(std::vector<std::string> args);
static std::vector<std::string> argsAgain();

/* The match over (the other player left or was lost, or the two games came apart): this game plays on by itself,
   its own controls on controller 1 again, and goes back to the title screen as the game's scene switch (0x502A0)
   does: the next scene asked for ([0x398704]) and the scene's task ([0x39871C]) ended. The other player's game
   plays on from the host's save in the match's folder, which it keeps until it is started again. Then each comes
   back if it can (above). */
static bool netPrelude, netParked; // (--net-prelude, below: preludeFrame)
/* Practice (--practice C1:O1:C2:O2:STAGE, the launcher's, as Tekken 6 Online's): the game goes unshown and unheard to
   VS Battle's select as the online prelude does, puts the chosen fighters under the cursors, confirms, and at the
   fight gives the controls to the player (controller 1 player 1, controller 2 player 2) with training's refill, endless clock, frame data, input display and combo readout on, and the controls' hint
   shown for 15 s (the in-game menu has the rest). Offline: no match, nothing sent. */
static double perfNow();
extern "C" void recomp_scene_freeze_set(bool frozen);
static bool practice;
/* (the launcher's one process, as Tekken 6 Online's: snapMenu = the mode select the practice walk starts from, snapPark =
   the parking, put back when the practice ends or a match is accepted in it) */
static sc2::SaveState snapMenu, snapPark;
/* (as T6's Q: taken once by a hidden walk at the first parking, before the first 'ready', and every practice goes on from
   it, whatever its fighters are: the character select as it waits. Not its end as in T6: here the fighters are loaded as
   soon as they are confirmed, so from a later place they would be the walk's) */
static sc2::SaveState snapQ;
static unsigned snapQFrames, snapQLive, snapQConfirm;
static bool practicePrepare, practiceFromQ;
static int practiceStageUsed = -1; // (the stage the last one was on: 'resume' plays on it)
static unsigned snapMenuFrames, practiceOutside;
/* (the walk's own counters as at the parking: the other side's game never had a practice, so they are put back with it) */
static unsigned snapParkAutoFrames, snapParkLiveFrames, snapParkConfirmFrom;
static bool snapParkSelectSeen;
static bool practiceWanted, practiceHold, practiceStartNow, practiceSearching, practiceBefore, practiceEver;
static int practicePrompt; // (0 none, 1 asked, 2 accepted)
static std::string practicePromptInfo, practiceAnswer;
static double practiceAskedAt, practiceStep[4]; // (measurement: the select reached, the walk on, the fight's scene, its frames)
static sc2::Training practiceAids;
static unsigned practiceHint; // (frames the controls' hint is still shown: practiceHintFrame)
static int practicePick[2] = {-1, -1}, practiceOutfit[2] = {-1, -1}, practiceStage = -1;
extern "C" int sc2_pick_active;
extern "C" int sc2_audio_quiet;
static std::string netIconsFolder; // (the launcher's character icons, made from the player's disc once: preludeFrame)
static void sessionOver(int code, const char *why);
static bool launcherMode();
static void endFrame();
static bool endAsked, parkAsked; // (the match is over for this side / the launcher's 'park': done by endFrame at the start of the next frame)
static void ggClose();

static void endMatch()
{
    if (launcherMode()) {
        // (called from anywhere, GGPO's own calls too: the session is left at the start of the next frame, endFrame)
        endAsked = true;
        sc2_net_frame = endFrame;
        return;
    }
    if (netPrelude) sessionOver(netplay->left || netplay->lost ? 22 : 2, netplay->ended.c_str());
    std::fprintf(stderr, "online: %s; playing on alone\n", netplay->ended.c_str());
    if (!netplay->lost) netplay->Leave(); // (a goodbye to a side gone quiet would tell it, when it is heard again, that this one left)
    netplay->Report(true);
    netEnded = netplay->ended;
    netEndedShown = 60u * 6u;
    const bool host = netplay->Local() == 0, met = netConnected;
    const bool again = met && (host ? netplay->left || netplay->lost : netplay->lost);
    const std::string code = netplay->Code(), secret = netplay->Secret();
    netplay.reset();
    if (again && !host) rejoin();
    if (netLive) {
        *recomp_memory_u32(0x398704u) = 5u;
        const std::uint32_t scene = *recomp_memory_u32(0x39871Cu);
        if (scene) *reinterpret_cast<std::uint16_t *>(recomp_memory(scene + 0x28u, 2u)) = 1u;
    }
    netLive = false;
    netConnected = false;
    netFromMenu = netTitleAsked = netTyping = netLeaving = false;
    netChat.clear();
    sc2_net_live = 0;
    sc2_net_frame = nullptr;
    sc2_fast_forward = 0;
    recomp_kernel_set_deterministic(false);
    if (again && host) hostAgain(code, secret);
}

static void endedOverlay()
{
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    if (netEndedShown && --netEndedShown) {
        recomp_overlay_set_title(&text, "ONLINE");
        recomp_overlay_add_line(&text, "%s", netEnded.c_str());
        recomp_overlay_set_hint(&text, "The match is over; the game goes on here alone.");
    }
    recomp_overlay_publish(&text);
}

static void netFrame();
static std::uint64_t netVirtualBase; // (the kernel's virtual clock at the match's start)

/* As the match begins at the title, the host's game goes on by itself to VS Battle's character select: Start, then
   on the mode select A (Original), down (VS Battle, there with two players) and A, each a few frames into its scene.
   They are the host's controls, so both games take them as the match's. Over once the select is current (scene 10). */
static bool netRoute;
/* The fight chosen in the launcher (--net-pick, --net-stage; sc2_netplay.h): from the title both sides go to it by
   themselves, each giving both players the controls this script makes, the same on both (it reads only the scene
   and its frames), so neither waits on the other there; the select's cursors are put on the chosen fighters
   (sc2_hooks.c, cursor_move). From the fight on the controls go over the network again, with rollback. */
static bool netAuto;
static RecompInputGamepad netAutoPads[2];
static int netAutoOutfit[2] = {-1, -1};
/* (a mirror in the same outfit: the select takes no second fighter alike, the walk stands still there (measured 2026-11-24,
   characters 3 and 5, outfits 0 and 1): player 2 has the other one, the same on both sides of a match) */
static void mirrorOutfit(const int pick[2])
{
    if (pick[0] == pick[1] && netAutoOutfit[0] == netAutoOutfit[1] && netAutoOutfit[1] >= 0) netAutoOutfit[1] = 1 - netAutoOutfit[1];
}
static int netAutoStage = -1;
static double netBeganAt;
static int netPreludeMuted = -1; // (the prelude: the sound as it was before it)
static bool netAutoMuted;        // (the sound was off before the way to the fight)
static std::uint64_t netAutoCardAt;

/* (on the way to the chosen fight nothing of the menus is shown: the frames are drawn by nobody (sc2_hide_frames) but
   paced as ever, or the sound's queue overflows; the window shows a black card, with the overlay's "loading" over it,
   a few times a second) */
static void autoCard(std::uint64_t frame)
{
    if (frame < netAutoCardAt) return;
    netAutoCardAt = frame + 20u;
    static const std::uint32_t black = 0xff000000u;
    const int unshown = sc2_fast_forward, hidden = sc2_hide_frames;
    sc2_fast_forward = sc2_hide_frames = 0;
    recomp_d3d8_present_card(&black, 1u, 1u, 4.0f / 3.0f, 255u);
    sc2_fast_forward = unshown;
    sc2_hide_frames = hidden;
}

static std::uint32_t autoScene = ~0u;
static unsigned autoFrames, autoLiveFrames, autoConfirmFrom;
static bool autoSelectSeen, autoRematch;

extern "C" void sub_000502C0(void);
/* (as Tekken 6 Online's hold point: the parked select is left straight for the fight, the fighters, outfits and stage
   put in place and the select's own request for the fight's scene made (0x502C0(7, 30), as the select does at its
   end, 0x10258A): no cursor or confirm on the way. SC2_SELECT_ROUTE=1: the confirms as before.) */
static bool autoDirectDone;
static void autoDirect()
{
    const auto push = [](std::uint32_t value) {
        recomp_runtime.registers.esp -= 4u;
        *recomp_memory_u32(recomp_runtime.registers.esp) = value;
    };
    push(0x1Eu);
    push(7u);
    push(0u); // (the return address, popped by the callee's ret)
    sub_000502C0();
    recomp_runtime.registers.esp += 8u; // (cdecl: the caller's pop of the two arguments)
    autoDirectDone = true;
    std::fprintf(stderr, "online: the fight asked for straight from the parking (frame %llu)\n", (unsigned long long)sc2_net_reads);
}

/* Team battle (the launcher's start line mode=team, --net-team): at the meeting both sides put back the mode select the
   practice walk starts from (snapMenu, Original's list open, the cursor on Arcade; the host's pages then make the two
   alike) and the hidden way goes to VS Team Battle, five down (the practice's six); once its select is current the
   controls are the players' own (lockstep outside the fight, rollback in it, as ever): they choose the team, fighters
   and order there, and the game the stage. */
static bool netTeam, teamMenuSeen, teamHanded;
static std::uint64_t teamSelectAt; // (tests: the frame its select was entered, the mash counted from it)
static sc2::SaveState teamSelectStart; // (its select as the players got it: a rematch goes back there, the wins kept)

/* (the team's size, chosen in the lobby (team=N): set by the hidden way on the select's first screen, the same for both
   players: left kTeamSizeSteps times (down to the game's least, kGameTeamMin), right up to N, A; then teamSizeSet and the
   controls are the players' for the fighters) */
static int netTeamSize = 3;
static bool teamSizeSet;
constexpr unsigned kGameTeamMin = 1u, kTeamSizeSteps = 10u, kTeamSizeFrom = 60u;
static unsigned teamSizeAt(unsigned step) { return kTeamSizeFrom + 10u * step; }

static void teamWalk(std::uint32_t current)
{
    const auto pulse = [](unsigned at, unsigned now) { return now >= at && now < at + 4u; };
    if (current == 10u && teamMenuSeen && !teamSizeSet) {
        const unsigned rights = static_cast<unsigned>(std::max(netTeamSize, int(kGameTeamMin))) - kGameTeamMin;
        for (auto &pad : netAutoPads) {
            for (unsigned i = 0; i < kTeamSizeSteps; ++i)
                if (pulse(teamSizeAt(i), autoFrames)) pad.buttons |= 0x4u;
            for (unsigned i = 0; i < rights; ++i)
                if (pulse(teamSizeAt(kTeamSizeSteps + i), autoFrames)) pad.buttons |= 0x8u;
            if (pulse(teamSizeAt(kTeamSizeSteps + rights + 1u), autoFrames)) pad.analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        }
        if (autoFrames >= teamSizeAt(kTeamSizeSteps + rights + 4u)) teamSizeSet = true;
        return;
    }
    if (current != 6u) return;
    teamMenuSeen = true;
    for (unsigned i = 0; i < 5u; ++i)
        if (pulse(140u + 20u * i, autoFrames)) netAutoPads[0].buttons |= 0x2u;
    if (pulse(260u, autoFrames)) netAutoPads[0].analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
}

static void autoPads()
{
    netAutoPads[0] = netAutoPads[1] = RecompInputGamepad{};
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    if (current != autoScene) {
        autoScene = current;
        autoFrames = 0;
    }
    if (netTeam && !practice && sc2_net_live) {
        // (after the meeting only: the way from the title to the parking is the same as ever)
        teamWalk(current);
        ++autoFrames;
        return;
    }
    const auto pulse = [](unsigned at, unsigned now) { return now >= at && now < at + 6u; };
    if (!autoRematch) {
        // (player 1 to VS Battle's character select, as routeToSelect: Start, A, down, A)
        if (current == 5u && pulse(60u, autoFrames)) netAutoPads[0].buttons |= RECOMP_INPUT_BUTTON_START;
        // (practice: Practice is six down from Arcade, Original's first entry)
        if (current == 6u && (pulse(80u, autoFrames) || pulse(practice ? 300u : 200u, autoFrames)))
            netAutoPads[0].analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        if (current == 6u && (pulse(140u, autoFrames) || (practice && autoFrames >= 160u && autoFrames < 260u &&
                                                                       pulse(autoFrames / 20u * 20u, autoFrames))))
            netAutoPads[0].buttons |= 0x2u;
        // (after the title's Start the game loads its save (scene 34), and asks to make one if there is none: Yes,
        // the cursor up and left first, then A)
        if (current == 34u && autoFrames % 40u < 4u) netAutoPads[0].buttons |= 0x1u | 0x4u;
        if (current == 34u && autoFrames % 40u >= 20u && autoFrames % 40u < 24u)
            netAutoPads[0].analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
    } else if (current == 7u && !autoSelectSeen && autoFrames % 30u < 6u) {
        // (a rematch: from the results, still the fight's scene ("PRESS START TO SKIP"), on to the select)
        for (auto &pad : netAutoPads) pad.buttons |= RECOMP_INPUT_BUTTON_START;
    }
    autoSelectSeen |= current == 10u;
    // (the chosen outfits: each fighter's record, +0x64, as the select leaves it; put there each frame from the
    // select on, until the fight, so the fight loads them)
    // (and the chosen stage, which VS Battle otherwise draws at random: 0x398770, with its copy at 0x39876C; the fight
    // setup 0x3BED0 keeps its stage there too)
    if (autoSelectSeen && current != 7u && netAutoStage >= 0) {
        *recomp_memory_u32(0x39876Cu) = static_cast<std::uint32_t>(netAutoStage);
        *recomp_memory_u32(0x398770u) = static_cast<std::uint32_t>(netAutoStage);
    }
    if (autoSelectSeen && current != 7u)
        for (std::uint32_t p = 0; p < 2u; ++p)
            if (netAutoOutfit[p] >= 0)
                *reinterpret_cast<std::int16_t *>(recomp_memory(SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE +
                    SC2_FIGHTER_FIELD(0x64u), 2u)) = static_cast<std::int16_t>(netAutoOutfit[p]);
    // (the select and what follows it up to the fight: both confirm, once a second; after the prelude's meeting or a
    // rematch's start at once, 10 frames in (the cursors put on the chosen fighters first), then every half second)
    if (sc2_net_live || practice) ++autoLiveFrames;
    const unsigned since = autoLiveFrames - autoConfirmFrom;
    static const unsigned confirmEvery = std::getenv("SC2_AUTO_PACED") ? 30u : 12u; // (unpaced: as soon as the screens take it)
    const bool confirm = netPrelude || autoRematch ? since >= 10u && (since - 10u) % confirmEvery < 6u
                                                   : autoFrames >= 60u && autoFrames % 60u < 6u && autoLiveFrames >= 30u;
    static const bool selectRoute = std::getenv("SC2_DIRECT_SETUP") == nullptr; // (the direct way: not working yet, the fight lacks what the select prepares)
    if (!selectRoute && (netPrelude || autoRematch) && current == 10u && sc2_net_live) {
        if (!autoDirectDone && since >= 2u) autoDirect();
        ++autoFrames;
        return;
    }
    if (autoSelectSeen && current != 5u && current != 6u && current != 7u && (sc2_net_live || (practice && netAuto)) && confirm) {
        netAutoPads[0].analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        static const int p2Button = [] {
            const char *b = std::getenv("SC2_TEST_P2_BUTTON"); // (diagnosis: the button player 2 confirms with)
            return !b ? RECOMP_INPUT_ANALOG_A : *b == 'X' ? RECOMP_INPUT_ANALOG_X : *b == 'Y' ? RECOMP_INPUT_ANALOG_Y
                : RECOMP_INPUT_ANALOG_A;
        }();
        netAutoPads[1].analog_buttons[p2Button] = 0xff;
    }
    ++autoFrames;
}

/* After a match (VS Battle's results, the wins counted): REMATCH? Each player's A, B or Y, read from the controls
   both sides have (the results run in lockstep, so every frame's are known on both): both A, the same fighters and
   stage again, the way to them hidden as the first time; B (leave) or Y (other fighters, chosen in the launcher)
   ends the session, the program's exit code telling the launcher which (kExitLeave, kExitChange; kExitGone when the
   other side went). Each player's wins, P1's at 0x39DE58 and P2's at 0x3AECB8 (+0x4c38 in each fighter's record),
   the draws at 0x39DE5A. */
constexpr int kExitLeave = 20, kExitChange = 21, kExitGone = 22;
static std::uint32_t netStartWins;
static std::uint64_t netOverAt;   // (the match's first frame with the wins counted, 0: none; found again on a rollback)
static bool netWantsRematch[2];
static sc2::Netplay::Settings preludeSettings; // (the prelude's match, preludeMeet)
/* (the fight as it began, both sides alike there: a rematch with the same fighters and stage puts it back, as T6's
   snapshot S, the results being the fight's scene still; nothing loads again. The wins of the results kept.) */
static sc2::SaveState netFightStart;

static std::uint32_t winsCounted()
{
    return *reinterpret_cast<const std::uint16_t *>(recomp_memory(0x39DE58u, 2u)) +
           *reinterpret_cast<const std::uint16_t *>(recomp_memory(0x3AECB8u, 2u)) +
           *reinterpret_cast<const std::uint16_t *>(recomp_memory(0x39DE5Au, 2u));
}

/* The launcher's game (SC2_NET_STATUS, SC2_NET_CMD: two files), as Tekken 6 Online's: the game goes to its parking
   unshown ("state ready"); a line "start player=1|2 port=P remote=IP:P delay=N char1=C char2=C stage=S name1=HEX
   name2=HEX" begins a match against a side whose launcher opened the way on that UDP port (player 1 hosts; a
   fighter's C is its id x 4 + its outfit); then "state loading", "state fight", and after it "winner 0|1|2" with
   "state result". "rematch" plays it again, "park" and "quit" end the program (the launcher starts it again: what
   a match leaves in memory, the next opponent's game would not have). */
static std::string launcherState = "boot";
static int launcherWinner = -1;
static bool launcherGone, launcherRematch;
static std::uint32_t launcherWins[3]; // (P1's, P2's, the draws as the match began)

static bool launcherMode()
{
    static const bool on = std::getenv("SC2_NET_STATUS") != nullptr && std::getenv("SC2_NET_CMD") != nullptr;
    return on;
}

static void launcherWrite()
{
    static std::string last;
    std::string text = "state " + launcherState + "\n";
    if (launcherWinner >= 0) text += "winner " + std::to_string(launcherWinner) + "\n";
    if (launcherGone) text += "disconnected\n";
    if (practice && !netPrelude) text += "practice 1\n";
    if (!practiceAnswer.empty()) text += "prompt " + practiceAnswer + "\n";
    std::error_code there;
    if (text == last && std::filesystem::exists(std::filesystem::u8path(std::getenv("SC2_NET_STATUS")), there)) return;
    const std::filesystem::path to = std::filesystem::u8path(std::getenv("SC2_NET_STATUS"));
    std::filesystem::path part = to;
    part += ".new";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
    // (the launcher may be reading it at this moment: a few tries; not written, the next call tries again)
    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        std::filesystem::rename(part, to, ec);
        if (!ec) {
            last = text;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void launcherSet(const char *state)
{
    launcherState = state;
    launcherWrite();
    std::fprintf(stderr, "online: state %s\n", state);
}

static std::uint32_t winsAt(std::uint32_t address)
{
    return *reinterpret_cast<const std::uint16_t *>(recomp_memory(address, 2u));
}

static void launcherFight()
{
    if (!launcherMode()) return;
    launcherWins[0] = winsAt(0x39DE58u);
    launcherWins[1] = winsAt(0x3AECB8u);
    launcherWins[2] = winsAt(0x39DE5Au);
    launcherWinner = -1;
    launcherRematch = false;
    launcherSet("fight");
}

// (the match decided: the win pose is seen first, four seconds of it, then the launcher takes over)
static void launcherOver(std::uint64_t frame)
{
    if (launcherWinner >= 0 || netOverAt == 0 || frame < netOverAt + 240u) return;
    launcherWinner = winsAt(0x39DE58u) != launcherWins[0] ? 1 : winsAt(0x3AECB8u) != launcherWins[1] ? 2 : 0;
    launcherSet("result");
}

[[noreturn]] static void launcherExit()
{
    ggClose();
    if (netplay && netConnected && !netplay->lost) netplay->Leave();
    std::fflush(stderr);
    std::_Exit(0);
}

// The launcher's commands since the last look; true: a match is to start (preludeSettings)
static bool launcherPoll()
{
    const std::filesystem::path path = std::filesystem::u8path(std::getenv("SC2_NET_CMD"));
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (text.empty()) return false;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    bool start = false;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string command, word;
        words >> command;
        if (command == "quit") launcherExit();
        if (command == "park" && !practice && (netplay || netLive)) {
            // (as T6's: back to the parking, no new game; without the parking's snapshot the program ends and the launcher
            // starts it again)
            if (!snapPark.valid) launcherExit();
            parkAsked = true;
            sc2_net_frame = endFrame;
        }
        if (command == "rematch") launcherRematch = true;
        if (command == "hold" && (practice || practiceWanted)) practiceHold = true; // (also on the way to its fight)
        if (command == "prompt" && practice) {
            words >> word;
            practicePrompt = word == "on" ? 1 : 0;
            practiceAnswer.clear();
            practicePromptInfo.clear();
            if (practicePrompt) {
                std::getline(words, practicePromptInfo);
                practicePromptInfo.erase(0, practicePromptInfo.find_first_not_of(' '));
            }
            launcherWrite();
        }
        if (command == "practice" && !netConnected && !practice && !(snapMenu.valid && snapPark.valid))
            launcherSet("error practice: the game is not ready for it");
        if (command == "practice" && !netConnected && !practice && snapMenu.valid && snapPark.valid) {
            std::map<std::string, std::string> v;
            while (words >> word)
                if (const std::size_t at = word.find('='); at != std::string::npos) v[word.substr(0, at)] = word.substr(at + 1);
            const bool resume = practiceEver && v.count("resume") && v["resume"] != "0"; // (the opponent and the stage of the last one)
            const auto fighter = [&v](const char *key, int p) {
                const int code = v.count(key) ? std::atoi(v[key].c_str()) : 4;
                practicePick[p] = code / 4;
                practiceOutfit[p] = code % 4;
            };
            fighter("char1", 0);
            if (!resume) {
                fighter("char2", 1);
                practiceStage = v.count("stage") ? std::atoi(v["stage"].c_str()) : -1;
            } else if (practiceStageUsed >= 0) {
                practiceStage = practiceStageUsed;
            }
            practiceEver = practiceWanted = true;
            practiceAskedAt = perfNow();
        }
        if (command != "start" || netConnected) continue;
        std::map<std::string, std::string> v;
        while (words >> word)
            if (const std::size_t at = word.find('='); at != std::string::npos) v[word.substr(0, at)] = word.substr(at + 1);
        const auto number = [&v](const char *key, int otherwise) {
            return v.count(key) ? std::atoi(v[key].c_str()) : otherwise;
        };
        const int player = number("player", 0), port = number("port", 0);
        if ((player != 1 && player != 2) || port <= 0 || v["remote"].empty()) {
            launcherSet("error the start line did not make sense");
            continue;
        }
        sc2::Netplay::Settings s = netBase;
        s.hosting = player == 1;
        s.port = port;
        s.bindPort = port;
        s.address = s.hosting ? std::string() : v["remote"];
        s.service = "off";
        s.restartOnHello = false;
        s.delay = number("delay", 1);
        const int own = number(player == 1 ? "char1" : "char2", 4);
        s.pick = std::to_string(own / 4) + ":" + std::to_string(own % 4);
        s.stage = number("stage", -1);
        netTeam = v["mode"] == "team"; // (team battle: chosen in the game, teamWalk)
        netTeamSize = std::clamp(number("team", 3), 1, 8);
        std::string name;
        const std::string hex = v[player == 1 ? "name1" : "name2"];
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
            name += static_cast<char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
        s.name = name.substr(0, sc2::Netplay::kNameMost);
        preludeSettings = s;
        start = true;
    }
    return start;
}

// (parked: the picture is renewed now and then, so the window answers the system)
static void launcherPark()
{
    launcherSet("ready");
    for (unsigned n = 0; !launcherPoll() && !practiceWanted; ++n) {
        if (n % 10u == 0u) {
            netAutoCardAt = 0;
            autoCard(0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    launcherSet("loading");
    if (practiceWanted) return;
    netplay = std::make_unique<sc2::Netplay>(preludeSettings);
}

/* The match over without a result (the other side gone, the two games apart, no connection), or the other side gone
   after it: told, and the launcher's "quit" or "park" waited for; it ends the program itself otherwise. */
// (the match over without a result, or the other side gone after it: told, and the launcher's "park" or "quit" waited for)
static void launcherEnded()
{
    std::fprintf(stderr, "online: the session is over: %s\n", netplay ? netplay->ended.c_str() : "");
    const bool met = netConnected;
    if (launcherWinner < 0 && netplay && (!met || !(netplay->left || netplay->lost))) launcherSet(("error " + netplay->ended).c_str());
    launcherGone = true;
    launcherWrite();
    const auto began = std::chrono::steady_clock::now();
    while (!parkAsked && std::chrono::steady_clock::now() - began < std::chrono::minutes(10)) {
        launcherPoll();
        launcherWrite();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!parkAsked) launcherExit();
}

static void netSessionReset();
static void autoRestart()
{
    autoRematch = true;
    autoSelectSeen = false;
    autoDirectDone = false;
    autoConfirmFrom = autoLiveFrames;
    netStartWins = winsCounted();
    netOverAt = 0;
    netWantsRematch[0] = netWantsRematch[1] = false;
}

static void sessionOver(int code, const char *why)
{
    std::fprintf(stderr, "online: the session is over: %s (exit %d)\n", why, code);
    ggClose();
    if (netplay && !netplay->lost) netplay->Leave();
    std::fflush(stderr);
    std::_Exit(code);
}

static void routeToSelect(RecompInputGamepad *pad)
{
    static const struct { std::uint32_t scene; unsigned at; std::uint16_t buttons; int analog; } kSteps[] = {
        {5u, 60u, RECOMP_INPUT_BUTTON_START, -1}, {6u, 80u, 0u, RECOMP_INPUT_ANALOG_A},
        {6u, 140u, 0x2u, -1}, {6u, 200u, 0u, RECOMP_INPUT_ANALOG_A}};
    static std::uint32_t scene = ~0u;
    static unsigned frames;
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    if (current == 10u) {
        netRoute = false;
        return;
    }
    if (current != scene) {
        scene = current;
        frames = 0;
    }
    for (const auto &step : kSteps) {
        if (step.scene != current || frames < step.at || frames >= step.at + 6u) continue;
        pad->buttons |= step.buttons;
        if (step.analog >= 0) pad->analog_buttons[step.analog] = 0xff;
    }
    ++frames;
}

static void beginAuto();

static bool beginMatch()
{
    netLive = true;
    netReads = 0;
    sc2_net_live = 1;
    sc2_net_reads = 0;
    netPlayers[0] = netPlayers[1] = RecompInputGamepad{};
    sc2_net_frame = netFrame;
    netVirtualBase = recomp_kernel_interrupt_time();
    recomp_kernel_virtual_time = netVirtualBase;
    recomp_kernel_set_deterministic(true);
    std::vector<std::uint8_t> blob;
    if (netplay->Local() == 0) {
        for (const auto &region : kMatchState) {
            const std::uint8_t *p = recomp_memory(region.address, region.size);
            blob.insert(blob.end(), p, p + region.size);
        }
        const std::uint32_t shown = recomp_d3d8_swap_counter();
        blob.insert(blob.end(), reinterpret_cast<const std::uint8_t *>(&shown), reinterpret_cast<const std::uint8_t *>(&shown) + 4);
    }
    if (!netplay->SyncState(blob)) {
        endMatch();
        return false;
    }
    std::size_t at = 0;
    for (const auto &region : kMatchState) {
        if (at + region.size <= blob.size()) std::memcpy(recomp_memory(region.address, region.size), blob.data() + at, region.size);
        at += region.size;
    }
    if (at + 4u <= blob.size()) {
        std::uint32_t shown;
        std::memcpy(&shown, blob.data() + at, 4u);
        sc2_d3d8_set_swap_counter(shown);
    }
    netRoute = netplay->Local() == 0 && scenePresses.empty(); // (tests script their own way)
    beginAuto();
    std::fprintf(stderr, "online: the match begins at the title screen%s\n", netRoute ? "; on to VS Battle's character select" : "");
    return true;
}

// (the fight chosen in the launcher, by both: on to it by themselves)
static void beginAuto()
{
    autoDirectDone = false;
    netStartWins = winsCounted();
    netOverAt = 0;
    sc2_net_pick[0] = sc2_net_pick[1] = -1;
    netAuto = !netplay->Pick(0).empty() && !netplay->Pick(1).empty();
    if (netAuto) {
        netRoute = false;
        netBeganAt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        netAutoStage = netplay->Stage();
        netAutoMuted = netPreludeMuted >= 0 ? netPreludeMuted != 0 : recomp_audio_output_muted() != 0;
        recomp_audio_output_set_muted(1);
        netAutoCardAt = 0;
        for (int p = 0; p < 2; ++p) {
            const std::string &pick = netplay->Pick(p);
            sc2_net_pick[p] = std::atoi(pick.c_str());
            const std::size_t colon = pick.find(':');
            netAutoOutfit[p] = colon == std::string::npos ? -1 : std::atoi(pick.c_str() + colon + 1);
        }
        mirrorOutfit(sc2_net_pick);
        std::fprintf(stderr, "online: the fight chosen: %s vs %s, stage %d\n", netplay->Pick(0).c_str(),
                     netplay->Pick(1).c_str(), netplay->Stage());
    }
    teamMenuSeen = teamHanded = teamSizeSet = false;
    if (netAuto && netTeam) {
        // (team battle: nothing chosen in the launcher is put in; the players choose in the game)
        sc2_net_pick[0] = sc2_net_pick[1] = -1;
        netAutoOutfit[0] = netAutoOutfit[1] = -1;
        netAutoStage = -1;
        std::fprintf(stderr, "online: team battle: on to VS Team Battle's select\n");
    }
}

// (team battle: VS Team Battle's select current, the controls the players' from this frame on, the same frame on both sides)
static void teamHandOver(std::uint64_t frame)
{
    if (!netTeam || !netAuto || !teamMenuSeen || teamHanded) return;
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    if (current == 6u || *recomp_memory_u32(0x398704u) != 0xFFFFFFFFu || !teamSizeSet) return;
    teamHanded = true;
    netAuto = false;
    recomp_kernel_drain_others();
    sc2::SaveFrame(teamSelectStart);
    teamSelectAt = frame;
    netStartWins = winsCounted();
    launcherFight();
    recomp_audio_output_set_muted(netAutoMuted ? 1 : 0);
    std::fprintf(stderr, "online: team battle: at its select (scene %u, frame %llu, team of %d), the controls are the players'\n", current,
                 (unsigned long long)frame, netTeamSize);
}

// (diagnosis while team battle's end is measured: each scene and each change of the wins, with the fighters' records)
static void teamTrace(std::uint64_t frame)
{
    static std::uint32_t scene = ~0u, wins = ~0u;
    if (!netTeam || !teamHanded) {
        scene = wins = ~0u;
        return;
    }
    const std::uint32_t now = *recomp_memory_u32(0x398700u), counted = winsCounted();
    // (the rounds won of each fighter's record, +0x154C, and its character, +0x58: what a member's KO and the next one change)
    const auto field = [](std::uint32_t p, std::uint32_t at) {
        return *recomp_memory_u32(SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE + SC2_FIGHTER_FIELD(at));
    };
    static std::uint32_t rounds[2] = {~0u, ~0u}, chars[2] = {~0u, ~0u};
    bool changed = now != scene || counted != wins;
    for (std::uint32_t p = 0; p < 2u; ++p) {
        const std::uint32_t r = field(p, 0x154Cu), c = field(p, 0x58u) & 0xffffu;
        if (r != rounds[p] || c != chars[p]) {
            changed = true;
            std::fprintf(stderr, "team trace: frame %llu player %u rounds won %u (to win %u) character %u%c", (unsigned long long)frame,
                         p + 1u, r, field(p, 0x1550u), c, 10);
        }
        rounds[p] = r;
        chars[p] = c;
    }
    if (!changed) return;
    scene = now;
    wins = counted;
    const auto character = [](std::uint32_t p) {
        return *reinterpret_cast<const std::int16_t *>(recomp_memory(SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE + SC2_FIGHTER_FIELD(0x58u), 2u));
    };
    std::fprintf(stderr, "team trace: frame %llu scene %u next %x wins %u %u draws %u; chars %d %d; stage %u%c",
                 (unsigned long long)frame, now, *recomp_memory_u32(0x398704u), winsAt(0x39DE58u), winsAt(0x3AECB8u),
                 winsAt(0x39DE5Au), character(0u), character(1u), *recomp_memory_u32(0x398770u), 10);
}

/* What the screen says of the match: the code to give while hosting, then the ping */
/* The other player's chat taken in, and what is said kept for kChatMs, kChatLines at most */
static void hearChat()
{
    if (!netplay) return;
    const std::uint64_t now = nowMs();
    for (const std::string &line : netplay->TakeHeard()) {
        const std::string name = netplay->Name(1 - netplay->Local());
        std::fprintf(stderr, "online: chat: %s: %s\n", name.empty() ? "(no name)" : name.c_str(), line.c_str());
        netChat.push_back({(name.empty() ? "Them" : name) + ": " + line, now + kChatMs});
    }
    while (!netChat.empty() && (netChat.size() > kChatLines || netChat.front().until <= now))
        netChat.erase(netChat.begin());
}

/* A key's press (held now, not before) */
static bool keyPressed(SDL_Scancode code)
{
    static bool held[SDL_SCANCODE_COUNT];
    const bool now = recomp_input_scancode_held(code);
    const bool pressed = now && !held[code];
    held[code] = now;
    return pressed;
}

/* The match's keys, read at each of controller 1's reads: T opens the chat (letters, digits and space; Backspace;
   Return sends, Esc cancels); Esc asks whether to leave (Return or A: yes; Esc or B: no). The game is given no
   controls of this side's meanwhile (netFrame). */
static void matchKeys()
{
    static const struct { SDL_Scancode code; char lower, upper; } kTyped[] = {
        {SDL_SCANCODE_A, 'a', 'A'}, {SDL_SCANCODE_B, 'b', 'B'}, {SDL_SCANCODE_C, 'c', 'C'}, {SDL_SCANCODE_D, 'd', 'D'},
        {SDL_SCANCODE_E, 'e', 'E'}, {SDL_SCANCODE_F, 'f', 'F'}, {SDL_SCANCODE_G, 'g', 'G'}, {SDL_SCANCODE_H, 'h', 'H'},
        {SDL_SCANCODE_I, 'i', 'I'}, {SDL_SCANCODE_J, 'j', 'J'}, {SDL_SCANCODE_K, 'k', 'K'}, {SDL_SCANCODE_L, 'l', 'L'},
        {SDL_SCANCODE_M, 'm', 'M'}, {SDL_SCANCODE_N, 'n', 'N'}, {SDL_SCANCODE_O, 'o', 'O'}, {SDL_SCANCODE_P, 'p', 'P'},
        {SDL_SCANCODE_Q, 'q', 'Q'}, {SDL_SCANCODE_R, 'r', 'R'}, {SDL_SCANCODE_S, 's', 'S'}, {SDL_SCANCODE_T, 't', 'T'},
        {SDL_SCANCODE_U, 'u', 'U'}, {SDL_SCANCODE_V, 'v', 'V'}, {SDL_SCANCODE_W, 'w', 'W'}, {SDL_SCANCODE_X, 'x', 'X'},
        {SDL_SCANCODE_Y, 'y', 'Y'}, {SDL_SCANCODE_Z, 'z', 'Z'}, {SDL_SCANCODE_1, '1', '!'}, {SDL_SCANCODE_2, '2', '@'},
        {SDL_SCANCODE_3, '3', '#'}, {SDL_SCANCODE_4, '4', '$'}, {SDL_SCANCODE_5, '5', '%'}, {SDL_SCANCODE_6, '6', '^'},
        {SDL_SCANCODE_7, '7', '&'}, {SDL_SCANCODE_8, '8', '*'}, {SDL_SCANCODE_9, '9', '('}, {SDL_SCANCODE_0, '0', ')'},
        {SDL_SCANCODE_SPACE, ' ', ' '}, {SDL_SCANCODE_COMMA, ',', '<'}, {SDL_SCANCODE_PERIOD, '.', '>'},
        {SDL_SCANCODE_SLASH, '/', '?'}, {SDL_SCANCODE_APOSTROPHE, '\'', '"'}, {SDL_SCANCODE_MINUS, '-', '_'},
        {SDL_SCANCODE_SEMICOLON, ';', ':'},
    };
    const uint64_t pad = recomp_input_host_pad_buttons();
    static uint64_t padBefore;
    const auto padPressed = [&](SDL_GamepadButton b) { return (pad >> b & 1u) && !(padBefore >> b & 1u); };
    if (netTyping) {
        const bool shift = recomp_input_scancode_held(SDL_SCANCODE_LSHIFT) || recomp_input_scancode_held(SDL_SCANCODE_RSHIFT);
        for (const auto &key : kTyped)
            if (keyPressed(key.code) && netTyped.size() < sc2::Netplay::kChatMost) netTyped += shift ? key.upper : key.lower;
        if (keyPressed(SDL_SCANCODE_BACKSPACE) && !netTyped.empty()) netTyped.pop_back();
        if (keyPressed(SDL_SCANCODE_RETURN)) {
            if (!netTyped.empty()) {
                netplay->Say(netTyped);
                const std::string name = netplay->Name(netplay->Local());
                netChat.push_back({(name.empty() ? "You" : name) + ": " + netTyped, nowMs() + kChatMs});
            }
            netTyping = false;
        }
        if (keyPressed(SDL_SCANCODE_ESCAPE)) netTyping = false;
    } else if (netLeaving) {
        if (keyPressed(SDL_SCANCODE_RETURN) || padPressed(SDL_GAMEPAD_BUTTON_SOUTH)) {
            netLeaving = false;
            netplay->ended = "You left the match.";
            endMatch();
        } else if (keyPressed(SDL_SCANCODE_ESCAPE) || padPressed(SDL_GAMEPAD_BUTTON_EAST)) {
            netLeaving = false;
        }
    } else {
        const bool chat = keyPressed(SDL_SCANCODE_T); // (before the letters below, T among them, are taken as held)
        for (const auto &key : kTyped) (void)keyPressed(key.code); // (the letters held as the chat opens are not typed)
        if (chat) {
            netTyping = true;
            netTyped.clear();
        } else if (keyPressed(SDL_SCANCODE_ESCAPE) || padPressed(SDL_GAMEPAD_BUTTON_BACK)) {
            netLeaving = true;
        }
    }
    padBefore = pad;
}

/* What the screen says of online play: hosting, the code to give (and that it is in the lobby); then, in the match,
   the chat (its lines, the one being typed) and Esc's question above, and at the bottom the names, the ping and the
   input delay as the Online features have them, or that the other player has gone quiet. */
static void netOverlay()
{
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    if (!netConnected && netWaitingReturn) {
        recomp_overlay_set_title(&text, "ONLINE: WAITING FOR THE OTHER PLAYER TO COME BACK");
        recomp_overlay_add_line(&text, "%s", netEnded.c_str());
        if (!netplay->code.empty()) recomp_overlay_add_line(&text, "Match code: %s", netplay->code.c_str());
        recomp_overlay_set_hint(&text, "They can come back with the same code; the game starts over for the match when they do. "
                                       "Play on meanwhile. The menu's Online page stops hosting.");
    } else if (!netConnected) {
        recomp_overlay_set_title(&text, "ONLINE: HOSTING");
        if (!netplay->code.empty())
            recomp_overlay_add_line(&text, "Match code: %s%s", netplay->code.c_str(), netplay->Listed() ? "  (in the lobby)" : "");
        recomp_overlay_add_line(&text, "%s", netplay->waiting.c_str());
        if (!netplay->Invited().empty()) {
            const std::string &said = netplay->Invitation();
            recomp_overlay_add_line(&text, "%s %s", netplay->Invited().c_str(),
                said == "declined" ? "said no (the code still joins)"
                : said == "away"   ? "couldn't be invited (they left the waiting room)"
                                   : "is invited: waiting for them to accept");
        }
        recomp_overlay_set_hint(&text, netFromMenu ? "Give the other player the code; play on meanwhile. The menu's Online page stops hosting."
                                                   : "Give the other player the code. Esc: the menu (Quit game stops hosting).");
    } else if (!netLive) {
        recomp_overlay_set_hint(&text, "ONLINE: the other player is here; the match begins at the title screen");
    } else if (!netAuto && netOverAt != 0 && launcherMode()) {
        // (the result, the rematch and leaving are the launcher's pages, as Tekken 6 Online's: nothing here)
    } else if (!netAuto && netOverAt != 0) {
        recomp_overlay_set_title(&text, "REMATCH?");
        recomp_overlay_add_line(&text, "A: rematch   B: leave   Y: other fighters");
        for (int p = 0; p < 2; ++p)
            if (netWantsRematch[p])
                recomp_overlay_add_line(&text, "%s wants a rematch",
                                        netplay->Name(p).empty() ? (p == 0 ? "P1" : "P2") : netplay->Name(p).c_str());
    } else if (netAuto) {
        recomp_overlay_set_title(&text, "LOADING THE MATCH");
        recomp_overlay_add_line(&text, "%s vs %s", netplay->Name(0).empty() ? "P1" : netplay->Name(0).c_str(),
                                netplay->Name(1).empty() ? "P2" : netplay->Name(1).c_str());
    } else {
        matchKeys();
        if (!netplay) return; // (left)
        hearChat();
        for (const ChatLine &line : netChat) recomp_overlay_add_line(&text, "%s", line.text.c_str());
        if (netTyping) recomp_overlay_add_line(&text, "> %s_   (Return: send, Esc: cancel)", netTyped.c_str());
        if (netLeaving) recomp_overlay_add_line(&text, "Leave the match?   Return or A: leave   Esc or B: stay");
        std::string hint;
        if (netplay->Silent() > 1000) {
            hint = "WAITING FOR THE OTHER PLAYER";
        } else {
            if (!netShown.hide_names) {
                hint = (netplay->Name(0).empty() ? std::string("P1") : netplay->Name(0)) + " vs " +
                       (netplay->Name(1).empty() ? std::string("P2") : netplay->Name(1));
            }
            char numbers[48] = "";
            if (netShown.ping && netShown.delay) std::snprintf(numbers, sizeof numbers, "PING %d ms   DELAY %d", netplay->Ping(), netplay->Delay());
            else if (netShown.ping) std::snprintf(numbers, sizeof numbers, "PING %d ms", netplay->Ping());
            else if (netShown.delay) std::snprintf(numbers, sizeof numbers, "DELAY %d", netplay->Delay());
            if (*numbers) hint += (hint.empty() ? "" : "   ") + std::string(numbers);
            if (!netTyping && !netLeaving) hint += (hint.empty() ? "" : "   ") + std::string("T: chat");
        }
        // (the bar under a match, names, ping, delay: hidden in this fork, as T6 Online has none; SC2_MATCH_BAR=1
        // shows it, for tests)
        static const bool matchBar = std::getenv("SC2_MATCH_BAR") != nullptr;
        if (matchBar) recomp_overlay_set_hint(&text, hint.c_str());
    }
    recomp_overlay_publish(&text);
}

namespace sc2 {

OnlineShown &OnlineDisplay() { return netShown; }
bool OnlineHosting() { return netplay && !netConnected; }
bool OnlineInMatch() { return netplay && netConnected; }
bool OnlineCanHost() { return !netplay; }
const char *OnlineNote() { return netNote.c_str(); }

void OnlineHost()
{
    if (netplay) return;
    sc2::Netplay::Settings settings = netBase;
    settings.hosting = true;
    settings.listed = !settings.name.empty();
    settings.restartOnHello = true;
    netplay = std::make_unique<sc2::Netplay>(settings);
    std::string error;
    if (!netplay->Begin(error)) {
        netNote = "Hosting did not start: " + error;
        std::fprintf(stderr, "online: %s\n", netNote.c_str());
        netplay.reset();
        return;
    }
    netNote.clear();
    netFromMenu = true;
    netTitleAsked = false;
    netConnected = netLive = false;
    sc2_net_seed = netplay->Seed();
    sc2_net_seeded = 1; // (the sound's random numbers its own from now, as a game started online has them: sc2_hooks.c)
    std::fprintf(stderr, "online: hosting from the menu\n");
}

void StartGameAgain()
{
    if (netplay) {
        netplay->Leave(); // (hosting: the code stops working, as Stop hosting does)
        netplay.reset();
    }
    std::fprintf(stderr, "recomp sc2: the game starts again (Reset game)\n");
    std::vector<std::string> args = argsAgain();
    args.push_back("--started-over"); // (on Windows this copy may still hold the save journal as the new one opens it)
    startAgain(args);
}

void OnlineJoin(const std::string &match)
{
    std::vector<std::string> args = argsAgain();
    args.push_back("--join");
    args.push_back(match);
    if (!netBase.name.empty()) {
        args.push_back("--net-name");
        args.push_back(netBase.name);
    }
    if (netplay) {
        netplay->Leave();
        netplay.reset();
    }
    std::fprintf(stderr, "online: the game starts again to join %s\n", match.c_str());
    startAgain(args);
}

namespace {
std::future<std::vector<OpenMatch>> lobbyFetch;
std::vector<OpenMatch> lobbyMatches;
bool lobbyCame;
}

void OnlineLobbyFetch()
{
    if (lobbyFetch.valid() || !*Netplay::kService) return; // (no outside service: nothing asked)
    lobbyFetch = std::async(std::launch::async, [] {
        std::vector<launcher::OpenMatch> found;
        std::vector<OpenMatch> matches;
        if (launcher::fetch_lobby(std::string(Netplay::kService) + "/v1/lobby/soulcalibur2?protocol=" +
                                      std::to_string(Netplay::kProtocol), found)) {
            for (const launcher::OpenMatch &one : found)
                matches.push_back({one.code, one.name, one.version.empty() ? std::string(SC2_VERSION) : one.version});
        }
        return matches;
    });
}

bool OnlineLobby(std::vector<OpenMatch> &matches)
{
    if (lobbyFetch.valid() && lobbyFetch.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        lobbyMatches = lobbyFetch.get();
        lobbyCame = true;
    }
    matches = lobbyMatches;
    return lobbyCame;
}

void OnlineStopHosting()
{
    if (!netplay || netConnected) return;
    netplay->Leave();
    netplay.reset();
    netFromMenu = netWaitingReturn = false;
    netNote = "Hosting stopped: the code no longer works.";
    std::fprintf(stderr, "online: hosting stopped from the menu\n");
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    recomp_overlay_publish(&text);
}

} // namespace sc2

/* Rollback (netFrame): at the start of every frame (sc2_frame.cpp) the frame's controls are settled and a save state
   taken (sc2_state.h). In a fight, once its loading is done, the game runs on with the other player's controls
   guessed (their last) up to kRollbackFrames ahead of what has come; when theirs come and differ from the guess, the
   frame they were for is put back and the game runs on from there, unshown and unpaced (sc2_fast_forward), to the
   frame it was at. Elsewhere (menus, loading; the other threads busy) each frame waits for the other's controls. */
constexpr unsigned kRollbackFrames = 7, kStates = kRollbackFrames + 2;
constexpr std::uint32_t kNetRounds = 3u; // (rounds to win, online)
constexpr std::uint32_t kFightScene = 7u, kFightSettled = 300u; // (scene 7 for 5 seconds: its loading is done)
static sc2::SaveState netStates[kStates];
static std::uint64_t netStateFrame[kStates];
static std::uint32_t netStateHash[kStates];
static RecompInputGamepad netGuessed[kStates];
static bool netGuess[kStates];
static std::uint64_t netFrameAt, netCheckedUpTo, netResimUntil, netHashedUpTo, netRollbacks, netResimFrames, netSaves;
static std::uint64_t netFightSince = UINT64_MAX;
/* (measurement, SC2_NET_PERF=1: each rollback's cost, from loading the frame to being back at the live frame, per frame
   run again; and each save's) */
static double perfNow()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static double perfRbStart = -1.0, perfRbSum, perfRbMax, perfSaveSum, perfSaveMax, perfLiveLast, perfLiveMax;
extern "C" double sc2_perf_visibility_ms;
extern "C" unsigned long long sc2_perf_visibility_calls;
static double perfLoadSum, perfDrainResim;
static std::uint64_t perfRbFrames, perfRbDone, perfRbFramesAll, perfSaves, perfLive;

static bool samePad(const RecompInputGamepad &a, const RecompInputGamepad &b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}

static double gapWait, gapRb, gapSave;
static std::uint64_t gapRbFrames;
static double perfInside, perfOutside, perfLeftAt = -1.0;
static std::uint64_t perfOutsideFrames;
static bool perfLastResim;

static volatile bool ggInResim; // (the profiler: a frame run again is on)
#if defined(_WIN32)
/* (diagnosis, SC2_PROFILE=FILE: the frame thread's instruction pointer every millisecond while frames run unshown,
   written to FILE as "address count" lines at the match's end; names from llvm-nm on the program) */
static std::atomic<bool> profStarted;
static void profStart()
{
    const char *file = std::getenv("SC2_PROFILE");
    if (!file || profStarted.exchange(true)) return;
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    const std::string path = file;
    std::fprintf(stderr, "profile: started, thread handle %p (error %lu), to %s\n", thread, GetLastError(), file);
    std::thread([thread, path] {
        std::map<std::uint64_t, std::uint32_t> hits;
        std::map<std::vector<std::uint64_t>, std::uint32_t> stacks; // (outermost last)
        std::uint64_t samples = 0;
        if (HMODULE winmm = LoadLibraryA("winmm.dll"))
            if (const auto begin = reinterpret_cast<UINT(WINAPI *)(UINT)>(GetProcAddress(winmm, "timeBeginPeriod"))) begin(1);
        for (;;) {
            Sleep(1);
            static const bool all = std::getenv("SC2_PROFILE_ALL") != nullptr; // (the shown frames too)
            if (!sc2_fast_forward && !all && !ggInResim) continue;
            if (sc2_net_boundary && !all && !ggInResim) continue; // (online: the frames run again only)
            if (SuspendThread(thread) == DWORD(-1)) {
                std::fprintf(stderr, "profile: SuspendThread failed (%lu)\n", GetLastError());
                break;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            std::uint64_t frames[96];
            unsigned depth = 0;
            const bool got = GetThreadContext(thread, &context) != 0;
            const std::uint64_t rip = context.Rip;
            // (the thread's own stack, unwound while it stands; nothing here allocates)
            while (got && depth < 96u && context.Rip) {
                frames[depth++] = context.Rip;
                DWORD64 image = 0;
                const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
                if (!f) {
                    context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                    context.Rsp += 8;
                    if (depth > 1u) break;
                    continue;
                }
                PVOID handler = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, f, &context, &handler, &establisher, nullptr);
            }
            ResumeThread(thread);
            if (got) {
                ++hits[rip];
                ++stacks[std::vector<std::uint64_t>(frames, frames + depth)];
            }
            if (++samples % 200u == 0u) {
                std::fprintf(stderr, "profile: %llu samples\n", (unsigned long long)samples);
                if (FILE *out = std::fopen(path.c_str(), "w")) {
                    std::fprintf(out, "base %llx\n", (unsigned long long)(std::uintptr_t)GetModuleHandleW(nullptr));
                    for (const auto &hit : hits) std::fprintf(out, "%llx %u\n", (unsigned long long)hit.first, hit.second);
                    std::fclose(out);
                }
                if (FILE *out = std::fopen((path + ".stacks").c_str(), "w")) {
                    std::fprintf(out, "base %llx\n", (unsigned long long)(std::uintptr_t)GetModuleHandleW(nullptr));
                    for (const auto &stack : stacks) {
                        std::fprintf(out, "%u", stack.second);
                        for (const std::uint64_t at : stack.first) std::fprintf(out, " %llx", (unsigned long long)at);
                        std::fputc(10, out);
                    }
                    std::fclose(out);
                }
            }
        }
    }).detach();
}
#else
static void profStart() {}
#endif

extern "C" void recomp_screenshot_request(void);
static void netFrameBody();
static void netFrame()
{
    profStart();
    // (measurement: of a frame run again, the time in here and the time the game's own frame took)
    const double enter = perfNow();
    if (perfLastResim && perfLeftAt >= 0.0) {
        perfOutside += enter - perfLeftAt;
        ++perfOutsideFrames;
    }
    netFrameBody();
    perfLeftAt = perfNow();
    perfLastResim = sc2_fast_forward != 0;
    if (perfLastResim) perfInside += perfLeftAt - enter;
}

static RecompInputGamepad netMine(std::uint64_t frame, bool over)
{
    (void)frame;
    RecompInputGamepad mine{};
    recomp_input_pulse_source_sample(&inputPulses, &mine);
    if (!scenePresses.empty()) applyScenePresses(&mine);
    if (netRoute) routeToSelect(&mine);
    if (static const char *mash = std::getenv("SC2_TEST_MASH"); mash) {
        // (tests: past the title, A every 40 frames in the boot's questions and the character select)
        static bool titleSeen;
        const std::uint32_t at = *recomp_memory_u32(0x398700u);
        titleSeen |= at == 5u || teamHanded; // (team battle: its select after the meeting)
        // (the boot's "no save data, create it?": cursor to Yes first (up and left), then A)
        if (at == 34u && frame % 40u < 4u) mine.buttons |= 0x1u | 0x4u;
        if (at == 34u && frame % 40u >= 20u && frame % 40u < 24u) mine.analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        // (team battle: counted from its select's start, so a rematch's select gets the same presses as the first)
        const std::uint64_t t = teamHanded && frame >= teamSelectAt ? frame - teamSelectAt : frame;
        if (titleSeen && at == 10u && !teamHanded && t % 40u < 4u) mine.analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        if (teamHanded && at == 10u) {
            // (team battle: the cursor goes back to its first fighter after each pick, and a fighter is taken at most twice:
            // in the Nth window of 160 frames N steps to the right, then A, so every pick is another fighter)
            const std::uint64_t n = t / 160u, in = t % 160u;
            if (in < 8u * n && in % 8u < 3u) mine.buttons |= 0x8u;
            if (in >= 150u && in < 154u) mine.analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
        }
    }
    if (static const char *mash = std::getenv("SC2_TEST_MASH"); mash && !over && *recomp_memory_u32(0x398700u) == kFightScene) {
        // (tests: in a fight, controls that change every few frames, from a seed: rollbacks to measure)
        static std::uint32_t rng = std::strtoul(mash, nullptr, 10) * 2654435761u + 1u, held;
        static unsigned left;
        if (left == 0u) {
            rng = rng * 1664525u + 1013904223u;
            held = rng >> 8;
            left = 3u + (rng >> 28);
        }
        --left;
        mine.buttons |= held & 0xfu;
        static const int kButtons[] = {RECOMP_INPUT_ANALOG_A, RECOMP_INPUT_ANALOG_B, RECOMP_INPUT_ANALOG_X,
                                       RECOMP_INPUT_ANALOG_Y};
        for (int b = 0; b < 4; ++b)
            if (held & (0x10u << b)) mine.analog_buttons[kButtons[b]] = 0xff;
    }
    if (netTyping || netLeaving) mine = RecompInputGamepad{}; // (the keys are the chat's, or the question's)
    if (netAuto) {
        autoPads();
        mine = netAutoPads[netplay->Local()];
    }
    if (launcherMode() && over && !netAuto) {
        // (after the match the launcher has the controller; its "rematch" is this side's A)
        mine = RecompInputGamepad{};
        if (launcherRematch) mine.analog_buttons[RECOMP_INPUT_ANALOG_A] = 0xff;
    }
    return mine;
}

// A frame's work before the game runs it, live or again: from the game's own state and both players' controls only
static void netWork(std::uint64_t frame, bool live, bool over)
{
    if (over && live && launcherMode()) launcherOver(frame);
    if (over) {
        if (live && netOverAt != 0 && frame >= netOverAt + 60u) {
            static const bool testRematch = std::getenv("SC2_TEST_REMATCH") != nullptr; // (tests: both choose A)
            for (int p = 0; p < 2; ++p) {
                const RecompInputGamepad &pad = netPlayers[p];
                if (pad.analog_buttons[RECOMP_INPUT_ANALOG_A] || (testRematch && frame >= netOverAt + 120u))
                    netWantsRematch[p] = true;
                if (launcherMode()) continue; // (leaving and other fighters: the launcher's own pages)
                if (pad.analog_buttons[RECOMP_INPUT_ANALOG_B]) sessionOver(kExitLeave, p == netplay->Local() ? "this player left" : "the other player left");
                if (pad.analog_buttons[RECOMP_INPUT_ANALOG_Y]) sessionOver(kExitChange, "other fighters, in the launcher");
            }
            if (netWantsRematch[0] && netWantsRematch[1]) {
                std::fprintf(stderr, "online: rematch (frame %llu)\n", (unsigned long long)frame);
                static const bool reload = std::getenv("SC2_REMATCH_RELOAD") != nullptr; // (the way through the select)
                if (netTeam && teamSelectStart.valid) {
                    // (team battle: back to its select, the teams chosen again)
                    std::uint16_t wins[3];
                    const std::uint32_t at[3] = {0x39DE58u, 0x3AECB8u, 0x39DE5Au};
                    for (int i = 0; i < 3; ++i) std::memcpy(&wins[i], recomp_memory(at[i], 2u), 2u);
                    sc2::LoadFrame(teamSelectStart);
                    for (int i = 0; i < 3; ++i) std::memcpy(recomp_memory(at[i], 2u), &wins[i], 2u);
                    netStartWins = winsCounted();
                    netOverAt = 0;
                    netWantsRematch[0] = netWantsRematch[1] = false;
                    launcherFight();
                    teamSelectAt = frame;
                    std::fprintf(stderr, "online: team battle: at its select again (frame %llu)\n", (unsigned long long)frame);
                } else if (netFightStart.valid && !reload) {
                    std::uint16_t wins[3];
                    const std::uint32_t at[3] = {0x39DE58u, 0x3AECB8u, 0x39DE5Au};
                    for (int i = 0; i < 3; ++i) std::memcpy(&wins[i], recomp_memory(at[i], 2u), 2u);
                    sc2::LoadFrame(netFightStart);
                    for (int i = 0; i < 3; ++i) std::memcpy(recomp_memory(at[i], 2u), &wins[i], 2u);
                    netStartWins = winsCounted();
                    netOverAt = 0;
                    netWantsRematch[0] = netWantsRematch[1] = false;
                    launcherFight();
                    std::fprintf(stderr, "online: at the fight again (frame %llu)\n", (unsigned long long)frame);
                } else {
                    autoRestart();
                    netAuto = true;
                    recomp_audio_output_set_muted(1);
                    netBeganAt = perfNow();
                }
            }
        }
        if (!netAuto) netPlayers[0] = netPlayers[1] = RecompInputGamepad{};
    }
    {
        // (diagnosis: the scene by the match's frames; SC2_TEST_NET_KO=1 empties player 2's health every 240 frames of
        // a fight, the same on both sides, so fights end)
        static std::uint32_t loggedScene = ~0u;
        const std::uint32_t scene = *recomp_memory_u32(0x398700u);
        if (live && scene != loggedScene) {
            loggedScene = scene;
            std::fprintf(stderr, "online: scene %u at frame %llu\n", scene, (unsigned long long)frame);
        }
        static const bool ko = std::getenv("SC2_TEST_NET_KO") != nullptr;
        if (ko && scene == kFightScene && frame % 240u == 0u) {
            const float zero = 0.0f;
            std::memcpy(recomp_memory(SC2_FIGHTER_RECORD + 0x155cu + SC2_FIGHTER_STRIDE, 4u), &zero, 4u);
        }
    }
    /* Three rounds won take the match, whatever the save's options say (VS Battle's own two): each fighter's record keeps
       the rounds to win at +0x1550, which the round's end compares its rounds won (+0x154C) with (0x38B30). Written at a
       frame's start from the game's own state, so alike on both sides and in frames run again. */
    if (*recomp_memory_u32(0x398700u) == kFightScene && !netTeam) // (team battle: its own rounds)
        for (std::uint32_t p = 0; p < 2u; ++p) {
            std::uint32_t *rounds = recomp_memory_u32(SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE + SC2_FIGHTER_FIELD(0x1550u));
            if (*rounds == 0u || *rounds == kNetRounds) continue;
            *rounds = kNetRounds;
            // (the life bars' marks, set from it as the fight was set up (0x36670): again, 0xD17F0(player, rounds), thiscall
            // of the bars' object [0x398044])
            if (const std::uint32_t bars = *recomp_memory_u32(0x398044u)) {
                const RecompRegisters saved = recomp_runtime.registers;
                RecompFpuContext fpu;
                recomp_fpu_context_save(&fpu);
                recomp_runtime.registers.ecx = bars;
                const std::uint32_t args[2] = {p, kNetRounds};
                kernel_call_guest(0x000D17F0u, args, 2u);
                recomp_fpu_context_restore(&fpu);
                recomp_runtime.registers = saved;
            }
        }
    sc2_net_reads = frame;
    recomp_kernel_virtual_time = netVirtualBase + frame * 10000000ull / 60u;
    sc2_fast_forward = !live;
    static const bool showAuto = std::getenv("SC2_TEST_SHOW_AUTO") != nullptr; // (tests: the way shown)
    sc2_hide_frames = netAuto && !showAuto;
    if (netAuto && live) autoCard(frame);
}

static void netDumps(std::uint64_t frame)
{
    static const char *const netDump = std::getenv("SC2_NET_DUMP"); // (read once: getenv each frame showed in profiles)
    if (const char *dump = netDump) {
        // (tests: SC2_NET_DUMP=FRAME[,FRAME...]:FILE saves guest RAM at the start of those frames)
        const char *file = std::strchr(dump, ':');
        bool wanted = false;
        for (const char *at = dump; at && at < file; at = std::strchr(at, ',') ? std::strchr(at, ',') + 1 : nullptr)
            wanted |= std::strtoull(at, nullptr, 10) == frame;
        if (file && wanted) {
            // (each side its own file, FILE.FRAME.p0 or .p1: two copies on one machine; the frame's last run wins, a
            // rollback's after the one on a guess)
            const std::string path = std::string(file + 1) + "." + std::to_string(frame) + ".p" +
                                     std::to_string(netplay->Local());
            if (FILE *out = std::fopen(path.c_str(), "wb")) {
                std::fwrite(recomp_memory(0u, RECOMP_XBOX_RAM_SIZE), 1, RECOMP_XBOX_RAM_SIZE, out);
                std::fclose(out);
            }
        }
    }
}

static void netShots(std::uint64_t frame)
{
    // (tests: SC2_TEST_SHOT_AT=FRAME, the game's own screenshot at that frame of the match)
    static const char *shotAt = std::getenv("SC2_TEST_SHOT_AT");
    // (FRAME,FRAME,...: several)
    for (const char *at = shotAt; at && *at; at = std::strchr(at, ',') ? std::strchr(at, ',') + 1 : nullptr)
        if (std::strtoull(at, nullptr, 10) == frame) recomp_screenshot_request();
}

static void netAutoEnds(std::uint64_t frame)
{
    if (netAuto && *recomp_memory_u32(0x398700u) == kFightScene && autoSelectSeen) {
        netAuto = false;
        launcherFight();
        recomp_audio_output_set_muted(netAutoMuted ? 1 : 0);
        // (what the fight was set up with: each player's record [0x398A08], [0x398A04], its character +0x58 and outfit
        // +0x64, and the stage the fight setup keeps at 0x398050)
        for (std::uint32_t p = 0; p < 2u; ++p) {
            const std::uint32_t record = SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE;
            std::fprintf(stderr, "online: player %u at the fight: character %d, outfit %d\n", p,
                         *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x58u), 2u)),
                         *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x64u), 2u)));
        }
        std::fprintf(stderr, "online: stage at the fight %u\n", *recomp_memory_u32(0x398770u));
        // (diagnosis: the fight setup's globals, 0x3BED0 writes the players and the stage there; as 16-bit words)
        std::fprintf(stderr, "online: fight globals 398020:");
        for (std::uint32_t a = 0x398020u; a < 0x398070u; a += 2u)
            std::fprintf(stderr, " %d", *reinterpret_cast<const std::int16_t *>(recomp_memory(a, 2u)));
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "online: at the fight (frame %llu, %.1f s after the match began): controls over the network "
                     "from here\n", (unsigned long long)frame, (perfNow() - netBeganAt) / 1000.0);
    }
}

static void netFrameBody()
{
    if (!netplay || !netLive) return;
    std::uint64_t frame = netFrameAt;
    if (!netplay->Poll()) {
        endMatch();
        return;
    }
    // (a guess that the other's controls have since shown wrong: back to that frame)
    const std::uint64_t known = std::min(netplay->RemoteUpTo(), frame);
    for (std::uint64_t at = netCheckedUpTo; at < known; ++at) {
        const unsigned slot = at % kStates;
        if (!netGuess[slot] || samePad(netplay->RemoteAt(at), netGuessed[slot])) continue;
        if (netStateFrame[slot] != at || !netStates[slot].valid || (sc2::RamTracked() && !sc2::RamLoad(at))) {
            netplay->ended = "The match could not go back to a frame (rollback).";
            netplay->failed = true;
            endMatch();
            return;
        }
        if (perfRbStart < 0.0) perfRbStart = perfNow();
        perfRbFrames += frame - at;
        const double loadAt = perfNow();
        sc2::LoadFrame(netStates[slot], !sc2::RamTracked());
        perfLoadSum += perfNow() - loadAt;
        netResimUntil = std::max(netResimUntil, frame);
        netResimFrames += frame - at;
        ++netRollbacks;
        frame = at;
        break;
    }
    netCheckedUpTo = std::min(netplay->RemoteUpTo(), frame);
    const bool live = frame >= netResimUntil;
    if (live && perfRbStart >= 0.0) {
        const double perFrame = (perfNow() - perfRbStart) / double(perfRbFrames ? perfRbFrames : 1u);
        perfRbSum += perfNow() - perfRbStart;
        perfRbFramesAll += perfRbFrames;
        perfRbMax = std::max(perfRbMax, perFrame);
        ++perfRbDone;
        perfRbStart = -1.0;
        perfRbFrames = 0;
    }
    if (live) {
        // (time between live frames: the pace the player sees)
        const double t = perfNow();
        if (perfLiveLast > 0.0) perfLiveMax = std::max(perfLiveMax, t - perfLiveLast);
        // (SC2_NET_GAPS=MS: each live frame that came later than that after the one before, and what took the time)
        static const char *gapsText = std::getenv("SC2_NET_GAPS");
        static const double gapsMs = gapsText ? std::atof(gapsText) : 0.0;
        if (gapsMs > 0.0 && perfLiveLast > 0.0 && t - perfLiveLast > gapsMs && !netAuto)
            std::fprintf(stderr, "online gap frame %llu: %.1f ms: waited %.1f, run again %.1f (%llu frames), saves %.1f\n",
                         (unsigned long long)frame, t - perfLiveLast, gapWait, perfRbSum - gapRb,
                         (unsigned long long)(perfRbFramesAll - gapRbFrames), perfSaveSum - gapSave);
        gapWait = 0.0;
        gapRb = perfRbSum;
        gapRbFrames = perfRbFramesAll;
        gapSave = perfSaveSum;
        perfLiveLast = t;
        ++perfLive;
        static const bool report = std::getenv("SC2_NET_PERF") != nullptr;
        if (report && perfLive % 600u == 0u) {
            std::fprintf(stderr, "online perf frame %llu: scene %u, rollbacks %llu (%llu frames again), %.3f ms per frame again "
                "(max %.3f), save %.3f ms (max %.3f, %llu saves), slowest live frame gap %.1f ms; per frame again: load %.3f, drain %.3f, visibility %.3f ms (%llu); in netFrame %.3f, game frame %.3f ms\n",
                (unsigned long long)frame, *recomp_memory_u32(0x398700u), (unsigned long long)perfRbDone,
                (unsigned long long)perfRbFramesAll, perfRbFramesAll ? perfRbSum / double(perfRbFramesAll) : 0.0, perfRbMax,
                perfSaves ? perfSaveSum / double(perfSaves) : 0.0, perfSaveMax, (unsigned long long)perfSaves, perfLiveMax,
                perfRbFramesAll ? perfLoadSum / double(perfRbFramesAll) : 0.0,
                perfRbFramesAll ? perfDrainResim / double(perfRbFramesAll) : 0.0,
                perfRbFramesAll ? sc2_perf_visibility_ms / double(perfRbFramesAll) : 0.0, sc2_perf_visibility_calls,
                perfOutsideFrames ? perfInside / double(perfOutsideFrames) : 0.0,
                perfOutsideFrames ? perfOutside / double(perfOutsideFrames) : 0.0);
            perfLiveMax = 0.0;
        }
    }
    // (how far ahead of the other's controls this frame may run)
    /* (a fight, with no next scene asked for: once one is, its loading may begin on the other threads, which a frame
       put back would not undo; from then each frame waits, and the fight's next rollback needs the next scene's) */
    const bool switching = *recomp_memory_u32(0x398704u) != 0xFFFFFFFFu;
    const bool fight = *recomp_memory_u32(0x398700u) == kFightScene && !switching;
    if (!fight) netFightSince = UINT64_MAX;
    else if (netFightSince == UINT64_MAX) netFightSince = frame;
    // (the fight loaded, its intros to come: kept for a rematch, which then shows no loading; taken once a session, on
    // a frame both sides run alike, live; the state the rollback's own saves take from then on)
    if (fight && !netFightStart.valid && live && !netAuto && frame == netFightSince + kFightSettled) {
        sc2::SaveFrame(netFightStart);
        std::fprintf(stderr, "online: the fight's start kept for a rematch (frame %llu)\n", (unsigned long long)frame);
    }
    // (the match over, its wins counted: from then on each frame waits, so the choice after it is read alike)
    const bool over = !netAuto && winsCounted() != netStartWins;
    if (over && (netOverAt == 0 || frame < netOverAt)) netOverAt = frame;
    if (!over && netOverAt != 0 && frame <= netOverAt) netOverAt = 0;
    const std::uint64_t ahead = fight && !over && frame >= netFightSince + kFightSettled ? kRollbackFrames : 0u;
    netAutoEnds(frame);
    // (on the way to the chosen fight neither side needs the other's controls: up to a second apart)
    if (live && launcherMode() && frame % 6u == 0u) launcherPoll();
    const std::uint64_t waitAhead = netAuto ? 60u : ahead;
    if (live) {
        const double waitAt = perfNow();
        if (netplay->RemoteUpTo() + waitAhead <= frame && !netplay->WaitFor(frame + 1u - waitAhead)) {
            endMatch();
            return;
        }
        gapWait += perfNow() - waitAt;
        const RecompInputGamepad mine = netMine(frame, over);
        netplay->Post(frame, mine);
    }
    const unsigned slot = frame % kStates;
    netGuess[slot] = !netAuto && netplay->RemoteUpTo() <= frame;
    netGuessed[slot] = netAuto ? netAutoPads[1 - netplay->Local()] : !netGuess[slot] ? netplay->RemoteAt(frame)
        : netplay->RemoteUpTo() ? netplay->RemoteAt(netplay->RemoteUpTo() - 1u) : RecompInputGamepad{};
    /* (the other threads run until each waits, once the frame's controls are in: after any wait for them, so the
       kernel's fallback for a thread spinning on the others, by the host's time, never mistakes that wait) */
    {
        const double drainAt = perfNow();
        recomp_kernel_drain_others();
        if (!live) perfDrainResim += perfNow() - drainAt;
    }
    netPlayers[netplay->Local()] = netplay->LocalAt(frame);
    netPlayers[1 - netplay->Local()] = netGuessed[slot];
    if (netAuto) {
        // (both from the script, this frame's: not this side's own as held back by the delay)
        netPlayers[0] = netAutoPads[0];
        netPlayers[1] = netAutoPads[1];
    }
    netWork(frame, live, over);
    /* (a frame is gone back to only when it ran on a guess of the other's controls that proved wrong: only those
       frames are saved, the whole of guest RAM each (~5 ms on a Steam Deck); a frame whose controls had come runs
       once) */
    if (!netGuess[slot] && fight && sc2::RamTracked() && frame < netFightSince + kFightSettled && frame % 30u == 0u)
        sc2::RamSave(frame); // (so the fight's first save has little left to copy: the first of all copies the whole RAM)
    if (netGuess[slot]) {
        static const bool dirtyLog = std::getenv("SC2_NET_DIRTY") != nullptr;
        const unsigned prev = (frame + kStates - 1u) % kStates;
        if (dirtyLog && netStateFrame[prev] == frame - 1u && netStates[prev].valid && !netStates[prev].ram.empty()) {
            // (measurement: 4 KB pages changed since the frame before, and in which 1 MB of guest RAM)
            static std::uint64_t frames, pages, perMb[64];
            const std::uint8_t *now = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE), *was = netStates[prev].ram.data();
            for (std::uint32_t at = 0; at < RECOMP_XBOX_RAM_SIZE; at += 4096u)
                if (std::memcmp(now + at, was + at, 4096u) != 0) { ++pages; ++perMb[at >> 20]; }
            if (++frames % 300u == 0u) {
                std::fprintf(stderr, "online dirty: %.1f pages a frame (%.0f KB); by MB:", double(pages) / double(frames),
                    double(pages) * 4.0 / double(frames));
                for (int mb = 0; mb < 64; ++mb)
                    if (perMb[mb]) std::fprintf(stderr, " %d:%.1f", mb, double(perMb[mb]) / double(frames));
                std::fprintf(stderr, "\n");
            }
        }
        const double saveAt = perfNow();
        sc2::SaveFrame(netStates[slot], !sc2::RamTracked());
        if (sc2::RamTracked()) sc2::RamSave(frame);
        const double saveMs = perfNow() - saveAt;
        perfSaveSum += saveMs;
        perfSaveMax = std::max(perfSaveMax, saveMs);
        ++perfSaves;
        ++netSaves;
    } else {
        netStates[slot].valid = false;
    }
    netStateFrame[slot] = frame;
    netStateHash[slot] = matchHash();
    netDumps(frame);
    // (the hashes of frames whose controls are all known and checked against the guesses)
    for (std::uint64_t at = netHashedUpTo + sc2::Netplay::kHashEvery; at <= std::min<std::uint64_t>(frame, netCheckedUpTo);
         at += sc2::Netplay::kHashEvery) {
        if (netStateFrame[at % kStates] != at) continue;
        netplay->Hash(at, netStateHash[at % kStates]);
        netHashedUpTo = at;
        static const bool hashLog = std::getenv("SC2_NET_HASHLOG") != nullptr;
        if (hashLog)
            std::fprintf(stderr, "online hash %llu: %08x (rollbacks %llu, frames again %llu, saved %llu)\n",
                (unsigned long long)at, netStateHash[at % kStates], (unsigned long long)netRollbacks,
                (unsigned long long)netResimFrames, (unsigned long long)netSaves);
    }
    if (live) {
        netShots(frame);
        // (tests: SC2_NET_SAY=FRAME:TEXT says TEXT in the chat at that frame of the match)
        static const char *say = std::getenv("SC2_NET_SAY");
        if (say && std::strtoull(say, nullptr, 10) == frame && std::strchr(say, ':')) {
            netplay->Say(std::strchr(say, ':') + 1);
            netChat.push_back({std::string("You: ") + (std::strchr(say, ':') + 1), nowMs() + kChatMs});
        }
    }
    netFrameAt = frame + 1u;
}

/* ================= The online session's rollback: Tekken 6 Online's (T6NetFrame, t6_session.cpp; GGPO as it has it).
   The boundary is before each frame of the game's main loop (sc2_net_boundary, 0x2DD40). In order, as there:
   the frame that ran is counted and its state saved (and what came meanwhile rolled back); the frame clock's wait; the
   mode (lockstep with a longer input delay outside the fight, rollback in it); this side's controls, read as late as
   that; the wait for the other side when the prediction is full, taken off the clock; the pace of the two sides kept
   together (the share of predicted frames of each, whole frames before the fight). States are kept by the pages
   written (sc2_state.h); inside a rollback only those that can be gone back to (g_ggpo_save_confirmed). */
extern int g_ggpo_predicted, g_ggpo_local_late, g_ggpo_remote_late, g_ggpo_save_confirmed, g_ggpo_late_save, g_ggpo_network_delay;
extern unsigned int g_ggpo_local_hash_frame, g_ggpo_local_hash, g_ggpo_remote_hash_frame, g_ggpo_remote_hash;
extern "C" void sub_0002DD40(void);
extern "C" void sub_000F0EC0(void);

static void netNoFrame() {}

static GGPOSession *gg;
static GGPOPlayerHandle ggLocal, ggRemote;
static bool ggRunning, ggGone, ggPending, ggSaveWanted = true, ggMissing;
static std::uint64_t ggFrame;
static unsigned ggSession; // (one more with each session: what a function keeps of the last one starts again)
static std::map<std::uint64_t, std::uint32_t> ggHashes;
static int ggMode, ggCurDelay = -1, ggAhead;          // (mode: 1 lockstep, -1 rollback)
static std::uint64_t ggGraceUntil;                    // (after the fight's end: prediction kept until that frame)
constexpr unsigned kGgRing = 8;                       // (MAX_PREDICTION_FRAMES 6 + 2)
static sc2::SaveState ggStates[kGgRing];
static std::uint64_t ggStateFrame[kGgRing];
constexpr int kGgPad = 14;
// (what is measured, per 600 live frames, as T6's window lines)
static std::uint64_t ggLoads, ggResim, ggSaves, ggSavesLeft, ggLateFrames, ggOver17, ggOver25, ggWindow;
static double ggStepMs, ggLoadMs, ggSaveMs, ggSaveMax, ggRbMs, ggRbMax, ggRbCur, ggRbMark, ggWaitMs, ggPeriodMax, ggLiveMs, ggSlackMs;
static double ggClockPush, ggPhaseShift;

static void ggPack(const RecompInputGamepad &pad, std::uint8_t *to)
{
    to[0] = std::uint8_t(pad.buttons);
    to[1] = std::uint8_t(pad.buttons >> 8);
    std::memcpy(to + 2, pad.analog_buttons, 8u);
    to[10] = std::uint8_t(pad.thumb_lx >> 8);
    to[11] = std::uint8_t(pad.thumb_ly >> 8);
    to[12] = std::uint8_t(pad.thumb_rx >> 8);
    to[13] = std::uint8_t(pad.thumb_ry >> 8);
}

static RecompInputGamepad ggUnpack(const std::uint8_t *from)
{
    RecompInputGamepad pad{};
    pad.buttons = std::uint16_t(from[0] | from[1] << 8);
    std::memcpy(pad.analog_buttons, from + 2, 8u);
    const auto stick = [](std::uint8_t v) { return std::int16_t(std::int8_t(v) * 256); };
    pad.thumb_lx = stick(from[10]);
    pad.thumb_ly = stick(from[11]);
    pad.thumb_rx = stick(from[12]);
    pad.thumb_ry = stick(from[13]);
    return pad;
}

static void ggSync()
{
    std::uint8_t pads[2 * kGgPad] = {};
    int gone = 0;
    if (!GGPO_SUCCEEDED(ggpo_synchronize_input(gg, pads, sizeof pads, &gone))) return;
    netPlayers[0] = ggUnpack(pads);
    netPlayers[1] = ggUnpack(pads + kGgPad);
}

// (team battle too: its wins are counted once a whole team is beaten, not at a member's KO (measured, teamTrace))
static bool ggOverNow() { return !netAuto && winsCounted() != netStartWins; }

/* Team battle: a member beaten counts a round won of the other's record (+0x154C; to win +0x1550 = the team's size), and
   the next member comes in later in the same fight scene, its character and outfit written to the record (+0x58,
   +0x64) and its data read from the disc then (measured 2026-11-25: KO frame 3361, next member 3930, ~5 MB read right
   after). Read in a rollback, run again, those loads parted the two games; so from the KO the frames run in lockstep
   until the next member has stood kFightSettled frames, as the fight's own start (netFightSince). From the game's state
   only, alike on both sides. */
static bool teamGap;
static void teamMembers(bool fight, std::uint64_t frame)
{
    static bool known;
    static std::uint32_t rounds[2], member[2];
    if (!netTeam || !fight) {
        known = teamGap = false;
        return;
    }
    std::uint32_t r[2], m[2];
    for (std::uint32_t p = 0; p < 2u; ++p) {
        const std::uint32_t record = SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE;
        // (the low 16 bits: the rounds won, one more at each KO; the high 16 bits change too, ~20 frames after the next
        // member is in, which is no KO)
        r[p] = *recomp_memory_u32(record + SC2_FIGHTER_FIELD(0x154Cu)) & 0xffffu;
        m[p] = (*reinterpret_cast<const std::uint16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x58u), 2u))) |
               std::uint32_t(*reinterpret_cast<const std::uint16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x64u), 2u))) << 16;
    }
    if (known && (r[0] != rounds[0] || r[1] != rounds[1]) && !teamGap) {
        teamGap = true;
        std::fprintf(stderr, "online: team battle: a member beaten (frame %llu): lockstep until the next one stands%c", (unsigned long long)frame, 10);
    }
    if (known && teamGap && (m[0] != member[0] || m[1] != member[1])) {
        teamGap = false;
        netFightSince = frame;
        std::fprintf(stderr, "online: team battle: the next member in (frame %llu)%c", (unsigned long long)frame, 10);
    }
    known = true;
    for (int p = 0; p < 2; ++p) {
        rounds[p] = r[p];
        member[p] = m[p];
    }
}

// The mode the frame at this boundary runs in: rollback in the loaded fight only (the state decides, alike on both sides)
static int ggWant()
{
    const bool fight = *recomp_memory_u32(0x398700u) == kFightScene && *recomp_memory_u32(0x398704u) == 0xFFFFFFFFu;
    return fight && !netAuto && !ggOverNow() && !teamGap && netFightSince != UINT64_MAX && ggFrame >= netFightSince + kFightSettled ? -1 : 1;
}

// A frame's work before the game runs it, live or again
static void ggRunFrame();
/* (measurement, SC2_BENCH=1: at the fight's kept start, 120 frames run as frames run again (unshown, the same controls
   every time) from that one state, 7 times; the fastest and the median ms a frame, then the program ends. The same
   work every run: a change to the code is measured without the fight's own changes.) */
static void ggBench(std::uint64_t frame)
{
    sc2::SaveState start;
    sc2::SaveFrame(start);
    std::vector<double> runs;
    std::uint32_t hash = 0;
    for (int rep = 0; rep < 7; ++rep) {
        sc2::LoadFrame(start);
        recomp_kernel_drain_others();
        const double at = perfNow();
        for (int k = 0; k < 120; ++k) {
            netPlayers[0] = netPlayers[1] = RecompInputGamepad{};
            netPlayers[k / 8 % 2].buttons = std::uint16_t(1u << (k / 16 % 4)); // (a few directions, alike every time)
            sc2_net_reads = frame + k;
            recomp_kernel_virtual_time = netVirtualBase + (frame + k) * 10000000ull / 60u;
            static const bool live = std::getenv("SC2_BENCH") && std::getenv("SC2_BENCH")[0] == 'l'; // (SC2_BENCH=live: shown frames)
            sc2_fast_forward = live ? 0 : 1;
            sc2_audio_quiet = 1;
            ggInResim = true;
            ggRunFrame();
        }
        runs.push_back((perfNow() - at) / 120.0);
        const std::uint32_t h = matchHash();
        if (rep == 0) hash = h;
        else if (h != hash) std::fprintf(stderr, "bench: run %d ended in another state (%08x, not %08x)\n", rep, h, hash);
    }
    std::sort(runs.begin(), runs.end());
    std::fprintf(stderr, "bench: 120 frames run again x7: fastest %.3f ms a frame, median %.3f, slowest %.3f (state %08x)\n",
                 runs.front(), runs[runs.size() / 2], runs.back(), hash);
    std::fflush(stderr);
    std::_Exit(0);
}

static void ggPrepare(bool live)
{
    const std::uint64_t frame = ggFrame;
    if (live) sc2_audio_quiet = netAuto && !std::getenv("SC2_AUTO_PACED") ? 1 : 0;
    recomp_kernel_drain_others();
    const bool fight = *recomp_memory_u32(0x398700u) == kFightScene && *recomp_memory_u32(0x398704u) == 0xFFFFFFFFu;
    if (!fight) netFightSince = UINT64_MAX;
    else if (netFightSince == UINT64_MAX || netFightSince > frame) netFightSince = frame;
    if (live || !fight) teamMembers(fight, frame); // (live frames only: what a frame run again shows was seen live first or is seen at the next)
    if (fight && !netFightStart.valid && live && !netAuto && frame == netFightSince + kFightSettled) {
        sc2::SaveFrame(netFightStart);
        std::fprintf(stderr, "online: the fight's start kept for a rematch (frame %llu)\n", (unsigned long long)frame);
        static const bool bench = std::getenv("SC2_BENCH") != nullptr;
        if (bench) ggBench(frame);
    }
    const bool over = ggOverNow();
    if (over && (netOverAt == 0 || frame < netOverAt)) netOverAt = frame;
    if (!over && netOverAt != 0 && frame <= netOverAt) netOverAt = 0;
    if (live) teamHandOver(frame);
    if (live) teamTrace(frame);
    if (live) netAutoEnds(frame);
    if (netAuto) {
        // (both players' controls from the script; computed once a frame, in lockstep)
        netPlayers[0] = netAutoPads[0];
        netPlayers[1] = netAutoPads[1];
    }
    netWork(frame, live, over);
    netDumps(frame);
    // (the desync check: the hash of every 60th frame, once it is too old to be run again, against the other side's)
    std::map<std::uint64_t, std::uint32_t> &hashes = ggHashes;
    if (frame % 60u == 0u && frame != 0u) {
        hashes[frame] = matchHash();
        while (hashes.size() > 64u) hashes.erase(hashes.begin());
    }
    if (live && frame >= 80u && frame % 60u == 20u) {
        const std::uint64_t at = frame - 20u;
        g_ggpo_local_hash_frame = unsigned(at);
        g_ggpo_local_hash = hashes[at];
        static const bool log = std::getenv("SC2_NET_HASHLOG") != nullptr;
        if (log) std::fprintf(stderr, "online hash %llu: %08x (rollbacks %llu, frames again %llu, saved %llu)\n",
                              (unsigned long long)at, hashes[at], (unsigned long long)ggLoads, (unsigned long long)ggResim,
                              (unsigned long long)ggSaves);
    }
    if (live && g_ggpo_remote_hash_frame) {
        const auto own = hashes.find(g_ggpo_remote_hash_frame);
        if (own != hashes.end() && frame > own->first + 10u && own->second != g_ggpo_remote_hash) {
            netplay->ended = "The two games came apart (a desync at frame " + std::to_string(own->first) + "): the match is stopped.";
            netplay->failed = true;
            endMatch();
        }
    }
}

// One frame of the main loop and the loop's edge to the next one (0x33CB5)
static void ggRunFrame()
{
    const auto push = [](std::uint32_t value) {
        recomp_runtime.registers.esp -= 4u;
        *recomp_memory_u32(recomp_runtime.registers.esp) = value;
    };
    sub_0002DD40();
    if (recomp_runtime.registers.eax != 1u) recomp_stop(2, "online:main-loop-left:%08x", recomp_runtime.registers.eax);
    push(0x00033CBAu);
    sub_000F0EC0();
    push(0x00033CBFu);
}

static bool __cdecl ggCbBegin(const char *) { return true; }
static bool __cdecl ggCbLog(char *, unsigned char *, int) { return true; }
static void __cdecl ggCbFree(void *buffer) { delete[] static_cast<std::uint32_t *>(buffer); }

static bool __cdecl ggCbSave(unsigned char **buffer, int *len, int *checksum, int frame)
{
    auto *token = new std::uint32_t[1]{std::uint32_t(frame)};
    *buffer = reinterpret_cast<unsigned char *>(token);
    *len = 4;
    *checksum = 0;
    const bool again = g_ggpo_save_confirmed >= 0;
    // (inside a rollback no state for a frame whose controls are all known: it is never gone back to. In lockstep none
    // at all, but for the pages' chain now and then: the fight's first save has little left to copy)
    if (again ? frame <= g_ggpo_save_confirmed : !ggSaveWanted) {
        ++ggSavesLeft;
        if (!again && frame % 30 == 0 && sc2::RamTracked()) sc2::RamSave(std::uint64_t(frame));
        return true;
    }
    const double at = perfNow();
    const unsigned slot = unsigned(frame) % kGgRing;
    sc2::SaveFrame(ggStates[slot], !sc2::RamTracked());
    if (sc2::RamTracked()) sc2::RamSave(std::uint64_t(frame));
    ggStateFrame[slot] = std::uint64_t(frame);
    const double ms = perfNow() - at;
    ggSaveMs += ms;
    ggSaveMax = std::max(ggSaveMax, ms);
    ++ggSaves;
    return true;
}

static bool __cdecl ggCbLoad(unsigned char *buffer, int)
{
    sc2_audio_quiet = 1; // (the frames run again from here: heard once already; the next live frame's sound goes out)
    const std::uint64_t frame = *reinterpret_cast<std::uint32_t *>(buffer);
    const unsigned slot = unsigned(frame) % kGgRing;
    const double at = perfNow();
    ggRbMark = at;
    ggRbCur = 0.0;
    recomp_kernel_drain_others();
    if (ggStateFrame[slot] != frame || !ggStates[slot].valid || (sc2::RamTracked() && !sc2::RamLoad(frame))) {
        std::fprintf(stderr, "online: no state of frame %llu to go back to (at %llu)\n", (unsigned long long)frame, (unsigned long long)ggFrame);
        ggMissing = true;
        return false;
    }
    sc2::LoadFrame(ggStates[slot], !sc2::RamTracked());
    ggFrame = frame;
    ++ggLoads;
    ggLoadMs += perfNow() - at;
    return true;
}

static bool __cdecl ggCbAdvance(int)
{
    if (ggMissing) return true;
    ggSync();
    ggPrepare(false);
    const double at = perfNow();
    ggInResim = true;
    ggRunFrame();
    ggInResim = false;
    ggStepMs += perfNow() - at;
    ++ggFrame;
    ++ggResim;
    ggpo_advance_frame(gg);
    const double now = perfNow();
    ggRbMs += now - ggRbMark;
    ggRbCur += now - ggRbMark;
    ggRbMax = std::max(ggRbMax, ggRbCur);
    ggRbMark = now;
    return true;
}

static bool __cdecl ggCbEvent(GGPOEvent *event)
{
    switch (event->code) {
    case GGPO_EVENTCODE_RUNNING: ggRunning = true; break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER: ggGone = true; break;
    case GGPO_EVENTCODE_TIMESYNC: ggAhead = event->u.timesync.frames_ahead; break; // (not slept: the clock keeps the pace)
    default: break;
    }
    return true;
}

static void ggDelay(int delay)
{
    if (delay == ggCurDelay) return;
    ggCurDelay = delay;
    ggpo_set_frame_delay(gg, ggLocal, delay);
}

/* The lockstep's input delay (outside the fight, and team battle's member change): the other side's controls must be there
   within it, or each frame waits. 4 frames (67 ms) held up to a ping of ~100 ms; many players have about that (user
   2026-11-25: "spoustu lidi bude mit kolem 100ms"), so it follows the ping, taken as the lockstep begins and kept for it:
   half the ping and 25 ms to spare, in frames, 4 to 10 (ping 100 -> 5, 150 -> 6, 250 -> 9). Each side its own; GGPO
   delivers both sides' controls alike whatever the delays. */
static int ggLockDelay = 4;
static int ggLockDelayFor()
{
    GGPONetworkStats stats{};
    int ping = 0;
    if (gg && GGPO_SUCCEEDED(ggpo_get_network_stats(gg, ggRemote, &stats)) && stats.network.ping > 0 && stats.network.ping < 2000)
        ping = stats.network.ping;
    const int frames = static_cast<int>(std::ceil((ping / 2.0 + 25.0) / (1000.0 / 60.0)));
    return std::max(netplay->Delay(), std::clamp(frames, 4, 10));
}

static void ggBoundary();

static bool ggStart()
{
    std::string ip;
    int port = 0;
    const int own = netplay->LocalPort();
    if (!netplay->PeerAddress(ip, port)) {
        netplay->ended = "no connection to the opponent (no IPv4 address)";
        netplay->failed = true;
        return false;
    }
    netplay->CloseSocket();
    ++ggSession;
    ggHashes.clear();
    g_ggpo_network_delay = netBase.fakeLag;
    g_ggpo_late_save = 1;
    GGPOSessionCallbacks cb{};
    cb.begin_game = ggCbBegin;
    cb.save_game_state = ggCbSave;
    cb.load_game_state = ggCbLoad;
    cb.log_game_state = ggCbLog;
    cb.free_buffer = ggCbFree;
    cb.advance_frame = ggCbAdvance;
    cb.on_event = ggCbEvent;
    if (!GGPO_SUCCEEDED(ggpo_start_session(&gg, &cb, "sc2", 2, kGgPad, static_cast<unsigned short>(own)))) {
        netplay->ended = "cannot open UDP port " + std::to_string(own);
        netplay->failed = true;
        return false;
    }
    ggpo_set_disconnect_timeout(gg, 5000);
    ggpo_set_disconnect_notify_start(gg, 1000);
    for (int p = 1; p <= 2; ++p) {
        GGPOPlayer player{};
        player.size = sizeof player;
        player.player_num = p;
        GGPOPlayerHandle handle = GGPO_INVALID_HANDLE;
        if (p - 1 == netplay->Local()) {
            player.type = GGPO_PLAYERTYPE_LOCAL;
            ggpo_add_player(gg, &player, &handle);
            ggLocal = handle;
        } else {
            player.type = GGPO_PLAYERTYPE_REMOTE;
            std::snprintf(player.u.remote.ip_address, sizeof player.u.remote.ip_address, "%s", ip.c_str());
            player.u.remote.port = static_cast<unsigned short>(port);
            ggpo_add_player(gg, &player, &handle);
            ggRemote = handle;
        }
    }
    const double began = perfNow();
    while (!ggRunning && perfNow() - began < 15000.0) {
        ggpo_idle(gg, 1);
        if (launcherMode()) launcherPoll();
        if (parkAsked) break;
    }
    if (!ggRunning) {
        netplay->ended = "no connection to the opponent";
        netplay->failed = true;
        return false;
    }
    std::fprintf(stderr, "online: the session runs (player %d, port %d, delay %d), the other side there after %.0f ms\n",
                 netplay->Local() + 1, own, netplay->Delay(), perfNow() - began);
    sc2_own_pace = 1;
    sc2_net_boundary = ggBoundary;
    return true;
}

static void ggClose()
{
    if (!gg) return;
    ggpo_close_session(gg);
    gg = nullptr;
    sc2_net_boundary = nullptr;
    sc2_own_pace = 0;
}

static void ggSleep(double ms)
{
#if defined(_WIN32)
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* high resolution */, TIMER_MODIFY_STATE | SYNCHRONIZE);
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<long long>(ms * 10000.0);
    if (timer && due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
        return;
    }
#endif
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

/* The frame clock: 60 a second. The wait is after the save and the rollback and before the controls are read; a late
   frame is not waited for, more than 50 ms behind it starts again from now. What was waited for the other side's
   controls is added at once (this side ran ahead by it); a shift of the pace is spread, at most 1 ms a frame. */
static void ggPace()
{
    static double next = 0.0;
    static unsigned session;
    if (session != ggSession) {
        session = ggSession;
        next = 0.0;
    }
    double now = perfNow();
    const double push = next != 0.0 ? ggClockPush : 0.0;
    ggClockPush = 0.0;
    next += push;
    if (next == 0.0 || now > next + 50.0) next = now;
    if (ggPhaseShift > 0.0) {
        const double step = std::min(ggPhaseShift, 1.0);
        next += step;
        ggPhaseShift -= step;
    }
    ggLateFrames += now > next;
    const double at = now;
    while ((now = perfNow()) < next) {
        const double left = next - now;
        if (left > 1.0) ggSleep(left - 0.5);
        else std::this_thread::yield();
    }
    ggSlackMs += perfNow() - at;
    next += 1000.0 / 60.0;
}

static void ggBalance()
{
    static int predicted, frames, since = 100, lockstep, prev, wait;
    static unsigned session;
    if (session != ggSession) {
        session = ggSession;
        predicted = frames = lockstep = prev = wait = 0;
        since = 100;
    }
    predicted += g_ggpo_predicted;
    lockstep += ggMode == 1;
    if (++frames == 60) {
        g_ggpo_local_late = lockstep ? -1 : predicted * 1000 / frames;
        ++since;
        const int diff = g_ggpo_local_late - g_ggpo_remote_late;
        const bool ok = g_ggpo_local_late >= 0 && g_ggpo_remote_late >= 0;
        const int units = ok && diff > 800 && since >= 2 ? 2 : ok && diff > 450 && since >= 6 ? 1 : 0;
        static const bool log = std::getenv("SC2_NET_PERF") != nullptr;
        if (log && (units || ok))
            std::fprintf(stderr, "online: frame %llu: predicted %d per mille of the frames, the other side %d%s\n", (unsigned long long)ggFrame,
                         g_ggpo_local_late, g_ggpo_remote_late, units == 2 ? " -> 10.7 ms later" : units ? " -> 2.7 ms later" : "");
        if (units) {
            ggPhaseShift += units == 2 ? 10.67 : 2.67;
            since = 0;
        }
        predicted = frames = lockstep = 0;
    }
    // (whole frames, before the fight)
    if (wait > 0) --wait;
    if (ggMode == 1 && ggFrame >= 120u && ggFrame % 60u == 0u) {
        GGPONetworkStats stats{};
        ggpo_get_network_stats(gg, ggRemote, &stats);
        int remote = stats.timesync.remote_frames_behind;
        if (remote > 127) remote -= 256;
        const int ahead = (remote - stats.timesync.local_frames_behind) / 2;
        const int move = wait ? 0 : std::min({ahead, prev, 2});
        if (move >= 1) {
            ggPhaseShift += move * 16.0;
            wait = 90;
            prev = 0;
        } else {
            prev = ahead;
        }
    }
}

extern "C" double recomp_acquire_wait_ms;
extern "C" double sc2_part_ms[4];
extern int g_ggpo_local_fps10, g_ggpo_remote_fps10;
extern "C" void sc2_net_hud_set(int ping_ms, int fps10);
/* (as Tekken 6 Online's net HUD: this side's live frames a second go out in the quality reports (fps10); every 30
   frames the ping and the other side's FPS go to the panel over the life bars (overlay_imgui_panel.cpp)) */
static void ggNetHud()
{
    static double second;
    static int frames;
    const double now = perfNow();
    if (second == 0.0) second = now;
    ++frames;
    if (now - second >= 1000.0) {
        g_ggpo_local_fps10 = int(frames * 10000.0 / (now - second) + 0.5);
        frames = 0;
        second = now;
    }
    if (ggFrame % 30u == 0u) {
        GGPONetworkStats stats{};
        const bool ok = GGPO_SUCCEEDED(ggpo_get_network_stats(gg, ggRemote, &stats));
        sc2_net_hud_set(ok && stats.network.ping >= 0 && stats.network.ping < 10000 ? stats.network.ping : -1, g_ggpo_remote_fps10);
    }
    if (ggFrame % 600u == 0u) {
        GGPONetworkStats stats{};
        ggpo_get_network_stats(gg, ggRemote, &stats);
        std::fprintf(stderr, "online net HUD: frame %llu: ping %d ms, opponent fps %.1f, this side fps %.1f\n",
                     (unsigned long long)ggFrame, stats.network.ping, g_ggpo_remote_fps10 / 10.0, g_ggpo_local_fps10 / 10.0);
    }
}

static double ggLiveEnd, ggLiveMax;
static void ggBoundary()
{
    if (!gg || endAsked || parkAsked) return;
    profStart();
    if (ggLiveEnd > 0.0) {
        const double live = perfNow() - ggLiveEnd; // (the live frame: game, draw, present)
        ggLiveMs += live;
        ggLiveMax = std::max(ggLiveMax, live);
    }
    {
        static double last;
        static unsigned session;
        if (session != ggSession) {
            session = ggSession;
            last = 0.0;
        }
        const double now = perfNow();
        if (last > 0.0 && !netAuto) {
            const double period = now - last;
            ggPeriodMax = std::max(ggPeriodMax, period);
            ggOver17 += period > 17.5;
            ggOver25 += period > 25.0;
            static const char *gaps = std::getenv("SC2_NET_GAPS");
            static const double least = gaps ? std::atof(gaps) : 0.0;
            if (least > 0.0 && period > least)
                std::fprintf(stderr, "online gap frame %llu: %.1f ms: waited %.1f, run again %.1f (%llu frames), saves %.1f\n",
                             (unsigned long long)ggFrame, period, gapWait, ggRbMs - gapRb, (unsigned long long)(ggResim - gapRbFrames),
                             ggSaveMs - gapSave);
        }
        gapWait = 0.0;
        gapRb = ggRbMs;
        gapRbFrames = ggResim;
        gapSave = ggSaveMs;
        last = now;
    }
    if (ggPending) {
        ggPending = false;
        ++ggFrame;
        // (also the first frame after the rollback's end (ggMode still -1): GGPO may go back to it before the mode below turns)
        ggSaveWanted = ggWant() == -1 || ggMode == -1 || ggGraceUntil > ggFrame;
        ggpo_advance_frame(gg);
    }
    /* (the hidden way from the parking to the fight: both players' controls are the script's, alike on both sides,
       so neither waits for the other's: it runs unpaced, with a long input delay so the network never holds it, and
       unheard; the fight's clock starts again at its first frame. SC2_AUTO_PACED=1: at 60 a second as before.) */
    static const bool autoPaced = std::getenv("SC2_AUTO_PACED") != nullptr;
    const bool fastAuto = netAuto && !autoPaced;
    if (!fastAuto) ggPace();
    const int want = ggWant();
    if (want != ggMode) {
        const bool ended = ggMode == -1 && want == 1; // (the fight's end: the delay goes up at once, the prediction stays 8 frames)
        ggMode = want;
        ggGraceUntil = ended ? ggFrame + 8u : 0u;
        ggpo_set_max_prediction(want == 1 && ggGraceUntil == 0u ? 1 : -1);
        if (want == 1) ggLockDelay = ggLockDelayFor();
        ggDelay(want == 1 ? ggLockDelay : netplay->Delay());
        std::fprintf(stderr, "online: frame %llu: %s, delay %d\n", (unsigned long long)ggFrame, want == 1 ? "lockstep" : "rollback",
                     want == 1 ? ggLockDelay : netplay->Delay());
    } else if (ggMode == 1 && ggGraceUntil && ggFrame >= ggGraceUntil) {
        ggGraceUntil = 0u;
        ggpo_set_max_prediction(1);
    }
    // (a lockstep from the session's start (the select) began before any ping was known: raised once a second when the ping
    // asks for more, never lowered inside one)
    if (ggMode == 1 && !fastAuto && ggFrame % 60u == 0u) ggLockDelay = std::max(ggLockDelay, ggLockDelayFor());
    ggDelay(fastAuto ? 30 : ggMode == 1 ? ggLockDelay : netplay->Delay());
    ggpo_idle(gg, 0);
    if (ggMissing) {
        netplay->ended = "The match could not go back to a frame (rollback).";
        netplay->failed = true;
        endMatch();
        return;
    }
    if (launcherMode() && ggFrame % 6u == 0u) launcherPoll();
    std::uint8_t pad[kGgPad];
    ggPack(netMine(ggFrame, ggOverNow()), pad);
    const double wait = perfNow();
    double polled = wait;
    while (true) {
        if (ggGone) {
            netplay->ended = "The other player left.";
            netplay->left = netplay->lost = true;
            endMatch();
            return;
        }
        const GGPOErrorCode err = ggpo_add_local_input(gg, ggLocal, pad, kGgPad);
        if (GGPO_SUCCEEDED(err)) break;
        if (parkAsked) return; // (at the start of the next frame: endFrame)
        if (err != GGPO_ERRORCODE_PREDICTION_THRESHOLD && err != GGPO_ERRORCODE_NOT_SYNCHRONIZED) {
            std::fprintf(stderr, "online: the controls were not taken (%d)\n", int(err));
            break;
        }
        ggpo_idle(gg, 1);
        if (launcherMode() && perfNow() - polled > 50.0) {
            polled = perfNow();
            launcherPoll();
        }
    }
    const double waited = perfNow() - wait;
    ggWaitMs += waited;
    gapWait += waited;
    if (waited > 1.0) ggClockPush += waited;
    ggSync();
    ggBalance();
    ggPrepare(true);
    ggNetHud();
    netShots(ggFrame);
    ggPending = true;
    ggLiveEnd = perfNow();
    static const bool report = std::getenv("SC2_NET_PERF") != nullptr;
    if (report && ++ggWindow % 600u == 0u) {
        static std::uint64_t loads, resim, saves, left;
        static double step, load, save, rollback, waits;
        const std::uint64_t dl = ggLoads - loads, dr = ggResim - resim, ds = ggSaves - saves;
        std::fprintf(stderr, "online perf frame %llu: scene %u, last 600 frames: %llu rollbacks, %llu frames again, game step of a frame again %.2f ms, "
                     "load %.2f, save %.2f ms (max %.2f, %llu made, %llu left out), whole rollback avg %.2f max %.1f ms; period max %.1f ms, over 17.5: %llu, "
                     "over 25: %llu; waited for the other side %.0f ms, late %llu, slack avg %.2f ms; live frame avg %.2f max %.1f ms, present waited %.0f ms; frames again: draw task %.2f, view build %.2f, fighters %.2f, loop edge %.2f ms\n",
                     (unsigned long long)ggFrame, *recomp_memory_u32(0x398700u), (unsigned long long)dl, (unsigned long long)dr,
                     dr ? (ggStepMs - step) / double(dr) : 0.0, dl ? (ggLoadMs - load) / double(dl) : 0.0, ds ? (ggSaveMs - save) / double(ds) : 0.0,
                     ggSaveMax, (unsigned long long)ds, (unsigned long long)(ggSavesLeft - left), dl ? (ggRbMs - rollback) / double(dl) : 0.0, ggRbMax,
                     ggPeriodMax, (unsigned long long)ggOver17, (unsigned long long)ggOver25, ggWaitMs - waits, (unsigned long long)ggLateFrames,
                     ggSlackMs / 600.0, ggLiveMs / 600.0, ggLiveMax, recomp_acquire_wait_ms, dr ? sc2_part_ms[0] / double(dr) : 0.0, dr ? sc2_part_ms[1] / double(dr) : 0.0, dr ? sc2_part_ms[2] / double(dr) : 0.0, dr ? sc2_part_ms[3] / double(dr) : 0.0);
        sc2_part_ms[0] = sc2_part_ms[1] = sc2_part_ms[2] = sc2_part_ms[3] = 0.0;
        recomp_acquire_wait_ms = 0.0;
        loads = ggLoads;
        resim = ggResim;
        saves = ggSaves;
        left = ggSavesLeft;
        step = ggStepMs;
        load = ggLoadMs;
        save = ggSaveMs;
        rollback = ggRbMs;
        waits = ggWaitMs;
        ggSaveMax = ggRbMax = ggPeriodMax = ggSlackMs = ggLiveMs = ggLiveMax = 0.0;
        ggOver17 = ggOver25 = ggLateFrames = 0;
    }
}

/* This program and its arguments, for hosting from the menu: when someone comes, the game starts over for the match
   (Netplay::Settings::restartOnHello), as a new copy hosting on the same port with the launcher's name; the other
   side's hello comes again, and that copy takes them in as --host does. */
static std::string runnerPath;
static std::vector<std::string> runnerArgs;

/* This copy replaced by a new one with these arguments (on Windows: a new process, this one ending) */
[[noreturn]] static void startAgain(std::vector<std::string> args)
{
    netplay.reset(); // (the port, for the new copy)
    std::fflush(stdout);
    std::fflush(stderr);
    std::vector<char *> argv;
    for (std::string &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
#if defined(_WIN32)
    /* (_spawnv joins the arguments with spaces and no quotes, so a path with a space in it, the disc's
       "Soulcalibur II (USA).iso" among them, came apart and the new copy never started: each argument quoted as
       CommandLineToArgvW reads it, the program's output handed on) */
    (void)argv;
    const auto wide = [](const std::string &text) {
        std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0)), L'\0');
        if (!out.empty()) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), static_cast<int>(out.size()));
        if (!out.empty()) out.pop_back();
        return out;
    };
    std::wstring line;
    for (const std::string &arg : args) {
        if (!line.empty()) line += L' ';
        line += L'"';
        std::size_t slashes = 0;
        for (const wchar_t c : wide(arg)) {
            if (c == L'\\') { ++slashes; continue; }
            line.append(c == L'"' ? slashes * 2u + 1u : slashes, L'\\');
            slashes = 0;
            line += c;
        }
        line.append(slashes * 2u, L'\\');
        line += L'"';
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(wide(runnerPath).c_str(), line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
            &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::_Exit(0);
    }
    std::fprintf(stderr, "online: the game could not start over (error %lu)\n", GetLastError());
    std::_Exit(3);
#elif defined(__ANDROID__)
    /* (no program to run: the activity starts the library again in a new process) */
    if (sc2_android_start_again(std::vector<std::string>(args.begin() + 1, args.end()))) std::_Exit(0);
    std::fprintf(stderr, "online: the game could not start over\n");
    std::_Exit(3);
#else
    execv(runnerPath.c_str(), argv.data());
    std::perror("online: the game could not start over");
    std::_Exit(3);
#endif
}

/* This program's arguments without those of online play's that a new copy is given afresh */
static std::vector<std::string> argsAgain()
{
    std::vector<std::string> args{runnerPath};
    for (std::size_t i = 0; i < runnerArgs.size(); ++i) {
        const std::string &arg = runnerArgs[i];
        if (arg == "--menu-shot") { ++i; continue; } // (a test's menu picture: once)
        if (arg == "--net-renew") { i += 2; continue; }
        if (arg == "--net-again" || arg == "--net-fresh" || arg == "--started-over") continue;
        if (arg == "--net-invite" || arg == "--net-invite-name") { ++i; continue; } // (they have the match's code)
        if (arg == "--host") {
            if (i + 1 < runnerArgs.size() && std::isdigit(static_cast<unsigned char>(runnerArgs[i + 1][0]))) ++i;
            continue;
        }
        args.push_back(arg);
    }
    return args;
}

/* A disc of the other release (US and PAL, sc2_xbe.h): this program's arguments given to that release's program,
   beside this one (soulcalibur2 and soulcalibur2-pal, each with this one's extension). */
[[noreturn]] static void handToOtherRelease()
{
#if defined(__ANDROID__)
    std::fprintf(stderr, "recomp runner: this app plays Soulcalibur II (%s) only\n", SC2_PAL ? "Europe" : "USA");
    std::_Exit(1);
#else
    const std::filesystem::path self = std::filesystem::u8path(runnerPath);
    std::string stem = self.stem().u8string();
    if (SC2_PAL) {
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "-pal") == 0) stem.resize(stem.size() - 4);
    } else {
        stem += "-pal";
    }
    const std::filesystem::path other = self.parent_path() / std::filesystem::u8path(stem + self.extension().u8string());
    std::error_code error;
    if (!std::filesystem::is_regular_file(other, error)) {
        std::cerr << "recomp runner: this is Soulcalibur II (" << (SC2_PAL ? "USA" : "Europe") << "), whose program ("
                  << other.u8string() << ") isn't here\n";
        std::_Exit(1);
    }
    std::vector<std::string> args{other.u8string()};
    args.insert(args.end(), runnerArgs.begin(), runnerArgs.end());
    runnerPath = other.u8string();
    startAgain(std::move(args));
#endif
}

[[noreturn]] static void restartForMatch()
{
    std::vector<std::string> args = argsAgain();
    args.push_back("--host");
    args.push_back(std::to_string(netBase.port));
    args.push_back("--net-fresh");
    if (!netBase.name.empty()) {
        args.push_back("--net-name");
        args.push_back(netBase.name);
        args.push_back("--net-list");
    }
    /* (the same code, so that a player coming back with it finds this copy) */
    if (netplay && !netplay->Code().empty()) {
        args.push_back("--net-renew");
        args.push_back(netplay->Code());
        args.push_back(netplay->Secret());
    }
    startAgain(args);
}

[[noreturn]] static void rejoin()
{
    std::fprintf(stderr, "online: the game starts again to come back to the match\n");
    std::vector<std::string> args = argsAgain();
    args.push_back("--net-again");
    startAgain(args);
}

static void hostAgain(const std::string &code, const std::string &secret)
{
    sc2::Netplay::Settings settings = netBase;
    settings.hosting = true;
    settings.listed = !settings.name.empty();
    settings.restartOnHello = true;
    settings.renewCode = code;
    settings.renewSecret = secret;
    netplay = std::make_unique<sc2::Netplay>(settings);
    std::string error;
    if (!netplay->Begin(error)) {
        std::fprintf(stderr, "online: hosting again did not start: %s\n", error.c_str());
        netplay.reset();
        return;
    }
    netFromMenu = netTitleAsked = true; // (sent to the title already)
    netWaitingReturn = true;
    sc2_net_seed = netplay->Seed();
    sc2_net_seeded = 1;
    std::fprintf(stderr, "online: hosting again with the same code, for the other player to come back\n");
}

static bool sampleNetplay(std::uint32_t port, RecompInputGamepad *gamepad)
{
    if (port > 1u) return false;
    if (port == 0u) netOverlay();
    if (!netConnected) {
        /* (hosting: the game is the player's own while the other player comes, their controls passed on as
           usual; it starts over for the match when they do) */
        if (port == 0u) {
            std::string error;
            const int state = netplay->Waiting(error);
            if (state < 0) {
                netplay->ended = error;
                netplay->failed = true;
                endMatch();
                return false;
            }
            if (state == 2) restartForMatch();
            if (state > 0) netConnected = true;
        }
        /* (the copy started over for the match (--net-fresh) waits hands off: the match is about to begin) */
        if (netFresh || netConnected) {
            *gamepad = RecompInputGamepad{};
            return true;
        }
        return false;
    }
    if (!netLive) {
        if (port == 0u) netplay->Flush();
        /* Both games begin the match at the title screen; one in the attract demo is sent back to it (Start), and a
           host whose game was its own (hosting from the menu) is sent there as a match's end does (endMatch) */
        const std::uint32_t current = *recomp_memory_u32(0x398700u);
        if (port == 0u && netFromMenu && !netTitleAsked && current != 5u && current != 7u) {
            netTitleAsked = true;
            *recomp_memory_u32(0x398704u) = 5u;
            const std::uint32_t scene = *recomp_memory_u32(0x39871Cu);
            if (scene) *reinterpret_cast<std::uint16_t *>(recomp_memory(scene + 0x28u, 2u)) = 1u;
        }
        if (port != 0u || sc2_scene != 5u || current != 5u) {
            *gamepad = RecompInputGamepad{};
            if (port == 0u && current == 7u && (netReads++ / 8u) % 2u == 0u) gamepad->buttons = RECOMP_INPUT_BUTTON_START;
            return true;
        }
        if (!beginMatch()) return false;
    }
    /* (the other guest threads, the loader among them, run until each is waiting before every read: their work
       between two reads is then the same on both machines) */
    if (port == 0u) recomp_kernel_drain_others();
    *gamepad = netPlayers[port];
    return true;
}

/* The prelude (--net-prelude), as T6's parked session: before anyone has met, the game goes by itself from the title
   to VS Battle's select, unshown and muted, and waits there for a match (parked); then only the select's confirm
   and the fight's loading are left. SC2_PRELUDE_DUMP=FILE (diagnosis): guest RAM at the parking, then the program
   ends. */
// (netPrelude, netParked: with netAuto, above)
static std::uint64_t preludeFrames;
static void preludeFrame();
static void parkedOn();
static void parkPutBack();

static bool preludeMeet();
static void launcherPark();
static unsigned inSelect;
static void practicePoll();

// The practice begins: the mode select put back, the walk to Practice from there (preludeFrame, autoPads), unshown,
// unheard and unpaced to the fight
static void practiceBegin()
{
    recomp_kernel_drain_others();
    const double at = perfNow();
    const bool fromQ = !practicePrepare && snapQ.valid;
    if (!practicePrepare) {
        int stage = practiceStage;
        if (stage < 0) {
            static constexpr int kPracticeStages[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14};
            stage = kPracticeStages[static_cast<std::size_t>(std::fmod(perfNow() * 7.3, double(std::size(kPracticeStages))))];
        }
        practiceStageUsed = stage;
    }
    sc2::LoadFrame(fromQ ? snapQ : snapMenu);
    practiceWanted = practiceStartNow = false;
    practicePrompt = 0;
    practiceAnswer.clear();
    practice = netPrelude = true;
    netParked = netAuto = autoSelectSeen = autoRematch = practiceFromQ = false;
    autoScene = *recomp_memory_u32(0x398700u);
    autoFrames = snapMenuFrames;
    autoLiveFrames = autoConfirmFrom = 0u;
    inSelect = practiceOutside = 0u;
    practiceBefore = true;
    netAutoPads[0] = netAutoPads[1] = RecompInputGamepad{};
    sc2_fast_forward = sc2_hide_frames = sc2_audio_quiet = 1;
    for (double &step : practiceStep) step = 0.0;
    if (fromQ) {
        // (at Q: the select stands; this practice's fighters are taken at the next frame, as after the walk)
        autoSelectSeen = true;
        inSelect = 59u;
        autoFrames = snapQFrames;
        autoLiveFrames = snapQLive;
        autoConfirmFrom = snapQConfirm;
        practiceStep[0] = practiceStep[1] = perfNow();
    }
    std::fprintf(stderr, "practice: %s put back in %.0f ms (scene %u)%c", fromQ ? "Q" : "the mode select", perfNow() - at, autoScene, 10);
}

// The parking put back as it was at the first 'ready', for another game to play on from it
static void parkPutBack()
{
    sc2::LoadFrame(snapPark);
    netPrelude = netParked = true;
    autoFrames = snapParkAutoFrames;
    autoLiveFrames = snapParkLiveFrames;
    autoConfirmFrom = snapParkConfirmFrom;
    autoSelectSeen = snapParkSelectSeen;
    autoRematch = false;
    netAuto = false;
    sc2_pick_active = 0;
    sc2_net_pick[0] = sc2_net_pick[1] = -1;
    netAutoOutfit[0] = netAutoOutfit[1] = netAutoStage = -1;
    netAutoPads[0] = netAutoPads[1] = RecompInputGamepad{};
    autoScene = *recomp_memory_u32(0x398700u);
    inSelect = 121u;
    sc2_hide_frames = 1;
    sc2_fast_forward = sc2_audio_quiet = 0;
    sc2_net_frame = preludeFrame;
}

// The practice over (closed, or a match accepted in it): the parking put back, nothing of the practice left on
static void practiceEnd()
{
    recomp_kernel_drain_others();
    const double at = perfNow();
    recomp_audio_output_set_muted(1);
    sc2::Training &aids = sc2::TrainingState();
    if (!netPrelude) practiceAids = aids; // (left on the way to the fight: they were not on yet)
    const bool fps = aids.fps_counter;
    aids = sc2::Training{};
    aids.fps_counter = fps;
    sc2::TrainingForget();
    sc2::StatesForget();
    sc2::MenuSpeedFull();
    recomp_scene_freeze_set(false);
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    recomp_overlay_publish(&text);
    parkPutBack();
    practice = practiceHold = practiceSearching = practiceFromQ = false;
    practicePrompt = 0;
    practiceAnswer.clear();
    practiceHint = 0u;
    std::fprintf(stderr, "practice: over, the parking put back in %.0f ms (scene %u)\n", perfNow() - at, autoScene);
    parkedOn();
}

static void preludeFrame()
{
    if (!netIconsFolder.empty()) {
        // (once: the select screen's icons for the launcher, from this player's own disc)
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(netIconsFolder) / "icon_1_0.png", ec)) {
            std::string error;
            const int made = sc2::ExportCharacterIcons(netIconsFolder, error);
            std::fprintf(stderr, "online: character icons for the launcher: %d made in %s%s%s\n", made, netIconsFolder.c_str(),
                         made < 0 ? ": " : "", made < 0 ? error.c_str() : "");
        }
        netIconsFolder.clear();
    }
    autoPads();
    ++preludeFrames;
    if ((launcherMode() || netTeam) && !snapMenu.valid && autoScene == 6u && autoFrames == 131u) {
        recomp_kernel_drain_others();
        snapMenuFrames = autoFrames;
        sc2::SaveFrame(snapMenu);
    }
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    inSelect = current == 10u ? inSelect + 1u : 0u;
    // (every character and stage open for the launcher's picks: a fresh save has them locked. After the save's load,
    // at the mode select and the select until parked, the same on both sides; upstream's "Unlock everything")
    if (!netParked && (current == 6u || current == 10u)) {
        static bool told;
        if (sc2::TrainingUnlockAll(true) && !told) {
            told = true;
            std::fprintf(stderr, "online: everything unlocked (prelude)\n");
        }
    }
    if (preludeFrames == 1u || preludeFrames % 20u == 0u) autoCard(preludeFrames);
    if (practice && std::getenv("SC2_PRACTICE_TRACE") && preludeFrames % 30u == 0u) {
        float fightClock;
        std::memcpy(&fightClock, recomp_memory(0x39D2ACu, 4u), 4u);
        std::fprintf(stderr, "practice trace: frame %llu scene %u switches %u mode %u clock %g\n",
                     (unsigned long long)preludeFrames, current, (unsigned)sc2_scene_switches,
                     (unsigned)*static_cast<const std::uint8_t *>(recomp_memory(0x39895Eu, 1u)), fightClock);
    }
    if (practice && std::getenv("SC2_PRACTICE_TRACE") && current == 10u && preludeFrames % 60u == 0u) {
        // (measurement) the two fighters as the select has them: character +0x58, outfit +0x64, and what the select shows
        std::string line;
        for (std::uint32_t p = 0; p < 2u; ++p) {
            const std::uint32_t record = SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE;
            char text[96];
            std::snprintf(text, sizeof text, " p%u char %d outfit %d", p, *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x58u), 2u)),
                          *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x64u), 2u)));
            line += text;
        }
        std::fprintf(stderr, "practice select: frame %u live %u%s stage %u auto %d pick %d %d active %d prepare %d fromQ %d next %x inSelect %u%c", autoFrames, autoLiveFrames, line.c_str(), *recomp_memory_u32(0x398770u), netAuto ? 1 : 0, sc2_net_pick[0], sc2_net_pick[1], sc2_pick_active, practicePrepare ? 1 : 0, practiceFromQ ? 1 : 0, *recomp_memory_u32(0x398704u), inSelect, 10);
    }
    if (practice && launcherMode() && !practicePrepare) {
        practicePoll();
        if (practiceHold) {
            practiceEnd();
            return;
        }
    }
    if (practice && netAuto && current == 7u) {
        if (practiceStep[2] == 0.0) practiceStep[2] = perfNow();
        // (the fight: the player's once its fighting has begun, the same float upstream's training reads; at most 15 s)
        float begun;
        std::memcpy(&begun, recomp_memory(0x39D2ACu, 4u), 4u);
        if (practiceBefore && begun == 0.0f) practiceBefore = false;
        if ((practiceBefore || begun == 0.0f) && autoFrames < 900u) return;
        netAuto = false;
        sc2_pick_active = 0;
        netPrelude = false;
        practiceOutside = 0u;
        practiceFromQ = false;
        sc2_hide_frames = sc2_fast_forward = sc2_audio_quiet = 0;
        sc2_net_frame = nullptr;
        recomp_audio_output_set_muted(netPreludeMuted > 0 ? 1 : 0);
        static bool first = true;
        sc2::Training &aids = sc2::TrainingState();
        if (first) {
            practiceAids = aids;
            practiceAids.health_refill = practiceAids.infinite_time = practiceAids.frame_data = true;
            practiceAids.input_display = practiceAids.combo_readout = true;
            first = false;
        }
        practiceAids.dummy = sc2::Dummy::Off;
        aids = practiceAids;
        practiceHint = 60u * 15u;
        std::fprintf(stderr, "practice: at the fight (prelude frame %llu, %u of the scene, %.0f ms after the command): stage %u, the controls are the player's\n",
                     (unsigned long long)preludeFrames, autoFrames, practiceAskedAt != 0.0 ? perfNow() - practiceAskedAt : 0.0,
                     *recomp_memory_u32(0x398770u));
        for (std::uint32_t p = 0; p < 2u; ++p) {
            const std::uint32_t record = SC2_FIGHTER_RECORD + p * SC2_FIGHTER_STRIDE;
            std::fprintf(stderr, "practice: player %u at the fight: character %d, outfit %d%c", p + 1u,
                         *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x58u), 2u)),
                         *reinterpret_cast<const std::int16_t *>(recomp_memory(record + SC2_FIGHTER_FIELD(0x64u), 2u)), 10);
        }
        std::fprintf(stderr, "practice timing: menu %.0f ms, select standing %.0f, confirm + load %.0f, the fight's intro to the fighting %.0f (%u frames)%c",
                     practiceStep[0] - practiceAskedAt, practiceStep[1] - practiceStep[0], practiceStep[2] - practiceStep[1], perfNow() - practiceStep[2], autoFrames, 10);
        if (launcherMode()) launcherSet("practice");
        return;
    }
    if (practice && inSelect == 1u && practiceStep[0] == 0.0) practiceStep[0] = perfNow();
    if (inSelect == (launcherMode() ? 60u : 120u) && !netParked && practice) {
        practiceStep[1] = perfNow();
        if (practicePrepare) {
            recomp_kernel_drain_others();
            snapQFrames = autoFrames;
            snapQLive = autoLiveFrames;
            snapQConfirm = autoConfirmFrom;
            sc2::SaveFrame(snapQ);
            practicePrepare = practice = false;
            std::fprintf(stderr, "practice: the select as it waits kept (Q)%c", 10);
            parkPutBack();
            parkedOn();
            return;
        }
        netParked = true;
        for (int p = 0; p < 2; ++p) {
            sc2_net_pick[p] = practicePick[p];
            netAutoOutfit[p] = practiceOutfit[p];
        }
        mirrorOutfit(practicePick);
        netAutoStage = practicePrepare ? practiceStage : practiceStageUsed;
        netAuto = true;
        sc2_pick_active = 1;
        autoConfirmFrom = autoLiveFrames;
        std::fprintf(stderr, "practice: %d:%d vs %d:%d, stage %d: on to the fight\n", practicePick[0], practiceOutfit[0],
                     practicePick[1], practiceOutfit[1], practiceStage);
        return;
    }
    if (inSelect == 120u && !netParked) {
        netParked = true;
        std::fprintf(stderr, "online: parked at the select (prelude frame %llu)\n",
                     (unsigned long long)preludeFrames);
        if (launcherMode()) {
            recomp_kernel_drain_others();
            sc2::SaveFrame(snapPark);
            snapParkAutoFrames = autoFrames;
            snapParkLiveFrames = autoLiveFrames;
            snapParkConfirmFrom = autoConfirmFrom;
            snapParkSelectSeen = autoSelectSeen;
            if (!snapQ.valid && snapMenu.valid && !std::getenv("SC2_NO_Q")) {
                practicePick[0] = 3;
                practicePick[1] = 12;
                practiceOutfit[0] = practiceOutfit[1] = 0;
                practiceStage = 1;
                practicePrepare = true;
                practiceBegin();
                return;
            }
        }
        parkedOn();
    }
}

/* The session over and everything of it as a new game has it, so the next match begins as the first. */
static void netSessionReset()
{
    if (netplay) {
        if (!netplay->lost) netplay->Leave();
        netplay->Report(true);
        netplay.reset();
    }
    ggClose();
    ggRunning = ggGone = ggPending = ggMissing = false;
    ggSaveWanted = true;
    ggFrame = ggGraceUntil = 0u;
    ggMode = 0;
    ggCurDelay = -1;
    ggAhead = 0;
    for (unsigned r = 0; r < kGgRing; ++r) {
        ggStates[r] = sc2::SaveState{};
        ggStateFrame[r] = 0u;
    }
    ggLoads = ggResim = ggSaves = ggSavesLeft = ggLateFrames = ggOver17 = ggOver25 = ggWindow = 0u;
    ggStepMs = ggLoadMs = ggSaveMs = ggSaveMax = ggRbMs = ggRbMax = ggRbCur = ggRbMark = ggWaitMs = ggPeriodMax = ggLiveMs = ggSlackMs = 0.0;
    ggClockPush = ggPhaseShift = ggLiveEnd = ggLiveMax = 0.0;
    g_ggpo_predicted = 0;
    g_ggpo_local_late = g_ggpo_remote_late = g_ggpo_save_confirmed = -1;
    g_ggpo_local_hash_frame = g_ggpo_local_hash = g_ggpo_remote_hash_frame = g_ggpo_remote_hash = 0u;
    ggHashes.clear();
    netLive = netConnected = netRoute = false;
    teamMenuSeen = teamHanded = false;
    teamSelectStart = sc2::SaveState{};
    netReads = 0u;
    netPlayers[0] = netPlayers[1] = RecompInputGamepad{};
    sc2_net_live = 0;
    sc2_net_reads = 0;
    for (unsigned r = 0; r < kStates; ++r) {
        netStates[r] = sc2::SaveState{};
        netStateFrame[r] = 0u;
        netStateHash[r] = 0u;
        netGuess[r] = false;
    }
    netFrameAt = netCheckedUpTo = netResimUntil = netHashedUpTo = netRollbacks = netResimFrames = netSaves = 0u;
    netFightSince = UINT64_MAX;
    netStartWins = 0u;
    netOverAt = 0u;
    netWantsRematch[0] = netWantsRematch[1] = false;
    netFightStart = sc2::SaveState{};
    netAutoPads[0] = netAutoPads[1] = RecompInputGamepad{};
    netEnded.clear();
    netEndedShown = 0u;
    netChat.clear();
    netTyping = netLeaving = false;
    launcherWinner = -1;
    launcherGone = launcherRematch = false;
    endAsked = parkAsked = false;
    sc2::RamForget();
    recomp_kernel_set_deterministic(false);

    RecompOverlayText text;
    recomp_overlay_clear(&text);
    recomp_overlay_publish(&text);
    sc2_net_hud_set(-1, 0);
}

/* At the start of a frame: the match is over for this side (endMatch) or the launcher said 'park': the result or the
   error told and the launcher waited for, the session left, the parking put back and 'ready' said again. No new game. */
static void endFrame()
{
    if (ggInResim) return;
    const double at = perfNow();
    if (!parkAsked) {
        recomp_audio_output_set_muted(1);
        launcherEnded();
    }
    recomp_kernel_drain_others();
    recomp_audio_output_set_muted(1);
    netSessionReset();
    parkPutBack();
    std::fprintf(stderr, "online: the parking put back after the match (%.0f ms)\n", perfNow() - at);
    parkedOn();
}

// (parked: the launcher's match or practice waited for)
static void parkedOn()
{
    if (launcherMode()) {
        if (practiceStartNow) {
            practiceStartNow = false;
            launcherSet("loading");
            netplay = std::make_unique<sc2::Netplay>(preludeSettings);
        } else {
            launcherPark(); // (until the launcher starts a match: netplay then is made)
        }
        if (practiceWanted) {
            practiceBegin();
            return;
        }
    }
    if (netplay && !preludeMeet()) {
        endMatch();
        return;
    }
    if (const char *dump = std::getenv("SC2_PRELUDE_DUMP")) {
        if (FILE *out = std::fopen(dump, "wb")) {
            std::fwrite(recomp_memory(0u, RECOMP_XBOX_RAM_SIZE), 1, RECOMP_XBOX_RAM_SIZE, out);
            std::fclose(out);
        }
        std::fprintf(stderr, "online: the prelude's RAM written to %s\n", dump);
        std::fflush(stderr);
        std::_Exit(0);
    }
}

/* The meeting, at the parking: the other player found (the host waits for them, the other side asks until it is
   answered), then the two games made alike: the other side sends a hash of each page of its guest RAM, the host the
   pages that differ and its records (sc2_state.h, PutPages), and both begin the match there, frame 0, as at the title,
   the select's confirm and the fight's loading still to go (netAuto). */
static bool preludeMeet()
{
    const double began = perfNow();
    std::string error;
    if (launcherMode()) {
        /* (the way between the two is open, the launchers made it: both sides are there within seconds) */
        bool met = false;
        while (!met && perfNow() - began < 20000.0) {
            error.clear();
            netplay = std::make_unique<sc2::Netplay>(preludeSettings);
            if (netplay->Begin(error)) {
                int state = 0;
                while ((state = netplay->Waiting(error)) == 0 && perfNow() - began < 20000.0) {
                    launcherPoll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                met = state > 0;
            }
            if (!met) {
                std::fprintf(stderr, "online: not met yet (%s)\n", error.c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        if (!met) {
            netplay->ended = "no connection to the opponent";
            netplay->failed = true;
            return false;
        }
    } else if (netplay->Hosting()) {
        int state = 0;
        while ((state = netplay->Waiting(error)) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (state < 0) {
            netplay->ended = error;
            netplay->failed = true;
            return false;
        }
    } else {
        while (!netplay->Connect(error)) {
            std::fprintf(stderr, "online: no match yet (%s); asking again\n", error.c_str());
            netplay = std::make_unique<sc2::Netplay>(preludeSettings);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    netConnected = true;
    // (from here the sound's and the effects' random numbers are their own (sc2_hooks.c, rand_hook): drawn from the
    // game's, as often as each machine's sound has them, they parted the two games' numbers at the first round)
    sc2_net_seed = netplay->Seed();
    sc2_net_seeded = 1;
    const double met = perfNow();
    recomp_kernel_drain_others();
    if (netTeam) {
        // (team battle: on from the mode select, teamWalk)
        if (!snapMenu.valid) {
            netplay->ended = "team battle: the game has no mode select kept";
            netplay->failed = true;
            return false;
        }
        sc2::LoadFrame(snapMenu);
        recomp_kernel_drain_others();
        sc2::TrainingUnlockAll(true); // (as the prelude has it at the mode select)
        autoScene = *recomp_memory_u32(0x398700u);
        autoFrames = snapMenuFrames;
        std::fprintf(stderr, "online: team battle: the mode select put back (scene %u)\n", autoScene);
    }
    std::vector<std::uint8_t> blob;
    const std::vector<std::uint32_t> mine = sc2::PageHashes();
    if (!netplay->Hosting()) blob.assign(reinterpret_cast<const std::uint8_t *>(mine.data()),
                                         reinterpret_cast<const std::uint8_t *>(mine.data() + mine.size()));
    if (!netplay->SyncState(blob, 1)) return false;
    std::size_t pageCount = 0;
    if (netplay->Hosting()) {
        if (blob.size() != mine.size() * 4u) {
            netplay->ended = "The other game's memory did not come whole.";
            netplay->failed = true;
            return false;
        }
        std::vector<std::uint32_t> pages;
        for (std::uint32_t p = 0; p < mine.size(); ++p) {
            std::uint32_t theirs;
            std::memcpy(&theirs, blob.data() + p * 4u, 4u);
            if (theirs != mine[p]) pages.push_back(p);
        }
        pageCount = pages.size();
        std::vector<std::uint8_t> out;
        const auto put32 = [&out](std::uint32_t v) { out.insert(out.end(), reinterpret_cast<std::uint8_t *>(&v), reinterpret_cast<std::uint8_t *>(&v) + 4); };
        put32(static_cast<std::uint32_t>(pages.size()));
        for (const std::uint32_t page : pages) put32(page);
        const std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
        for (const std::uint32_t page : pages) out.insert(out.end(), ram + page * sc2::kSyncPage, ram + (page + 1u) * sc2::kSyncPage);
        sc2::SaveState records;
        sc2::SaveFrame(records, false);
        const std::vector<std::uint8_t> packed = sc2::PackFrame(records);
        put32(static_cast<std::uint32_t>(packed.size()));
        out.insert(out.end(), packed.begin(), packed.end());
        put32(recomp_d3d8_swap_counter());
        blob = std::move(out);
        if (!netplay->SyncState(blob, 0)) return false;
    } else {
        blob.clear();
        if (!netplay->SyncState(blob, 0)) return false;
        const std::uint8_t *at = blob.data(), *end = blob.data() + blob.size();
        const auto get32 = [&at, end](std::uint32_t &v) {
            if (end - at < 4) return false;
            std::memcpy(&v, at, 4);
            at += 4;
            return true;
        };
        std::uint32_t count = 0, size = 0, shown = 0;
        bool ok = get32(count) && std::size_t(end - at) >= std::size_t(count) * (4u + sc2::kSyncPage);
        std::vector<std::uint32_t> pages(ok ? count : 0u);
        for (std::uint32_t i = 0; ok && i < count; ++i) ok = get32(pages[i]);
        const std::uint8_t *data = at;
        if (ok) at += std::size_t(count) * sc2::kSyncPage;
        sc2::SaveState records;
        ok = ok && get32(size) && std::size_t(end - at) >= size && sc2::UnpackFrame(at, size, records);
        if (ok) at += size;
        ok = ok && get32(shown);
        if (!ok) {
            netplay->ended = "The host's game did not come whole.";
            netplay->failed = true;
            return false;
        }
        sc2::PutPages(pages, data);
        {
            // (the host's records, set beside this side's own: the guest's RAM only is taken, unless
            // SC2_PRELUDE_RECORDS=1; the records are the host program's and crashed this side taken whole)
            sc2::SaveState own;
            sc2::SaveFrame(own, false);
            std::fprintf(stderr, "online: records host/own: kernel %zu/%zu (%s), sound device %zu/%zu (%s), buffers %zu/%zu "
                         "(%s), registers %s, host state %s\n",
                         records.kernel_memory.size(), own.kernel_memory.size(),
                         records.kernel_memory == own.kernel_memory ? "same" : "differ",
                         records.sound_device.size(), own.sound_device.size(),
                         records.sound_device == own.sound_device ? "same" : "differ",
                         records.sound_buffers.size(), own.sound_buffers.size(),
                         records.sound_buffers == own.sound_buffers ? "same" : "differ",
                         std::memcmp(&records.registers, &own.registers, sizeof own.registers) ? "differ" : "same",
                         std::memcmp(&records.host, &own.host, sizeof own.host) ? "differ" : "same");
            if (std::getenv("SC2_PRELUDE_RECORDS")) sc2::LoadFrame(records, false);
        }
        sc2_d3d8_set_swap_counter(shown);
        pageCount = count;
    }
    // (the match begins here, as at the title (beginMatch), but for the state, made alike above)
    netLive = true;
    netReads = 0;
    sc2_net_live = 1;
    sc2_net_reads = 0;
    netPlayers[0] = netPlayers[1] = RecompInputGamepad{};
    static const bool old = std::getenv("SC2_OLD_ROLLBACK") != nullptr; // (the port's own, to compare)
    sc2_net_frame = old ? netFrame : netNoFrame;
    netVirtualBase = recomp_kernel_interrupt_time();
    recomp_kernel_virtual_time = netVirtualBase;
    recomp_kernel_set_deterministic(true);
    beginAuto();
    if (!old && !ggStart()) return false;
    std::fprintf(stderr, "online: met at the parking after %.0f ms; %zu pages made alike, %.0f ms\n", met - began,
                 pageCount, perfNow() - met);
    return true;
}

/* In practice, each read of controller 1: the launcher's commands, the search and the match question (T6's: A accepts,
   B declines, a press counts after half a second with both let go), and the controls' hint */
static void practiceOverlay()
{
    static std::string shown = "?";
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    std::string key;
    if (practicePrompt) {
        recomp_overlay_set_title(&text, "MATCH FOUND");
        std::string info = practicePromptInfo, voice;
        if (const std::size_t v = info.find(" ^ "); v != std::string::npos) {
            voice = info.substr(v + 3);
            info.resize(v);
        }
        const std::size_t warn = info.find(" | ");
        if (!info.empty()) recomp_overlay_add_line(&text, "%s", info.substr(0, warn).c_str());
        if (warn != std::string::npos) recomp_overlay_add_line(&text, "%s", info.substr(warn + 3).c_str());
        if (!voice.empty()) recomp_overlay_add_line(&text, "%s", voice.c_str());
        recomp_overlay_add_line(&text, "%s", practicePrompt == 1 ? "A: accept     B: decline" : "Accepted - waiting for the opponent...");
        key = "p" + std::to_string(practicePrompt) + practicePromptInfo;
    } else if (practiceSearching) {
        static const char *const kDots[] = {"", ".", "..", "..."};
        const int dots = static_cast<int>(perfNow() / 400.0) % 4;
        recomp_overlay_set_hint(&text, (std::string("SEARCHING FOR A GAME") + kDots[dots]).c_str());
        key = "s" + std::to_string(dots);
    } else if (practiceHint != 0u) {
        --practiceHint;
        recomp_overlay_set_hint(&text, (std::string("START: practice menu (reset, CPU actions, command list)    ") +
                                        (sc2::MenuBackFree() ? "BACK" : "LEFT STICK CLICK") +
                                        ": training menu (frame data, hit boxes, recording, save states)").c_str());
        key = "h";
    }
    if (key == shown) return;
    shown = key;
    recomp_overlay_publish(&text);
}

static void practiceEndFrame() { practiceEnd(); }

static void practicePoll()
{
    if (!launcherMode()) return;
    static double polled, looked;
    const double now = perfNow();
    if (now - polled > 50.0) {
        polled = now;
        if (launcherPoll()) practiceHold = practiceStartNow = true;
        launcherWrite();
    }
    if (const char *file = std::getenv("SC2_SEARCH_FILE"); file && now - looked > 500.0) {
        looked = now;
        std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
        practiceSearching = in && in.get() == '1';
    }
}

static void practiceRead()
{
    practicePoll();
    // (the game's own pause menu left for its menus: the practice is over)
    const std::uint32_t current = *recomp_memory_u32(0x398700u);
    practiceOutside = current == 5u || current == 6u ? practiceOutside + 1u : 0u;
    if (practiceOutside > 10u && launcherMode()) practiceHold = true;
    if (practiceHold && launcherMode()) sc2_net_frame = practiceEndFrame; // (at the next frame's start)
    practiceOverlay();
}

// The match question's answer, as T6's: A accepts, B declines; a press counts once both were let go for half a second, so
// the buttons of a running combo do not answer it; the rest of the pad goes on to the fight.
static void practiceAnswerRead(RecompInputGamepad *pad)
{
    static int freeFrames;
    static bool before, eaten;
    const bool a = pad->analog_buttons[RECOMP_INPUT_ANALOG_A] > 0x40u, b = pad->analog_buttons[RECOMP_INPUT_ANALOG_B] > 0x40u;
    const bool held = a || b, pressed = held && !before;
    before = held;
    if (!held) eaten = false;
    if (practicePrompt == 1) {
        if (freeFrames >= 30 && pressed) {
            practiceAnswer = a ? "accept" : "decline";
            practicePrompt = a ? 2 : 0;
            freeFrames = 0;
            eaten = true;
            if (!a) sc2::TrainingNote("Declined - the search stopped");
            launcherWrite();
            std::fprintf(stderr, "practice: the match question: %s%c", practiceAnswer.c_str(), 10);
        } else {
            freeFrames = held ? 0 : freeFrames + 1;
        }
    }
    if (eaten) pad->analog_buttons[RECOMP_INPUT_ANALOG_A] = pad->analog_buttons[RECOMP_INPUT_ANALOG_B] = 0u;
}

// (the in-game menu: the launcher heard while it holds the game; true: it closes)
extern "C" int sc2_practice_poll(void)
{
    if (!practice || netPrelude) return 0;
    practicePoll();
    return practiceHold || practicePrompt == 1;
}
extern "C" int sc2_practice_closable(void) { return practice && !netPrelude && launcherMode(); }
extern "C" void sc2_practice_close(void) { practiceHold = true; }

/* SC2_LAG_TEST=1 (measurement): every 150 reads in practice player 1's A is held for 4 reads; reads, swaps and ms from
   the first read that has it to the fighter not being free (+0xa0), and from each read to the swap given to the
   display */
static const bool lagTest = std::getenv("SC2_LAG_TEST") != nullptr;
static double lagReadAt, lagPressAt, lagSwapSum, lagSwapMost;
static unsigned lagReads, lagPressRead, lagSwaps, lagSwapCount, lagReadsToBusy;
static bool lagWaiting;
static bool lagBusy() { return *static_cast<const std::uint8_t *>(recomp_memory(SC2_FIGHTER_RECORD + 0xa0u, 1u)) != 0u; }
static void lagRead(RecompInputGamepad *pad)
{
    lagReadAt = perfNow();
    ++lagReads;
    const unsigned in = lagReads % 150u;
    if (in < 4u) pad->analog_buttons[RECOMP_INPUT_ANALOG_X] = 0xff;
    if (in == 0u && !lagBusy()) {
        lagWaiting = true;
        lagPressAt = lagReadAt;
        lagPressRead = lagReads;
        lagSwaps = 0u;
    } else if (lagWaiting && lagBusy()) {
        lagReadsToBusy = lagReads - lagPressRead;
        std::fprintf(stderr, "lag test: read %u: busy seen at read +%u%c", lagPressRead, lagReadsToBusy, 10);
    }
}
extern "C" void sc2_lag_swap(void)
{
    if (!lagTest || !practice || netPrelude || lagReadAt == 0.0) return;
    const double now = perfNow(), ms = now - lagReadAt;
    lagSwapSum += ms;
    lagSwapMost = std::max(lagSwapMost, ms);
    if (++lagSwapCount == 300u) {
        std::fprintf(stderr, "lag test: last read to swap submitted: avg %.2f ms, most %.2f%c", lagSwapSum / lagSwapCount, lagSwapMost, 10);
        lagSwapSum = lagSwapMost = 0.0;
        lagSwapCount = 0u;
    }
    if (lagWaiting) {
        ++lagSwaps;
        if (lagBusy()) {
            std::fprintf(stderr, "lag test: press at read %u -> busy at swap %u, %.2f ms after the read%c", lagPressRead, lagSwaps,
                         now - lagPressAt, 10);
            lagWaiting = false;
        }
    }
}

/* SC2_INPUT_DIAG=1 (measurement): the time between the game's reads of controller 1 in practice, every 600 reads */
static void inputDiag()
{
    static const bool on = std::getenv("SC2_INPUT_DIAG") != nullptr;
    if (!on) return;
    static double last, sum, most, least;
    static unsigned n, over17, over25, under10;
    const double now = perfNow(), gap = last != 0.0 ? now - last : 0.0;
    last = now;
    if (gap == 0.0) return;
    if (n == 0u) { sum = most = 0.0; least = 1e9; over17 = over25 = under10 = 0u; }
    sum += gap;
    most = std::max(most, gap);
    least = std::min(least, gap);
    over17 += gap > 17.5;
    over25 += gap > 25.0;
    under10 += gap < 10.0;
    if (++n == 600u) {
        std::fprintf(stderr, "input diag: 600 reads: avg %.3f ms, least %.2f, most %.2f, over 17.5 ms %u, over 25 ms %u, under 10 ms %u%c",
                     sum / n, least, most, over17, over25, under10, 10);
        n = 0u;
    }
}

static bool sampleInput(std::uint32_t port, RecompInputGamepad *gamepad)
{
    if (port == 0u && practice && !netPrelude) practiceRead();
    if (port == 0u && practice && !netPrelude) inputDiag();
    if (netPrelude && !netLive && port < 2u) {
        // (the prelude: both players' controls from its script, from the title on)
        if (port == 0u && *recomp_memory_u32(0x398700u) == 5u && sc2_net_frame == nullptr) {
            sc2_net_frame = preludeFrame;
            sc2_hide_frames = std::getenv("SC2_TEST_SHOW_AUTO") == nullptr;
            if (netPreludeMuted < 0) netPreludeMuted = recomp_audio_output_muted();
            recomp_audio_output_set_muted(1);
            std::fprintf(stderr, "online: the prelude begins at the title\n");
        }
        *gamepad = sc2_net_frame == preludeFrame ? netAutoPads[port] : RecompInputGamepad{};
        if (port == 0u && launcherMode()) launcherWrite();
        return true;
    }
    if (netplay) {
        if (sampleNetplay(port, gamepad)) return true;
        if (netplay && netConnected) return false; // (no controllers 3 and 4 in a match)
        /* (hosting, no one yet: the player's own controls, below) */
    }
    if (netEndedShown && port == 0u) endedOverlay();
    /* Test aids (docs/bring-up.md, "Test runs"): SC2_TEST_ITEMS=FIRST-LAST owns those items (0x3BB368) at the
       title; SC2_TEST_KO=1 empties player 2's health every 240 reads of a fight (SC2_TEST_KO=1:N up to the Nth fight of a
       run that counts them, [0x398B30], so the run then ends on a loss and its results show). */
    if (const char *items = std::getenv("SC2_TEST_ITEMS"))
        if (port == 0u && *recomp_memory_u32(0x398700u) == 5u) {
            unsigned first = 0, last = 0;
            if (std::sscanf(items, "%u-%u", &first, &last) == 2 && last < 0x12cu)
                std::memset(recomp_memory(0x3bb368u + first, last - first + 1u), 3, last - first + 1u);
        }
    const char *test_ko = std::getenv("SC2_TEST_KO");
    unsigned ko_until = 0;
    if (test_ko && std::strchr(test_ko, ':')) ko_until = static_cast<unsigned>(std::atoi(std::strchr(test_ko, ':') + 1));
    if (port == 0u && test_ko && *recomp_memory_u32(0x398700u) == 7u &&
        (ko_until == 0u || *recomp_memory_u32(0x398b30u) <= ko_until)) {
        static std::uint64_t polls;
        if (++polls % 240u == 0u) {
            const float zero = 0.0f;
            std::memcpy(recomp_memory(SC2_FIGHTER_RECORD + 0x155cu + SC2_FIGHTER_STRIDE, 4u), &zero, 4u);
        }
    }
    if (port != 0u) {
        const bool sampled = !scriptedInputOnly && recomp_input_host_sample(port, gamepad);
        /* (after a match controller 2 stays plugged in, as it was, so the game does not stop to ask for it) */
        if (!sampled && port == 1u && (!netEnded.empty() || practice)) { // (practice: with no controller 2, player 2 a standing dummy)
            *gamepad = RecompInputGamepad{};
            return sc2::TrainingPad(port, gamepad, true);
        }
        return sc2::TrainingPad(port, gamepad, sampled);
    }
    bool sampled = recomp_input_pulse_source_sample(&inputPulses, gamepad);
    if (!scenePresses.empty()) {
        applyScenePresses(gamepad);
        sampled = true;
    }
    if (sc2::test_start_reads > 0) {
        --sc2::test_start_reads;
        gamepad->buttons |= RECOMP_INPUT_BUTTON_START;
        sampled = true;
    }
    if (lagTest && practice && !netPrelude) {
        lagRead(gamepad);
        sampled = true;
    }
    if (practice && !netPrelude && sampled) practiceAnswerRead(gamepad);
    static bool startHeld;
    const bool start = sampled && (gamepad->buttons & RECOMP_INPUT_BUTTON_START) != 0u;
    if (start != startHeld) {
        std::fprintf(stderr, "recomp input: Start %s poll=%llu\n", start ? "down" : "up",
            static_cast<unsigned long long>(inputPulses.sample_count));
        startHeld = start;
    }
    return sc2::TrainingPad(0u, gamepad, sampled);
}

#if !defined(__ANDROID__)
/* Same user folder the launcher uses (recomp-common/folders.h), so running the game directly shares saves and
   settings with the launcher without writing anything beside a disc image. */
static std::filesystem::path defaultPlayerFolder()
{
    const std::string name = recomp::FolderName("Soulcalibur II");
    if (const char *base = SDL_GetBasePath(); base != nullptr && *base) {
        const std::filesystem::path portable = recomp::PortableFolder(std::filesystem::u8path(base));
        if (!portable.empty()) return portable / std::filesystem::u8path(name);
    }
    if (char *pref = SDL_GetPrefPath("", name.c_str()); pref != nullptr) {
        const std::filesystem::path folder = std::filesystem::u8path(pref);
        SDL_free(pref);
        return folder;
    }
    return {};
}
#endif

static void printUsage(std::ostream &out, const char *argv0)
{
    std::string program = "soulcalibur2";
    if (argv0 != nullptr && *argv0) {
        const std::string name = std::filesystem::u8path(argv0).filename().u8string();
        if (!name.empty()) program = name;
    }
    out << "usage: " << program << " [--disc <image> | --xbe <Default.xbe> [--disc-root <dir>]] [options]\n\n"
        << "  --disc <image>           Xbox disc image (.iso); uses the user saves folder if --storage is omitted\n"
        << "  --xbe <Default.xbe>      extracted disc's Default.xbe (--disc-root defaults to its folder)\n"
        << "  --storage <dir>          saves folder (default: Soulcalibur II/saves for --disc or launcher setup)\n"
        << "  --settings <file>        settings.ini (default: Soulcalibur II/settings.ini if present)\n"
        << "  --fullscreen             start full screen\n"
        << "  --widescreen             start in 16:9 widescreen\n"
        << "  --mute                   start muted\n"
        << "  --free-camera            enable free camera (` or right-stick click)\n"
        << "  --guests <root.olk>      built Link and Heihachi file\n"
        << "  --disc-file <name>=<file>  override a disc file\n"
        << "  --host [port]            host an online match\n"
        << "  --join <code|addr[:port]>  join an online match\n"
        << "  --list-features          print available features and exit\n\n"
        << "With no --disc or --xbe, " << program << " uses the disc last chosen in the launcher.\n"
        << "You can also run soulcalibur2-launcher --play to launch straight into the game with all launcher settings.\n";
}

int main(int argc, char **argv)
{
#ifdef __ANDROID__
    sc2_android_start();
#endif
#ifdef _WIN32
    /* (Windows hands main its arguments in the system's code page; the program reads them as UTF-8, as the
       launcher writes them, and starts over with them so (startAgain)) */
    static std::vector<std::string> utf8Args;
    static std::vector<char *> utf8Argv;
    {
        int count = 0;
        if (LPWSTR *wideArgs = CommandLineToArgvW(GetCommandLineW(), &count)) {
            for (int i = 0; i < count; ++i) {
                const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, nullptr, 0, nullptr, nullptr);
                std::string arg(size > 0 ? static_cast<std::size_t>(size) : 1u, '\0');
                if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wideArgs[i], -1, arg.data(), size, nullptr, nullptr);
                arg.pop_back();
                utf8Args.push_back(std::move(arg));
            }
            LocalFree(wideArgs);
            for (std::string &arg : utf8Args) utf8Argv.push_back(arg.data());
            utf8Argv.push_back(nullptr);
            argc = count;
            argv = utf8Argv.data();
        }
    }
#endif
    {
        std::error_code ignored;
        runnerPath = std::filesystem::absolute(std::filesystem::u8path(argv[0]), ignored).u8string();
        runnerArgs.assign(argv + 1, argv + argc);
    }
    recomp_install_host_diagnostics();
    recomp_d3d_frame_breakdown = recomp_kernel_thread_times;
    std::string xbePath, discRoot, expectStop, milestoneLog, settingsPath, storageRoot, discImage, menuShot, exportPictures;
    std::vector<std::string> discFiles;
    std::string guestLink, guestHeihachi, guests;
    sc2::Netplay::Settings netSettings;
    bool netWanted = false;
    recomp_input_pulse_source_init(&inputPulses, recomp_input_host_sample, 0u);
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage(std::cout, argc > 0 ? argv[0] : nullptr);
            return 0;
        }
        if (argument == "--input-script-only") {
            scriptedInputOnly = true;
            inputPulses.base = nullptr;
            continue;
        }
        if (argument == "--skip-intro") {
            recomp_skip_intro = true;
            continue;
        }
        if (argument == "--list-features") {
            std::fputs(recomp::FeatureListText(sc2::Features()).c_str(), stdout);
            return 0;
        }
        if (argument == "--fullscreen" || argument == "--widescreen") {
            /* The presenter reads these when it starts. */
            setenv(argument == "--fullscreen" ? "RECOMP_D3D_FULLSCREEN" : "RECOMP_D3D_WIDESCREEN", "1", 1);
            continue;
        }
        if (argument == "--quick-match" && i + 1 < argc) {
            /* Read by the fight setup hook (sc2_hooks.c). */
            setenv("RECOMP_QUICK_MATCH", argv[++i], 1);
            continue;
        }
        if (argument == "--free-camera") {
            /* Read once, when the camera is installed (free_camera_adapter.c). */
            setenv("RECOMP_FREE_CAMERA", "1", 1);
            continue;
        }
        if (argument == "--mute") {
            /* Muted, not without sound: the in-game menu switches it back on. */
            recomp_audio_output_set_muted(1);
            continue;
        }
        if (argument == "--host") {
            netSettings.hosting = true;
            netWanted = true;
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) netSettings.port = std::atoi(argv[++i]);
            continue;
        }
        if (argument == "--join" && i + 1 < argc) {
            netSettings.address = argv[++i];
            netWanted = true;
            continue;
        }
        if (argument == "--net-name" && i + 1 < argc) { netSettings.name = argv[++i]; continue; }
        if (argument == "--net-pick" && i + 1 < argc) { netSettings.pick = argv[++i]; continue; }
        if (argument == "--net-stage" && i + 1 < argc) { netSettings.stage = std::atoi(argv[++i]); continue; }
        if (argument == "--net-renew" && i + 2 < argc) {
            netSettings.renewCode = argv[++i];
            netSettings.renewSecret = argv[++i];
            continue;
        }
        if (argument == "--net-again") { netAgain = true; continue; }
        if (argument == "--started-over") { startedOver = true; continue; }
        if (argument == "--net-fresh") { netFresh = true; continue; }
        if (argument == "--net-prelude") { netPrelude = true; continue; }
        if (argument == "--net-team") { netTeam = true; continue; }
        if (argument == "--net-team-size" && i + 1 < argc) { netTeamSize = std::clamp(std::atoi(argv[++i]), 1, 8); continue; }
        if (argument == "--practice" && i + 1 < argc) {
            const std::string spec = argv[++i];
            if (std::sscanf(spec.c_str(), "%d:%d:%d:%d:%d", &practicePick[0], &practiceOutfit[0], &practicePick[1],
                            &practiceOutfit[1], &practiceStage) < 4) {
                std::cerr << "recomp runner: --practice C1:O1:C2:O2[:STAGE]\n";
                return 64;
            }
            practice = netPrelude = true;
            continue;
        }
        if (argument == "--net-invite" && i + 1 < argc) {
            const std::string value = argv[++i];
            const std::size_t colon = value.find(':');
            if (colon != std::string::npos) {
                netSettings.invite = value.substr(0, colon);
                netSettings.from = value.substr(colon + 1);
            }
            continue;
        }
        if (argument == "--net-invite-name" && i + 1 < argc) { netSettings.inviteName = argv[++i]; continue; }
        if (argument == "--net-delay" && i + 1 < argc) { netSettings.delay = std::atoi(argv[++i]); continue; }
        if (argument == "--net-service" && i + 1 < argc) { netSettings.service = argv[++i]; continue; }
        if (argument == "--net-list") { netSettings.listed = true; continue; }
        if (argument == "--net-lag" && i + 1 < argc) { netSettings.fakeLag = std::atoi(argv[++i]); continue; }
        if (argument == "--input-scene" && i + 1 < argc) {
            ScenePress press;
            if (!parseScenePress(argv[++i], press)) {
                std::cerr << "recomp runner: --input-scene takes SCENE[#N]:POLL:KEY\n";
                return 64;
            }
            scenePresses.push_back(press);
            scriptedInputOnly = true;
            continue;
        }
        if (argument == "--input-start-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid Start poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-a-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_a_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid A poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-buttons-pulse-at" && i + 2 < argc) {
            const std::string value = argv[++i], maskValue = argv[++i];
            std::uint64_t poll = 0u;
            std::uint16_t mask = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            const auto parsedMask = std::from_chars(maskValue.data(), maskValue.data() + maskValue.size(), mask);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                parsedMask.ec != std::errc() || parsedMask.ptr != maskValue.data() + maskValue.size() ||
                mask == 0u || (mask & 0xff00u) != 0u ||
                !recomp_input_pulse_source_add_buttons_poll(&inputPulses, poll, mask)) {
                std::cerr << "recomp runner: invalid button poll/mask or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-analog-pulse-at" && i + 2 < argc) {
            const std::string value = argv[++i], indexValue = argv[++i];
            std::uint64_t poll = 0u;
            std::uint32_t index = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            const auto parsedIndex = std::from_chars(indexValue.data(), indexValue.data() + indexValue.size(), index);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                parsedIndex.ec != std::errc() || parsedIndex.ptr != indexValue.data() + indexValue.size() ||
                index >= RECOMP_INPUT_ANALOG_BUTTON_COUNT ||
                !recomp_input_pulse_source_add_analog_poll(&inputPulses, poll,
                    static_cast<std::uint8_t>(index), RECOMP_INPUT_ANALOG_PRESSED)) {
                std::cerr << "recomp runner: invalid analog poll/index or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-analog-pulse-every" && i + 4 < argc) {
            const std::string firstValue = argv[++i], periodValue = argv[++i], lastValue = argv[++i],
                indexValue = argv[++i];
            std::uint64_t first = 0u, period = 0u, last = 0u;
            std::uint32_t index = 0u;
            const auto parsedFirst = std::from_chars(firstValue.data(), firstValue.data() + firstValue.size(), first);
            const auto parsedPeriod = std::from_chars(periodValue.data(), periodValue.data() + periodValue.size(), period);
            const auto parsedLast = std::from_chars(lastValue.data(), lastValue.data() + lastValue.size(), last);
            const auto parsedIndex = std::from_chars(indexValue.data(), indexValue.data() + indexValue.size(), index);
            if (parsedFirst.ec != std::errc() || parsedFirst.ptr != firstValue.data() + firstValue.size() ||
                parsedPeriod.ec != std::errc() || parsedPeriod.ptr != periodValue.data() + periodValue.size() ||
                parsedLast.ec != std::errc() || parsedLast.ptr != lastValue.data() + lastValue.size() ||
                parsedIndex.ec != std::errc() || parsedIndex.ptr != indexValue.data() + indexValue.size() ||
                index >= RECOMP_INPUT_ANALOG_BUTTON_COUNT ||
                !recomp_input_pulse_source_add_analog_repeat(&inputPulses, first, period, last,
                    static_cast<std::uint8_t>(index))) {
                std::cerr << "recomp runner: invalid analog repeat or too many rules\n";
                return 64;
            }
            continue;
        }
        if ((argument != "--xbe" && argument != "--disc-root" && argument != "--settings" &&
             argument != "--disc" && argument != "--menu-shot" && argument != "--export-pictures" &&
             argument != "--storage" && argument != "--disc-file" && argument != "--guest-link" &&
             argument != "--guest-heihachi" && argument != "--guests" &&
             argument != "--expect-stop" && argument != "--milestone-log") || i + 1 >= argc) {
            printUsage(std::cerr, argc > 0 ? argv[0] : nullptr);
            return 64;
        }
        const std::string value = argv[++i];
        if (argument == "--xbe") xbePath = value;
        else if (argument == "--disc-root") discRoot = value;
        else if (argument == "--settings") settingsPath = value;
        else if (argument == "--disc") discImage = value;
        else if (argument == "--menu-shot") menuShot = value;
        else if (argument == "--export-pictures") exportPictures = value;
        else if (argument == "--storage") storageRoot = value;
        else if (argument == "--disc-file") discFiles.push_back(value);
        else if (argument == "--guest-link") guestLink = value;
        else if (argument == "--guest-heihachi") guestHeihachi = value;
        else if (argument == "--guests") guests = value;
        else if (argument == "--expect-stop") expectStop = value;
        else milestoneLog = value;
    }
#if !defined(__ANDROID__)
    const std::filesystem::path playerDir = defaultPlayerFolder();
    bool discFromLauncher = false;
    if (xbePath.empty() && discImage.empty() && !playerDir.empty()) {
        recomp::Ini launcher((playerDir / "launcher.ini").u8string());
        launcher.load();
        const std::string savedDisc = launcher.get("game", "disc");
        std::error_code ec;
        if (!savedDisc.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(savedDisc), ec)) {
            std::string ext = std::filesystem::u8path(savedDisc).extension().u8string();
            for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".xbe") xbePath = savedDisc;
            else discImage = savedDisc;
            discFromLauncher = true;
        }
    }
#endif
    if (xbePath.empty() == discImage.empty()) {
        std::cerr << "soulcalibur2: "
                  << (xbePath.empty()
                          ? "no disc image or Default.xbe given.\n"
                            "Pass --disc <image.iso> or --xbe <Default.xbe>, or open soulcalibur2-launcher once to choose your game.\n"
                            "Run with --help for options.\n"
                          : "give --disc or --xbe, not both.\n");
        return 64;
    }
#if !defined(__ANDROID__)
    if ((!discImage.empty() || discFromLauncher) && storageRoot.empty() && !playerDir.empty()) {
        const std::filesystem::path savesDir = playerDir / "saves";
        std::error_code ec;
        std::filesystem::create_directories(savesDir, ec);
        storageRoot = savesDir.u8string();
    }
    if (settingsPath.empty() && !playerDir.empty()) {
        const std::filesystem::path defaultSettings = playerDir / "settings.ini";
        std::error_code ec;
        if (std::filesystem::is_regular_file(defaultSettings, ec)) {
            settingsPath = defaultSettings.u8string();
        }
    }
#endif
    // (every draw shader this machine made, made again at the next start: no fight waits for one. The player's own
    // folder, which every start keeps; else the storage)
    if (!std::getenv("RECOMP_SHADER_LIST") && (!playerDir.empty() || !storageRoot.empty())) {
        const std::filesystem::path list = (!playerDir.empty() ? playerDir : std::filesystem::u8path(storageRoot)) / "draw_shaders.bin";
        setenv("RECOMP_SHADER_LIST", list.u8string().c_str(), 0);
    }
    // (the package's list beside the program, shaders/draw_shaders.bin)
    if (!std::getenv("RECOMP_SHADER_LIST_SHIPPED")) {
        std::error_code ec;
        const std::filesystem::path shipped = std::filesystem::path(argv[0]).parent_path() / "shaders" / "draw_shaders.bin";
        if (std::filesystem::is_regular_file(shipped, ec)) setenv("RECOMP_SHADER_LIST_SHIPPED", shipped.u8string().c_str(), 0);
    }
    if (!discImage.empty() && storageRoot.empty()) {
        std::cerr << "recomp runner: --disc needs --storage (nothing is written beside an image)\n";
        return 64;
    }
    recomp_stop_configure(expectStop.empty() ? nullptr : expectStop.c_str(),
        milestoneLog.empty() ? nullptr : milestoneLog.c_str());
    setenv("RECOMP_WINDOW_TITLE", "Soulcalibur II", 0);
    setenv("SDL_WINDOWS_INTRESOURCE_ICON", "1", 0); // (the window's icon: the program's, sc2.rc)
    setenv("SDL_WINDOWS_INTRESOURCE_ICON_SMALL", "1", 0);
    setenv("RECOMP_SCREENSHOT_NAME", "Soulcalibur II", 0); // (Pictures/Soulcalibur II/Soulcalibur II-....png)
    int shader = 0;
    if (!settingsPath.empty()) {
        const sc2::Settings settings = sc2::LoadSettings(settingsPath);
        shader = settings.shader;
        sc2::TrainingState().health_refill = settings.health_refill;
        sc2::TrainingState().infinite_time = settings.infinite_time;
        sc2::TrainingState().input_display = settings.input_display;
        sc2::TrainingState().combo_readout = settings.combo_readout;
        sc2::TrainingState().hit_boxes = settings.hit_boxes;
        sc2::TrainingState().frame_data = settings.frame_data;
        sc2::TrainingState().no_damage[0] = settings.no_damage[0];
        sc2::TrainingState().no_damage[1] = settings.no_damage[1];
        sc2::TrainingState().one_hit = settings.one_hit;
        sc2::TrainingState().fps_counter = settings.fps_counter;
        sc2::OnlineDisplay() = {settings.online_ping, settings.online_delay, settings.online_hide_names};
        /* (kernel_config.c: XC_LANGUAGE; the European program's alone, the US disc having English only) */
        if (SC2_PAL && std::getenv("RECOMP_XBOX_LANGUAGE") == nullptr)
            setenv("RECOMP_XBOX_LANGUAGE", std::to_string(sc2::XboxLanguage(settings.language)).c_str(), 1);
        if (settings.scale > 1u && std::getenv("RECOMP_D3D_SCALE") == nullptr)
            setenv("RECOMP_D3D_SCALE", std::to_string(settings.scale).c_str(), 1);
        sc2::ApplyBindings(settingsPath);
        sc2::ApplyTextures(settingsPath, settings.hd_textures, settings.texture_dump);
    } else {
        sc2::ApplyTextures({}, false, false);
    }
    sc2::InstallMenu(settingsPath, menuShot);
    if (!exportPictures.empty()) sc2::ExportPicturesAt(exportPictures);
    sc2::SetShader(shader);
    /* A disc image: its Default.xbe is read out of it, and the D: drive is
       served from it (disc_image.h). */
    std::vector<std::uint8_t> imageXbe;
    if (!discImage.empty()) {
        RecompDiscImageEntry entry;
        if (!recomp_disc_image_open(discImage.c_str()) ||
            !recomp_disc_image_lookup("default.xbe", &entry) || entry.directory || entry.size > (64u << 20)) {
            std::cerr << "recomp runner: the disc image has no Default.xbe\n";
            return 1;
        }
        imageXbe.resize(static_cast<std::size_t>(entry.size));
        if (recomp_disc_image_read(entry.offset, imageXbe.data(), imageXbe.size()) != imageXbe.size()) {
            std::cerr << "recomp runner: the disc image could not be read\n";
            return 1;
        }
    }
    const auto parsed = discImage.empty() ? doaxbv::XbeParser().parse(std::filesystem::u8path(xbePath))
                                          : doaxbv::XbeParser().parse(std::move(imageXbe));
    if (!parsed.ok()) {
        std::cerr << "recomp runner: " << parsed.errors.front() << '\n';
        return 1;
    }
    /* The other release's disc (sc2_xbe.h): its own program, beside this one, is started with the same arguments. */
    if (parsed.metadata.fileSha256 == SC2_OTHER_XBE_SHA256 || parsed.metadata.fileSha256 == SC2_OTHER_RETAIL_XBE_SHA256)
        handToOtherRelease();
    /* The port's media-patched copy, or the retail disc's (sc2_xbe.h). */
    const bool retailXbe = parsed.metadata.fileSha256 == SC2_RETAIL_XBE_SHA256;
    if (parsed.metadata.fileSha256 != RECOMP_XBE_SHA256 && !retailXbe) {
        std::cerr << "recomp runner: XBE SHA-256 " << parsed.metadata.fileSha256
                  << " is neither Soulcalibur II (" << (SC2_PAL ? "Europe" : "USA")
                  << ") retail nor the program target\n";
        return 1;
    }
    // (guest RAM: its written pages listed where the system can, for rollback's save states, sc2_dirty.h)
    std::uint8_t *const memory = sc2::dirty::AllocateRam(RECOMP_XBOX_RAM_SIZE);
    std::string error;
    {
        std::vector<std::uint8_t> image(RECOMP_XBOX_RAM_SIZE, 0u);
        // The file was authenticated by SHA-256 above; the patched copy's .text digest is stale (docs/bring-up.md).
        if (!memory || !recomp_load_xbe(parsed, image, error, true)) {
            std::cerr << "recomp runner: " << (memory ? error : std::string("no memory for guest RAM")) << '\n';
            return 1;
        }
        std::memcpy(memory, image.data(), image.size());
    }
    if (retailXbe) {
        if (memory[SC2_MEDIA_PATCH_ADDRESS] != SC2_MEDIA_PATCH_RETAIL) {
            std::cerr << "recomp runner: the retail XBE's media check byte is not where it should be\n";
            return 1;
        }
        memory[SC2_MEDIA_PATCH_ADDRESS] = SC2_MEDIA_PATCH_BYTE;
    }
    const auto imageEnd = static_cast<std::uint64_t>(parsed.metadata.header.baseAddress) +
        parsed.metadata.header.sizeImage;
    if (!xbox_HeapSetImageEnd(static_cast<std::uint32_t>(imageEnd))) {
        std::cerr << "recomp runner: invalid heap image boundary\n";
        return 1;
    }
    if (!discImage.empty()) discRoot = std::filesystem::absolute(std::filesystem::u8path(discImage)).string();
    if (discRoot.empty()) discRoot = std::filesystem::absolute(std::filesystem::u8path(xbePath)).parent_path().string();
    const auto discPath = std::filesystem::u8path(discRoot);
    std::error_code filesystemError;
    if (discImage.empty() && !std::filesystem::is_directory(discPath, filesystemError)) {
        std::cerr << "recomp runner: disc root is not a readable directory\n";
        return 1;
    }
    recomp_disc_root_path = discRoot.c_str();
    if (!storageRoot.empty()) recomp_storage_root_path = storageRoot.c_str();
    /* (a guests' file built from the other release's disc is left out: the game reads its menus by this disc's numbers) */
    if (!guests.empty() && !sc2::GuestFileMatches(guests.c_str())) guests.clear();
    if (!guests.empty()) discFiles.push_back("root.olk=" + guests);
    else if (!guestLink.empty()) discFiles.push_back("root.olk=" + guestLink);
    else if (!guestHeihachi.empty()) discFiles.push_back("root.olk=" + guestHeihachi);
    for (const std::string &file : discFiles) {
        const std::size_t equals = file.find('=');
        const std::string name = file.substr(0, equals);
        const std::string host = equals == std::string::npos ? std::string()
            : std::filesystem::absolute(std::filesystem::u8path(file.substr(equals + 1))).string();
        if (host.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(host), filesystemError) ||
            !recomp_disc_override(name.c_str(), host.c_str())) {
            std::cerr << "recomp runner: --disc-file " << file << " is not NAME=FILE of a readable file\n";
            return 64;
        }
        if (name == "root.olk") sc2::SetPicturesRootOlk(host);   /* (the gallery's pictures are read from it) */
    }
    std::string matchStorage;
    {
        /* Online: both play a match from the host's save, in a folder of the match's own beside the player's
           (sc2_netplay.h); the guests' file must be the same build. Kept for hosting from the menu too, with the
           launcher's name for the lobby (launcher.ini beside settings.ini). */
        const std::filesystem::path own = storageRoot.empty() ? discPath / ".recomp-storage"
                                                              : std::filesystem::u8path(storageRoot);
        netSettings.storage = own.u8string();
        matchStorage = (own.parent_path() / (own.filename().u8string() + "-online")).u8string();
        netSettings.matchStorage = matchStorage;
        std::error_code sizeError;
        /* (the guests' file by its size and the converter version stamped beside it, guest/build.h's
           GUESTS_VERSION: "0" for a file built before stamps) */
        std::string guestsStamp = "0";
        if (!guests.empty()) {
            std::ifstream stamp(std::filesystem::u8path(guests + ".version"));
            std::string word;
            int version = 0;
            if (stamp >> word >> version && word == "sc2-guests") guestsStamp = std::to_string(version);
        }
        netSettings.build = std::string(RECOMP_XBE_SHA256).substr(0, 16) + "/" +
            (guests.empty() ? std::string("-")
                            : std::to_string(std::filesystem::file_size(std::filesystem::u8path(guests), sizeError)) +
                                  "v" + guestsStamp);
        netBase = netSettings;
        if (netBase.name.empty() && !settingsPath.empty()) {
            recomp::Ini launcher((std::filesystem::u8path(settingsPath).parent_path() / "launcher.ini").u8string());
            launcher.load();
            netBase.name = launcher.get("online", "name").substr(0, sc2::Netplay::kNameMost);
        }
        std::atexit([] {
            if (netplay) {
                netplay->Leave();
                netplay->Report(true);
            }
        });
    }
    if (launcherMode()) {
        recomp_skip_intro = true;
        netPrelude = true;
        launcherWrite();
    }
    /* (an online game: the frames keep the session's clock (ggPace), never the display's. The presenter is made
       without a wait for the display's refresh (mailbox), or its thread at the monitor's rate held the game back
       through its two-frame queue. Set before the window is made: the present mode is chosen there) */
    if (netPrelude) recomp_d3d_presenter_set_immediate_present(true);
    if (netPrelude) {
        // (the hidden start, before the loading and the title, is not heard either: muted from here to the fight)
        netPreludeMuted = recomp_audio_output_muted();
        recomp_audio_output_set_muted(1);
    }
    if (netPrelude && !playerDir.empty()) netIconsFolder = (playerDir / "icons").u8string();
    if (netWanted) {
        recomp_skip_intro = true; /* (online, both games go straight to the title: the match begins there) */
        /* (a host's game is the player's own while it waits, and goes where they take it: when someone comes it starts
           over for the match, as hosting from the menu does, and that copy (--net-fresh) takes them in) */
        if (netSettings.hosting && !netFresh && !netPrelude) netSettings.restartOnHello = true;
        preludeSettings = netSettings;
        netplay = std::make_unique<sc2::Netplay>(netSettings);
        std::string netError;
        if (netSettings.hosting) {
            /* The host's game starts at once and waits in the background (sampleNetplay), showing its code; it
               plays the match from its own save, which the other side is sent. */
            if (!netplay->Begin(netError)) {
                std::fprintf(stderr, "online: %s\n", netError.c_str());
                return 3;
            }
        } else if (netPrelude) {
            /* (the prelude: the other side asks for the match once its game is parked, preludeMeet) */
        } else {
            /* The other player's game connects first: it plays the match from the host's save. Coming back after a
               drop (--net-again), it tries for a minute (the host hosts again once it has noticed), and then plays
               alone from the player's own save. */
            bool connected = netplay->Connect(netError);
            const auto began = std::chrono::steady_clock::now();
            while (!connected && netAgain && std::chrono::steady_clock::now() - began < std::chrono::seconds(60)) {
                std::fprintf(stderr, "online: not back yet (%s); trying again\n", netError.c_str());
                netplay.reset();
                std::this_thread::sleep_for(std::chrono::seconds(2));
                netplay = std::make_unique<sc2::Netplay>(netSettings);
                connected = netplay->Connect(netError);
            }
            if (!connected && !netAgain) {
                std::fprintf(stderr, "online: %s\n", netError.c_str());
                return 3;
            }
            if (connected) {
                netConnected = true;
                storageRoot = matchStorage;
                recomp_storage_root_path = storageRoot.c_str();
            } else {
                std::fprintf(stderr, "online: couldn't get back to the match (%s); playing alone\n", netError.c_str());
                netEnded = "Couldn't get back to the match: " + netError;
                netEndedShown = 60u * 8u;
                netplay.reset();
            }
        }
        if (netplay && !netPrelude) {
            sc2_net_seed = netplay->Seed();
            sc2_net_seeded = 1;
        }
    }
    /* (a save the game asked to have deleted, sc2_dashboard: set aside before it is opened) */
    if (!storageRoot.empty()) {
        if (!netplay) dashboardStorage = storageRoot;
        setSavesAside(std::filesystem::u8path(storageRoot));
    }
    const auto saveInitialize = [&] {
        return storageRoot.empty() ? recomp_save_initialize(recomp_disc_root_path)
                                   : recomp_save_initialize_storage(recomp_storage_root_path);
    };
    bool saveReady = saveInitialize();
    /* (a copy started over or again for online play: on Windows the copy before it is still ending as this one
       starts, its hold on the save journal (opened for itself alone) with it, so this one asks again a while) */
    for (int tries = 0; !saveReady && (netFresh || netAgain || startedOver) && tries < 100; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        saveReady = saveInitialize();
        if (saveReady) std::fprintf(stderr, "online: the save journal opened after %d ms\n", (tries + 1) * 100);
    }
    if (!saveReady) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        std::cerr << "recomp stop: save:journal-unreadable (the save folder couldn't be opened) exit_code=2\n";  // (for the launcher)
        return 2;
    }
    const RecompMemoryRegion region = {0u, RECOMP_XBOX_RAM_SIZE, memory};
    recomp_runtime_init(&region, 1u, nullptr, 0u, nullptr, 0u);
    if (!guests.empty()) {
        if (!sc2::ApplyGuestRoster(guests.c_str())) std::cerr << "recomp runner: " << guests << " is not a guests' root.olk\n";
    } else if (!guestLink.empty()) sc2::ApplyGuestLink();
    else if (!guestHeihachi.empty()) sc2::ApplyGuestHeihachi();
    recomp_runtime_set_lookup(recomp_program_lookup);
    for (unsigned i = 0, h = 0; i < sc2_direct_target_count; ++i) {
        while (h < sc2_direct_hooked_count && sc2_direct_hooked[h] < sc2_direct_targets[i]) ++h;
        const bool asked = h < sc2_direct_hooked_count && sc2_direct_hooked[h] == sc2_direct_targets[i];
        if (!asked && recomp_lookup_manual(sc2_direct_targets[i]) != nullptr) {
            std::cerr << "recomp runner: a hooked function (0x" << std::hex << sc2_direct_targets[i] << std::dec
                      << ") the program calls directly: build again (tools/build_game.py direct_calls)\n";
            return 1;
        }
    }
    recomp_input_adapter_reset();
    recomp_input_adapter_set_source(sampleInput);
    /* The free camera and the scene freeze run only with RECOMP_FREE_CAMERA=1;
       off, input samples and task switches go unhooked. */
    recomp_free_camera_install();
    if (!scriptedInputOnly) recomp_input_adapter_set_feedback_sink(recomp_input_host_set_vibration);
    std::atexit(recomp_input_host_stop_vibration);
    std::atexit(recomp_d3d8_shutdown);
    std::atexit(recomp_dsound_api_shutdown);
    recomp_runtime.registers.esp = XBOX_STARTUP_STACK_TOP;
    std::fprintf(stderr, "recomp runner: entry=0x%08x image_end=0x%08x native-d3d8-dsound\n",
        parsed.metadata.selectedEntryPoint, static_cast<unsigned>(imageEnd));
    recomp_watchdog_start();
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    recomp_dispatch_indirect_site(parsed.metadata.selectedEntryPoint,
        XBOX_STARTUP_STACK_TOP, __FILE__, __LINE__);
    recomp_kernel_run_threads();
    recomp_stop(0, "program-returned");
}
