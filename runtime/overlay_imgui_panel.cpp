/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The overlay's look and layout in Dear ImGui: the runtime's panel (the free camera's, online play's) under the life
   bars, its hint along the bottom, and Soulcalibur II's training panels (sc2_panel.h) down each player's side: the
   combo readout under the life bar, the input display below it. A note for the player goes above the hint. */
#include "overlay_imgui_panel.h"
#include "sc2_panel.h"
#include "sc2_touch.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>

/* (online, as Tekken 6 Online's net HUD: the ping and the opponent's FPS, top centre over the life bars; gone 2 s
   after the last value, at the session's end; SC2_NET_HUD=0 leaves it out. Set by runner.cpp, drawn here.) */
#include <atomic>
#include <chrono>
static std::atomic<int> net_hud_ping{-1}, net_hud_fps10{0};
static std::atomic<long long> net_hud_at{0};
static long long NetHudNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" void sc2_net_hud_set(int ping_ms, int fps10)
{
    static const bool off = [] { const char *v = std::getenv("SC2_NET_HUD"); return v && v[0] == '0'; }();
    if (off) return;
    net_hud_ping = ping_ms;
    net_hud_fps10 = fps10;
    net_hud_at = NetHudNow();
}
extern "C" bool sc2_net_hud_active(void)
{
    const long long at = net_hud_at.load();
    return at != 0 && NetHudNow() - at < 2000;
}

namespace {

/* Text is sized for the target's height, as the game's own is: ImGui's
   13-pixel font at kReferenceHeight lines, built once at that size. */
constexpr float kReferenceHeight = 480.0f;
constexpr float kFontPixels = 13.0f;
float panel_scale = 1.0f;

constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoNav;

/* One player's side: x at the picture's edge (pivot 0 on the left, 1 on the right) */
void Side(const Sc2Panel &panel, int player, float width, float height)
{
    const float x = player == 0 ? width * 0.02f : width * 0.98f, pivot = player == 0 ? 0.0f : 1.0f;
    char name[32];
    if (panel.readout_rows[player] != 0u) {
        ImGui::SetNextWindowPos(ImVec2(x, height * 0.27f), 0, ImVec2(pivot, 0.0f));
        snprintf(name, sizeof name, "sc2-readout-%d", player);
        ImGui::Begin(name, nullptr, kFlags);
        for (unsigned i = 0u; i < panel.readout_rows[player]; ++i) {
            if (i == 0u) ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), "%s", panel.readout[player][i]);
            else ImGui::TextUnformatted(panel.readout[player][i]);
        }
        ImGui::End();
    }
    if (panel.input_rows[player] != 0u) {
        ImGui::SetNextWindowPos(ImVec2(x, height * 0.46f), 0, ImVec2(pivot, 0.0f));
        /* (a fixed width, so the box keeps its size as the rows change) */
        const float fixed = ImGui::CalcTextSize(" 99  9  A+B+K+G  ").x + ImGui::GetStyle().WindowPadding.x * 2.0f;
        ImGui::SetNextWindowSizeConstraints(ImVec2(fixed, 0.0f), ImVec2(fixed, height));
        snprintf(name, sizeof name, "sc2-input-%d", player);
        ImGui::Begin(name, nullptr, kFlags);
        for (unsigned i = 0u; i < panel.input_rows[player]; ++i) {
            if (i == 0u) ImGui::TextUnformatted(panel.input[player][i]);
            else ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.82f, 1.0f), "%s", panel.input[player][i]);
        }
        ImGui::End();
    }
}

/* The hit boxes, over the picture (the game's 640x480 frame, kept to its aspect in the window, as the presenter
   places it), under the panels */
void Shapes(const Sc2Panel &panel, float width, float height)
{
    if (panel.shape_count == 0u) return;
    static const float aspect = std::getenv("RECOMP_D3D_WIDESCREEN") ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = width, h = height;
    if (width / height > aspect) w = height * aspect;
    else h = width / aspect;
    const float left = (width - w) * 0.5f, top = (height - h) * 0.5f, sx = w / 640.0f, sy = h / 480.0f;
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const float thick = 1.5f * panel_scale;
    for (unsigned i = 0u; i < panel.shape_count && i < SC2_PANEL_SHAPES; ++i) {
        const Sc2PanelShape &shape = panel.shapes[i];
        const ImVec2 a(left + shape.x0 * sx, top + shape.y0 * sy), b(left + shape.x1 * sx, top + shape.y1 * sy);
        switch (shape.kind) {
        case SC2_SHAPE_HURT:
            draw->AddCircleFilled(a, shape.radius * sy, IM_COL32(60, 140, 255, 40));
            draw->AddCircle(a, shape.radius * sy, IM_COL32(90, 170, 255, 200), 0, thick);
            break;
        case SC2_SHAPE_PUSH:
            draw->AddCircle(a, shape.radius * sy, IM_COL32(80, 230, 110, 150), 0, thick);
            break;
        case SC2_SHAPE_ATTACK:
            draw->AddLine(a, b, IM_COL32(255, 190, 60, 170), thick * 2.0f);
            break;
        default:
            draw->AddLine(a, b, IM_COL32(255, 50, 40, 255), thick * 3.0f);
            draw->AddCircleFilled(b, thick * 2.5f, IM_COL32(255, 50, 40, 255));
            break;
        }
    }
}

/* The frame meter: a row of cells for each fighter above the bottom of the picture, a frame a cell, the newest
   on the right: free (grey), startup (green), active (red), recovery (blue), hit or guard stun (yellow) */
void Meter(const Sc2Panel &panel, float width, float height)
{
    if (panel.meter_length == 0u) return;
    static const ImU32 kColours[] = {IM_COL32(0, 0, 0, 0), IM_COL32(70, 70, 80, 200), IM_COL32(60, 200, 90, 240),
        IM_COL32(240, 60, 50, 255), IM_COL32(60, 120, 240, 240), IM_COL32(240, 200, 50, 240)};
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    const float cell = width * 0.9f / SC2_PANEL_METER, rows = cell * 1.6f;
    const float left = width * 0.05f, top = height * 0.80f;
    draw->AddRectFilled(ImVec2(left - 3.0f, top - 3.0f), ImVec2(left + cell * SC2_PANEL_METER + 3.0f, top + rows * 2.0f + 5.0f),
        IM_COL32(5, 15, 25, 170), 3.0f);
    for (int p = 0; p < 2; ++p) {
        const float y = top + p * (rows + 2.0f);
        for (unsigned i = 0u; i < panel.meter_length && i < SC2_PANEL_METER; ++i) {
            const unsigned kind = panel.meter[p][i] < 6u ? panel.meter[p][i] : 0u;
            if (kind == SC2_METER_NONE) continue;
            const float x = left + (SC2_PANEL_METER - panel.meter_length + i) * cell;
            draw->AddRectFilled(ImVec2(x + 0.5f, y), ImVec2(x + cell - 0.5f, y + rows), kColours[kind]);
        }
    }
}

}  // namespace

void recomp_overlay_imgui_style(float target_height)
{
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    panel_scale = target_height > kReferenceHeight ? target_height / kReferenceHeight : 1.0f;
    ImFontConfig font{};
    font.SizePixels = kFontPixels * panel_scale;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.06f, 0.10f, 0.70f);
    style.ScaleAllSizes(panel_scale);
}

static void NetHud(float width, float height)
{
    if (!sc2_net_hud_active()) return;
    const int ping = net_hud_ping.load(), fps10 = net_hud_fps10.load();
    char ping_text[16], fps_text[16];
    if (ping >= 0) std::snprintf(ping_text, sizeof ping_text, "%d ms", ping);
    else std::snprintf(ping_text, sizeof ping_text, "-");
    if (fps10 > 0) std::snprintf(fps_text, sizeof fps_text, "%d", (fps10 + 5) / 10);
    else std::snprintf(fps_text, sizeof fps_text, "-");
    const ImU32 green = IM_COL32(110, 230, 120, 255), yellow = IM_COL32(240, 210, 80, 255), red = IM_COL32(240, 90, 80, 255),
                label = IM_COL32(200, 200, 210, 255);
    const ImU32 ping_colour = ping < 0 ? label : ping < 90 ? green : ping < 160 ? yellow : red;
    const ImU32 fps_colour = fps10 <= 0 ? label : fps10 >= 585 ? green : fps10 >= 540 ? yellow : red;
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 0.95f;
    const char *parts[4] = {"PING ", ping_text, "   OPP FPS ", fps_text};
    const ImU32 colours[4] = {label, ping_colour, label, fps_colour};
    float total = 0.0f;
    for (const char *part : parts) total += font->CalcTextSizeA(size, 1e9f, 0.0f, part).x;
    const float pad = size * 0.45f, top = height * 0.008f, x0 = width * 0.5f - total * 0.5f;
    draw->AddRectFilled(ImVec2(x0 - pad, top), ImVec2(x0 + total + pad, top + size + pad * 1.2f), IM_COL32(10, 12, 20, 190),
                       size * 0.35f);
    float x = x0;
    for (int i = 0; i < 4; ++i) {
        draw->AddText(font, size, ImVec2(x, top + pad * 0.6f), colours[i], parts[i]);
        x += font->CalcTextSizeA(size, 1e9f, 0.0f, parts[i]).x;
    }
}

void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height)
{
    NetHud(width, height);
    Sc2Panel panel;
    sc2_panel_take(&panel);
    sc2_touch_draw(width, height);
    if (sc2_touch_menu()) return; /* (over the in-game menu: the touch controls alone) */
    Shapes(panel, width, height);
    Meter(panel, width, height);
    Side(panel, 0, width, height);
    Side(panel, 1, width, height);
    if (panel.fps[0] != '\0') {
        ImGui::SetNextWindowPos(ImVec2(width * 0.98f, height * 0.02f), 0, ImVec2(1.0f, 0.0f));
        ImGui::Begin("sc2-fps", nullptr, kFlags);
        ImGui::TextUnformatted(panel.fps);
        ImGui::End();
    }
    if (panel.note[0] != '\0') {
        ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * (text.hint[0] != '\0' ? 0.86f : 0.95f)), 0, ImVec2(0.5f, 1.0f));
        ImGui::Begin("sc2-note", nullptr, kFlags);
        ImGui::TextUnformatted(panel.note);
        ImGui::End();
    }
    if (text.title[0] != '\0' || text.count != 0u) {
        /* A small box under the life bars, centred: the title, then one line per entry. */
        ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.17f), 0, ImVec2(0.5f, 0.0f));
        ImGui::Begin("recomp-overlay-panel", nullptr, kFlags);
        if (text.title[0] != '\0') ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), "%s", text.title);
        for (unsigned i = 0u; i < text.count; ++i) ImGui::TextUnformatted(text.line[i]);
        ImGui::End();
    }
    /* The hint: the controls, along the bottom. */
    if (text.hint[0] == '\0') return;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.97f), 0, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width * 0.96f, height));
    ImGui::Begin("recomp-overlay-hint", nullptr, kFlags);
    ImGui::PushTextWrapPos(width * 0.94f);
    ImGui::TextUnformatted(text.hint);
    ImGui::PopTextWrapPos();
    ImGui::End();
}
