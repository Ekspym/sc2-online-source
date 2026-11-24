// SPDX-License-Identifier: GPL-3.0-or-later
#include "sc2_dirty.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sc2::dirty {

namespace {
std::uint8_t *ram;
std::size_t ramSize;
bool tracked;
#if defined(_WIN32)
std::vector<void *> addresses;
#endif
} // namespace

std::uint8_t *AllocateRam(std::size_t size)
{
    ramSize = size;
#if defined(_WIN32)
    if (!std::getenv("SC2_NO_WRITE_WATCH")) {
        ram = static_cast<std::uint8_t *>(
            VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        tracked = ram != nullptr;
    }
    if (!ram) ram = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (tracked) addresses.resize(size / PageSize());
#else
    ram = static_cast<std::uint8_t *>(std::calloc(1, size));
#endif
    return ram;
}

bool Available()
{
    return tracked;
}

std::size_t PageSize()
{
    return 4096u;
}

void Take(std::vector<std::uint32_t> &pages)
{
    pages.clear();
#if defined(_WIN32)
    if (!tracked) return;
    ULONG_PTR count = addresses.size();
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, ram, ramSize, addresses.data(), &count, &granularity) != 0) {
        // (no list: every page, as a whole copy would)
        for (std::uint32_t i = 0; i < ramSize / PageSize(); ++i) pages.push_back(i);
        return;
    }
    pages.reserve(count);
    for (ULONG_PTR i = 0; i < count; ++i)
        pages.push_back(static_cast<std::uint32_t>((static_cast<std::uint8_t *>(addresses[i]) - ram) / PageSize()));
#endif
}

void Reset()
{
#if defined(_WIN32)
    if (tracked) ResetWriteWatch(ram, ramSize);
#endif
}

} // namespace sc2::dirty
