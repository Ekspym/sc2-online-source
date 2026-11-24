// SPDX-License-Identifier: GPL-3.0-or-later
// Guest RAM whose written pages can be listed (online play's rollback, sc2_state.h: a save state keeps only the pages
// written since the one before, ~1 MB of the 64 a fight frame, not all of guest RAM).
//
// Windows: the RAM is allocated with MEM_WRITE_WATCH and the system lists the pages written (GetWriteWatch), its own
// writes into them (ReadFile) included. Elsewhere, for now, nothing is tracked (Available() false) and save states
// copy the whole of guest RAM as before.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sc2::dirty {

// Guest RAM, zeroed, page aligned; tracked where the system can. Never freed (the program's whole run).
std::uint8_t *AllocateRam(std::size_t size);

// Whether the pages written can be listed.
bool Available();

// The tracking page's size (a power of two, 4 KB or more).
std::size_t PageSize();

// The pages written since the last Take or Reset, as page indexes; the list is then started again.
void Take(std::vector<std::uint32_t> &pages);
void Reset();

} // namespace sc2::dirty
