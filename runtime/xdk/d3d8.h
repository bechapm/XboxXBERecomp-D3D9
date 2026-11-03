#pragma once
#include <atomic>
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the game's full-screen passes over its own finished frame (a soften, a glow, a grade: the draws with the frame
// buffer at stage 0) drawn only up to this many a frame, in the order drawn; -1 every one, 0 none
inline std::atomic<int> g_frame_passes{-1};

}  // namespace d3d
