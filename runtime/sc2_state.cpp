// SPDX-License-Identifier: GPL-3.0-or-later
#include "sc2_state.h"
#include "sc2_dirty.h"

extern "C" {
#include "kernel_abi.h"
#include "xbox_memory_layout.h"
}

extern "C" {
size_t sc2_dsound_device_state_size(void);
void sc2_dsound_device_state_save(void *out);
void sc2_dsound_device_state_load(const void *in);
size_t sc2_dsound_buffers_state_size(void);
void sc2_dsound_buffers_state_save(void *out);
void sc2_dsound_buffers_state_load(const void *in);
}

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>

namespace sc2 {

namespace {

// (left alone by a load: the other threads' memory, and the kernel's page and above; sorted)
std::vector<std::pair<std::uint32_t, std::uint32_t>> KeptRanges()
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> kept;
    std::uint32_t base = 0u, size = 0u;
    bool current = false;
    for (unsigned i = 0u; recomp_kernel_thread_region(i, &base, &size, &current); ++i)
        if (size != 0u && !current) kept.emplace_back(base, base + size);
    kept.emplace_back(XBOX_KERNEL_DATA_BASE, RECOMP_XBOX_RAM_SIZE);
    std::sort(kept.begin(), kept.end());
    return kept;
}

// (from..to of guest RAM put back from source, but for the kept ranges)
void RestoreRange(std::uint8_t *ram, const std::uint8_t *source, std::uint32_t from, std::uint32_t to,
                  const std::vector<std::pair<std::uint32_t, std::uint32_t>> &kept)
{
    std::uint32_t at = from;
    for (const auto &range : kept) {
        if (range.second <= at) continue;
        if (range.first >= to) break;
        if (range.first > at) std::memcpy(ram + at, source + (at - from), range.first - at);
        at = std::max(at, range.second);
        if (at >= to) return;
    }
    if (at < to) std::memcpy(ram + at, source + (at - from), to - at);
}

struct RamLog {
    std::uint64_t frame = 0;
    std::vector<std::uint32_t> pages;
    std::vector<std::uint8_t> data; // (each page as it was at the save before)
};
constexpr std::size_t kRamLogs = 16; // (more than rollback goes back: kStates in runner.cpp)
std::deque<RamLog> ramLogs, ramSpare;
std::vector<std::uint8_t> ramShadow; // (guest RAM as at the last save)
std::vector<std::uint32_t> ramTaken;
std::uint64_t ramBase;
bool ramStarted;

} // namespace

std::vector<std::uint32_t> PageHashes()
{
    const std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
    std::vector<std::uint32_t> hashes(RECOMP_XBOX_RAM_SIZE / kSyncPage);
    for (std::size_t p = 0; p < hashes.size(); ++p) {
        // (FNV-1a over 32-bit words)
        std::uint32_t h = 2166136261u;
        const std::uint8_t *at = ram + p * kSyncPage;
        for (std::uint32_t i = 0; i < kSyncPage; i += 4u) {
            std::uint32_t w;
            std::memcpy(&w, at + i, 4u);
            h = (h ^ w) * 16777619u;
        }
        hashes[p] = h;
    }
    return hashes;
}

void PutPages(const std::vector<std::uint32_t> &pages, const std::uint8_t *data)
{
    auto kept = KeptRanges();
    // (the D3D library's state, 0x15E190-0x1606D0: each machine's own, as the match's beginning leaves it)
    kept.emplace_back(0x15E190u, 0x1606D0u);
    std::sort(kept.begin(), kept.end());
    std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::uint32_t at = pages[i] * kSyncPage;
        if (at >= RECOMP_XBOX_RAM_SIZE) continue;
        RestoreRange(ram, data + i * kSyncPage, at, at + kSyncPage, kept);
    }
}

namespace {
template <typename T> void Put(std::vector<std::uint8_t> &out, const T &value)
{
    const auto *p = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
void PutBytes(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &bytes)
{
    Put(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}
template <typename T> bool Get(const std::uint8_t *&at, const std::uint8_t *end, T &value)
{
    if (std::size_t(end - at) < sizeof value) return false;
    std::memcpy(&value, at, sizeof value);
    at += sizeof value;
    return true;
}
bool GetBytes(const std::uint8_t *&at, const std::uint8_t *end, std::vector<std::uint8_t> &bytes)
{
    std::uint32_t size = 0;
    if (!Get(at, end, size) || std::size_t(end - at) < size) return false;
    bytes.assign(at, at + size);
    at += size;
    return true;
}
} // namespace

std::vector<std::uint8_t> PackFrame(const SaveState &state)
{
    std::vector<std::uint8_t> out;
    Put(out, state.registers);
    Put(out, state.fpu);
    Put(out, state.host);
    PutBytes(out, state.kernel_memory);
    PutBytes(out, state.sound_device);
    PutBytes(out, state.sound_buffers);
    return out;
}

bool UnpackFrame(const std::uint8_t *data, std::size_t size, SaveState &state)
{
    const std::uint8_t *at = data, *end = data + size;
    state.ram.clear();
    state.valid = Get(at, end, state.registers) && Get(at, end, state.fpu) && Get(at, end, state.host) &&
                  GetBytes(at, end, state.kernel_memory) && GetBytes(at, end, state.sound_device) &&
                  GetBytes(at, end, state.sound_buffers) && at == end;
    return state.valid;
}

bool RamTracked()
{
    return dirty::Available();
}

void RamForget()
{
    ramStarted = false;
    ramLogs.clear();
    ramSpare.clear();
    ramBase = 0;
}

void RamSave(std::uint64_t frame)
{
    std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
    const std::size_t page = dirty::PageSize();
    if (!ramStarted) {
        dirty::Reset();
        ramShadow.assign(ram, ram + RECOMP_XBOX_RAM_SIZE);
        ramLogs.clear();
        ramBase = frame;
        ramStarted = true;
        return;
    }
    dirty::Take(ramTaken);
    RamLog log;
    if (!ramSpare.empty()) {
        log = std::move(ramSpare.back());
        ramSpare.pop_back();
    }
    log.frame = frame;
    log.pages = ramTaken;
    log.data.resize(ramTaken.size() * page);
    for (std::size_t i = 0; i < ramTaken.size(); ++i) {
        const std::size_t at = std::size_t(ramTaken[i]) * page;
        std::memcpy(log.data.data() + i * page, ramShadow.data() + at, page);
        std::memcpy(ramShadow.data() + at, ram + at, page);
    }
    ramLogs.push_back(std::move(log));
    while (ramLogs.size() > kRamLogs) {
        ramBase = ramLogs.front().frame;
        ramSpare.push_back(std::move(ramLogs.front()));
        ramLogs.pop_front();
    }
}

bool RamLoad(std::uint64_t frame)
{
    if (!ramStarted || frame < ramBase) return false;
    bool found = frame == ramBase;
    for (const auto &log : ramLogs) found |= log.frame == frame;
    if (!found) return false;
    std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
    const std::size_t page = dirty::PageSize();
    const auto kept = KeptRanges();
    // (back to the last save: the pages written since)
    dirty::Take(ramTaken);
    for (const std::uint32_t p : ramTaken) {
        const std::uint32_t at = std::uint32_t(p * page);
        RestoreRange(ram, ramShadow.data() + at, at, std::uint32_t(at + page), kept);
        std::memcpy(ramShadow.data() + at, ram + at, page);
    }
    // (then each save after the frame undone, newest first)
    while (!ramLogs.empty() && ramLogs.back().frame > frame) {
        RamLog &log = ramLogs.back();
        for (std::size_t i = 0; i < log.pages.size(); ++i) {
            const std::uint32_t at = std::uint32_t(log.pages[i] * page);
            RestoreRange(ram, log.data.data() + i * page, at, std::uint32_t(at + page), kept);
            std::memcpy(ramShadow.data() + at, ram + at, page);
        }
        ramSpare.push_back(std::move(log));
        ramLogs.pop_back();
    }
    return true;
}

void SaveFrame(SaveState &state, bool withRam)
{
    if (withRam) {
        state.ram.resize(RECOMP_XBOX_RAM_SIZE);
        std::memcpy(state.ram.data(), recomp_memory(0u, RECOMP_XBOX_RAM_SIZE), RECOMP_XBOX_RAM_SIZE);
    }
    state.registers = recomp_runtime.registers;
    recomp_fpu_context_save(&state.fpu);
    sc2_host_state_get(&state.host);
    state.kernel_memory.resize(recomp_kernel_memory_state_size());
    recomp_kernel_memory_state_save(state.kernel_memory.data());
    state.sound_device.resize(sc2_dsound_device_state_size());
    sc2_dsound_device_state_save(state.sound_device.data());
    state.sound_buffers.resize(sc2_dsound_buffers_state_size());
    sc2_dsound_buffers_state_save(state.sound_buffers.data());
    state.valid = true;
}

void LoadFrame(const SaveState &state, bool withRam)
{
    if (!state.valid) return;
    if (withRam) {
        // (only the pages that differ are written: written whole, each would count as changed at the next RamSave)
        std::uint8_t *ram = recomp_memory(0u, RECOMP_XBOX_RAM_SIZE);
        const auto kept = KeptRanges();
        for (std::uint32_t at = 0u; at < RECOMP_XBOX_RAM_SIZE; at += 4096u)
            if (std::memcmp(ram + at, state.ram.data() + at, 4096u) != 0)
                RestoreRange(ram, state.ram.data() + at, at, at + 4096u, kept);
    }
    recomp_runtime.registers = state.registers;
    recomp_fpu_context_restore(&state.fpu);
    sc2_host_state_set(&state.host);
    recomp_kernel_memory_state_load(state.kernel_memory.data());
    sc2_dsound_device_state_load(state.sound_device.data());
    sc2_dsound_buffers_state_load(state.sound_buffers.data());
}

} // namespace sc2
