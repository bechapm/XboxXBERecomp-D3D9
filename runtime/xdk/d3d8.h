#pragma once
#include <atomic>
#include <mutex>

#include "gpu/backend.h"
#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// the game's full-screen passes over its own finished frame (a soften, a glow, a grade: the draws with the frame
// buffer at stage 0) drawn only up to this many a frame, in the order drawn; -1 every one, 0 none
inline std::atomic<int> g_frame_passes{-1};

// a skinned mesh drawn by fixed function (gpu::SkinnedMesh), in order with the title's draws, on the title's thread.
// SkinnedMeshes: a mod asked for them (d3d9_skinned_meshes) and the D3D9 renderer draws on the title's thread, so a
// hook that has one to give can give it; otherwise it draws as the title would
bool SkinnedMeshes();
void DrawSkinned(const gpu::SkinnedMesh& m);
inline std::atomic<bool> g_skinned_meshes{false};  // what the mods asked

}  // namespace d3d
