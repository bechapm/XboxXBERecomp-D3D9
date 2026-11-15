#include "gpu/picture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <dxgiformat.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "xdk/d3d8.h"

namespace gpu {

namespace {

std::atomic<bool> g_overflow{false};

using Vec = std::array<float, 4>;

// an input register of vertex v as the vertex shader reads it, from the buffers the draw was assembled into
bool ReadInput(const Assembled& a, int reg, uint32_t v, Vec& o) {
    o = {0, 0, 0, 1};
    const VertexElement& e = a.elements[size_t(reg)];
    const VertexBuffer& b = a.buffers[e.buffer];
    if (!b.src || !b.stride) return false;
    size_t at = size_t(v) * b.stride + e.offset;
    if (at + 4 > b.bytes) return false;
    const uint8_t* p = b.src + at;
    uint32_t dec = uint32_t(a.decode >> (reg * 4)) & 15;
    auto floats = [&](int n) {
        for (int i = 0; i < n; i++) std::memcpy(&o[size_t(i)], p + i * 4, 4);
    };
    auto snorm = [&](int n) {
        for (int i = 0; i < n; i++) {
            int16_t x;
            std::memcpy(&x, p + i * 2, 2);
            o[size_t(i)] = dec == kDecodeInt || dec == kDecodeIntW1 ? float(x) : std::max(x / 32767.0f, -1.0f);
        }
    };
    switch (e.format) {
        case DXGI_FORMAT_R32_FLOAT: floats(1); break;
        case DXGI_FORMAT_R32G32_FLOAT: floats(2); break;
        case DXGI_FORMAT_R32G32B32_FLOAT:
            floats(3);
            if (dec == kDecodeFloat2H) o = {o[0], o[1], 0, o[2]};
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: floats(4); break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: o = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f}; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: o = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f}; break;
        case DXGI_FORMAT_R8G8_UNORM: o = {p[0] / 255.0f, p[1] / 255.0f, 0, 1}; break;
        case DXGI_FORMAT_R8_UNORM: o = {p[0] / 255.0f, 0, 0, 1}; break;
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT: snorm(1); break;
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT: snorm(2); break;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: snorm(4); break;
        case DXGI_FORMAT_R32_UINT: {  // NORMPACKED3
            uint32_t u;
            std::memcpy(&u, p, 4);
            o = {std::max(float(int32_t(u << 21) >> 21) / 1023, -1.0f), std::max(float(int32_t(u << 10) >> 21) / 1023, -1.0f),
                 std::max(float(int32_t(u) >> 22) / 511, -1.0f), 1};
            break;
        }
        default: return false;
    }
    if (dec == kDecodeW1 || dec == kDecodeIntW1) o[3] = 1;
    return true;
}

uint32_t VertexCount(const Assembled& a, int reg) {
    const VertexBuffer& b = a.buffers[a.elements[size_t(reg)].buffer];
    if (!b.stride) return 0;
    return std::min<uint32_t>(a.vertex_count, b.bytes / b.stride);
}

// A vertex program's oPos, run on the CPU for a 2D draw's extent: the microcode as shaders.cpp reads it
struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

float MacMul(float a, float b) { return (a == 0 || b == 0) ? 0 : a * b; }
Vec Splat(float v) { return {v, v, v, v}; }

// 1 mov, 2 mul, 3 add, 4 mad, 5 dp3, 6 dph, 7 dp4, 8 dst, 9 min, 10 max, 11 slt, 12 sge, 13 arl
Vec Mac(uint32_t op, const Vec& a, const Vec& b, const Vec& c) {
    Vec r{};
    switch (op) {
        case 1:
        case 13: return a;
        case 2: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]); return r;
        case 3: for (int i = 0; i < 4; i++) r[i] = a[i] + c[i]; return r;
        case 4: for (int i = 0; i < 4; i++) r[i] = MacMul(a[i], b[i]) + c[i]; return r;
        case 5: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
        case 6: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]);
        case 7: return Splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        case 8: return {1, a[1] * b[1], a[2], b[3]};
        case 9: for (int i = 0; i < 4; i++) r[i] = std::min(a[i], b[i]); return r;
        case 10: for (int i = 0; i < 4; i++) r[i] = std::max(a[i], b[i]); return r;
        case 11: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.f : 0.f; return r;
        case 12: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.f : 0.f; return r;
        default: return r;
    }
}

// 1 mov, 2 rcp, 3 rcc, 4 rsq, 5 exp, 6 log, 7 lit
Vec Ilu(uint32_t op, const Vec& c) {
    float v = c[0];
    switch (op) {
        case 1: return c;
        case 2: return Splat(1.0f / v);
        case 3: {
            float m = std::clamp(std::fabs(1.0f / v), 5.42101e-20f, 1.84467e+19f);
            return Splat(v < 0 ? -m : m);
        }
        case 4: return Splat(v == 0 ? INFINITY : 1.0f / std::sqrt(std::fabs(v)));
        case 5: {
            float whole = std::floor(v);
            return {std::exp2(whole), v - whole, std::exp2(v), 1};
        }
        case 6: {
            float mag = std::fabs(v);
            if (mag == 0) return {-INFINITY, 1, -INFINITY, 1};
            float ex = std::floor(std::log2(mag));
            return {ex, mag / std::exp2(ex), std::log2(mag), 1};
        }
        case 7: {
            const float cap = 127.99609375f;
            float power = std::clamp(c[3], -cap, cap), diffuse = std::max(c[0], 0.f), spec = std::max(c[1], 0.f);
            return {1, diffuse, diffuse > 0 ? std::exp2(power * std::log2(spec)) : 0, 1};
        }
        default: return Vec{};
    }
}

struct Program {
    const State& s;
    Vec in[16]{};
    Vec temp[12]{}, out[16]{};  // out 0 oPos
    int a0 = 0;

    Vec& Reg(uint32_t r) { return r == 12 ? out[0] : temp[r]; }

    Vec Operand(const Tok& k, int which) {
        uint32_t neg, sw[4], reg, mux;
        if (which == 0) {
            neg = k.f(1, 8, 1);
            sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
            reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
        } else if (which == 1) {
            neg = k.f(2, 25, 1);
            sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
            reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
        } else {
            neg = k.f(2, 10, 1);
            sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
            reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
            uint32_t ilu = k.f(1, 25, 3);
            if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
        }
        Vec base;
        if (mux == 1) {
            base = reg == 12 ? out[0] : temp[reg > 12 ? 0 : reg];
        } else if (mux == 2) {
            base = in[k.f(1, 9, 4)];
        } else {
            int cr = int(k.f(1, 13, 8));
            if (k.f(3, 1, 1)) cr = std::clamp(cr + a0, 0, 191);
            const auto& c = s.vsc[size_t(cr)];
            base = {c[0], c[1], c[2], c[3]};
        }
        Vec r;
        for (int i = 0; i < 4; i++) r[i] = neg ? -base[sw[i]] : base[sw[i]];
        return r;
    }

    static void Masked(Vec& dst, const Vec& src, uint32_t xyzw) {
        for (int i = 0; i < 4; i++)
            if (xyzw >> (3 - i) & 1) dst[i] = src[i];
    }

    void Run(const std::vector<uint32_t>& prog) {
        out[0] = {0, 0, 0, 1};
        for (Vec& t : temp) t = {};
        a0 = 0;
        for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
            Tok k{&prog[n]};
            uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
            if (mac || ilu) {
                Vec a = Operand(k, 0), b = Operand(k, 1), c = Operand(k, 2);
                Vec mr = mac ? Mac(mac, a, b, c) : Vec{}, ir = ilu ? Ilu(ilu, c) : Vec{};
                uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
                bool use_ilu = k.f(3, 2, 1) == 1;
                if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13)))
                    Masked(out[k.f(3, 3, 8) & 0xF], use_ilu ? ir : mr, omask);
                if (ilu) {
                    uint32_t dst = mac ? 1 : r;
                    if (dst <= 12) Masked(Reg(dst), ir, k.f(3, 16, 4));
                }
                if (mac == 13) {
                    a0 = int(std::floor(mr[0] + 0.001f));
                } else if (mac) {
                    uint32_t m = ilu && r == 1 ? 0 : k.f(3, 24, 4);
                    if (r <= 12) Masked(Reg(r), mr, m);
                }
            }
            if (k.f(3, 0, 1)) break;
        }
    }
};

bool Orthographic(const float* p) { return p[3] == 0 && p[7] == 0 && p[11] == 0 && p[15] == 1; }

void Mul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += x[r * 4 + k] * y[k * 4 + c];
            o[r * 4 + c] = v;
        }
}

// TRANSFORM indices of D3DTS_VIEW, D3DTS_PROJECTION and D3DTS_WORLD
constexpr int kTsView = 2, kTsProjection = 3, kTsWorld = 256;

// at most this many vertices are measured: 2D draws are small, and this runs only for them
constexpr uint32_t kMeasureMax = 8192;

// the leftmost and rightmost x a 2D draw reaches in the target's console pixels, false when they cannot be found
bool Extent(const State& s, const Assembled& a, bool program, uint32_t gw, float& lo, float& hi) {
    lo = 3.4e38f, hi = -3.4e38f;
    if (program) {
        Program p{s};
        uint32_t n = 0;
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) n = std::max(n, VertexCount(a, r));
        n = std::min(n, kMeasureMax);
        for (uint32_t v = 0; v < n; v++) {
            for (int r = 0; r < 16; r++)
                if ((a.regs >> r) & 1) ReadInput(a, r, v, p.in[r]);
            p.Run(s.vs->program);
            // oPos is in screen pixels, its w what the hardware divides the rest by
            lo = std::min(lo, p.out[0][0]);
            hi = std::max(hi, p.out[0][0]);
        }
        return n > 0;
    }
    if (!(a.regs & 1)) return false;
    uint32_t n = std::min(VertexCount(a, 0), kMeasureMax);
    float wvp[16], wv[16];
    if (!a.pretransformed) {
        Mul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        Mul(wv, s.transforms[kTsProjection].data(), wvp);
    }
    for (uint32_t v = 0; v < n; v++) {
        Vec p;
        if (!ReadInput(a, 0, v, p)) return false;
        float x = p[0];
        if (!a.pretransformed) {
            float cx = p[0] * wvp[0] + p[1] * wvp[4] + p[2] * wvp[8] + wvp[12];
            float cw = p[0] * wvp[3] + p[1] * wvp[7] + p[2] * wvp[11] + wvp[15];
            x = cw != 0 ? (cx / cw + 1) * 0.5f * float(gw) : 0;
        }
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    return n > 0;
}

// where a draw lies on its target, in console pixels: the leftmost, topmost, rightmost and lowest its vertices reach;
// with depth and w, the range of its depth (0 near to 1 far, as the depth buffer takes it) and of its clip w. samples
// limits the vertices measured, spread through the draw (0 for all, up to kMeasureMax); with front, vertices behind
// the eye (w at or below 0) are left out. false when it cannot be found (ScreenBox, DrawWhere)
bool BoxOf(const State& s, const Assembled& a, bool program, uint32_t gw, uint32_t gh, float box[4],
           float* depth = nullptr, float* w = nullptr, uint32_t samples = 0, bool front = false) {
    float zr[2] = {3.4e38f, -3.4e38f}, wr[2] = {3.4e38f, -3.4e38f};
    box[0] = box[1] = 3.4e38f;
    box[2] = box[3] = -3.4e38f;
    bool any = false;
    auto take = [&](float x, float y, float z, float cw) {
        if (front && cw <= 0) return;
        any = true;
        box[0] = std::min(box[0], x);
        box[1] = std::min(box[1], y);
        box[2] = std::max(box[2], x);
        box[3] = std::max(box[3], y);
        zr[0] = std::min(zr[0], z);
        zr[1] = std::max(zr[1], z);
        wr[0] = std::min(wr[0], cw);
        wr[1] = std::max(wr[1], cw);
    };
    auto step = [&](uint32_t n) { return samples && n > samples ? (n + samples - 1) / samples : 1u; };
    auto done = [&] {
        if (depth) depth[0] = zr[0], depth[1] = zr[1];
        if (w) w[0] = wr[0], w[1] = wr[1];
        return any;
    };
    if (program) {
        Program p{s};
        uint32_t n = 0;
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) n = std::max(n, VertexCount(a, r));
        n = std::min(n, kMeasureMax);
        for (uint32_t v = 0, d = step(n); v < n; v += d) {
            for (int r = 0; r < 16; r++)
                if ((a.regs >> r) & 1) ReadInput(a, r, v, p.in[r]);
            p.Run(s.vs->program);
            // oPos is in screen pixels and depth buffer units, its w what the hardware divides the rest by
            take(p.out[0][0], p.out[0][1], p.out[0][2] / s.zscale, p.out[0][3]);
        }
        return done();
    }
    if (!(a.regs & 1)) return false;
    uint32_t n = std::min(VertexCount(a, 0), kMeasureMax);
    float wvp[16], wv[16];
    if (!a.pretransformed) {
        Mul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        Mul(wv, s.transforms[kTsProjection].data(), wvp);
    }
    for (uint32_t v = 0, d = step(n); v < n; v += d) {
        Vec p;
        if (!ReadInput(a, 0, v, p)) return false;
        if (a.pretransformed) {
            // x, y in pixels, z in depth buffer units, and 1/w
            take(p[0], p[1], p[2] / s.zscale, p[3] != 0 ? 1 / p[3] : 1);
            continue;
        }
        float cx = p[0] * wvp[0] + p[1] * wvp[4] + p[2] * wvp[8] + wvp[12];
        float cy = p[0] * wvp[1] + p[1] * wvp[5] + p[2] * wvp[9] + wvp[13];
        float cz = p[0] * wvp[2] + p[1] * wvp[6] + p[2] * wvp[10] + wvp[14];
        float cw = p[0] * wvp[3] + p[1] * wvp[7] + p[2] * wvp[11] + wvp[15];
        if (cw == 0) {
            take(0, 0, 0, cw);  // as ScreenBox has always taken it
            continue;
        }
        take((cx / cw + 1) * 0.5f * float(gw), (1 - cy / cw) * 0.5f * float(gh), cz / cw, cw);
    }
    return done();
}

// the viewport and scissor's horizontal span in console pixels
void Visible(const State& s, float& x0, float& x1) {
    x0 = float(s.viewport.x);
    x1 = float(s.viewport.x + s.viewport.w);
    if (s.scissor_count) {
        x0 = std::max(x0, float(s.scissors[0]));
        x1 = std::min(x1, float(s.scissors[2]));
    }
}

// the draw goes onto the screen in screen coordinates the game worked out, not through its camera
bool TwoD(const State& s, const Assembled& a, bool program) {
    if (program) {
        int preg = g_projection_register;
        // a game that names no projection register says nothing of which of its programs are 2D
        if (preg < 0 || preg + 4 > int(s.vsc.size())) return false;
        if (!ReadsConstant(s.vs->program, uint32_t(preg))) return true;
        float m[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) m[r * 4 + c] = s.vsc[size_t(preg + c)][size_t(r)];
        return Orthographic(m);
    }
    return a.pretransformed || Orthographic(s.transforms[kTsProjection].data());
}

float Anchor(uint32_t base) { return base == kPlaceLeft ? -1.0f : base == kPlaceRight ? 1.0f : 0.0f; }

// XBE_2D_TAGS=1: every 300 frames, each call site the game tagged, with its 2D draws and the extent they reached
// (clipped by the scissor, in console pixels). How a game builds its table of anchors: it tags draws with the code that
// made them, and this says what each tag drew
struct TagStat {
    uint32_t placement = 0, draws = 0, spanned = 0;
    float lo = 3.4e38f, hi = -3.4e38f;
};
std::mutex g_tag_lock;
std::map<uint32_t, TagStat> g_tags;
bool TagLog() {
    static const bool on = [] {
        const char* v = std::getenv("XBE_2D_TAGS");
        return v && *v && *v != '0';
    }();
    return on;
}

void NoteTag(const State& s, bool measured, float lo, float hi, bool spans) {
    std::lock_guard<std::mutex> g(g_tag_lock);
    TagStat& t = g_tags[s.place_tag];
    t.placement = s.place;
    t.draws++;
    if (spans) t.spanned++;
    if (measured) {
        t.lo = std::min(t.lo, lo);
        t.hi = std::max(t.hi, hi);
    }
}

}  // namespace

void SetTwoDPlacement(uint32_t placement, uint32_t tag) {
    if ((placement & kPlaceMask) > kPlaceLast) placement &= ~uint32_t(kPlaceMask);
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    State& s = d3d::State();
    s.place = placement;
    s.place_tag = tag;
}

void SetTwoDOverflow(bool on) { g_overflow = on; }

float PictureWide(uint32_t gw, uint32_t gh) {
    if (!app::kGame.hor_plus || !gw || !gh) return 1;
    return std::max(app::PictureAspect() * float(gh) / float(gw), 1.0f);
}

void Placed::Scissor(float x0, float x1, uint32_t gw, uint32_t hw, long& out0, long& out1) const {
    // within 2 pixels of the screen's width is the screen and not a box: TimeSplitters 2's own scissor is 0 to 638
    if (free && x0 <= 2 && x1 >= float(gw) - 2) {
        out0 = 0;
        out1 = long(hw);
        return;
    }
    auto map = [&](float x) {
        float c = 2 * x / float(gw) - 1;
        return (c * k + offset + 1) * 0.5f * float(hw);
    };
    out0 = long(map(x0));
    out1 = long(map(x1));
}

Placed Place(const State& s, const Assembled& a, bool program, uint32_t gw, uint32_t gh, float wide) {
    Placed out;
    if (!(wide > 1.001f) || !gw || !gh) return out;
    program = program && s.vs && !s.vs->program.empty();
    // what spans the picture: a scissor of the whole screen is the picture's
    out.free = true;
    uint32_t base = s.place & kPlaceMask;
    float k = 1 / wide;
    auto squeeze = [&](float anchor) {
        out.k = k;
        out.offset = anchor * (1 - k);
    };
    if (!TwoD(s, a, program)) {
        // a camera narrower than the picture, which the world's never is: a model the game's 2D layout holds. A tenth
        // short of it, so a camera a hair off the picture's shape is still the world, and a 4:3 camera is still
        // narrower at 16:10
        int preg = g_projection_register;
        if ((s.place & kPlacePanel3d) && base != kPlaceStretch && program && preg >= 0 && preg + 2 <= int(s.vsc.size())) {
            float x = s.vsc[size_t(preg)][0], y = s.vsc[size_t(preg + 1)][1];
            float aspect = x != 0 ? std::fabs(y / x) : 0;
            float picture = wide * float(gw) / float(gh);
            if (aspect > 0 && aspect < picture * 0.9f) {
                squeeze((s.place & kPlacePanelAnchored) ? Anchor(base) : 0);
                // through a viewport of the whole screen it may reach past 4:3, as a menu's backdrop does: its own
                // viewport, a panel's, holds it
                out.free = !(s.place & kPlacePanelAnchored);
            }
        }
        return out;
    }
    float lo = 0, hi = 0;
    bool measured = Extent(s, a, program, gw, lo, hi);
    float raw_lo = lo, raw_hi = hi;
    // what the draw can reach, not what it is: a quad the size of the screen under a scissor the size of a box is that
    // box's background
    float v0, v1;
    Visible(s, v0, v1);
    lo = std::max(lo, v0);
    hi = std::min(hi, v1);
    float w = float(gw);
    bool spans = measured && hi - lo >= w * 0.98f;
    out.free = g_overflow;
    switch (base) {
        case kPlaceStretch: break;
        case kPlaceCentre:
        case kPlaceLeft:
        case kPlaceRight: squeeze(Anchor(base)); break;
        case kPlaceSide: {
            float mid = (lo + hi) * 0.5f;
            squeeze(!measured ? 0 : mid < w / 3 ? -1 : mid > w * 2 / 3 ? 1 : 0);
            break;
        }
        case kPlaceEdge:
            // touching counts as reaching: a quad past the edge measures as touching it under a scissor of the screen
            if (!(spans || (measured && (lo <= 0.5f || hi >= w - 0.5f)))) squeeze(0);
            break;
        case kPlaceSpread:
            if (!spans) squeeze(measured ? (lo + hi) / w - 1 : 0);
            break;
        case kPlaceFill:
            if (!spans) {
                squeeze(0);
                // a piece flush with the screen's edge was made to reach it; TimeSplitters 2's sniper mask stops at 639
                out.bleed = g_overflow && measured &&
                            (std::fabs(raw_lo) <= 0.75f || (raw_hi >= w - 1.75f && raw_hi <= w + 0.75f));
            }
            break;
        default:
            if (!spans) squeeze(0);
            break;
    }
    if (!out.On()) out.free = true;
    if (TagLog()) NoteTag(s, measured, lo, hi, !out.On());
    return out;
}

void PlacementFrame() {
    if (!TagLog()) return;
    static uint32_t frames = 0;
    if (++frames % 300) return;
    std::lock_guard<std::mutex> g(g_tag_lock);
    for (const auto& [tag, t] : g_tags)
        LOGR("2d tags: %08X placement %X: %u draws, %u spanned, x %.1f to %.1f", tag, t.placement, t.draws, t.spanned,
             t.lo, t.hi);
    g_tags.clear();
}

bool ScreenBox(const State& s, const Assembled& a, bool program, uint32_t gw, uint32_t gh, float box[4]) {
    return BoxOf(s, a, program, gw, gh, box);
}

bool DrawWhere(const State& s, const Assembled& a, bool program, uint32_t gw, uint32_t gh, uint32_t samples,
               float box[4], float depth[2], float w[2]) {
    return BoxOf(s, a, program, gw, gh, box, depth, w, samples, true);
}

bool PickDraw(const State& s, const Assembled& a, bool program, const PlainProgram* plain, uint32_t gw, uint32_t gh,
              PickPoint* pts, size_t n, uint32_t& budget) {
    if (a.topology != Topology::Triangles || !n) return true;
    // each vertex where the screen has it: x, y in console pixels, depth, w; w at or below 0 is behind the eye
    uint32_t count = 0;
    if (program && !plain) {
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) count = std::max(count, VertexCount(a, r));
    } else {
        int pos = plain ? plain->position : 0;
        if (pos >= 0 && ((a.regs >> pos) & 1)) count = VertexCount(a, pos);
    }
    count = std::min(count, kMeasureMax);
    if (!count) return false;
    // a program run on the CPU costs; a plain transform is two matrices, which are not counted
    if (program && !plain) {
        if (count > budget) {
            budget = 0;
            return false;
        }
        budget -= count;
    }
    std::vector<std::array<float, 4>> sp(count);
    if (plain) {
        // as the program does it: model-view rows, projection rows, then the screen lines s * clip / w + o
        const auto& sc = s.vsc[size_t(plain->screen_scale)];
        const auto& of = s.vsc[size_t(plain->screen_offset)];
        auto row = [&](const Vec& v, int reg) {
            const auto& c = s.vsc[size_t(reg)];
            return v[0] * c[0] + v[1] * c[1] + v[2] * c[2] + v[3] * c[3];
        };
        for (uint32_t v = 0; v < count; v++) {
            Vec q, m;
            if (!ReadInput(a, plain->position, v, q)) return false;
            if (plain->modelview >= 0) {
                for (int k = 0; k < 4; k++) m[size_t(k)] = row(q, plain->modelview + k);
            } else {
                m = q;
            }
            Vec c;
            for (int k = 0; k < 4; k++) c[size_t(k)] = row(m, plain->projection + k);
            sp[v] = c[3] > 0 ? std::array<float, 4>{c[0] * sc[0] / c[3] + of[0], c[1] * sc[1] / c[3] + of[1],
                                                    (c[2] * sc[2] / c[3] + of[2]) / s.zscale, c[3]}
                             : std::array<float, 4>{0, 0, 0, 0};
        }
    } else if (program) {
        Program p{s};
        for (uint32_t v = 0; v < count; v++) {
            for (int r = 0; r < 16; r++)
                if ((a.regs >> r) & 1) ReadInput(a, r, v, p.in[r]);
            p.Run(s.vs->program);
            sp[v] = {p.out[0][0], p.out[0][1], p.out[0][2] / s.zscale, p.out[0][3]};
        }
    } else {
        float wvp[16], wv[16];
        if (!a.pretransformed) {
            Mul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
            Mul(wv, s.transforms[kTsProjection].data(), wvp);
        }
        for (uint32_t v = 0; v < count; v++) {
            Vec q;
            if (!ReadInput(a, 0, v, q)) return false;
            if (a.pretransformed) {
                sp[v] = {q[0], q[1], q[2] / s.zscale, q[3] != 0 ? 1 / q[3] : 1};
                continue;
            }
            float cx = q[0] * wvp[0] + q[1] * wvp[4] + q[2] * wvp[8] + wvp[12];
            float cy = q[0] * wvp[1] + q[1] * wvp[5] + q[2] * wvp[9] + wvp[13];
            float cz = q[0] * wvp[2] + q[1] * wvp[6] + q[2] * wvp[10] + wvp[14];
            float cw = q[0] * wvp[3] + q[1] * wvp[7] + q[2] * wvp[11] + wvp[15];
            sp[v] = cw > 0 ? std::array<float, 4>{(cx / cw + 1) * 0.5f * float(gw), (1 - cy / cw) * 0.5f * float(gh), cz / cw, cw}
                           : std::array<float, 4>{0, 0, 0, 0};
        }
    }
    // the triangles, as DrawGeometry gives them to the device: the title's 16 bit indices moved by base_vertex, the
    // rebuilt 32 bit list as it is, or the vertices in order
    uint32_t total = a.index_count ? a.index_count : a.draw_count;
    auto index = [&](uint32_t i) -> uint32_t {
        if (!a.index_count) return i;
        if (a.index16) return uint32_t(int32_t(static_cast<const uint16_t*>(a.index_src)[i]) + a.base_vertex);
        return static_cast<const uint32_t*>(a.index_src)[i];
    };
    uint32_t tris = a.strip ? (total >= 3 ? total - 2 : 0) : total / 3;
    for (uint32_t t = 0; t < tris; t++) {
        uint32_t i0 = a.strip ? t : t * 3;
        uint32_t v0 = index(i0), v1 = index(i0 + 1), v2 = index(i0 + 2);
        if (v0 >= count || v1 >= count || v2 >= count || v0 == v1 || v1 == v2 || v0 == v2) continue;
        const auto &A = sp[v0], &B = sp[v1], &C = sp[v2];
        if (A[3] <= 0 || B[3] <= 0 || C[3] <= 0) continue;
        float area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
        if (std::fabs(area) < 1e-6f) continue;
        float lo_x = std::min({A[0], B[0], C[0]}), hi_x = std::max({A[0], B[0], C[0]});
        float lo_y = std::min({A[1], B[1], C[1]}), hi_y = std::max({A[1], B[1], C[1]});
        for (size_t k = 0; k < n; k++) {
            PickPoint& pt = pts[k];
            if (pt.x < lo_x || pt.x > hi_x || pt.y < lo_y || pt.y > hi_y) continue;
            // barycentric in screen space; depth goes linearly there, w by its reciprocal
            float b1 = ((pt.x - A[0]) * (C[1] - A[1]) - (pt.y - A[1]) * (C[0] - A[0])) / area;
            float b2 = ((B[0] - A[0]) * (pt.y - A[1]) - (B[1] - A[1]) * (pt.x - A[0])) / area;
            float b0 = 1 - b1 - b2;
            if (b0 < 0 || b1 < 0 || b2 < 0) continue;
            float z = b0 * A[2] + b1 * B[2] + b2 * C[2];
            if (z < 0 || z > 1 || (pt.hit && z >= pt.depth)) continue;
            pt.hit = true;
            pt.depth = z;
            pt.w = 1 / (b0 / A[3] + b1 / B[3] + b2 / C[3]);
        }
    }
    return true;
}

void PlaceMatrix(const Placed& p, const float* m, float* out) {
    // row vectors: x' = x * k + w * offset, the placement's first column
    for (int r = 0; r < 4; r++) {
        out[r * 4 + 0] = m[r * 4 + 0] * p.k + m[r * 4 + 3] * p.offset;
        out[r * 4 + 1] = m[r * 4 + 1];
        out[r * 4 + 2] = m[r * 4 + 2];
        out[r * 4 + 3] = m[r * 4 + 3];
    }
}

}  // namespace gpu
