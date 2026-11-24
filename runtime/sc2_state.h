// SPDX-License-Identifier: GPL-3.0-or-later
// Save states: the game as it is at the start of a frame (0x32630, the frame routine's first call, on the game's
// main thread), to be put back there later. The free camera's freeze puts the same frame back every frame; online
// play's rollback goes back to the frame a late control changed (sc2_rollback.h).
//
// A state is all of guest RAM, the main thread's registers and FPU, the host's side of what the game's run depends
// on (sc2_hooks.h, Sc2HostState), and the host's records that the guest's refer to: the kernel's allocations and the
// DirectSound device and buffers. Putting one back leaves the other guest threads' own memory (their
// objects, stacks, TLS) and the kernel's page alone: they are run until each waits before a state is taken or put
// back (recomp_kernel_drain_others), so they wait where they waited, and only the main thread's frame goes back.
#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include "runtime.h"
#include "sc2_hooks.h"
}

namespace sc2 {

struct SaveState {
    std::vector<std::uint8_t> ram;
    RecompRegisters registers{};
    RecompFpuContext fpu{};
    Sc2HostState host{};
    // (the host's records the guest's own refer to: its allocations, the sound device and buffers)
    std::vector<std::uint8_t> kernel_memory, sound_device, sound_buffers;
    bool valid = false;
};

// withRam false: all but guest RAM, which online play's rollback keeps by its written pages (RamSave, RamLoad).
void SaveFrame(SaveState &state, bool withRam = true);
void LoadFrame(const SaveState &state, bool withRam = true);

// Rollback's guest RAM by the pages written (sc2_dirty.h), where they can be listed (RamTracked): RamSave keeps, for each
// page written since the save before, what it held then (~1 MB a fight frame); RamLoad puts back the RAM of a frame
// saved before, newer saves dropped, and false when that frame is no longer kept. As LoadFrame, it leaves the other
// threads' memory and the kernel's page and above alone.
// Online play's prelude (runner.cpp): two games that went the same way to the same point alone differ in a few
// hundred pages of guest RAM (what boot left different) and in the host's records. PageHashes: a hash of each 4 KB
// page; PutPages: the host's pages put in (as LoadFrame, but for the other threads' memory, the kernel's page and
// the D3D library's state, which each machine keeps its own); PackFrame/UnpackFrame: a state but for guest RAM.
constexpr std::uint32_t kSyncPage = 4096u;
std::vector<std::uint32_t> PageHashes();
void PutPages(const std::vector<std::uint32_t> &pages, const std::uint8_t *data);
std::vector<std::uint8_t> PackFrame(const SaveState &state);
bool UnpackFrame(const std::uint8_t *data, std::size_t size, SaveState &state);

bool RamTracked();
void RamSave(std::uint64_t frame);
bool RamLoad(std::uint64_t frame);
void RamForget(); // (a session over: the next one's frames start again)

// The in-game menu's save states (sc2_frame.cpp): nine slots, kept while the game runs. Asked for between frames,
// done at the start of the next. A state loads only in the scene it was saved in (the same fight, the same menu):
// the game's other threads (the loader, the sound) are not put back with it.
constexpr int kStateSlots = 9;
void AskSaveState(int slot); // 1-9
void AskLoadState(int slot);
bool StateSaved(int slot);
void StatesForget(); // (practice left: its states are of a fight that is gone)

} // namespace sc2
