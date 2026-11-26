/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The player's key and button layout (sc2_settings.cpp's table, as
   settings.ini chooses it) given to the runtime's host input. */
#include "sc2_settings.h"

#include "input_host_win32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace sc2 {

void ApplyBindings(const std::string &path)
{
    static const struct { const char *name; RecompInputControl control; } kControls[] = {
        {"up", RECOMP_INPUT_CONTROL_UP}, {"down", RECOMP_INPUT_CONTROL_DOWN},
        {"left", RECOMP_INPUT_CONTROL_LEFT}, {"right", RECOMP_INPUT_CONTROL_RIGHT},
        {"start", RECOMP_INPUT_CONTROL_START}, {"back", RECOMP_INPUT_CONTROL_BACK},
        {"a", RECOMP_INPUT_CONTROL_A}, {"b", RECOMP_INPUT_CONTROL_B},
        {"x", RECOMP_INPUT_CONTROL_X}, {"y", RECOMP_INPUT_CONTROL_Y},
        {"black", RECOMP_INPUT_CONTROL_BLACK}, {"white", RECOMP_INPUT_CONTROL_WHITE},
        {"ltrigger", RECOMP_INPUT_CONTROL_LEFT_TRIGGER}, {"rtrigger", RECOMP_INPUT_CONTROL_RIGHT_TRIGGER},
    };
    const recomp::BindTable &table = BindingTable();
    const recomp::Bindings bindings = recomp::LoadBindings(path, table);
    RecompInputHostLayout layout;
    for (uint32_t port = 0; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        for (uint32_t control = 0; control < RECOMP_INPUT_CONTROL_COUNT; ++control)
            layout.controls[port][control] = {-1, -1, -1};
        for (uint32_t combo = 0; combo < RECOMP_INPUT_HOST_COMBO_COUNT; ++combo)
            layout.combos[port][combo] = {-1, 0};
    }
    for (int action = 0; action < table.count; ++action) {
        for (const auto &entry : kControls) {
            if (std::strcmp(entry.name, table.actions[action].name) != 0) continue;
            for (int player = 0; player < table.players; ++player) {
                RecompInputHostBinding &binding = layout.controls[player][entry.control];
                const SDL_Scancode key = bindings.key[player][action];
                binding.scancode = key != SDL_SCANCODE_UNKNOWN ? static_cast<int16_t>(key) : -1;
                binding.pad_button = static_cast<int16_t>(bindings.pad[player][action].button);
                binding.pad_axis = static_cast<int16_t>(bindings.pad[player][action].axis);
            }
        }
    }
    /* [keycombo1] / [keycombo2]: one key for several controls, "x+y = U" (the
       launcher's KEYBOARD MAPPING, as Tekken 6 Online's 1+2 keys) */
#ifdef __cpp_char8_t
    std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())));
#else
    std::ifstream file(std::filesystem::u8path(path));
#endif
    const auto trim = [](const std::string &text) {
        const size_t first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    std::string line, section;
    uint32_t used[RECOMP_INPUT_PORT_COUNT] = {};
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty() && line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos || section.rfind("keycombo", 0) != 0) continue;
        const int player = std::atoi(section.c_str() + 8) - 1;
        if (player < 0 || player >= table.players || used[player] >= RECOMP_INPUT_HOST_COMBO_COUNT) continue;
        const SDL_Scancode key = recomp::KeyFromText(trim(line.substr(equals + 1)));
        if (key == SDL_SCANCODE_UNKNOWN) { std::fprintf(stderr, "sc2 keys: no key \"%s\"\n", line.c_str()); continue; }
        uint16_t controls = 0;
        const std::string names = trim(line.substr(0, equals)) + "+";
        for (size_t at = 0, plus; (plus = names.find('+', at)) != std::string::npos; at = plus + 1)
            for (const auto &entry : kControls)
                if (names.substr(at, plus - at) == entry.name)
                    controls |= static_cast<uint16_t>(1u << entry.control);
        if (controls != 0) layout.combos[player][used[player]++] = {static_cast<int16_t>(key), controls};
    }
    std::fprintf(stderr, "sc2 keys: %u combination keys (player 1)\n", used[0]);
    recomp_input_host_set_layout(&layout);
}

} // namespace sc2
