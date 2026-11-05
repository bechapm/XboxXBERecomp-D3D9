#pragma once
#include <cstdint>
#include <vector>

#include "mods/recomp_mod.h"

// mods: libraries in the mods folder beside the exe, loaded as the device is first made; recomp_mod.h is the
// contract, docs/mods.md the modder's guide
namespace mods {

void Load();    // the game's thread, once, before the renderer starts
void Frame();   // after each Present, on the thread that presents

// what mods asked of the Direct3D 9 renderer (d3d9.cpp), all registered from their init
struct DrawHook {
    RecompModBeforeDraw before;
    RecompModAfterDraw after;
    void* user;
};
struct DeviceHook {
    RecompModDeviceFn fn;
    void* user;
};
struct ClearHook {
    RecompModClearFn fn;
    void* user;
};
struct ResizeHook {
    RecompModResizeFn fn;
    void* user;
};
struct CopyHook {
    RecompModCopyFn fn;
    void* user;
};
struct D3d9Hooks {
    std::vector<DrawHook> draw;
    std::vector<DeviceHook> device, present;
    std::vector<ClearHook> clear;
    std::vector<CopyHook> copy;
    std::vector<ResizeHook> resize;
    bool remake_written = false;   // d3d9_remake_written_textures
    bool fixed_transform = false;  // d3d9_fixed_transform
    RecompModFixedFn fixed_keep = nullptr;
    void* fixed_keep_user = nullptr;
};
const D3d9Hooks& D3d9();
// a mod hears of draws and may read guest memory there: the renderer stays on the game's thread, where memory is
// as the draw found it, not a frame on
bool HooksDraws();

// the renderer's side of the api while it runs; without one (D3D12) those calls answer nothing
struct Renderer {
    virtual ~Renderer() = default;
    // the constants of the draw being made, in hardware order
    virtual uint32_t VertexConstants(uint32_t first, uint32_t count, float (*out)[4]) = 0;
    virtual uint32_t DrawVertexBuffer(uint32_t* data, uint32_t* stride, uint32_t* first) = 0;
    virtual bool RenderState(uint32_t state, uint32_t* value) = 0;
    virtual void SetProjection(const float m[16]) = 0;
    virtual void SetTransform(uint32_t type, const float m[16]) = 0;
};
void SetRenderer(Renderer* r);

}  // namespace mods
