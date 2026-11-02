// The title's Direct3D 8 calls drawn with Direct3D 9, for the tools that hook it (RTX Remix and the like): fixed function
// draws go to Direct3D 9's own fixed function pipeline with the title's transforms, lights and texture stages, the rest
// to Shader Model 3 shaders from the same generator as the D3D12 renderer. d3d9.dll is loaded at run time, so a tool's
// d3d9.dll beside the exe is found before the system's.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/testdrive.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "mods/mods.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr D3DFORMAT kFmtIntz = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));  // depth a shader can sample
constexpr uint32_t kVbRingBytes = 64u << 20;
constexpr uint32_t kIbRingBytes = 16u << 20;
constexpr uint32_t kQueries = 4096;   // occlusion queries kept for counted draws
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kReportWait = 4;   // presents a visibility report may wait before its queries are waited for

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

// D3DBLEND, the console uses GL values
D3DBLEND Blend(uint32_t v) {
    switch (v) {
        case 0: return D3DBLEND_ZERO;
        case 1: return D3DBLEND_ONE;
        case 0x300: return D3DBLEND_SRCCOLOR;
        case 0x301: return D3DBLEND_INVSRCCOLOR;
        case 0x302: return D3DBLEND_SRCALPHA;
        case 0x303: return D3DBLEND_INVSRCALPHA;
        case 0x304: return D3DBLEND_DESTALPHA;
        case 0x305: return D3DBLEND_INVDESTALPHA;
        case 0x306: return D3DBLEND_DESTCOLOR;
        case 0x307: return D3DBLEND_INVDESTCOLOR;
        case 0x308: return D3DBLEND_SRCALPHASAT;
        case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR;
        case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
        default: return D3DBLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3DBLENDOP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3DBLENDOP_SUBTRACT;
        case 0x800B: return D3DBLENDOP_REVSUBTRACT;
        case 0x8007: return D3DBLENDOP_MIN;
        case 0x8008: return D3DBLENDOP_MAX;
        default: return D3DBLENDOP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, 0 to 7: the shaders' compare index, Direct3D 9's D3DCMP minus 1
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
D3DCMPFUNC Cmp(uint32_t v) { return D3DCMPFUNC(CmpIndex(v) + 1); }

// D3DSTENCILOP, GL values
D3DSTENCILOP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3DSTENCILOP_ZERO;
        case 0x1E01: return D3DSTENCILOP_REPLACE;
        case 0x1E02: return D3DSTENCILOP_INCRSAT;
        case 0x1E03: return D3DSTENCILOP_DECRSAT;
        case 0x150A: return D3DSTENCILOP_INVERT;
        case 0x8507: return D3DSTENCILOP_INCR;
        case 0x8508: return D3DSTENCILOP_DECR;
        default: return D3DSTENCILOP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3DTEXTUREADDRESS Address(uint32_t v) {
    switch (v) {
        case 2: return D3DTADDRESS_MIRROR;
        case 3: case 5: return D3DTADDRESS_CLAMP;
        case 4: return D3DTADDRESS_BORDER;
        default: return D3DTADDRESS_WRAP;
    }
}

D3DFORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return D3DFMT_DXT1;
        case Host::BC2: return D3DFMT_DXT3;
        case Host::BC3: return D3DFMT_DXT5;
        case Host::R32F: return D3DFMT_R32F;
        case Host::R16Unorm: return D3DFMT_L16;
        default: return D3DFMT_A8R8G8B8;
    }
}

bool Compressed(D3DFORMAT f) { return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5; }

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// a console surface the renderer draws into: a render target texture, or a depth buffer (INTZ, so draws can sample
// it, where the driver has it)
struct Target {
    ComPtr<IDirect3DTexture9> tex;  // null for a depth buffer the driver cannot sample
    ComPtr<IDirect3DSurface9> surf;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;  // last lock seen, guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    ComPtr<IDirect3DTexture9> fb;  // copy read by draws that sample the target they render to
};

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<IDirect3DBaseTexture9> res;
    TextureDesc d;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;       // hash of the colours a P8 texture was decoded with
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
    bool drawn = false;         // a draw read it since its last upload (d3d9_remake_written_textures)
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    IDirect3DBaseTexture9* res[4] = {};
    uint32_t levels[4] = {};
    bool rt[4] = {};       // stage samples a render target
    float wide[4] = {};    // host texels per console texel of a target read into a smaller one, 0 otherwise
    uint64_t id[4] = {};   // what each holds, for mods
    Target* feedback[4] = {};  // the target drawn into, read through its copy: copied once the draw is made
};

// a draw as the device will make it, its streams and indices in place (DrawGeometry)
struct Geometry {
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    uint32_t prims = 0;
    uint32_t vertices = 0;          // in the streams, from 0
    bool indexed = false;
    uint32_t first_index = 0;       // in the index buffer
    const void* indices = nullptr;  // the indices, rebased to 0, for the mods
    uint32_t index_bytes = 0, index_count = 0;
};

// a draw's shaders: a vertex program, Direct3D 9's fixed function pipeline, or the generator's fixed function shaders
struct DrawShaders {
    bool prog = false;
    bool ffp = false;              // drawn by Direct3D 9's own fixed function pipeline
    bool fixed_transform = false;  // fixed function on untransformed vertices
    bool sprite = false;           // a program's points as point sprites
    FixedKey fk;
    InputUsages usages{};          // each input's Direct3D 9 name
    uint64_t vkey = 0;
    uint64_t pkey = 0;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// D3DTOP of the XDK, which orders them apart from Direct3D 9 from BLENDCURRENTALPHA on; PREMODULATE is drawn as current
const uint8_t kTop[27] = {0,  D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
                          D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT,
                          D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA,
                          D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_SELECTARG1,
                          D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
                          D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
                          D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
constexpr uint32_t kTopDisable = 1;
constexpr uint32_t kTopPremodulate = 17;
constexpr uint32_t kTaCurrent = 1;  // D3DTA_CURRENT

constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// the guest viewport inside its target, from clip space as the title's projection leaves it: folded into the
// projection, since the host viewport covers the whole target (the scissor keeps draws inside the guest one)
void ViewportMatrix(const State& s, GuestExtent size, float* vpm) {
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w;
    float sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1;
    float oy = 1 - (2.0f * v.y + v.h) / size.h;
    const float m[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    std::memcpy(vpm, m, sizeof(m));
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi; D3DLIGHT9 is the same
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;
static_assert(sizeof(D3DLIGHT9) == kLightFloats * 4 && sizeof(D3DMATERIAL9) == 17 * 4);

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    for (int j = 0; j < 3; j++) o[5][j] = f[kLightAttenuation0 + j];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr float kAlphaTestAlways = 7.0f;  // alpha test off: compare always, in CmpIndex terms like the enabled test

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    float wv[16];
    float wvp[16];
    float vpm[16];
    float full[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    ViewportMatrix(s, size, vpm);
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kFillPoint = 0x1B00;      // D3DFILL_POINT
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr uint32_t kCullCcw = 0x901;         // D3DCULL_CCW
constexpr uint32_t kShadeFlat = 0x1D00;      // D3DSHADE_FLAT
constexpr uint32_t kTexfLinear = 2;          // D3DTEXF: 1 point, 2 linear, 3 anisotropic, then the console's own
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;
constexpr uint32_t kTtffProjected = 0x100;   // D3DTTFF_PROJECTED
constexpr uint32_t kAlphaKillEnable = 4;     // D3DTALPHAKILL_ENABLE
constexpr uint32_t kPointList = 1;           // D3DPT_POINTLIST
constexpr int kSpriteStage = 3;

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. Upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge: such draws clamp (as the D3D12
// renderer does, which says more)
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// a program's point sprites under the fixed function stages: the XDK builds their combiners from stage 3 alone
bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

// the D3DVSDT of each input register, from the declaration or the FVF
std::array<uint32_t, 16> InputTypes(const State& s) {
    std::array<uint32_t, 16> types{};
    VertexShader fvf;
    const VertexShader* layout = s.vs.get();
    if (!(s.vs_handle & 1)) {
        FvfInputs(s.vs_handle, fvf);
        layout = &fvf;
    }
    if (layout)
        for (int r = 0; r < 16; r++) types[size_t(r)] = layout->inputs[size_t(r)].type;
    return types;
}

// components the title gave an input: D3DVSDT's high nibble, NORMPACKED3 and FLOAT2H three
uint32_t TypeComponents(uint32_t type) {
    if (type == 0x16 || type == 0x72) return 3;
    return std::clamp<uint32_t>(type >> 4, 1, 4);
}

// one element as the input assembler would read it, finished as the vertex shader would (VertexDecode)
void ReadElement(uint8_t format, uint32_t decode, const uint8_t* p, float* o) {
    o[0] = o[1] = o[2] = 0;
    o[3] = 1;
    auto snorm16 = [&](int n) {
        for (int i = 0; i < n; i++) {
            int16_t v;
            std::memcpy(&v, p + i * 2, 2);
            o[i] = std::max(v / 32767.0f, -1.0f);
        }
    };
    auto sint16 = [&](int n) {
        for (int i = 0; i < n; i++) {
            int16_t v;
            std::memcpy(&v, p + i * 2, 2);
            o[i] = v;
        }
    };
    switch (format) {
        case DXGI_FORMAT_R32_FLOAT: std::memcpy(o, p, 4); break;
        case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(o, p, 8); break;
        case DXGI_FORMAT_R32G32B32_FLOAT: std::memcpy(o, p, 12); break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(o, p, 16); break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            o[0] = p[2] / 255.0f;
            o[1] = p[1] / 255.0f;
            o[2] = p[0] / 255.0f;
            o[3] = p[3] / 255.0f;
            break;
        case DXGI_FORMAT_R16_SNORM: snorm16(1); break;
        case DXGI_FORMAT_R16G16_SNORM: snorm16(2); break;
        case DXGI_FORMAT_R16G16B16A16_SNORM: snorm16(4); break;
        case DXGI_FORMAT_R8_UNORM: o[0] = p[0] / 255.0f; break;
        case DXGI_FORMAT_R8G8_UNORM: for (int i = 0; i < 2; i++) o[i] = p[i] / 255.0f; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f; break;
        case DXGI_FORMAT_R16_SINT: sint16(1); break;
        case DXGI_FORMAT_R16G16_SINT: sint16(2); break;
        case DXGI_FORMAT_R16G16B16A16_SINT: sint16(4); break;
        case DXGI_FORMAT_R32_UINT: {  // NORMPACKED3, 11 11 10 signed
            uint32_t v;
            std::memcpy(&v, p, 4);
            o[0] = std::max(float(int32_t(v << 21) >> 21) / 1023.0f, -1.0f);
            o[1] = std::max(float(int32_t(v << 10) >> 21) / 1023.0f, -1.0f);
            o[2] = std::max(float(int32_t(v) >> 22) / 511.0f, -1.0f);
            break;
        }
        default: break;
    }
    if (decode == kDecodeW1 || decode == kDecodeIntW1) o[3] = 1;
    if (decode == kDecodeFloat2H) {
        o[3] = o[2];
        o[2] = 0;
    }
}

uint32_t PackColor4(const float* c) {
    auto b = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

// what a vertex element is declared as: the type Direct3D 9 reads in place, or kConvert
constexpr BYTE kConvert = 0xFF;

BYTE ShaderDeclType(uint8_t format) {
    switch (format) {
        case DXGI_FORMAT_R32_FLOAT: return D3DDECLTYPE_FLOAT1;
        case DXGI_FORMAT_R32G32_FLOAT: return D3DDECLTYPE_FLOAT2;
        case DXGI_FORMAT_R32G32B32_FLOAT: return D3DDECLTYPE_FLOAT3;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return D3DDECLTYPE_FLOAT4;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return D3DDECLTYPE_D3DCOLOR;
        case DXGI_FORMAT_R16G16_SNORM: return D3DDECLTYPE_SHORT2N;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return D3DDECLTYPE_SHORT4N;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return D3DDECLTYPE_UBYTE4N;
        case DXGI_FORMAT_R16G16_SINT: return D3DDECLTYPE_SHORT2;
        case DXGI_FORMAT_R16G16B16A16_SINT: return D3DDECLTYPE_SHORT4;
        default: return kConvert;
    }
}

BYTE FloatDecl(uint32_t n) { return BYTE(D3DDECLTYPE_FLOAT1 + std::clamp<uint32_t>(n, 1, 4) - 1); }

uint32_t DeclBytes(BYTE type) {
    switch (type) {
        case D3DDECLTYPE_FLOAT1: case D3DDECLTYPE_D3DCOLOR: return 4;
        case D3DDECLTYPE_FLOAT2: return 8;
        case D3DDECLTYPE_FLOAT3: return 12;
        default: return 16;
    }
}

// the vertex declaration of one draw and the CPU converted stream it may need
struct DeclElement {
    BYTE stream, type, usage, index;
    uint16_t offset;
    int8_t reg = -1;  // converted: the input register it is made from
    uint8_t decode = kDecodeNone;
};

// the shader that turns the frame into the gamma corrected picture, drawn over a screen quad
const char* kGammaHlsl = R"(sampler2D frame : register(s0);
sampler2D ramp : register(s1);
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float3 c = tex2D(frame, uv).rgb * (255.0 / 256.0) + 0.5 / 256.0;  // entry n at its texel's centre
    return float4(tex2D(ramp, float2(c.r, 0.5)).r, tex2D(ramp, float2(c.g, 0.5)).g, tex2D(ramp, float2(c.b, 0.5)).b, 1);
}
)";

// a screen quad's vertex for the passes: Direct3D 9 pixels, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1
struct QuadVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};
constexpr DWORD kQuadFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

class D3D9Backend final : public Backend, public mods::Renderer {
public:
    ~D3D9Backend() override;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, presents_}); }
    // mods::Renderer
    uint32_t VertexConstants(uint32_t first, uint32_t count, float (*out)[4]) override;
    uint32_t DrawVertexBuffer(uint32_t* data, uint32_t* stride, uint32_t* first) override;
    bool RenderState(uint32_t state, uint32_t* value) override;
    void SetProjection(const float m[16]) override;
    void SetTransform(uint32_t type, const float m[16]) override;

private:
    // Init, in order
    bool CreateDevice(HWND hwnd);
    void FillPresentParams(D3DPRESENT_PARAMETERS& pp) const;
    void SnapBackBuffer(IDirect3DSurface9* bb);  // testdrive: the frame about to be shown, into a .bmp
    bool CreateDeviceObjects();
    // device loss of a plain (not Ex) device: false while the device cannot draw
    bool Usable();
    void ReleaseDeviceObjects();
    void Scene(bool on);
    // state, set only when it differs from what the device holds; Invalidate after anything else set it
    void Rs(D3DRENDERSTATETYPE t, DWORD v);
    void Tss(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v);
    void Samp(DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v);
    void SetTexture(DWORD stage, IDirect3DBaseTexture9* t);
    void Invalidate();
    // targets
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;
    };
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    bool CreateTargetResource(Target& t);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool TargetStale(uint32_t data) const;
    IDirect3DSurface9* Dummy(uint32_t w, uint32_t h);
    void Bind(IDirect3DSurface9* rt, IDirect3DSurface9* ds);
    bool BindTargets(const State& s);
    void UploadTarget(Target& t, const Surface& s);
    void ClearDepth(Target& t, const std::vector<D3DRECT>& rs, DWORD flags, float z, uint32_t stencil);
    void ClearChannels(Target& t, D3DCOLOR color, uint8_t mask, const std::vector<D3DRECT>& rs);
    IDirect3DTexture9* Feedback(Target& t);
    void CopyFeedback(const DrawTextures& stages);
    // a texture of guest pixels, then a quad of it over (part of) the bound target
    ComPtr<IDirect3DTexture9> PixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void DrawQuad(IDirect3DBaseTexture9* tex, const RECT& to, D3DCOLOR color, IDirect3DPixelShader9* ps);
    // textures
    IDirect3DBaseTexture9* BindTexture(const State& s, int stage, DrawTextures& stages);
    IDirect3DBaseTexture9* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                             DrawTextures& stages);
    IDirect3DBaseTexture9* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                            DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, texturedumper::Image& img);
    void ReleaseOldTextures();
    // draws
    bool FixedFunctionDraws(const State& s, const Assembled& a, const DrawTextures& stages) const;
    void BuildDeclaration(const State& s, const Assembled& a, const DrawShaders& sh, std::vector<DeclElement>& el,
                          uint64_t& decode, uint32_t& conv_stride);
    IDirect3DVertexDeclaration9* EnsureDeclaration(const std::vector<DeclElement>& el);
    const InputUsages& ProgramUsages(const State& s, uint16_t regs);
    IDirect3DVertexShader9* EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh,
                                               uint64_t decode);
    IDirect3DPixelShader9* EnsurePixelShader(const PixelKey& pk, uint64_t key);
    ID3DBlob* Compile(const std::string& src, const char* profile);
    void SetOutputState(const State& s, const Assembled& a, bool ffp);
    void SetSamplers(const State& s, const Assembled& a, const DrawTextures& stages);
    void SetFixedFunction(const State& s, const Assembled& a, const DrawTextures& stages, GuestExtent size);
    void SetShaderConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, GuestExtent size);
    void SetViewportScissor(const State& s, uint32_t gw);
    bool DrawGeometry(const Assembled& a, const std::vector<DeclElement>& el, uint32_t conv_stride, Geometry& g);
    // WORLD, VIEW and PROJECTION: the title's, or a mod's
    void HalfPixel(float* m) const;
    void FoldProjection(const State& s, GuestExtent size, const float* m, float* out) const;
    void SetDeviceTransform(int i, const float* m);
    void SetTitleTransforms(const State& s, const Assembled& a, GuestExtent size);
    // the mods' draw hooks
    bool Offscreen() const;
    bool ModsBefore(const State& s, const Assembled& a, const DrawShaders& sh, const DrawTextures& stages,
                    const Geometry& g);
    void ModsAfter();
    uint32_t RingVertices(const void* src, uint32_t bytes, void** write = nullptr);
    float TargetScale() const;
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    // visibility tests
    IDirect3DQuery9* BeginCountedDraw(const State& s);
    void EndCountedDraw(IDirect3DQuery9* q);
    float SamplesPerPixel() const;
    void DeliverReports(bool wait);
    // Present's steps
    RECT FrameRect() const;
    void UpdateGammaLut();
    void RenderUi();
    void CountFps();
    void ApplySettings();

    HMODULE lib_ = nullptr;
    ComPtr<IDirect3D9> d3d_;
    ComPtr<IDirect3D9Ex> d3d_ex_;
    ComPtr<IDirect3DDevice9> dev_;
    ComPtr<IDirect3DDevice9Ex> dev_ex_;
    HWND hwnd_ = nullptr;
    bool lost_ = false;
    bool in_scene_ = false;
    bool intz_ = false;
    bool vsync_ = true;
    D3DCAPS9 caps_{};
    // device state as last set
    DWORD rs_[256] = {};
    bool rs_ok_[256] = {};
    DWORD tss_[8][33] = {};
    bool tss_ok_[8][33] = {};
    DWORD samp_[16][14] = {};
    bool samp_ok_[16][14] = {};
    IDirect3DBaseTexture9* bound_tex_[8] = {};
    IDirect3DSurface9* bound_rt_surf_ = nullptr;
    IDirect3DSurface9* bound_ds_surf_ = nullptr;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    // dynamic buffers the draws stream their vertices and indices through
    ComPtr<IDirect3DVertexBuffer9> vb_;
    ComPtr<IDirect3DIndexBuffer9> ib16_;
    ComPtr<IDirect3DIndexBuffer9> ib32_;
    uint32_t vb_at_ = 0, ib16_at_ = 0, ib32_at_ = 0;
    ComPtr<IDirect3DTexture9> white_;  // fixed function stages that read a texture the title left unbound
    ComPtr<IDirect3DTexture9> lut_;    // the gamma ramp, 256 x 1
    ComPtr<IDirect3DPixelShader9> gamma_ps_;
    std::unordered_map<uint64_t, ComPtr<IDirect3DSurface9>> dummies_;  // colour targets of depth only draws
    std::unordered_map<uint64_t, ComPtr<IDirect3DVertexDeclaration9>> decls_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;  // by source hash, null when the compile failed
    std::unordered_map<uint64_t, ComPtr<IDirect3DVertexShader9>> vshaders_;  // by vertex key
    std::unordered_map<uint64_t, ComPtr<IDirect3DPixelShader9>> pshaders_;   // by pixel key hash
    std::unordered_map<uint64_t, InputUsages> usages_;  // by program hash and registers
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;
    std::unordered_map<uint64_t, Tex> textures_;       // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    Assembled asm_;
    std::vector<uint16_t> index_copy_;  // a draw's 16 bit indices rebased, for the mods
    // mods: the draw in hand, the transforms as the device has them (WORLD, VIEW, PROJECTION) and those a mod set
    const State* mod_state_ = nullptr;
    const Assembled* mod_asm_ = nullptr;
    GuestExtent mod_size_{};
    RecompModDraw mod_draw_{};
    size_t mods_called_ = 0;
    float dev_xf_[3][16] = {};
    bool dev_xf_ok_[3] = {};
    float title_xf_[3][16] = {};  // the title's at the last draw
    struct ModTransform {
        bool on = false;
        float m[16];
        float title[16];  // the title's when it was set: the title setting another ends it
    };
    ModTransform mod_xf_[3];
    uint32_t screens_[4] = {};  // the surfaces presented, by virtual data address
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        IDirect3DQuery9* q;
        float samples_per_pixel;
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t present;
    };
    std::vector<ComPtr<IDirect3DQuery9>> queries_;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;
    std::deque<Report> reports_;
    // the identity until the game sets one: a ramp of zeros would draw every frame black
    std::array<uint8_t, 768> ramp_ = [] {
        std::array<uint8_t, 768> r{};
        for (int i = 0; i < 768; i++) r[size_t(i)] = uint8_t(i & 255);
        return r;
    }();
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    RECT last_frame_rect_{};
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    uint64_t presents_ = 0;
    // where the draws went, reported twice early on: a black picture says which step refused them
    struct DrawCounts {
        uint32_t made, assemble, targets, decl, shader, ffp, geometry, mods;
    } counts_{};
    // what Remix is given to find the camera in, reported every ten seconds while a mod hooks the draws: the program
    // draws, and the first that draws the screen in 3D with the transforms it left on the device
    struct CameraCounts {
        uint32_t programs, screen, projected, two_d, skipped;
        bool sampled;
        float projection[16], view[16], device_projection[16];
        uint32_t w, h;
        // each frame: its 3D draws on screen, and the first draw a mod bound a vertex texture for (RTX Remix's UI mark:
        // the picture goes in there and the draws after are rasterised over it), after how many of them
        struct Frame {
            uint32_t three_d, marked_after, primitives, w, h;
            bool marked, offscreen, two_d;
            uint64_t program;
            uint32_t draws;            // every draw, a program's or fixed function
            uint32_t ffp_ui, ffp_ui_at;  // fixed function draws RTX Remix takes for UI itself, and the first one's number
            bool ffp_ui_early;           // the first came before the first marked draw
        } frame, last;
        uint32_t frames, early, unmarked, offscreen_size;
        // the frame's draws from the first marked one on, which RTX Remix draws over its picture
        struct After {
            uint64_t program;
            uint32_t primitives;
            uint8_t textures, copies;
            bool drawn, marked, two_d;
            const char* op;  // not a draw: what the renderer did to which target
            uint32_t from, to;
            uint8_t write, blend, src, dst;  // colour write mask, and the blend if on
            bool offscreen, zwrite, zenable;
            float min_z, max_z;  // the device's viewport depth range
            uint32_t w, h;       // the device's render target
            const char* remix;   // the camera RTX Remix takes from the device's transforms (RemixCamera)
            float fov, lean;     // its vertical field of view and sideways lean, degrees
            bool counted;        // inside an occlusion query (the title counting pixels), which Remix only rasterises
            bool ffp, ffp_ui;    // Direct3D 9's fixed function, and one Remix takes for UI by its own rule
        };
        std::vector<After> after, after_last;
        std::vector<After> first, first_last;  // the frame's first draws, marked or not
        // the frame before a report: the picture in the target of the first marked draw just before and after it, and
        // what Present is given; whether RTX Remix's picture went in there and stayed
        bool probe, probed;
        std::string before_mark, after_mark, at_present;
        // the same frame draw by draw: the draws after which the picture's mean brightness jumped
        struct Jump {
            uint32_t index;
            float from, to;
            uint64_t program;
            uint32_t primitives, w, h;
            bool two_d, marked, offscreen;
        };
        std::vector<Jump> jumps;
        uint32_t probe_draws;
        float probe_mean = -1;
        ComPtr<IDirect3DSurface9> thumb, thumb_sys;
    } camera_{};
    void CountCamera(const DrawShaders& sh, bool drawn);
    void Prime(const Geometry* g);
    ComPtr<IDirect3DSurface9> prime_;  // XBE_D3D9_PRIME's target, a row taller than the back buffer
    bool prime_due_ = false;
    float MeanBrightness(IDirect3DSurface9* rt);
    void TraceBrightness(const DrawShaders& sh);
    void NoteAfterMark(const char* op, uint32_t from, uint32_t to);
    void EndCameraFrame();
    void ReportCamera();
    uint64_t frame_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

D3D9Backend::~D3D9Backend() {
    mods::SetRenderer(nullptr);
    if (dev_) ImGui_ImplDX9_Shutdown();
}

// the back buffer is the scene's size: the tools take the draws into a target of the back buffer's size for the scene.
// The window gets it through Present's destination rectangle
void D3D9Backend::FillPresentParams(D3DPRESENT_PARAMETERS& pp) const {
    pp = {};
    pp.BackBufferWidth = bb_w_;
    pp.BackBufferHeight = bb_h_;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd_;
    pp.Windowed = TRUE;
    pp.PresentationInterval = vsync_ ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

bool D3D9Backend::CreateDevice(HWND hwnd) {
    lib_ = LoadLibraryW(L"d3d9.dll");
    if (!lib_) {
        LOGE("d3d9: d3d9.dll not found");
        return false;
    }
    using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    using Create9 = IDirect3D9*(WINAPI*)(UINT);
    // Ex first: its windowed device is never lost, so nothing has to be made again after the PC sleeps
    auto create_ex = reinterpret_cast<Create9Ex>(GetProcAddress(lib_, "Direct3DCreate9Ex"));
    if (create_ex && SUCCEEDED(create_ex(D3D_SDK_VERSION, &d3d_ex_))) {
        d3d_ = d3d_ex_;
    } else if (auto create = reinterpret_cast<Create9>(GetProcAddress(lib_, "Direct3DCreate9"))) {
        d3d_.Attach(create(D3D_SDK_VERSION));
    }
    if (!d3d_) {
        LOGE("d3d9: Direct3DCreate9 failed");
        return false;
    }
    hwnd_ = hwnd;
    vsync_ = app::Get().vsync;
    D3DPRESENT_PARAMETERS pp;
    FillPresentParams(pp);
    // the render thread draws while the title thread made the device; the title's x87 code wants its precision kept
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
    HRESULT hr;
    if (d3d_ex_) {
        hr = d3d_ex_->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, nullptr, &dev_ex_);
        dev_ = dev_ex_;
    } else {
        hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, &dev_);
    }
    if (FAILED(hr)) {
        LOGE("d3d9: no device (%08X)", unsigned(hr));
        return false;
    }
    D3DADAPTER_IDENTIFIER9 id{};
    d3d_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    LOGR("D3D9 adapter: %s%s", id.Description, d3d_ex_ ? " (Ex)" : "");
    dev_->GetDeviceCaps(&caps_);
    D3DDISPLAYMODE mode{};
    d3d_->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    intz_ = SUCCEEDED(d3d_->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_DEPTHSTENCIL,
                                              D3DRTYPE_TEXTURE, kFmtIntz));
    if (!intz_) LOGW("d3d9: no INTZ depth textures, draws that sample a depth buffer read nothing");
    return true;
}

// the device's own objects: what a reset of a plain device takes away
bool D3D9Backend::CreateDeviceObjects() {
    DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    if (FAILED(dev_->CreateVertexBuffer(kVbRingBytes, usage, 0, D3DPOOL_DEFAULT, &vb_, nullptr)) ||
        FAILED(dev_->CreateIndexBuffer(kIbRingBytes, usage, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib16_, nullptr)) ||
        FAILED(dev_->CreateIndexBuffer(kIbRingBytes, usage, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib32_, nullptr))) {
        LOGE("d3d9: no vertex or index buffers");
        return false;
    }
    vb_at_ = ib16_at_ = ib32_at_ = kVbRingBytes;  // the first lock discards
    std::vector<uint8_t> white(4, 255);
    white_ = PixelTexture(white, 4, 1, 1);
    dev_->CreateTexture(256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lut_, nullptr);
    ramp_dirty_ = true;
    Invalidate();
    return white_ && lut_;
}

void D3D9Backend::ReleaseDeviceObjects() {
    Scene(false);
    ImGui_ImplDX9_InvalidateDeviceObjects();
    for (int i = 0; i < 8; i++) dev_->SetTexture(i, nullptr);
    dev_->SetStreamSource(0, nullptr, 0, 0);
    dev_->SetIndices(nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    bound_rt_surf_ = bound_ds_surf_ = nullptr;
    vb_.Reset();
    ib16_.Reset();
    ib32_.Reset();
    white_.Reset();
    lut_.Reset();
    prime_.Reset();
    targets_.clear();
    textures_.clear();
    pack_textures_.clear();
    dummies_.clear();
    queries_.clear();
    counted_.clear();
    for (const Report& r : reports_) kernel::W32(r.va + 12, 0);  // a report that cannot be counted counts nothing
    reports_.clear();
}

bool D3D9Backend::Init(uint32_t width, uint32_t height) {
    g_shader_model3 = true;
    g_reversed_depth = false;
    guest_w_ = width;
    guest_h_ = height;
    scale_ = app::RenderScale(guest_h_);
    bb_w_ = std::max(1u, uint32_t(std::lround(width * scale_)));
    bb_h_ = std::max(1u, uint32_t(std::lround(height * scale_)));
    if (!CreateDevice(static_cast<HWND>(app::Window()))) return false;
    if (!CreateDeviceObjects()) return false;
    ImGui_ImplDX9_Init(dev_.Get());
    texturedumper::Update();
    Scene(true);
    LOGI("d3d9: %ux%u guest, %ux%u back buffer, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    mods::SetRenderer(this);
    for (const mods::DeviceHook& h : mods::D3d9().device) h.fn(dev_.Get(), h.user);
    return true;
}

bool D3D9Backend::Usable() {
    if (!lost_) return true;
    HRESULT hr = dev_->TestCooperativeLevel();
    if (hr == D3DERR_DEVICELOST) return false;
    if (hr == D3DERR_DEVICENOTRESET) {
        ReleaseDeviceObjects();
        D3DPRESENT_PARAMETERS pp;
        FillPresentParams(pp);
        if (FAILED(dev_->Reset(&pp))) return false;
        if (!CreateDeviceObjects()) return false;
        ImGui_ImplDX9_CreateDeviceObjects();
        LOGI("d3d9: device reset after a loss, targets start over");
    }
    lost_ = false;
    Scene(true);
    return true;
}

// every draw is inside a scene, and the copies and read-backs outside one
void D3D9Backend::Scene(bool on) {
    if (on == in_scene_) return;
    if (on) dev_->BeginScene();
    else dev_->EndScene();
    in_scene_ = on;
}

void D3D9Backend::Rs(D3DRENDERSTATETYPE t, DWORD v) {
    if (rs_ok_[t] && rs_[t] == v) return;
    rs_[t] = v;
    rs_ok_[t] = true;
    dev_->SetRenderState(t, v);
}

void D3D9Backend::Tss(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    if (tss_ok_[stage][t] && tss_[stage][t] == v) return;
    tss_[stage][t] = v;
    tss_ok_[stage][t] = true;
    dev_->SetTextureStageState(stage, t, v);
}

void D3D9Backend::Samp(DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (samp_ok_[stage][t] && samp_[stage][t] == v) return;
    samp_[stage][t] = v;
    samp_ok_[stage][t] = true;
    dev_->SetSamplerState(stage, t, v);
}

void D3D9Backend::SetTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    if (bound_tex_[stage] == t) return;
    bound_tex_[stage] = t;
    dev_->SetTexture(stage, t);
}

void D3D9Backend::Invalidate() {
    std::memset(rs_ok_, 0, sizeof(rs_ok_));
    std::memset(tss_ok_, 0, sizeof(tss_ok_));
    std::memset(samp_ok_, 0, sizeof(samp_ok_));
    std::fill(std::begin(bound_tex_), std::end(bound_tex_), reinterpret_cast<IDirect3DBaseTexture9*>(~uintptr_t(0)));
    std::memset(dev_xf_ok_, 0, sizeof(dev_xf_ok_));
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D9Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

D3D9Backend::TargetSlot D3D9Backend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

Target* D3D9Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (!slot.sized) return nullptr;
    target_used_[slot.data] = frame_;
    return &slot.it->second;
}

void D3D9Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) Bind(nullptr, nullptr);
    targets_.erase(it);
}

// the host surface of a new target at render scale, cleared
bool D3D9Backend::CreateTargetResource(Target& t) {
    t.w = std::max(1u, uint32_t(std::lround(t.gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * scale_)));
    if (!t.depth) {
        if (FAILED(dev_->CreateTexture(t.w, t.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex,
                                       nullptr))) {
            return false;
        }
        t.tex->GetSurfaceLevel(0, &t.surf);
        dev_->ColorFill(t.surf.Get(), nullptr, 0);
        return true;
    }
    if (intz_ && SUCCEEDED(dev_->CreateTexture(t.w, t.h, 1, D3DUSAGE_DEPTHSTENCIL, kFmtIntz, D3DPOOL_DEFAULT, &t.tex,
                                               nullptr))) {
        t.tex->GetSurfaceLevel(0, &t.surf);
    } else if (FAILED(dev_->CreateDepthStencilSurface(t.w, t.h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &t.surf,
                                                      nullptr))) {
        return false;
    }
    ClearDepth(t, {}, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 1, 0);
    return true;
}

Target* D3D9Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    target_used_[slot.data] = frame_;
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t)) {
        LOG_FIRST(4, "d3d9: a %ux%u target could not be made", t.w, t.h);
        return nullptr;
    }
    t.data = slot.data;
    t.bytes = depth ? slot.gw * slot.gh * 4 : GuestPitch(slot.gw) * slot.gh;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    return &(targets_[slot.key] = std::move(t));
}

// long unused: the title freed it and other data is at this address now
bool D3D9Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// Direct3D 9 always draws to a colour target: a depth only draw gets one of its size that it never writes
IDirect3DSurface9* D3D9Backend::Dummy(uint32_t w, uint32_t h) {
    auto& s = dummies_[uint64_t(w) << 32 | h];
    if (!s) dev_->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr);
    return s.Get();
}

void D3D9Backend::Bind(IDirect3DSurface9* rt, IDirect3DSurface9* ds) {
    if (rt && rt != bound_rt_surf_) dev_->SetRenderTarget(0, rt);
    if (ds != bound_ds_surf_) dev_->SetDepthStencilSurface(ds);
    bound_rt_surf_ = rt ? rt : bound_rt_surf_;
    bound_ds_surf_ = ds;
    if (!rt) bound_rt_surf_ = nullptr;
    bound_rt_ = bound_ds_ = nullptr;
}

bool D3D9Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    IDirect3DSurface9* rs = rt ? rt->surf.Get() : Dummy(ds->w, ds->h);
    IDirect3DSurface9* dss = ds ? ds->surf.Get() : nullptr;
    if (rs != bound_rt_surf_) dev_->SetRenderTarget(0, rs);
    if (dss != bound_ds_surf_) dev_->SetDepthStencilSurface(dss);
    bound_rt_surf_ = rs;
    bound_ds_surf_ = dss;
    bound_rt_ = rt;
    bound_ds_ = ds;
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    return true;
}

// BGRA pixels into a new w x h texture
ComPtr<IDirect3DTexture9> D3D9Backend::PixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                    uint32_t h) {
    ComPtr<IDirect3DTexture9> staging;
    ComPtr<IDirect3DTexture9> tex;
    if (FAILED(dev_->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr)) ||
        FAILED(dev_->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr))) {
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(staging->LockRect(0, &lr, nullptr, 0))) return nullptr;
    for (uint32_t y = 0; y < h; y++)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, &px[size_t(y) * pitch], w * 4);
    staging->UnlockRect(0);
    dev_->UpdateTexture(staging.Get(), tex.Get());
    return tex;
}

// a texture, or color without one, over to, a rect of the bound target in its pixels, by a screen quad: the fixed
// function stage 0 or ps. Blending and the write mask are the caller's
void D3D9Backend::DrawQuad(IDirect3DBaseTexture9* tex, const RECT& to, D3DCOLOR color, IDirect3DPixelShader9* ps) {
    // Direct3D 9 pixel centres are on whole numbers: the quad's edges go half a pixel up and left
    float x0 = float(to.left) - 0.5f, y0 = float(to.top) - 0.5f, x1 = float(to.right) - 0.5f, y1 = float(to.bottom) - 0.5f;
    QuadVertex v[4] = {{x0, y0, 0, 1, color, 0, 0}, {x1, y0, 0, 1, color, 1, 0},
                       {x0, y1, 0, 1, color, 0, 1}, {x1, y1, 0, 1, color, 1, 1}};
    dev_->SetVertexShader(nullptr);
    dev_->SetPixelShader(ps);
    dev_->SetFVF(kQuadFvf);
    Rs(D3DRS_ZENABLE, FALSE);
    // writes nothing with depth off; a tool that takes a draw writing no depth for the game's UI (RTX Remix, which puts
    // its picture in at the first such draw and draws the rest over it) leaves the renderer's own passes be
    Rs(D3DRS_ZWRITEENABLE, TRUE);
    Rs(D3DRS_STENCILENABLE, FALSE);
    Rs(D3DRS_CULLMODE, D3DCULL_NONE);
    Rs(D3DRS_FILLMODE, D3DFILL_SOLID);
    Rs(D3DRS_LIGHTING, FALSE);
    Rs(D3DRS_FOGENABLE, FALSE);
    Rs(D3DRS_ALPHATESTENABLE, FALSE);
    Rs(D3DRS_SCISSORTESTENABLE, FALSE);
    Rs(D3DRS_SPECULARENABLE, FALSE);
    Rs(D3DRS_DEPTHBIAS, 0);
    Rs(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    SetTexture(0, tex);
    Tss(0, D3DTSS_COLOROP, tex ? D3DTOP_SELECTARG1 : D3DTOP_SELECTARG2);
    Tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    Tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    Tss(0, D3DTSS_ALPHAOP, tex ? D3DTOP_SELECTARG1 : D3DTOP_SELECTARG2);
    Tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    Tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    Tss(0, D3DTSS_TEXCOORDINDEX, 0);
    Tss(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    Tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    Tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (DWORD i = 0; i < 2; i++) {
        Samp(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        Samp(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        Samp(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        Samp(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        Samp(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    // the textures left on the other samplers stay out of it: Direct3D 9 ignores a disabled stage's texture, but a tool
    // that reads which textures a draw has (RTX Remix, whose fixed function draws count stages 0 to 6 as used) takes them
    // as this draw's. A mod's UI mark left on a stage from the frame's last draw made a clear before the next frame's 3D
    // RTX Remix's UI, and the frame lost Remix's picture. They are put back after, so the mods find what they left
    constexpr DWORD kHidden[] = {1, 2, 3, 4, 5, 6, 7, D3DVERTEXTEXTURESAMPLER0, D3DVERTEXTEXTURESAMPLER1,
                                 D3DVERTEXTEXTURESAMPLER2, D3DVERTEXTEXTURESAMPLER3};
    IDirect3DBaseTexture9* hidden[std::size(kHidden)] = {};
    for (size_t i = 0; i < std::size(kHidden); i++) {
        if (SUCCEEDED(dev_->GetTexture(kHidden[i], &hidden[i])) && hidden[i]) dev_->SetTexture(kHidden[i], nullptr);
    }
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
    for (size_t i = 0; i < std::size(kHidden); i++) {
        if (!hidden[i]) continue;
        dev_->SetTexture(kHidden[i], hidden[i]);
        hidden[i]->Release();
    }
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// CPU pixels of a colour surface into its host target, scaled: after a Sync only the pixels the CPU changed go over
// the picture, which stays at render scale
void D3D9Backend::UploadTarget(Target& t, const Surface& s) {
    NoteAfterMark("CPU pixels", 0, t.data);
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d9: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<IDirect3DTexture9> tex = PixelTexture(px, pitch, d.width, d.height);
    if (tex) {
        Scene(true);
        IDirect3DSurface9* rt_was = bound_rt_surf_;
        IDirect3DSurface9* ds_was = bound_ds_surf_;
        Target* brt = bound_rt_;
        Target* bds = bound_ds_;
        Bind(t.surf.Get(), nullptr);
        D3DVIEWPORT9 vp{0, 0, t.w, t.h, 0, 1};
        dev_->SetViewport(&vp);
        // over: premultiplied, the target keeps its alpha
        Rs(D3DRS_ALPHABLENDENABLE, over);
        Rs(D3DRS_SRCBLEND, D3DBLEND_ONE);
        Rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        Rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        Rs(D3DRS_COLORWRITEENABLE, over ? 7u : 15u);
        DrawQuad(tex.Get(), RECT{0, 0, LONG(t.w), LONG(t.h)}, 0xFFFFFFFF, nullptr);
        if (rt_was && rt_was != t.surf.Get()) {
            Bind(rt_was, ds_was);
            bound_rt_ = brt;
            bound_ds_ = bds;
        }
    }
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// rs empty: the whole target
void D3D9Backend::ClearDepth(Target& t, const std::vector<D3DRECT>& rs, DWORD flags, float z, uint32_t stencil) {
    Scene(true);
    Bind(Dummy(t.w, t.h), t.surf.Get());
    D3DVIEWPORT9 vp{0, 0, t.w, t.h, 0, 1};
    dev_->SetViewport(&vp);
    Rs(D3DRS_SCISSORTESTENABLE, FALSE);
    dev_->Clear(DWORD(rs.size()), rs.empty() ? nullptr : rs.data(), flags, 0, std::clamp(z, 0.0f, 1.0f), stencil);
    t.drawn = true;
}

// a clear of some colour channels: a quad over each rect writing only those
void D3D9Backend::ClearChannels(Target& t, D3DCOLOR color, uint8_t mask, const std::vector<D3DRECT>& rs) {
    Scene(true);
    Bind(t.surf.Get(), nullptr);
    D3DVIEWPORT9 vp{0, 0, t.w, t.h, 0, 1};
    dev_->SetViewport(&vp);
    Rs(D3DRS_ALPHABLENDENABLE, FALSE);
    Rs(D3DRS_COLORWRITEENABLE, mask);
    for (const D3DRECT& r : rs) DrawQuad(nullptr, RECT{r.x1, r.y1, r.x2, r.y2}, color, nullptr);
}

// a guest rect scaled to the target and clamped to it
D3DRECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

void D3D9Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                        uint32_t stencil) {
    if (!Usable()) return;
    if (!mods::D3d9().clear.empty()) {
        uint32_t what = ((flags & kClearTarget) ? RECOMP_CLEAR_COLOR : 0) | ((flags & kClearZ) ? RECOMP_CLEAR_DEPTH : 0) |
                        ((flags & kClearStencil) ? RECOMP_CLEAR_STENCIL : 0);
        for (const mods::ClearHook& h : mods::D3d9().clear) h.fn(what, z, !(count && rects), h.user);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<D3DRECT> rs;
    for (Target* t : {rt, ds}) {
        if (!t) continue;
        rs.clear();
        if (count && rects) {
            for (uint32_t i = 0; i < count; i++) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
                rs.push_back(ScaledRect(*t, r[0], r[1], r[2], r[3]));
            }
        } else {
            const auto& v = s.viewport;
            rs.push_back(ScaledRect(*t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
        }
        rs.erase(std::remove_if(rs.begin(), rs.end(), [](const D3DRECT& r) { return r.x2 <= r.x1 || r.y2 <= r.y1; }),
                 rs.end());
        if (rs.empty()) continue;
        if (t->depth) {
            DWORD f = ((flags & kClearZ) ? D3DCLEAR_ZBUFFER : 0) | ((flags & kClearStencil) ? D3DCLEAR_STENCIL : 0);
            ClearDepth(*t, rs, f, z, stencil);
            continue;
        }
        NoteAfterMark("clear", 0, t->data);
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(*t)) UploadTarget(*t, s.rt);
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7: Direct3D 9's write mask has R G B A from bit 0
        uint32_t m = (flags & kClearTarget) >> 4;
        uint8_t mask = uint8_t((m >> 2 & 1) | (m >> 1 & 1) << 1 | (m & 1) << 2 | (m >> 3 & 1) << 3);
        if (mask == 15) {
            for (const D3DRECT& r : rs) {
                RECT rc{r.x1, r.y1, r.x2, r.y2};
                dev_->ColorFill(t->surf.Get(), &rc, color);
            }
        } else {
            ClearChannels(*t, color, mask, rs);
        }
        t->drawn = true;
    }
}

// the texture a draw reads the target it renders to through; CopyFeedback fills it
IDirect3DTexture9* D3D9Backend::Feedback(Target& t) {
    if (!t.fb && FAILED(dev_->CreateTexture(t.w, t.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                            &t.fb, nullptr))) {
        return nullptr;
    }
    return t.fb.Get();
}

// the targets the draw reads as it renders to them, copied once it is sure to be made: one the mods leave out costs no
// copy (under RTX Remix, a copy of the picture after Remix put its own in lost that picture)
void D3D9Backend::CopyFeedback(const DrawTextures& stages) {
    Target* done = nullptr;
    for (Target* t : stages.feedback) {
        if (!t || t == done || !t->fb) continue;
        ComPtr<IDirect3DSurface9> fs;
        t->fb->GetSurfaceLevel(0, &fs);
        dev_->StretchRect(t->surf.Get(), nullptr, fs.Get(), nullptr, D3DTEXF_POINT);
        done = t;
    }
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

IDirect3DBaseTexture9* D3D9Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                      DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    stages.rt[stage] = true;
    stages.id[stage] = Hash(&t.data, sizeof(t.data)) ^ (uint64_t(t.gw) << 48 | uint64_t(t.gh) << 32);
    stages.levels[stage] = 1;
    info.flags |= kTexTarget;
    info.kind = kTex2D;
    if (t.depth) {
        if (!t.tex) return nullptr;
        info.shadow = ShadowDepthKind(d.format);
        return t.tex.Get();
    }
    if (LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (&t == draw_rt_) {
        stages.feedback[stage] = &t;
        return Feedback(t);
    }
    return t.tex.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

bool D3D9Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    HRESULT hr;
    if (d.cube) {
        ComPtr<IDirect3DCubeTexture9> t;
        hr = dev_->CreateCubeTexture(w, tx.levels, 0, tx.fmt, D3DPOOL_DEFAULT, &t, nullptr);
        tx.res = t;
    } else if (d.depth > 1) {
        ComPtr<IDirect3DVolumeTexture9> t;
        hr = dev_->CreateVolumeTexture(w, h, d.depth, tx.levels, 0, tx.fmt, D3DPOOL_DEFAULT, &t, nullptr);
        tx.res = t;
    } else {
        ComPtr<IDirect3DTexture9> t;
        hr = dev_->CreateTexture(w, h, tx.levels, 0, tx.fmt, D3DPOOL_DEFAULT, &t, nullptr);
        tx.res = t;
    }
    if (FAILED(hr)) {
        LOG_FIRST(8, "d3d9: a %ux%u texture of format %u could not be made", w, h, unsigned(tx.fmt));
        tx.res.Reset();
        return false;
    }
    return true;
}

// rows of a decoded level into a locked one: block rows for the DXT formats, rows past the decoded ones zero
void CopyRows(uint8_t* dst, uint32_t dst_pitch, uint32_t rows, const std::vector<uint8_t>& px, uint32_t pitch) {
    size_t src_rows = pitch ? px.size() / pitch : 0;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t* d = dst + size_t(y) * dst_pitch;
        if (y < src_rows) std::memcpy(d, &px[size_t(y) * pitch], std::min(pitch, dst_pitch));
        else std::memset(d, 0, dst_pitch);
    }
}

// staged in system memory, then copied over by UpdateTexture, which a tool sees as one upload
void D3D9Backend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    D3DRESOURCETYPE type = tx.res->GetType();
    uint32_t w = std::max(d.width, 1u), h = std::max(d.height, 1u);
    if (Compressed(tx.fmt) && tx.levels == 1) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
    }
    ComPtr<IDirect3DBaseTexture9> staging;
    HRESULT hr;
    if (type == D3DRTYPE_CUBETEXTURE) {
        ComPtr<IDirect3DCubeTexture9> t;
        hr = dev_->CreateCubeTexture(w, tx.levels, 0, tx.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
        staging = t;
    } else if (type == D3DRTYPE_VOLUMETEXTURE) {
        ComPtr<IDirect3DVolumeTexture9> t;
        hr = dev_->CreateVolumeTexture(w, h, d.depth, tx.levels, 0, tx.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
        staging = t;
    } else {
        ComPtr<IDirect3DTexture9> t;
        hr = dev_->CreateTexture(w, h, tx.levels, 0, tx.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
        staging = t;
    }
    if (FAILED(hr)) return;
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t lh = std::max(h >> l, 1u);
            uint32_t rows = Compressed(tx.fmt) ? (lh + 3) / 4 : lh;
            if (type == D3DRTYPE_VOLUMETEXTURE) {
                auto* vt = static_cast<IDirect3DVolumeTexture9*>(staging.Get());
                D3DLOCKED_BOX lb;
                if (FAILED(vt->LockBox(l, &lb, nullptr, 0))) continue;
                uint32_t slices = std::max(d.depth >> l, 1u);
                size_t slice_bytes = size_t(rows) * pitch;
                for (uint32_t z = 0; z < slices; z++) {
                    std::vector<uint8_t> slice;
                    if (slice_bytes * (z + 1) <= px.size())
                        slice.assign(px.begin() + ptrdiff_t(slice_bytes * z), px.begin() + ptrdiff_t(slice_bytes * (z + 1)));
                    CopyRows(static_cast<uint8_t*>(lb.pBits) + size_t(z) * lb.SlicePitch, lb.RowPitch, rows, slice, pitch);
                }
                vt->UnlockBox(l);
                continue;
            }
            D3DLOCKED_RECT lr;
            if (type == D3DRTYPE_CUBETEXTURE) {
                auto* ct = static_cast<IDirect3DCubeTexture9*>(staging.Get());
                if (FAILED(ct->LockRect(D3DCUBEMAP_FACES(f), l, &lr, nullptr, 0))) continue;
                CopyRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, rows, px, pitch);
                ct->UnlockRect(D3DCUBEMAP_FACES(f), l);
            } else {
                auto* t2 = static_cast<IDirect3DTexture9*>(staging.Get());
                if (FAILED(t2->LockRect(l, &lr, nullptr, 0))) continue;
                CopyRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, rows, px, pitch);
                t2->UnlockRect(l);
            }
        }
    }
    dev_->UpdateTexture(staging.Get(), tx.res.Get());
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
IDirect3DBaseTexture9* D3D9Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d,
                                                     const FormatInfo& fi, DrawTextures& stages) {
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) {
        textures_.erase(key);
        return nullptr;
    }
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != tx.sig || !same) {
            // a D3D9 runtime that knows a texture by its contents, read once, gets a new one for new contents
            if (same && tx.drawn && mods::D3d9().remake_written) {
                if (!CreateTexture(tx, d, fi)) {
                    textures_.erase(key);
                    return nullptr;
                }
                tx.frame = presents_ + 1;
                tx.palette = palette.hash;
            }
            tx.sig = sig;
            UploadTexture(tx, d, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), colours);
            tx.pack_gen = 0;
            tx.drawn = false;
        }
    }
    tx.drawn = true;
    stages.id[stage] = Hash(&tx.sig, sizeof(tx.sig), key) | 1;
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    stages.levels[stage] = drawn.levels;
    if (fi.depth) stages.info[stage].shadow = ShadowDepthKind(d.format);
    return drawn.res.Get();
}

void D3D9Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

void D3D9Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tx.swap = &it->second;
    tx.pack_key = 0;
}

// a pack image in a format Direct3D 9 has: BGRA, RGBA turned to BGRA, BC1 to BC3; BC7 has none
void D3D9Backend::CreatePackTexture(Tex& r, texturedumper::Image& img) {
    switch (img.format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: r.fmt = D3DFMT_A8R8G8B8; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            r.fmt = D3DFMT_A8R8G8B8;
            for (auto& l : img.levels)
                for (size_t i = 0; i + 3 < l.px.size(); i += 4) std::swap(l.px[i], l.px[i + 2]);
            break;
        case DXGI_FORMAT_BC1_UNORM: r.fmt = D3DFMT_DXT1; break;
        case DXGI_FORMAT_BC2_UNORM: r.fmt = D3DFMT_DXT3; break;
        case DXGI_FORMAT_BC3_UNORM: r.fmt = D3DFMT_DXT5; break;
        default:
            LOG_FIRST(4, "texture pack: DXGI format %d has no Direct3D 9 format, the game's texture stays", int(img.format));
            return;
    }
    r.levels = uint32_t(img.levels.size());
    ComPtr<IDirect3DTexture9> staging;
    ComPtr<IDirect3DTexture9> tex;
    if (FAILED(dev_->CreateTexture(img.width, img.height, r.levels, 0, r.fmt, D3DPOOL_SYSTEMMEM, &staging, nullptr)) ||
        FAILED(dev_->CreateTexture(img.width, img.height, r.levels, 0, r.fmt, D3DPOOL_DEFAULT, &tex, nullptr))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) {
        D3DLOCKED_RECT lr;
        if (FAILED(staging->LockRect(l, &lr, nullptr, 0))) continue;
        uint32_t lh = std::max(img.height >> l, 1u);
        CopyRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, Compressed(r.fmt) ? (lh + 3) / 4 : lh, img.levels[l].px,
                 img.levels[l].pitch);
        staging->UnlockRect(l);
    }
    dev_->UpdateTexture(staging.Get(), tex.Get());
    r.res = tex;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

IDirect3DBaseTexture9* D3D9Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.feedback[stage] = nullptr;
    stages.wide[stage] = 0;
    stages.levels[stage] = 0;
    stages.id[stage] = 0;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

void D3D9Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) it = textures_.erase(it);
        else ++it;
    }
    Invalidate();  // a freed texture's address may come back as another's
}

// What Direct3D 9's fixed function pipeline draws as the console does. It does not have the register combiners, alpha
// kill, shadow compares, a texture read at coordinates in texels after a projection, two sided lighting or the wide
// taps of a shrinking pass; a vertex program it has no part in. Those go to the generator's shaders.
bool D3D9Backend::FixedFunctionDraws(const State& s, const Assembled& a, const DrawTextures& stages) const {
    if (s.ps) return false;
    if (s.rs[kRsLighting] && s.rs[kRsTwoSidedLighting] && !a.pretransformed) return false;
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        if (t[kTssColorOp] <= kTopDisable) break;
        if (t[kTssAlphaKill] == kAlphaKillEnable) return false;
        const TexInfo& info = stages.info[i];
        if (info.shadow || (info.flags & kTexWide)) return false;
        if (info.rect && (t[kTssTextureTransformFlags] & kTtffProjected)) return false;
    }
    return true;
}

// the D3DDECLUSAGE fixed function reads each input register as: the XDK's habit, which its FVFs follow; 0xFF unread
InputUsage FixedUsage(int reg) {
    if (reg == 0 || reg == 2 || reg == 3 || reg == 4 || reg == 6 || (reg >= 9 && reg <= 12)) return HabitUsage(reg);
    return {0xFF, 0};
}

// Each input register as a vertex element: read in place where Direct3D 9 has its type, else converted on the CPU into
// one more stream. Fixed function takes positions and normals as three floats, colours as D3DCOLOR (the only colour a
// tool such as RTX Remix reads) and coordinates as the floats the title gave; a pretransformed position is read as x
// y z under an orthographic projection (SetFixedFunction), which keeps it a POSITION that tools draw.
void D3D9Backend::BuildDeclaration(const State& s, const Assembled& a, const DrawShaders& sh,
                                   std::vector<DeclElement>& el, uint64_t& decode, uint32_t& conv_stride) {
    el.clear();
    decode = a.decode;
    conv_stride = 0;
    std::array<uint32_t, 16> types = InputTypes(s);
    BYTE conv_stream = BYTE(a.buffer_count);
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        uint32_t dec = uint32_t(a.decode >> (r * 4)) & 15;
        InputUsage u = sh.ffp ? FixedUsage(r) : sh.prog ? sh.usages[size_t(r)] : HabitUsage(r);
        if (u.usage == 0xFF) continue;
        DeclElement d{BYTE(e.buffer), 0, u.usage, u.index, e.offset};
        bool float_in = e.format == DXGI_FORMAT_R32G32B32_FLOAT || e.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (!sh.ffp) {
            d.type = ShaderDeclType(e.format);
            if (d.type != kConvert) {
                el.push_back(d);
                continue;
            }
            d.type = D3DDECLTYPE_FLOAT4;  // the decode is done here, the shader reads the floats as they are
            decode &= ~(uint64_t(15) << (r * 4));
        } else if (u.usage == D3DDECLUSAGE_POSITION || u.usage == D3DDECLUSAGE_NORMAL) {
            d.type = float_in && dec == kDecodeNone ? D3DDECLTYPE_FLOAT3 : kConvert;
            if (d.type == kConvert) d.type = D3DDECLTYPE_FLOAT3 | 0x80;
        } else if (u.usage == D3DDECLUSAGE_COLOR) {
            d.type = e.format == DXGI_FORMAT_B8G8R8A8_UNORM ? D3DDECLTYPE_D3DCOLOR : D3DDECLTYPE_D3DCOLOR | 0x80;
        } else if (u.usage == D3DDECLUSAGE_PSIZE) {
            d.type = e.format == DXGI_FORMAT_R32_FLOAT ? D3DDECLTYPE_FLOAT1 : D3DDECLTYPE_FLOAT1 | 0x80;
        } else {
            // a texture coordinate keeps its component count: it decides where a texture transform's translation is
            uint32_t n = TypeComponents(types[size_t(r)]);
            bool in_place = dec == kDecodeNone && ((n == 1 && e.format == DXGI_FORMAT_R32_FLOAT) ||
                                                   (n == 2 && e.format == DXGI_FORMAT_R32G32_FLOAT) ||
                                                   (n == 3 && float_in) || (n == 4 && e.format == DXGI_FORMAT_R32G32B32A32_FLOAT));
            d.type = in_place ? FloatDecl(n) : BYTE(FloatDecl(n) | 0x80);
        }
        if (sh.ffp && !(d.type & 0x80)) {
            el.push_back(d);
            continue;
        }
        d.type &= 0x7F;
        d.stream = conv_stream;
        d.offset = uint16_t(conv_stride);
        d.reg = int8_t(r);
        d.decode = uint8_t(dec);
        conv_stride += DeclBytes(d.type);
        el.push_back(d);
    }
    std::sort(el.begin(), el.end(), [](const DeclElement& x, const DeclElement& y) {
        return x.stream != y.stream ? x.stream < y.stream : x.offset < y.offset;
    });
}

IDirect3DVertexDeclaration9* D3D9Backend::EnsureDeclaration(const std::vector<DeclElement>& el) {
    D3DVERTEXELEMENT9 v[18];
    uint32_t n = 0;
    for (const DeclElement& e : el) {
        if (n == 16) break;
        v[n++] = {e.stream, e.offset, e.type, D3DDECLMETHOD_DEFAULT, e.usage, e.index};
    }
    v[n] = D3DDECL_END();
    uint64_t key = Hash(v, sizeof(D3DVERTEXELEMENT9) * n);
    auto& decl = decls_[key];
    if (!decl && FAILED(dev_->CreateVertexDeclaration(v, &decl))) LOG_FIRST(8, "d3d9: a vertex declaration was refused");
    return decl.Get();
}

// a program's input names, worked out once for each set of registers a draw gives it
const InputUsages& D3D9Backend::ProgramUsages(const State& s, uint16_t regs) {
    auto [it, added] = usages_.try_emplace(s.vs->hash ^ (uint64_t(regs) << 48));
    if (added) ProgramInputUsages(s.vs->program, regs, it->second);
    return it->second;
}

ID3DBlob* D3D9Backend::Compile(const std::string& src, const char* profile) {
    uint64_t hash = Hash(src.data(), src.size(), Hash(profile, std::strlen(profile)));
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d9: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        code = nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kSpriteSalt = 0x3C3C0000;

// a point sprite's program hands its point size to the stage 3 coordinates, which the sprite makes
std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at != std::string::npos) s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

IDirect3DVertexShader9* D3D9Backend::EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh,
                                                        uint64_t decode) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t key = Hash(&decode, 8, seed);
    if (!sh.prog && a.pretransformed) key ^= kPretransformedSalt;
    if (sh.sprite) key ^= kSpriteSalt;
    if (!sh.prog) key = Hash(&sh.fk, sizeof(sh.fk), key);
    auto it = vshaders_.find(key);
    if (it != vshaders_.end()) return it->second.Get();
    std::string src = sh.prog ? VertexProgramHlsl(s.vs->program, a.regs, decode, &sh.usages)
                              : FixedVertexHlsl(a.regs, a.pretransformed, decode, sh.fk);
    if (sh.sprite) src = SpriteSizeHlsl(std::move(src));
    ComPtr<IDirect3DVertexShader9> vs;
    if (ID3DBlob* code = Compile(src, "vs_3_0");
        code && FAILED(dev_->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &vs))) {
        LOGE("d3d9: a vertex shader was refused");
    }
    vshaders_[key] = vs;
    return vs.Get();
}

IDirect3DPixelShader9* D3D9Backend::EnsurePixelShader(const PixelKey& pk, uint64_t key) {
    auto it = pshaders_.find(key);
    if (it != pshaders_.end()) return it->second.Get();
    ComPtr<IDirect3DPixelShader9> ps;
    std::string src = diag::D3d9Solid() ? "float4 main() : COLOR { return float4(1, 0, 1, 1); }" : PixelHlsl(pk);
    if (ID3DBlob* code = Compile(src, "ps_3_0");
        code && FAILED(dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps))) {
        LOGE("d3d9: a pixel shader was refused");
    }
    pshaders_[key] = ps;
    return ps.Get();
}

float D3D9Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float D3D9Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

// blend, depth, stencil and rasterizer state, as the D3D12 renderer keys its pipelines
void D3D9Backend::SetOutputState(const State& s, const Assembled& a, bool ffp) {
    bool blend = SimpleState(s, kMBlendEnable) != 0;
    Rs(D3DRS_ALPHABLENDENABLE, blend);
    if (blend) {
        Rs(D3DRS_SRCBLEND, Blend(SimpleState(s, kMSrcBlend)));
        Rs(D3DRS_DESTBLEND, Blend(SimpleState(s, kMDestBlend)));
        Rs(D3DRS_BLENDOP, BlendOp(SimpleState(s, kMBlendOp)));
        Rs(D3DRS_BLENDFACTOR, SimpleState(s, kMBlendColor));
    }
    // console ARGB bytes to Direct3D 9's R G B A bits
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    Rs(D3DRS_COLORWRITEENABLE, bound_rt_ ? ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3 : 0);
    bool zen = bound_ds_ && s.rs[kRsZEnable];
    Rs(D3DRS_ZENABLE, zen ? D3DZB_TRUE : D3DZB_FALSE);
    if (zen) {
        Rs(D3DRS_ZWRITEENABLE, SimpleState(s, kMZWriteEnable) != 0);
        Rs(D3DRS_ZFUNC, Cmp(SimpleState(s, kMZFunc)));
    }
    bool sten = bound_ds_ && s.rs[kRsStencilEnable];
    Rs(D3DRS_STENCILENABLE, sten);
    if (sten) {
        Rs(D3DRS_STENCILFUNC, Cmp(SimpleState(s, kMStencilFunc)));
        Rs(D3DRS_STENCILREF, SimpleState(s, kMStencilRef) & 0xFF);
        Rs(D3DRS_STENCILMASK, SimpleState(s, kMStencilMask) & 0xFF);
        Rs(D3DRS_STENCILWRITEMASK, SimpleState(s, kMStencilWriteMask) & 0xFF);
        Rs(D3DRS_STENCILFAIL, StencilOp(s.rs[kRsStencilFail]));
        Rs(D3DRS_STENCILZFAIL, StencilOp(SimpleState(s, kMStencilZFail)));
        Rs(D3DRS_STENCILPASS, StencilOp(SimpleState(s, kMStencilPass)));
    }
    // polygon offset: the units are the depth buffer's, and its slopes per console pixel, an upscaled target's smaller
    float units = 0, slope = 0;
    if (bound_ds_ && OffsetsPolygons(s, a.topology)) {
        units = Bits(SimpleState(s, kMDepthOffset)) / s.zscale;
        slope = Bits(SimpleState(s, kMSlopeScale)) * TargetScale();
    }
    Rs(D3DRS_DEPTHBIAS, std::bit_cast<DWORD>(units));
    Rs(D3DRS_SLOPESCALEDEPTHBIAS, std::bit_cast<DWORD>(slope));
    // culling clockwise leaves counter clockwise as the front, as on D3D12
    uint32_t cull = s.rs[kRsCullMode];
    DWORD dcull = D3DCULL_NONE;
    if (a.topology == Topology::Triangles && cull == kCullCw) dcull = D3DCULL_CW;
    if (a.topology == Topology::Triangles && cull == kCullCcw) dcull = D3DCULL_CCW;
    Rs(D3DRS_CULLMODE, dcull);
    uint32_t fill = s.rs[kRsFillMode];
    Rs(D3DRS_FILLMODE, fill == kFillWireframe ? D3DFILL_WIREFRAME : fill == kFillPoint ? D3DFILL_POINT : D3DFILL_SOLID);
    Rs(D3DRS_SHADEMODE, SimpleState(s, kMShadeMode) == kShadeFlat ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    // the shaders test alpha and fog themselves
    bool at = ffp && SimpleState(s, kMAlphaTestEnable);
    Rs(D3DRS_ALPHATESTENABLE, at);
    if (at) {
        Rs(D3DRS_ALPHAREF, SimpleState(s, kMAlphaRef) & 0xFF);
        Rs(D3DRS_ALPHAFUNC, Cmp(SimpleState(s, kMAlphaFunc)));
    }
    if (!ffp) {
        Rs(D3DRS_FOGENABLE, FALSE);
        Rs(D3DRS_LIGHTING, FALSE);
        Rs(D3DRS_SPECULARENABLE, FALSE);
    }
    // points: the title's size, in console pixels
    if (a.topology == Topology::Points) {
        float size = Bits(s.rs[kRsPointSize]) * TargetScale();
        Rs(D3DRS_POINTSIZE, std::bit_cast<DWORD>(size));
        Rs(D3DRS_POINTSPRITEENABLE, s.rs[kRsPointSpriteEnable] != 0);
        Rs(D3DRS_POINTSCALEENABLE, ffp && s.rs[kRsPointScaleEnable]);
        if (ffp && s.rs[kRsPointScaleEnable]) {
            Rs(D3DRS_POINTSCALE_A, s.rs[kRsPointScaleA]);
            Rs(D3DRS_POINTSCALE_B, s.rs[kRsPointScaleB]);
            Rs(D3DRS_POINTSCALE_C, s.rs[kRsPointScaleC]);
            Rs(D3DRS_POINTSIZE_MIN, s.rs[kRsPointSizeMin]);
            Rs(D3DRS_POINTSIZE_MAX, s.rs[kRsPointSizeMax]);
        }
    }
}

D3DTEXTUREFILTERTYPE Filter(uint32_t f) {
    if (f >= kTexfAnisotropic) return f == kTexfAnisotropic ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
    return D3DTEXTUREFILTERTYPE(f);
}

void D3D9Backend::SetSamplers(const State& s, const Assembled& a, const DrawTextures& stages) {
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    for (DWORD i = 0; i < 4; i++) {
        const auto& t = s.tss[i];
        uint32_t mag = t[kTssMagFilter];
        uint32_t min = t[kTssMinFilter];
        uint32_t mip = t[kTssMipFilter];
        uint32_t an = (min == kTexfAnisotropic || mag == kTexfAnisotropic) ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
        // forced only where the title filters across mip levels and the texture has them
        if (aniso && min >= kTexfLinear && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
        an = std::min<uint32_t>(an, std::max<uint32_t>(caps_.MaxAnisotropy, 1));
        D3DTEXTUREFILTERTYPE fmin = an > 1 ? D3DTEXF_ANISOTROPIC : Filter(min);
        D3DTEXTUREFILTERTYPE fmag = Filter(mag);
        if (fmag == D3DTEXF_ANISOTROPIC && !(caps_.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC)) fmag = D3DTEXF_LINEAR;
        // depth is read as a value: the shader compares, a sampler may only take the nearest
        if (stages.info[i].shadow) fmin = fmag = D3DTEXF_POINT;
        Samp(i, D3DSAMP_MINFILTER, fmin);
        Samp(i, D3DSAMP_MAGFILTER, fmag);
        Samp(i, D3DSAMP_MIPFILTER, mip ? (mip == kTexfLinear ? D3DTEXF_LINEAR : D3DTEXF_POINT) : D3DTEXF_NONE);
        Samp(i, D3DSAMP_MAXANISOTROPY, an);
        Samp(i, D3DSAMP_ADDRESSU, Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]));
        Samp(i, D3DSAMP_ADDRESSV, Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]));
        Samp(i, D3DSAMP_ADDRESSW, Address(t[kTssAddressW]));
        Samp(i, D3DSAMP_MIPMAPLODBIAS, t[kTssMipLodBias]);
        Samp(i, D3DSAMP_MAXMIPLEVEL, t[kTssMaxMipLevel]);
        Samp(i, D3DSAMP_BORDERCOLOR, t[kTssBorderColor]);
    }
}

// Direct3D 9's fixed function pipeline set up as the title set the console's: its transforms (with the guest viewport
// and Direct3D 9's pixel centres folded into the projection, the host viewport being the whole target), lights,
// material, texture stages and fog
void D3D9Backend::SetFixedFunction(const State& s, const Assembled& a, const DrawTextures& stages, GuestExtent size) {
    SetTitleTransforms(s, a, size);

    bool lit = !a.pretransformed && s.rs[kRsLighting];
    Rs(D3DRS_LIGHTING, lit);
    Rs(D3DRS_SPECULARENABLE, s.rs[kRsSpecularEnable] != 0);
    if (lit) {
        dev_->SetMaterial(reinterpret_cast<const D3DMATERIAL9*>(s.material.data()));
        for (DWORD i = 0; i < 8; i++) {
            bool on = (s.light_enable >> i) & 1;
            if (on) dev_->SetLight(i, reinterpret_cast<const D3DLIGHT9*>(s.lights[i].data()));
            dev_->LightEnable(i, on);
        }
        Rs(D3DRS_AMBIENT, s.rs[kRsAmbient]);
        Rs(D3DRS_LOCALVIEWER, s.rs[kRsLocalViewer] != 0);
        Rs(D3DRS_NORMALIZENORMALS, s.rs[kRsNormalizeNormals] != 0);
        Rs(D3DRS_COLORVERTEX, s.rs[kRsColorVertex] != 0);
        Rs(D3DRS_DIFFUSEMATERIALSOURCE, s.rs[kRsDiffuseMaterialSource]);
        Rs(D3DRS_SPECULARMATERIALSOURCE, s.rs[kRsSpecularMaterialSource]);
        Rs(D3DRS_AMBIENTMATERIALSOURCE, s.rs[kRsAmbientMaterialSource]);
        Rs(D3DRS_EMISSIVEMATERIALSOURCE, s.rs[kRsEmissiveMaterialSource]);
    }
    Rs(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);  // vertex blending is not drawn, as on D3D12
    Rs(D3DRS_FOGENABLE, s.rs[kRsFogEnable] != 0);
    if (s.rs[kRsFogEnable]) {
        Rs(D3DRS_FOGCOLOR, s.rs[kRsFogColor]);
        Rs(D3DRS_FOGTABLEMODE, s.rs[kRsFogTableMode]);
        Rs(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
        Rs(D3DRS_FOGSTART, s.rs[kRsFogStart]);
        Rs(D3DRS_FOGEND, s.rs[kRsFogEnd]);
        Rs(D3DRS_FOGDENSITY, s.rs[kRsFogDensity]);
        Rs(D3DRS_RANGEFOGENABLE, s.rs[kRsRangeFogEnable] != 0);
    }
    Rs(D3DRS_TEXTUREFACTOR, s.rs[kRsTextureFactor]);

    DWORD i = 0;
    for (; i < 4; i++) {
        const auto& t = s.tss[i];
        uint32_t cop = t[kTssColorOp];
        if (cop <= kTopDisable || cop >= std::size(kTop)) break;
        uint32_t aop = t[kTssAlphaOp];
        // PREMODULATE is drawn as current, as the shaders draw it; alpha off under colour is current alpha
        auto op = [&](uint32_t o, D3DTEXTURESTAGESTATETYPE type) {
            bool keep = o == kTopPremodulate || o <= kTopDisable || o >= std::size(kTop);
            Tss(i, type, keep ? D3DTOP_SELECTARG1 : kTop[o]);
            return keep;
        };
        bool ckeep = op(cop, D3DTSS_COLOROP);
        Tss(i, D3DTSS_COLORARG1, ckeep ? kTaCurrent : t[kTssColorArg1]);
        Tss(i, D3DTSS_COLORARG2, t[kTssColorArg2]);
        Tss(i, D3DTSS_COLORARG0, t[kTssColorArg0]);
        bool akeep = op(aop, D3DTSS_ALPHAOP);
        Tss(i, D3DTSS_ALPHAARG1, akeep ? kTaCurrent : t[kTssAlphaArg1]);
        Tss(i, D3DTSS_ALPHAARG2, t[kTssAlphaArg2]);
        Tss(i, D3DTSS_ALPHAARG0, t[kTssAlphaArg0]);
        Tss(i, D3DTSS_RESULTARG, t[kTssResultArg]);
        Tss(i, D3DTSS_BUMPENVMAT00, t[kTssBumpEnvMat00]);
        Tss(i, D3DTSS_BUMPENVMAT01, t[kTssBumpEnvMat01]);
        Tss(i, D3DTSS_BUMPENVMAT10, t[kTssBumpEnvMat10]);
        Tss(i, D3DTSS_BUMPENVMAT11, t[kTssBumpEnvMat11]);
        Tss(i, D3DTSS_BUMPENVLSCALE, t[kTssBumpEnvLScale]);
        Tss(i, D3DTSS_BUMPENVLOFFSET, t[kTssBumpEnvLOffset]);
        // texture coordinates: the set and texgen as the title asked, a pretransformed draw's set as it is; a linear
        // texture's coordinates are in texels, which its transform scales to 0..1
        uint32_t tci = a.pretransformed ? t[kTssTexCoordIndex] & 0xFFFF : t[kTssTexCoordIndex];
        Tss(i, D3DTSS_TEXCOORDINDEX, tci);
        uint32_t ttff = a.pretransformed ? 0 : t[kTssTextureTransformFlags];
        float tm[16];
        std::memcpy(tm, a.pretransformed ? kIdentity : s.transforms[kTsTexture0 + i].data(), sizeof(tm));
        TextureDesc d;
        if (stages.info[i].rect && s.textures[i] && Describe(s.textures[i], d)) {
            const float texels[16] = {1.0f / d.width, 0, 0, 0, 0, 1.0f / d.height, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            float scaled[16];
            MatMul(tm, texels, scaled);
            std::memcpy(tm, ttff & 7 ? scaled : texels, sizeof(tm));
            if (!(ttff & 7)) ttff = D3DTTFF_COUNT2;
        }
        Tss(i, D3DTSS_TEXTURETRANSFORMFLAGS, ttff);
        if (ttff) dev_->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), reinterpret_cast<const D3DMATRIX*>(tm));
        // a stage reading no texture reads white, as the console does
        SetTexture(i, stages.res[i] ? stages.res[i] : white_.Get());
    }
    if (i < 8) {
        Tss(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
        Tss(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

// The generator's constants: a program's own registers c0 to c191 and the draw's from kSm3ProgramDraw, a fixed function
// shader's from c0, the pixel shader's from c0; c255 the host pixel, which takes Direct3D 9's pixel centres to D3D12's
void D3D9Backend::SetShaderConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                     GuestExtent size) {
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (asm_.topology == Topology::Points) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) {
            c.texsize[i][0] = float(d.width);
            c.texsize[i][1] = float(d.height);
            // a 3 console texel box per tap keeps the console chain's glow width and brightness
            if (stages.wide[i] > 0) {
                constexpr float kGlowBoxTexels = 3.0f;
                float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
                c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
                c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
                c.spread[i][2] = n;
            }
        }
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    constexpr uint32_t kDrawRegs = sizeof(DrawConstants) / 16;
    constexpr uint32_t kProgramDrawRegs = offsetof(DrawConstants, wv) / 16;  // surf to spread
    if (sh.prog) {
        dev_->SetVertexShaderConstantF(0, s.vsc[0].data(), UINT(s.vsc.size()));
        dev_->SetVertexShaderConstantF(kSm3ProgramDraw, c.surf, kProgramDrawRegs);
    } else {
        dev_->SetVertexShaderConstantF(0, c.surf, kDrawRegs);
    }
    float hw = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float hh = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    const float tail[8] = {0, 0, 0, 0, -1.0f / hw, 1.0f / hh, 0, 0};
    dev_->SetVertexShaderConstantF(kSm3DepthClamp, tail, 2);
    dev_->SetPixelShaderConstantF(0, c.surf, kDrawRegs);
}

// the host viewport covers the target, the guest one is in the transform; the scissor clips to it
void D3D9Backend::SetViewportScissor(const State& s, uint32_t gw) {
    uint32_t W = bound_rt_ ? bound_rt_->w : bound_ds_->w;
    uint32_t H = bound_rt_ ? bound_rt_->h : bound_ds_->h;
    D3DVIEWPORT9 vp{0, 0, W, H, 0, 1};
    dev_->SetViewport(&vp);
    float sc = float(W) / gw;
    LONG x0 = LONG(s.viewport.x * sc);
    LONG y0 = LONG(s.viewport.y * sc);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sc);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sc));
        y0 = std::max(y0, LONG(s.scissors[1] * sc));
        x1 = std::min(x1, LONG(s.scissors[2] * sc));
        y1 = std::min(y1, LONG(s.scissors[3] * sc));
    }
    RECT r{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)), std::clamp<LONG>(x1, 0, LONG(W)),
           std::clamp<LONG>(y1, 0, LONG(H))};
    dev_->SetScissorRect(&r);
    Rs(D3DRS_SCISSORTESTENABLE, TRUE);
}

// bytes into the vertex ring, 16 byte aligned; its offset, or ~0u when it does not fit
uint32_t D3D9Backend::RingVertices(const void* src, uint32_t bytes, void** write) {
    if (bytes > kVbRingBytes) return ~0u;
    uint32_t at = (vb_at_ + 15) & ~15u;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (at + bytes > kVbRingBytes) {
        at = 0;
        flags = D3DLOCK_DISCARD;
    }
    void* p = nullptr;
    if (FAILED(vb_->Lock(at, bytes, &p, flags))) return ~0u;
    if (src) std::memcpy(p, src, bytes);
    if (write) *write = p;
    else vb_->Unlock();
    vb_at_ = at + bytes;
    return at;
}

D3DPRIMITIVETYPE PrimitiveType(const Assembled& a) {
    if (a.topology == Topology::Points) return D3DPT_POINTLIST;
    if (a.topology == Topology::Lines) return a.strip ? D3DPT_LINESTRIP : D3DPT_LINELIST;
    return a.strip ? D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST;
}

uint32_t PrimitiveCount(const Assembled& a, uint32_t n) {
    if (a.topology == Topology::Points) return n;
    if (a.topology == Topology::Lines) return a.strip ? (n >= 2 ? n - 1 : 0) : n / 2;
    return a.strip ? (n >= 3 ? n - 2 : 0) : n / 3;
}

// the draw's streams through the ring, the converted elements made on the way, then its indices, rebased to 0; g is
// the draw to make
bool D3D9Backend::DrawGeometry(const Assembled& a, const std::vector<DeclElement>& el, uint32_t conv_stride,
                               Geometry& g) {
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        uint32_t at = RingVertices(b.src, b.bytes);
        if (at == ~0u) return false;
        dev_->SetStreamSource(i, vb_.Get(), at, b.stride);
    }
    if (conv_stride) {
        uint32_t n = a.vertex_count;
        uint8_t* out = nullptr;
        uint32_t at = RingVertices(nullptr, n * conv_stride, reinterpret_cast<void**>(&out));
        if (at == ~0u) return false;
        for (const DeclElement& e : el) {
            if (e.reg < 0) continue;
            const VertexElement& ve = a.elements[size_t(e.reg)];
            const VertexBuffer& b = a.buffers[ve.buffer];
            for (uint32_t v = 0; v < n; v++) {
                float f[4];
                const uint8_t* src = b.src + size_t(v) * b.stride + ve.offset;
                if (size_t(v) * b.stride + ve.offset + 4 > b.bytes) std::fill(f, f + 4, 0.0f);
                else ReadElement(ve.format, e.decode, src, f);
                uint8_t* dst = out + size_t(v) * conv_stride + e.offset;
                if (e.type == D3DDECLTYPE_D3DCOLOR) {
                    uint32_t c = PackColor4(f);
                    std::memcpy(dst, &c, 4);
                } else {
                    std::memcpy(dst, f, DeclBytes(e.type));
                }
            }
        }
        vb_->Unlock();
        dev_->SetStreamSource(a.buffer_count, vb_.Get(), at, conv_stride);
    }
    g.type = PrimitiveType(a);
    if (!a.index_count) {
        g.prims = PrimitiveCount(a, a.draw_count);
        g.vertices = a.draw_count;
        return true;
    }
    g.prims = PrimitiveCount(a, a.index_count);
    g.vertices = a.vertex_count;
    if (!g.prims) return true;
    bool wide = !a.index16;
    uint32_t bytes = a.index_count * (wide ? 4 : 2);
    IDirect3DIndexBuffer9* ib = wide ? ib32_.Get() : ib16_.Get();
    uint32_t& ring = wide ? ib32_at_ : ib16_at_;
    if (bytes > kIbRingBytes) return false;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (ring + bytes > kIbRingBytes) {
        ring = 0;
        flags = D3DLOCK_DISCARD;
    }
    void* p = nullptr;
    if (FAILED(ib->Lock(ring, bytes, &p, flags))) return false;
    g.indices = a.index_src;
    if (wide) {
        std::memcpy(p, a.index_src, bytes);
    } else {
        // the title's indices, moved down to the first vertex the buffers start at; a copy for the mods
        const uint16_t* in = static_cast<const uint16_t*>(a.index_src);
        uint16_t* o = static_cast<uint16_t*>(p);
        if (!mods::D3d9().draw.empty()) {
            index_copy_.resize(a.index_count);
            for (uint32_t i = 0; i < a.index_count; i++) index_copy_[i] = uint16_t(in[i] + a.base_vertex);
            std::memcpy(o, index_copy_.data(), bytes);
            g.indices = index_copy_.data();
        } else {
            for (uint32_t i = 0; i < a.index_count; i++) o[i] = uint16_t(in[i] + a.base_vertex);
        }
    }
    ib->Unlock();
    g.indexed = true;
    g.first_index = ring / (wide ? 4 : 2);
    g.index_bytes = wide ? 4 : 2;
    g.index_count = a.index_count;
    ring += bytes;
    dev_->SetIndices(ib);
    return true;
}

// Direct3D 9 puts pixel centres half a pixel off Direct3D 10's: half a host pixel left and up, times w
void D3D9Backend::HalfPixel(float* m) const {
    float hw = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float hh = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    const float half[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1.0f / hw, 1.0f / hh, 0, 1};
    std::memcpy(m, half, sizeof(half));
}

// a PROJECTION as the title's is drawn: the guest viewport inside the target and the half pixel folded in
void D3D9Backend::FoldProjection(const State& s, GuestExtent size, const float* m, float* out) const {
    float vpm[16];
    float pv[16];
    float half[16];
    ViewportMatrix(s, size, vpm);
    MatMul(m, vpm, pv);
    HalfPixel(half);
    MatMul(pv, half, out);
}

void D3D9Backend::SetDeviceTransform(int i, const float* m) {
    static const D3DTRANSFORMSTATETYPE kType[3] = {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION};
    if (dev_xf_ok_[i] && !std::memcmp(dev_xf_[i], m, sizeof(dev_xf_[i]))) return;
    std::memcpy(dev_xf_[i], m, sizeof(dev_xf_[i]));
    dev_xf_ok_[i] = true;
    dev_->SetTransform(kType[i], reinterpret_cast<const D3DMATRIX*>(m));
}

// the title's WORLD, VIEW and PROJECTION, or one a mod set until the title sets that one again. Fixed function draws
// with them; under a shader Direct3D 9 ignores them and a runtime that reads them (RTX Remix) does not
void D3D9Backend::SetTitleTransforms(const State& s, const Assembled& a, GuestExtent size) {
    const float* raw[3] = {s.transforms[kTsWorld].data(), s.transforms[kTsView].data(),
                           s.transforms[kTsProjection].data()};
    const float* want[3];
    for (int i = 0; i < 3; i++) {
        ModTransform& o = mod_xf_[i];
        if (o.on && std::memcmp(o.title, raw[i], sizeof(o.title))) o.on = false;
        std::memcpy(title_xf_[i], raw[i], sizeof(title_xf_[i]));
        want[i] = o.on ? o.m : raw[i];
    }
    float proj[16];
    if (a.pretransformed) {
        // screen pixels of the guest target to clip space; z is already depth
        float ortho[16] = {2.0f / size.w, 0, 0, 0, 0, -2.0f / size.h, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
        float half[16];
        HalfPixel(half);
        MatMul(ortho, half, proj);
        SetDeviceTransform(0, kIdentity);
        SetDeviceTransform(1, kIdentity);
    } else {
        FoldProjection(s, size, want[2], proj);
        SetDeviceTransform(0, want[0]);
        SetDeviceTransform(1, want[1]);
    }
    SetDeviceTransform(2, proj);
}

// the bound target is not one the title presents
bool D3D9Backend::Offscreen() const {
    if (!bound_rt_) return true;
    for (uint32_t d : screens_)
        if (d && d == bound_rt_->data) return false;
    return true;
}

// the mean colour of a target and how much of it is not black, read back (D3D9Backend::Present)
std::string PixelStats(IDirect3DDevice9* dev, IDirect3DSurface9* rt);

bool Orthographic(const float* p) { return p[3] == 0 && p[7] == 0 && p[11] == 0 && p[15] == 1; }

// the mods' befores, in the order they asked; false leaves the draw out, and no later before or any after is called
bool D3D9Backend::ModsBefore(const State& s, const Assembled& a, const DrawShaders& sh, const DrawTextures& stages,
                             const Geometry& g) {
    mods_called_ = 0;
    const auto& hooks = mods::D3d9().draw;
    if (hooks.empty()) return true;
    RecompModDraw& d = mod_draw_;
    d = {};
    d.size = sizeof(d);
    d.device = dev_.Get();
    d.title = 1;
    d.program = sh.prog;
    d.program_id = sh.prog ? s.vs->hash : 0;
    // a program projects through the camera when it reads the registers the game names for it
    int preg = g_projection_register;
    if (sh.prog && preg >= 0 && preg + 4 <= int(s.vsc.size()) && ReadsConstant(s.vs->program, uint32_t(preg))) {
        d.program_projection_set = 1;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) d.program_projection[r * 4 + c] = s.vsc[size_t(preg + c)][size_t(r)];
    }
    // 2D: a program that projects through nothing or orthographically, a fixed function draw in screen pixels or under
    // an orthographic projection
    const float* proj = s.transforms[kTsProjection].data();
    d.two_d = sh.prog ? !d.program_projection_set || Orthographic(d.program_projection)
                      : a.pretransformed || Orthographic(proj);
    d.offscreen = Offscreen();
    d.primitive = uint32_t(g.type);
    d.primitives = g.prims;
    for (int i = 0; i < 4; i++) {
        d.textures[i] = stages.res[i];
        d.texture_ids[i] = stages.res[i] ? stages.id[i] : 0;
        if (stages.res[i] && stages.rt[i]) d.screen_copies |= 1u << i;
    }
    d.depth_state = 1;
    d.depth_enable = rs_ok_[D3DRS_ZENABLE] && rs_[D3DRS_ZENABLE];
    d.depth_write = d.depth_enable && rs_ok_[D3DRS_ZWRITEENABLE] && rs_[D3DRS_ZWRITEENABLE];
    d.depth_func = rs_ok_[D3DRS_ZFUNC] ? rs_[D3DRS_ZFUNC] : D3DCMP_LESSEQUAL;
    d.projection_set = !sh.prog && !a.pretransformed;
    std::memcpy(d.projection, proj, sizeof(d.projection));
    std::memcpy(d.world, dev_xf_[0], sizeof(d.world));
    std::memcpy(d.view, dev_xf_[1], sizeof(d.view));
    d.indexed = g.indexed;
    d.first_vertex = 0;
    d.vertices = g.vertices;
    d.indices = g.indexed ? g.indices : nullptr;
    d.index_bytes = g.indexed ? g.index_bytes : 0;
    d.index_count = g.index_count;
    d.base_vertex = 0;
    for (const mods::DrawHook& h : hooks) {
        if (h.before && h.before(&d, h.user) == RECOMP_DRAW_SKIP) return false;
        mods_called_++;
    }
    return true;
}

// the camera RTX Remix takes from a draw's WORLD, VIEW and PROJECTION (CameraManager::processCameraData): none for an
// identity projection or a VIEW that leaves WORLD as it is, rejected for a projection leaning sideways over 0.01 radian
// or seeing under 0.001 radian high (an orthographic one), else the main camera, or the sky's where the depth range
// starts at 1. The angles are found from the frustum's planes as Remix finds them (MathLib DecomposeProjection)
static const char* RemixCamera(const float* world, const float* view, const float* p, float min_z, float& fov,
                               float& lean) {
    fov = lean = 0;
    if (!std::memcmp(p, kIdentity, sizeof(kIdentity))) return "none";
    float wv[16];
    MatMul(world, view, wv);
    if (!std::memcmp(wv, world, sizeof(wv)) && std::memcmp(wv, kIdentity, sizeof(wv))) return "none, VIEW leaves WORLD";
    if (p[15] != 1) {
        // the planes through the eye: left and right (w + x, w - x), bottom and top (w + y, w - y), as z over x or y
        float x0 = (p[11] + p[8]) / (p[3] + p[0]), x1 = (p[11] - p[8]) / (p[3] - p[0]);
        float y0 = (p[11] + p[9]) / (p[7] + p[5]), y1 = (p[11] - p[9]) / (p[7] - p[5]);
        fov = std::fabs(std::atan(y1) - std::atan(y0));
        lean = (std::atan(x0) + std::atan(x1)) * 0.5f;
    }
    bool bad = !(std::fabs(lean) <= 0.01f) || !(fov >= 0.001f);
    fov *= 57.29578f;
    lean *= 57.29578f;
    if (bad) return "rejected";
    return min_z >= 1 ? "sky" : "main";
}

// XBE_D3D9_PRIME, a test of what RTX Remix needs from a frame before it puts its picture in at the first UI draw: it does
// only after the frame drew into another target first (TS2's scanner map, from the TS2-RTX mod). Before each frame's
// first draw, into a target a row taller than the back buffer: 1 binds it and binds the title's back, 2 also draws a
// pixel into it, 3 draws the frame's first draw into it as well, as the title made it, 4 fills it black
void D3D9Backend::Prime(const Geometry* g) {
    static const int mode = [] {
        const char* e = std::getenv("XBE_D3D9_PRIME");
        return e ? std::atoi(e) : 0;
    }();
    if (!mode || !prime_due_ || (mode >= 3) != (g != nullptr)) return;
    prime_due_ = false;
    if (!prime_ && FAILED(dev_->CreateRenderTarget(bb_w_, bb_h_ + 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                   &prime_, nullptr))) {
        prime_ = nullptr;
        return;
    }
    if (mode == 4) {
        dev_->ColorFill(prime_.Get(), nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        return;
    }
    ComPtr<IDirect3DSurface9> rt, ds;
    dev_->GetRenderTarget(0, &rt);
    dev_->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 vp;
    RECT sc;
    dev_->GetViewport(&vp);
    dev_->GetScissorRect(&sc);
    dev_->SetDepthStencilSurface(nullptr);
    dev_->SetRenderTarget(0, prime_.Get());
    if (mode == 2) {
        Rs(D3DRS_ALPHABLENDENABLE, FALSE);
        Rs(D3DRS_COLORWRITEENABLE, 15);
        DrawQuad(nullptr, RECT{0, 0, 1, 1}, 0xFF000000, nullptr);
    } else if (mode == 3) {
        dev_->SetViewport(&vp);
        dev_->SetScissorRect(&sc);
        if (g->indexed) dev_->DrawIndexedPrimitive(g->type, 0, 0, g->vertices, g->first_index, g->prims);
        else dev_->DrawPrimitive(g->type, 0, g->prims);
    }
    dev_->SetRenderTarget(0, rt.Get());
    dev_->SetDepthStencilSurface(ds.Get());
    dev_->SetViewport(&vp);
    dev_->SetScissorRect(&sc);
}

void D3D9Backend::CountCamera(const DrawShaders& sh, bool drawn) {
    if (mods::D3d9().draw.empty()) return;
    const RecompModDraw& d = mod_draw_;
    CameraCounts& c = camera_;
    uint32_t index = c.frame.draws++;
    // RTX Remix's own rule for UI (D3D9Rtx::isRenderingUI): a draw without a vertex shader, under an orthographic
    // PROJECTION, writing no depth; it puts its picture in there, and draws everything after it flat. Only on a target
    // the back buffer's size, writing all of red, green and blue, outside an occlusion query, as Remix checks those first
    bool ffp_ui = false;
    if (drawn && sh.ffp && bound_rt_ && bound_rt_->w == bb_w_ && bound_rt_->h == bb_h_) {
        DWORD zw = TRUE, cw = 15;
        dev_->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
        dev_->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        bool counted = mod_state_ && mod_state_->simple[kMCountEnable >> 2];
        ffp_ui = dev_xf_[2][15] == 1.0f && !zw && (cw & 7) == 7 && !counted;
    }
    if (ffp_ui && !c.frame.ffp_ui++) {
        c.frame.ffp_ui_at = index;
        c.frame.ffp_ui_early = !c.frame.marked;
    }
    if (!sh.prog) {
        // fixed function draws only go in the lists
        CameraCounts::After a{};
        a.drawn = drawn;
        a.ffp = sh.ffp;
        a.ffp_ui = ffp_ui;
        a.primitives = d.primitives;
        a.two_d = d.two_d;
        a.offscreen = d.offscreen;
        for (int i = 0; i < 4; i++)
            if (d.textures[i]) a.textures |= uint8_t(1u << i);
        DWORD v = 0;
        a.write = uint8_t(SUCCEEDED(dev_->GetRenderState(D3DRS_COLORWRITEENABLE, &v)) ? v : 15);
        a.blend = SUCCEEDED(dev_->GetRenderState(D3DRS_ALPHABLENDENABLE, &v)) && v;
        if (SUCCEEDED(dev_->GetRenderState(D3DRS_SRCBLEND, &v))) a.src = uint8_t(v);
        if (SUCCEEDED(dev_->GetRenderState(D3DRS_DESTBLEND, &v))) a.dst = uint8_t(v);
        a.zenable = SUCCEEDED(dev_->GetRenderState(D3DRS_ZENABLE, &v)) && v;
        a.zwrite = SUCCEEDED(dev_->GetRenderState(D3DRS_ZWRITEENABLE, &v)) && v;
        D3DVIEWPORT9 vp{};
        if (SUCCEEDED(dev_->GetViewport(&vp))) {
            a.min_z = vp.MinZ;
            a.max_z = vp.MaxZ;
        }
        a.w = bound_rt_ ? bound_rt_->w : 0;
        a.h = bound_rt_ ? bound_rt_->h : 0;
        a.remix = RemixCamera(dev_xf_[0], dev_xf_[1], dev_xf_[2], a.min_z, a.fov, a.lean);
        if (c.frame.marked && c.after.size() < 48) c.after.push_back(a);
        if (c.first.size() < 12) c.first.push_back(a);
        return;
    }
    c.programs++;
    if (!drawn) c.skipped++;
    CameraCounts::Frame& f = c.frame;
    if (drawn && !f.marked) {
        IDirect3DBaseTexture9* vt = nullptr;
        if (SUCCEEDED(dev_->GetTexture(D3DVERTEXTEXTURESAMPLER3, &vt)) && vt) {
            vt->Release();
            f.marked = true;
            f.marked_after = f.three_d;
            f.primitives = d.primitives;
            f.offscreen = d.offscreen;
            f.two_d = d.two_d;
            f.program = d.program_id;
            f.w = bound_rt_ ? bound_rt_->w : 0;
            f.h = bound_rt_ ? bound_rt_->h : 0;
        }
    }
    auto entry = [&] {
        CameraCounts::After a{};
        a.program = d.program_id;
        a.primitives = d.primitives;
        a.copies = uint8_t(d.screen_copies);
        a.drawn = drawn;
        a.two_d = d.two_d;
        a.offscreen = d.offscreen;
        for (int i = 0; i < 4; i++)
            if (d.textures[i]) a.textures |= uint8_t(1u << i);
        a.write = uint8_t(rs_ok_[D3DRS_COLORWRITEENABLE] ? rs_[D3DRS_COLORWRITEENABLE] : 15);
        a.blend = rs_ok_[D3DRS_ALPHABLENDENABLE] && rs_[D3DRS_ALPHABLENDENABLE];
        a.src = uint8_t(rs_[D3DRS_SRCBLEND]);
        a.dst = uint8_t(rs_[D3DRS_DESTBLEND]);
        a.zenable = rs_ok_[D3DRS_ZENABLE] && rs_[D3DRS_ZENABLE];
        a.zwrite = rs_ok_[D3DRS_ZWRITEENABLE] && rs_[D3DRS_ZWRITEENABLE];
        D3DVIEWPORT9 vp{};
        if (SUCCEEDED(dev_->GetViewport(&vp))) {
            a.min_z = vp.MinZ;
            a.max_z = vp.MaxZ;
        }
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(dev_->GetRenderTarget(0, &rt)) && rt) {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(rt->GetDesc(&desc))) {
                a.w = desc.Width;
                a.h = desc.Height;
            }
            rt->Release();
        }
        IDirect3DBaseTexture9* vt = nullptr;
        if (drawn && SUCCEEDED(dev_->GetTexture(D3DVERTEXTEXTURESAMPLER3, &vt)) && vt) {
            vt->Release();
            a.marked = true;
        }
        a.remix = RemixCamera(dev_xf_[0], dev_xf_[1], dev_xf_[2], a.min_z, a.fov, a.lean);
        a.counted = drawn && mod_state_ && mod_state_->simple[kMCountEnable >> 2];
        return a;
    };
    if (f.marked && c.after.size() < 48) c.after.push_back(entry());
    if (c.first.size() < 12) c.first.push_back(entry());
    // a target other than the screen that RTX Remix takes for the picture all the same, being the back buffer's size
    if (drawn && d.offscreen && bound_rt_ && bound_rt_->w == bb_w_ && bound_rt_->h == bb_h_) c.offscreen_size++;
    if (d.offscreen) return;
    if (drawn && !d.two_d) f.three_d++;
    c.screen++;
    if (d.program_projection_set) c.projected++;
    if (d.two_d) c.two_d++;
    if (c.sampled || d.two_d || !drawn) return;
    c.sampled = true;
    std::memcpy(c.projection, d.program_projection, sizeof(c.projection));
    std::memcpy(c.view, dev_xf_[1], sizeof(c.view));
    std::memcpy(c.device_projection, dev_xf_[2], sizeof(c.device_projection));
    c.w = bound_rt_ ? bound_rt_->w : 0;
    c.h = bound_rt_ ? bound_rt_->h : 0;
}

// the mean brightness of a target, 0 to 255, from a small copy of it
float D3D9Backend::MeanBrightness(IDirect3DSurface9* rt) {
    constexpr UINT kW = 160, kH = 120;
    CameraCounts& c = camera_;
    if (!c.thumb && (FAILED(dev_->CreateRenderTarget(kW, kH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &c.thumb,
                                                     nullptr)) ||
                     FAILED(dev_->CreateOffscreenPlainSurface(kW, kH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &c.thumb_sys,
                                                              nullptr)))) {
        c.thumb = nullptr;
        return -1;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(dev_->StretchRect(rt, nullptr, c.thumb.Get(), nullptr, D3DTEXF_LINEAR)) ||
        FAILED(dev_->GetRenderTargetData(c.thumb.Get(), c.thumb_sys.Get())) ||
        FAILED(c.thumb_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        return -1;
    }
    uint64_t sum = 0;
    for (UINT y = 0; y < kH; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
        for (UINT x = 0; x < kW; x++) sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
    }
    c.thumb_sys->UnlockRect();
    return float(sum) / float(kW * kH * 3);
}

// after each draw of the probed frame: where the picture changed at once, as when RTX Remix put its own in
void D3D9Backend::TraceBrightness(const DrawShaders& sh) {
    CameraCounts& c = camera_;
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev_->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC desc;
    rt->GetDesc(&desc);
    float m = desc.Width == bb_w_ && desc.Height == bb_h_ ? MeanBrightness(rt) : -1;
    rt->Release();
    uint32_t index = c.probe_draws++;
    if (m < 0) return;
    if (c.probe_mean >= 0 && std::fabs(m - c.probe_mean) > 8 && c.jumps.size() < 32) {
        IDirect3DBaseTexture9* vt = nullptr;
        bool marked = SUCCEEDED(dev_->GetTexture(D3DVERTEXTEXTURESAMPLER3, &vt)) && vt;
        if (vt) vt->Release();
        const RecompModDraw& d = mod_draw_;
        c.jumps.push_back({index, c.probe_mean, m, sh.prog ? d.program_id : 0, d.primitives, desc.Width, desc.Height,
                           bool(d.two_d), marked, bool(d.offscreen)});
    }
    c.probe_mean = m;
}

// a copy, clear or upload after the frame's mark, which changes the picture behind RTX Remix's back
void D3D9Backend::NoteAfterMark(const char* op, uint32_t from, uint32_t to) {
    CameraCounts& c = camera_;
    if (mods::D3d9().draw.empty() || !c.frame.marked || c.after.size() >= 48) return;
    CameraCounts::After a{};
    a.op = op;
    a.from = from;
    a.to = to;
    c.after.push_back(a);
}

void D3D9Backend::EndCameraFrame() {
    CameraCounts& c = camera_;
    c.frames++;
    if (!c.frame.marked) c.unmarked++;
    else if (c.frame.marked_after * 2 < c.frame.three_d) c.early++;
    c.last = c.frame;
    c.frame = {};
    c.after_last.swap(c.after);
    c.after.clear();
    c.first_last.swap(c.first);
    c.first.clear();
}

void D3D9Backend::ReportCamera() {
    CameraCounts& c = camera_;
    auto row = [](const float* m) {
        char b[160];
        std::snprintf(b, sizeof(b), "[%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
                      m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
                      m[15]);
        return std::string(b);
    };
    LOGR("d3d9: camera: %u program draws, %u skipped by mods; %u on screen, %u read the projection, %u 2D",
         c.programs, c.skipped, c.screen, c.projected, c.two_d);
    if (c.sampled) {
        LOGR("d3d9: camera: first 3D draw on screen (target %ux%u, back buffer %ux%u): program projection %s",
             c.w, c.h, bb_w_, bb_h_, row(c.projection).c_str());
        LOGR("d3d9: camera: device VIEW %s", row(c.view).c_str());
        LOGR("d3d9: camera: device PROJECTION %s", row(c.device_projection).c_str());
    }
    LOGR("d3d9: camera: %u frames, %u with no draw marked, %u marked before half their 3D; %u draws into other targets "
         "of the back buffer's size",
         c.frames, c.unmarked, c.early, c.offscreen_size);
    const CameraCounts::Frame& l = c.last;
    if (l.marked)
        LOGR("d3d9: camera: last frame: %u 3D draws on screen, first marked after %u: program %016llX, %u primitives, "
             "%s, %s %ux%u",
             l.three_d, l.marked_after, (unsigned long long)l.program, l.primitives, l.two_d ? "2D" : "3D",
             l.offscreen ? "offscreen" : "screen", l.w, l.h);
    else
        LOGR("d3d9: camera: last frame: %u 3D draws on screen, none marked", l.three_d);
    if (l.ffp_ui)
        LOGR("d3d9: camera: last frame: %u fixed function draws RTX Remix takes for UI itself, the first at draw %u, %s "
             "the first marked one",
             l.ffp_ui, l.ffp_ui_at, l.ffp_ui_early ? "before" : "after");
    if (!c.before_mark.empty())
        LOGR("d3d9: camera: the frame before: its first marked draw's target before it %s; after it %s; presented %s",
             c.before_mark.c_str(), c.after_mark.c_str(), c.at_present.c_str());
    if (c.probe_draws)
        LOGR("d3d9: camera: the frame before, draw by draw: %u draws, %zu jumps in brightness", c.probe_draws,
             c.jumps.size());
    for (const CameraCounts::Jump& j : c.jumps)
        LOGR("d3d9: camera:   after draw %u: %.0f to %.0f, program %016llX, %u primitives, %s, %s, %s %ux%u", j.index,
             j.from, j.to, (unsigned long long)j.program, j.primitives, j.two_d ? "2D" : "3D",
             j.marked ? "marked" : "unmarked", j.offscreen ? "offscreen" : "screen", j.w, j.h);
    auto print = [&](const char* title, const std::vector<CameraCounts::After>& list) {
        LOGR("d3d9: camera: %s", title);
        for (size_t i = 0; i < list.size(); i++) {
            const CameraCounts::After& a = list[i];
            auto where = [this](uint32_t data) {
                for (uint32_t d : screens_)
                    if (d && d == data) return "the screen";
                return "a texture";
            };
            if (a.op) {
                LOGR("d3d9: camera:   %2zu %s %08X (%s) to %08X (%s)", i, a.op, a.from, a.from ? where(a.from) : "-", a.to,
                     where(a.to));
                continue;
            }
            char blend[24] = "no blend";
            if (a.blend) std::snprintf(blend, sizeof(blend), "blend %u %u", a.src, a.dst);
            char remix[64] = "";
            if (a.drawn && a.remix)
                std::snprintf(remix, sizeof(remix), ", Remix camera %s (fov %.1f, lean %.2f)", a.remix, a.fov, a.lean);
            char what[40];
            if (a.program) std::snprintf(what, sizeof(what), "program %016llX", (unsigned long long)a.program);
            else std::snprintf(what, sizeof(what), "%s", a.ffp ? "fixed function" : "generated fixed function");
            if (a.ffp_ui) std::snprintf(remix, sizeof(remix), ", Remix takes it for UI itself");
            LOGR("d3d9: camera:   %2zu %s, %u primitives, %s, textures %X, screen copies %X, writes %X, %s, "
                 "depth %s%s, viewport z %.2g-%.2g, %s %ux%u, %s%s%s",
                 i, what, a.primitives, a.two_d ? "2D" : "3D", a.textures, a.copies, a.write,
                 blend, a.zenable ? "on" : "off", a.zwrite ? " written" : "", a.min_z, a.max_z,
                 a.offscreen ? "offscreen" : "screen", a.w, a.h, !a.drawn ? "left out" : a.marked ? "marked" : "drawn",
                 a.counted ? ", counted" : "", remix);
        }
    };
    print("the last frame's first draws:", c.first_last);
    print("and its draws from the first marked one on:", c.after_last);
    c = {};
}

void D3D9Backend::ModsAfter() {
    const auto& hooks = mods::D3d9().draw;
    for (size_t i = mods_called_; i-- > 0;)
        if (hooks[i].after) hooks[i].after(&mod_draw_, hooks[i].user);
    mods_called_ = 0;
}

uint32_t D3D9Backend::VertexConstants(uint32_t first, uint32_t count, float (*out)[4]) {
    if (!mod_state_ || first >= mod_state_->vsc.size()) return 0;
    count = std::min<uint32_t>(count, uint32_t(mod_state_->vsc.size()) - first);
    for (uint32_t i = 0; i < count; i++) std::memcpy(out[i], mod_state_->vsc[first + i].data(), sizeof(out[i]));
    return count;
}

// the title's vertex buffer whose memory one of the draw's streams starts in, and the vertex it starts at
uint32_t D3D9Backend::DrawVertexBuffer(uint32_t* data, uint32_t* stride, uint32_t* first) {
    if (!mod_state_ || !mod_asm_) return 0;
    const State& s = *mod_state_;
    const Assembled& a = *mod_asm_;
    constexpr uint32_t kResData = 4;
    constexpr uint32_t kMaxVertices = 1u << 20;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1)) continue;
        const VertexBuffer& b = a.buffers[a.elements[size_t(r)].buffer];
        for (const State::Stream& st : s.streams) {
            if (!st.vb || !b.stride) continue;
            uint32_t va = mem::VirtualFromPhysical(kernel::R32(st.vb + kResData));
            const uint8_t* base = static_cast<const uint8_t*>(kernel::HostPtr(va));
            if (b.src < base) continue;
            size_t off = size_t(b.src - base);
            if (off % b.stride || off / b.stride >= kMaxVertices) continue;
            *data = va;
            *stride = b.stride;
            *first = uint32_t(off / b.stride);
            return st.vb;
        }
    }
    return 0;
}

bool D3D9Backend::RenderState(uint32_t state, uint32_t* value) {
    if (state >= std::size(rs_) || !rs_ok_[state]) return false;
    *value = rs_[state];
    return true;
}

void D3D9Backend::SetProjection(const float m[16]) {
    if (!mod_state_) return;
    float p[16];
    FoldProjection(*mod_state_, mod_size_, m, p);
    SetDeviceTransform(2, p);
}

// kept for the draws after until the title sets that transform again; the draw in hand gets it at once
void D3D9Backend::SetTransform(uint32_t type, const float m[16]) {
    int i = type == D3DTS_WORLD ? 0 : type == D3DTS_VIEW ? 1 : type == D3DTS_PROJECTION ? 2 : -1;
    if (i < 0) return;
    ModTransform& o = mod_xf_[i];
    o.on = true;
    std::memcpy(o.m, m, sizeof(o.m));
    std::memcpy(o.title, title_xf_[i], sizeof(o.title));
    if (i < 2) SetDeviceTransform(i, m);
    else SetProjection(m);
}

IDirect3DQuery9* D3D9Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2]) return nullptr;
    if (queries_.size() < kQueries) {
        ComPtr<IDirect3DQuery9> q;
        if (FAILED(dev_->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) {
            LOG_FIRST(1, "d3d9: no occlusion queries, visibility tests stay pending");
            return nullptr;
        }
        queries_.push_back(q);
    }
    IDirect3DQuery9* q = queries_[query_next_++ % queries_.size()].Get();
    q->Issue(D3DISSUE_BEGIN);
    return q;
}

void D3D9Backend::EndCountedDraw(IDirect3DQuery9* q) {
    if (!q) return;
    q->Issue(D3DISSUE_END);
    counted_.push_back({q, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale comes out
float D3D9Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
}

// reports whose queries are done, written as the console's GPU writes them: time, count, then 0 for done; wait: the
// oldest are waited for
void D3D9Backend::DeliverReports(bool wait) {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    while (!reports_.empty()) {
        const Report& r = reports_.front();
        bool force = wait || presents_ - r.present >= kReportWait;
        double pixels = 0;
        bool done = true;
        for (const Query& q : r.queries) {
            DWORD n = 0;
            HRESULT hr;
            while ((hr = q.q->GetData(&n, sizeof(n), D3DGETDATA_FLUSH)) == S_FALSE && force) {}
            if (hr != S_OK) {
                done = !force ? false : done;
                n = 0;
            }
            pixels += double(n) / q.samples_per_pixel;
        }
        if (!done) return;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

void D3D9Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    if (!Usable()) return;
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) {
        counts_.assemble++;
        return;
    }
    if (!BindTargets(s)) {
        counts_.targets++;
        return;
    }
    Prime(nullptr);
    Scene(true);

    // textures first: what the stages hold decides whether fixed function can draw them
    DrawTextures stages;
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    // a texture copy may have bound other targets
    if ((bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) && !BindTargets(s)) {
        counts_.targets++;
        return;
    }

    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = a.topology == Topology::Points && sh.prog && s.rs[kRsPointSpriteEnable];
    sh.ffp = !sh.prog && FixedFunctionDraws(s, a, stages);
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    if (sh.prog) {
        if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
        sh.usages = ProgramUsages(s, a.regs);
    }

    std::vector<DeclElement> el;
    uint64_t decode;
    uint32_t conv_stride;
    BuildDeclaration(s, a, sh, el, decode, conv_stride);
    IDirect3DVertexDeclaration9* decl = EnsureDeclaration(el);
    if (!decl) {
        counts_.decl++;
        return;
    }

    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    if (sh.ffp) {
        dev_->SetVertexShader(nullptr);
        dev_->SetPixelShader(nullptr);
        SetFixedFunction(s, a, stages, size);
    } else {
        sh.vs = EnsureVertexShader(s, a, sh, decode);
        PixelKey pk = MakePixelKey(s, stages.info);
        // Direct3D 9 has no front face setting: culling clockwise leaves counter clockwise as the front (PixelHlsl)
        if (pk.two_sided && s.rs[kRsCullMode] == kCullCw) pk.two_sided = 2;
        sh.pkey = Hash(&pk, sizeof(pk));
        sh.ps = EnsurePixelShader(pk, sh.pkey);
        if (!sh.vs || !sh.ps) {
            counts_.shader++;
            return;
        }
        dev_->SetVertexShader(sh.vs);
        dev_->SetPixelShader(sh.ps);
        SetShaderConstants(s, sh, stages, size);
        SetTitleTransforms(s, a, size);
        for (DWORD i = 0; i < 4; i++) SetTexture(i, stages.res[i]);
    }
    dev_->SetVertexDeclaration(decl);
    SetSamplers(s, a, stages);
    SetOutputState(s, a, sh.ffp);
    SetViewportScissor(s, size.w);

    Geometry g;
    if (!DrawGeometry(a, el, conv_stride, g)) {
        LOG_FIRST(4, "d3d9: a draw of %u vertices did not fit the buffers", a.vertex_count);
        counts_.geometry++;
        return;
    }
    if (!g.prims) {
        counts_.geometry++;
        return;
    }
    mod_state_ = &s;
    mod_asm_ = &a;
    mod_size_ = size;
    bool drawn = !(sh.prog && diag::D3d9SkipProgram(s.vs->hash)) && ModsBefore(s, a, sh, stages, g);
    CountCamera(sh, drawn);
    if (drawn) {
        CopyFeedback(stages);
        Prime(&g);
        IDirect3DSurface9* probe = nullptr;
        if (camera_.probe && !camera_.probed && camera_.frame.marked) {
            camera_.probed = true;
            dev_->GetRenderTarget(0, &probe);
            if (probe) camera_.before_mark = PixelStats(dev_.Get(), probe);
        }
        IDirect3DQuery9* query = BeginCountedDraw(s);
        if (g.indexed) dev_->DrawIndexedPrimitive(g.type, 0, 0, g.vertices, g.first_index, g.prims);
        else dev_->DrawPrimitive(g.type, 0, g.prims);
        EndCountedDraw(query);
        if (probe) {
            camera_.after_mark = PixelStats(dev_.Get(), probe);
            probe->Release();
        }
        if (camera_.probe) TraceBrightness(sh);
        ModsAfter();
        counts_.made++;
        if (sh.ffp) counts_.ffp++;
    } else {
        counts_.mods++;
    }
    mod_state_ = nullptr;
    mod_asm_ = nullptr;
}

void D3D9Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                       uint32_t dst_points) {
    if (!Usable()) return;
    Target* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d9: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    NoteAfterMark("copy", src_t->data, dst_t->data);
    Scene(false);
    float ss = float(src_t->w) / src_t->gw;
    float ds = float(dst_t->w) / dst_t->gw;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x0 = 0, y0 = 0, x1 = int32_t(std::min(src_t->gw, dst_t->gw)), y1 = int32_t(std::min(src_t->gh, dst_t->gh));
        int32_t dx = 0, dy = 0;
        if (listed) {
            uint32_t r = src_rects + i * 16;
            x0 = int32_t(kernel::R32(r));
            y0 = int32_t(kernel::R32(r + 4));
            x1 = int32_t(kernel::R32(r + 8));
            y1 = int32_t(kernel::R32(r + 12));
            dx = x0;
            dy = y0;
            if (dst_points) {
                dx = int32_t(kernel::R32(dst_points + i * 8));
                dy = int32_t(kernel::R32(dst_points + i * 8 + 4));
            }
        }
        // clamped to both targets
        x1 = std::min(x1, int32_t(std::min<int64_t>(src_t->gw, x0 + int64_t(dst_t->gw) - dx)));
        y1 = std::min(y1, int32_t(std::min<int64_t>(src_t->gh, y0 + int64_t(dst_t->gh) - dy)));
        if (x0 < 0 || y0 < 0 || dx < 0 || dy < 0 || x1 <= x0 || y1 <= y0) continue;
        RECT sr{LONG(x0 * ss), LONG(y0 * ss), LONG(x1 * ss), LONG(y1 * ss)};
        RECT dr{LONG(dx * ds), LONG(dy * ds), LONG((dx + x1 - x0) * ds), LONG((dy + y1 - y0) * ds)};
        sr.right = std::min<LONG>(sr.right, src_t->w);
        sr.bottom = std::min<LONG>(sr.bottom, src_t->h);
        dr.right = std::min<LONG>(dr.right, dst_t->w);
        dr.bottom = std::min<LONG>(dr.bottom, dst_t->h);
        if (sr.right <= sr.left || sr.bottom <= sr.top || dr.right <= dr.left || dr.bottom <= dr.top) continue;
        bool same = sr.right - sr.left == dr.right - dr.left && sr.bottom - sr.top == dr.bottom - dr.top;
        // a mod may hand a surface to read instead, the first answer standing
        IDirect3DSurface9* from = src_t->surf.Get();
        if (!mods::D3d9().copy.empty()) {
            RecompModCopy c{sizeof(RecompModCopy), dev_.Get(), from, dst_t->surf.Get(), 1, sr.left, sr.top, sr.right, sr.bottom};
            for (const mods::CopyHook& h : mods::D3d9().copy)
                if (void* r = h.fn(&c, h.user)) {
                    from = static_cast<IDirect3DSurface9*>(r);
                    break;
                }
        }
        dev_->StretchRect(from, &sr, dst_t->surf.Get(), &dr, same ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    }
    dst_t->drawn = true;
}

// the frame letterboxed in the window, 16:9 when the console reports a widescreen display, else 4:3
RECT D3D9Backend::FrameRect() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float w = float(std::max<LONG>(rc.right - rc.left, 1));
    float h = float(std::max<LONG>(rc.bottom - rc.top, 1));
    float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    LONG x = LONG((w - vw) / 2);
    LONG y = LONG((h - vh) / 2);
    return {x, y, x + LONG(vw), y + LONG(vh)};
}

// ramp_ holds the red, green and blue ramps, 256 entries each
void D3D9Backend::UpdateGammaLut() {
    ramp_dirty_ = false;
    std::vector<uint8_t> px(256 * 4);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        px[i * 4] = ramp_[512 + i];
        px[i * 4 + 1] = ramp_[256 + i];
        px[i * 4 + 2] = ramp_[i];
        px[i * 4 + 3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    if (!gamma_) return;
    if (!gamma_ps_) {
        // drawn under fixed function vertices, which a Shader Model 3 pixel shader cannot follow
        if (ID3DBlob* code = Compile(kGammaHlsl, "ps_2_0"))
            dev_->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &gamma_ps_);
    }
    if (ComPtr<IDirect3DTexture9> t = PixelTexture(px, 256 * 4, 256, 1)) lut_ = t;
}

// The window's mouse in back buffer pixels: the UI draws into the back buffer, which Present scales into the window
void ScaleUiMouse(const RECT& frame, uint32_t bb_w, uint32_t bb_h) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    float sx = float(bb_w) / std::max<LONG>(frame.right - frame.left, 1);
    float sy = float(bb_h) / std::max<LONG>(frame.bottom - frame.top, 1);
    for (ImGuiInputEvent& e : g.InputEventsQueue) {
        if (e.Type != ImGuiInputEventType_MousePos || e.MousePos.PosX == -FLT_MAX) continue;
        e.MousePos.PosX = (e.MousePos.PosX - frame.left) * sx;
        e.MousePos.PosY = (e.MousePos.PosY - frame.top) * sy;
    }
}

void D3D9Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ScaleUiMouse(FrameRect(), bb_w_, bb_h_);
    ImGui::GetIO().DisplaySize = ImVec2(float(bb_w_), float(bb_h_));
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    Invalidate();
}

void D3D9Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

// a new render scale or vsync makes every target and the back buffer again
void D3D9Backend::ApplySettings() {
    if (texturedumper::Update()) pack_textures_.clear(), Invalidate();
    float want = app::RenderScale(guest_h_);
    bool rescale = std::fabs(want - scale_) > 1e-4f;
    if (!rescale && app::Get().vsync == vsync_) return;
    vsync_ = app::Get().vsync;
    scale_ = want;
    Scene(false);
    Bind(nullptr, nullptr);
    targets_.clear();  // a plain device's reset frees them either way
    dummies_.clear();
    prime_.Reset();
    bb_w_ = std::max(1u, uint32_t(std::lround(guest_w_ * scale_)));
    bb_h_ = std::max(1u, uint32_t(std::lround(guest_h_ * scale_)));
    D3DPRESENT_PARAMETERS pp;
    FillPresentParams(pp);
    if (dev_ex_) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        dev_ex_->ResetEx(&pp, nullptr);
        ImGui_ImplDX9_CreateDeviceObjects();
        Invalidate();
    } else {
        lost_ = true;  // a plain device's reset gives back everything it made, as after a loss
        ReleaseDeviceObjects();
        if (FAILED(dev_->Reset(&pp)) || !CreateDeviceObjects()) return;
        ImGui_ImplDX9_CreateDeviceObjects();
        lost_ = false;
    }
    bound_rt_surf_ = bound_ds_surf_ = nullptr;
    LOGI("d3d9: render scale %.3f, %ux%u back buffer, vsync %s", scale_, bb_w_, bb_h_, vsync_ ? "on" : "off");
}

void D3D9Backend::SnapBackBuffer(IDirect3DSurface9* bb) {
    D3DSURFACE_DESC d;
    ComPtr<IDirect3DSurface9> sys;
    D3DLOCKED_RECT lr;
    if (!bb || FAILED(bb->GetDesc(&d)) ||
        FAILED(dev_->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(dev_->GetRenderTargetData(bb, sys.Get())) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        testdrive::Snap(0, 0, nullptr, 0);
        return;
    }
    testdrive::Snap(d.Width, d.Height, static_cast<const uint8_t*>(lr.pBits), uint32_t(lr.Pitch));
    sys->UnlockRect();
}

// the mean colour and the share of pixels not black of a render target, for the early reports
std::string PixelStats(IDirect3DDevice9* dev, IDirect3DSurface9* rt) {
    D3DSURFACE_DESC d;
    ComPtr<IDirect3DSurface9> sys;
    D3DLOCKED_RECT lr;
    if (!rt || FAILED(rt->GetDesc(&d)) ||
        FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(dev->GetRenderTargetData(rt, sys.Get())) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        return "unreadable";
    }
    uint64_t sum[3] = {}, lit = 0;
    for (uint32_t y = 0; y < d.Height; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
        for (uint32_t x = 0; x < d.Width; x++) {
            const uint8_t* px = row + x * 4;
            for (int i = 0; i < 3; i++) sum[i] += px[i];
            lit += (px[0] | px[1] | px[2]) != 0;
        }
    }
    sys->UnlockRect();
    uint64_t n = uint64_t(d.Width) * d.Height;
    char text[96];
    std::snprintf(text, sizeof(text), "mean rgb %u %u %u, %.1f%% not black", unsigned(sum[2] / n), unsigned(sum[1] / n),
                  unsigned(sum[0] / n), 100.0 * double(lit) / double(n));
    return text;
}

void D3D9Backend::Present(const Surface& front) {
    if (!Usable()) return;
    if (g_capture.exchange(false)) LOG_FIRST(1, "d3d9: frame capture (F3) is D3D12's alone");
    // the surfaces the title presents are its screen: draws into any other are offscreen, for the mods
    // by virtual address, as the targets are kept
    uint32_t screen = mem::VirtualFromPhysical(front.data);
    if (std::find(std::begin(screens_), std::end(screens_), screen) == std::end(screens_)) {
        std::copy_backward(std::begin(screens_), std::end(screens_) - 1, std::end(screens_));
        screens_[0] = screen;
    }
    for (const mods::DeviceHook& h : mods::D3d9().present) h.fn(dev_.Get(), h.user);
    prime_due_ = true;
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UpdateGammaLut();
    if (camera_.probe && t) camera_.at_present = PixelStats(dev_.Get(), t->surf.Get());
    ComPtr<IDirect3DSurface9> bb;
    dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    Bind(bb.Get(), nullptr);
    dev_->ColorFill(bb.Get(), nullptr, D3DCOLOR_XRGB(0, 0, 0));
    if (t && gamma_ && gamma_ps_) {
        Scene(true);
        D3DVIEWPORT9 vp{0, 0, bb_w_, bb_h_, 0, 1};
        dev_->SetViewport(&vp);
        Rs(D3DRS_ALPHABLENDENABLE, FALSE);
        Rs(D3DRS_COLORWRITEENABLE, 15);
        SetTexture(1, lut_.Get());
        DrawQuad(t->tex.Get(), RECT{0, 0, LONG(bb_w_), LONG(bb_h_)}, 0xFFFFFFFF, gamma_ps_.Get());
    } else if (t) {
        Scene(false);
        dev_->StretchRect(t->surf.Get(), nullptr, bb.Get(), nullptr, D3DTEXF_LINEAR);
    }
    Scene(true);
    D3DVIEWPORT9 vp{0, 0, bb_w_, bb_h_, 0, 1};
    dev_->SetViewport(&vp);
    RenderUi();
    Scene(false);
    if (testdrive::SnapDue()) SnapBackBuffer(bb.Get());
    bool report = presents_ + 1 == 60 || presents_ + 1 == 1800;
    if (report)
        LOGR("d3d9: presented target %s; back buffer %s%s", t ? PixelStats(dev_.Get(), t->surf.Get()).c_str() : "none",
             PixelStats(dev_.Get(), bb.Get()).c_str(), gamma_ ? "; through the gamma ramp" : "");
    // a smaller picture than before leaves its old edges in the window: the window thread paints them black
    RECT frame = FrameRect();
    if (!EqualRect(&frame, &last_frame_rect_)) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        last_frame_rect_ = frame;
    }
    HRESULT hr = dev_ex_ ? dev_ex_->PresentEx(nullptr, &frame, nullptr, nullptr, 0)
                         : dev_->Present(nullptr, &frame, nullptr, nullptr);
    if (hr == D3DERR_DEVICELOST) {
        LOGW("d3d9: device lost");
        lost_ = true;
    }
    Bind(nullptr, nullptr);
    bound_rt_surf_ = nullptr;
    presents_++;
    testdrive::Presented();
    if (report) {
        const DrawCounts& c = counts_;
        LOGR("d3d9: %llu frames, %u draws made (%u fixed function); refused: %u assembly, %u targets, %u declarations, "
             "%u shaders, %u geometry, %u by mods; front %08X %ux%u%s, Present %08X",
             (unsigned long long)presents_, c.made, c.ffp, c.assemble, c.targets, c.decl, c.shader, c.geometry, c.mods,
             front.data, t ? t->gw : 0, t ? t->gh : 0, t ? (t->drawn ? ", drawn" : ", never drawn") : ", no target",
             unsigned(hr));
    }
    if (!mods::D3d9().draw.empty()) {
        EndCameraFrame();
        if (presents_ % 600 == 0) ReportCamera();
        camera_.probe = presents_ % 600 == 599;
        camera_.probed = false;
    }
    frame_++;
    DeliverReports(false);
    ReleaseOldTextures();
    CountFps();
    ApplySettings();
    if (!lost_) Scene(true);
}

// the CPU reads a surface the GPU drew: bring the pixels back, shrunk to the console's size
void D3D9Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data) || !Usable()) return;
    Target& t = it->second;
    Scene(false);
    ComPtr<IDirect3DSurface9> down;
    IDirect3DSurface9* src = t.surf.Get();
    if (t.w != t.gw || t.h != t.gh) {
        if (FAILED(dev_->CreateRenderTarget(t.gw, t.gh, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &down,
                                            nullptr))) {
            return;
        }
        dev_->StretchRect(src, nullptr, down.Get(), nullptr, D3DTEXF_LINEAR);
        src = down.Get();
    }
    ComPtr<IDirect3DSurface9> sys;
    if (FAILED(dev_->CreateOffscreenPlainSurface(t.gw, t.gh, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(dev_->GetRenderTargetData(src, sys.Get()))) {
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    // colour targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    std::vector<uint8_t> px(size_t(t.gw) * t.gh * 4);
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
        std::memcpy(&px[size_t(y) * t.gw * 4], row, t.gw * 4);
        std::memcpy(guest + size_t(y) * pitch, row, t.gw * 4);
    }
    sys->UnlockRect();
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void D3D9Backend::Flush() {
    if (!dev_ || lost_) return;
    // a query issued at the end waits out everything before it
    ComPtr<IDirect3DQuery9> q;
    if (SUCCEEDED(dev_->CreateQuery(D3DQUERYTYPE_EVENT, &q))) {
        q->Issue(D3DISSUE_END);
        while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
    }
    DeliverReports(true);
}

void D3D9Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_.data(), ramp, ramp_.size());
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateD3D9Backend() { return std::make_unique<D3D9Backend>(); }

}  // namespace gpu
