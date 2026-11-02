#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// breakdown-recomp patch: vertical blanks whose title callback has run, and a wait for the next one (false on the
// timeout, or when no blank thread runs)
uint64_t BlanksDone();
bool WaitBlankDone(uint64_t seen, uint32_t timeout_ms);

}  // namespace d3d
