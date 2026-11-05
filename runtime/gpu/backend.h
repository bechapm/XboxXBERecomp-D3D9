#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

// A mesh a game's hook built and has skinned by fixed function: one matrix per vertex, by index (D3D9's indexed vertex
// blending, up to 256 matrices), each run a triangle list with one texture. Drawn in place of one of the title's vertex
// program draws, on the screen as that program puts it (its two screen lines, kViewportScaleConstant and
// kViewportOffsetConstant). Devices offer only matrices 0 to 8 to fixed function (MaxVertexBlendMatrixIndex 8 on
// D3D9 and on RTX Remix, measured), so this is for a tool that skins whole meshes itself (Remix: up to 256 bones). For a model the title draws in many pieces
// of a few matrices each (a vertex program's registers hold no more), so that a tool reading fixed function draws
// (RTX Remix) sees it whole
struct SkinnedVertex {
    float pos[3];     // in its matrix's space
    float normal[3];
    float uv[2];
    uint32_t matrix;  // 0..255
    // never read by fixed function (D3DVBF_0WEIGHTS: the one matrix, wholly), but RTX Remix skins a draw only when its
    // declaration has a BLENDWEIGHT, and otherwise places the vertices as they are, in their matrices' spaces
    float weight = 1.0f;
};
struct SkinnedRun {
    uint32_t texture;  // the guest D3D texture header, or 0
    uint32_t first;    // in indices
    uint32_t count;    // indices, a multiple of 3
};
struct SkinnedMesh {
    const SkinnedVertex* vertices = nullptr;
    uint32_t vertex_count = 0;
    const uint16_t* indices = nullptr;  // triangle lists
    const SkinnedRun* runs = nullptr;
    uint32_t run_count = 0;
    const float* matrices = nullptr;    // row vector, model (the vertex's matrix space) to view, 16 floats each
    uint32_t matrix_count = 0;
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // through the title's projection (g_projection_register) and its current targets and viewport; only the D3D9
    // backend draws it, and only when a mod asked (d3d::SkinnedMeshes)
    virtual void DrawSkinned(const State&, const SkinnedMesh&) {}
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// Direct3D 9, for tools that hook it (d3d9.cpp)
std::unique_ptr<Backend> CreateD3D9Backend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

}  // namespace gpu
