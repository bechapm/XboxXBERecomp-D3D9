#include "mods/mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/shaders.h"
#include "xdk/d3d8.h"
#include "kernel/kernel.h"

namespace mods {

namespace {

constexpr size_t kMaxMods = 16;
constexpr size_t kMaxFrame = 16;  // on_frame across mods
constexpr size_t kMaxHooks = 8;   // each of the others
constexpr size_t kMaxVertexHlsl = 16384;
constexpr uint32_t kXbeBase = 0x00010000;  // the image header, where the console has it
constexpr uint32_t kXbeCert = 0x118;       // header: the certificate's address
constexpr uint32_t kCertTitleId = 8;
constexpr uint32_t kCertTitleName = 0xC;   // 40 UTF-16 characters

struct FrameHook {
    void (*fn)(void*);
    void* user;
};
std::vector<FrameHook> g_frame;
D3d9Hooks g_d3d9;
Renderer* g_renderer = nullptr;
bool g_shaders_made = false;  // after init no more vertex HLSL is taken

template <class T>
int Add(std::vector<T>& v, size_t max, const T& h) {
    if (v.size() >= max) return 0;
    v.push_back(h);
    return 1;
}

// the api

uint32_t TitleId() {
    uint32_t cert = kernel::R32(kXbeBase + kXbeCert);
    return cert ? kernel::R32(cert + kCertTitleId) : 0;
}

const char* TitleName() {
    static std::string name;
    static bool read = false;
    if (!read) {
        read = true;
        uint32_t cert = kernel::R32(kXbeBase + kXbeCert);
        for (uint32_t i = 0; cert && i < 40; i++) {
            uint32_t c = kernel::R32(cert + kCertTitleName + i * 2) & 0xFFFF;
            if (!c) break;
            // UTF-8
            if (c < 0x80) {
                name += char(c);
            } else if (c < 0x800) {
                name += char(0xC0 | c >> 6);
                name += char(0x80 | (c & 0x3F));
            } else {
                name += char(0xE0 | c >> 12);
                name += char(0x80 | (c >> 6 & 0x3F));
                name += char(0x80 | (c & 0x3F));
            }
        }
    }
    return name.empty() ? nullptr : name.c_str();
}

void* GuestPointer(uint32_t va) { return kernel::HostPtr(va); }

int OnFrame(void (*fn)(void*), void* user) { return fn ? Add(g_frame, kMaxFrame, FrameHook{fn, user}) : 0; }

uint32_t VertexConstants(uint32_t first, uint32_t count, float (*out)[4]) {
    return g_renderer && out ? g_renderer->VertexConstants(first, count, out) : 0;
}

void Log(const char* line) { LOGR("[MOD] %s", line ? line : ""); }

int D3d9OnDevice(RecompModDeviceFn fn, void* user) {
    return fn ? Add(g_d3d9.device, kMaxHooks, DeviceHook{fn, user}) : 0;
}

int D3d9OnDraw(RecompModBeforeDraw before, RecompModAfterDraw after, void* user) {
    return before || after ? Add(g_d3d9.draw, kMaxHooks, DrawHook{before, after, user}) : 0;
}

int D3d9OnClear(RecompModClearFn fn, void* user) { return fn ? Add(g_d3d9.clear, kMaxHooks, ClearHook{fn, user}) : 0; }

int D3d9OnPresent(RecompModDeviceFn fn, void* user) {
    return fn ? Add(g_d3d9.present, kMaxHooks, DeviceHook{fn, user}) : 0;
}

int D3d9VertexHlsl(const char* hlsl) {
    if (!hlsl || g_shaders_made) return 0;
    std::string& p = gpu::g_sm3_vertex_prelude;
    if (p.size() + std::strlen(hlsl) + 1 > kMaxVertexHlsl) return 0;
    p += hlsl;
    p += '\n';
    return 1;
}

int D3d9RenderState(uint32_t state, uint32_t* value) {
    uint32_t v = 0;
    if (!g_renderer || !g_renderer->RenderState(state, &v)) return 0;
    if (value) *value = v;
    return 1;
}

void D3d9SetProjection(const float m[16]) {
    if (g_renderer && m) g_renderer->SetProjection(m);
}

void D3d9RemakeWrittenTextures(int on) { g_d3d9.remake_written = on != 0; }

void D3d9SetTransform(uint32_t type, const float m[16]) {
    if (g_renderer && m) g_renderer->SetTransform(type, m);
}

// no frame captures here (recomp_mod.h)
int Replaying() { return 0; }
int CaptureOnDraw(void (*fn)(void*), void*) { return fn ? 1 : 0; }
void CaptureNote(uint32_t, const void*, uint32_t) {}
int ReplayOnNote(void (*fn)(uint32_t, const void*, uint32_t, void*), void*) { return fn ? 1 : 0; }

int D3d9OnCopy(RecompModCopyFn fn, void* user) { return fn ? Add(g_d3d9.copy, kMaxHooks, CopyHook{fn, user}) : 0; }

uint32_t DrawVertexBuffer(uint32_t* data, uint32_t* stride, uint32_t* first) {
    uint32_t d = 0, s = 0, f = 0, vb = g_renderer ? g_renderer->DrawVertexBuffer(&d, &s, &f) : 0;
    if (data) *data = d;
    if (stride) *stride = s;
    if (first) *first = f;
    return vb;
}

void D3d9FixedTransform(int on, RecompModFixedFn keep, void* user) {
    g_d3d9.fixed_transform = on != 0;
    g_d3d9.fixed_keep = keep;
    g_d3d9.fixed_keep_user = user;
}

// counted across mods: the pointer is wanted while any of them has an overlay up, as input's own wants are
void PointerElsewhere(int on) {
    static std::atomic<int> n{0};
    if (on) {
        input::PointerWant(true);
        if (++n == 1) {
            input::CursorElsewhere(true);
            input::KeysElsewhere(true);
            LOGR("[MOD] an overlay has the mouse and keys");
        }
        return;
    }
    int had = n.load();
    while (had > 0 && !n.compare_exchange_weak(had, had - 1)) {
    }
    if (had <= 0) return;  // an off with no on before it
    if (had == 1) {
        input::KeysElsewhere(false);
        input::CursorElsewhere(false);
        LOGR("[MOD] the game has the mouse and keys again");
    }
    input::PointerWant(false);
}

void D3d9SkinnedMeshes(int on) {
    d3d::g_skinned_meshes = on != 0;
    LOGR("mods: skinned meshes %s", on ? "on" : "off");
}

RecompModApi MakeApi() {
    RecompModApi a{};
    a.version = RECOMP_MOD_API_VERSION;
    a.size = sizeof(RecompModApi);
    a.title_id = TitleId;
    a.title_name = TitleName;
    a.guest_pointer = GuestPointer;
    a.on_frame = OnFrame;
    a.vertex_constants = VertexConstants;
    a.log = Log;
    a.d3d9_on_device = D3d9OnDevice;
    a.d3d9_on_draw = D3d9OnDraw;
    a.d3d9_on_clear = D3d9OnClear;
    a.d3d9_on_present = D3d9OnPresent;
    a.d3d9_vertex_hlsl = D3d9VertexHlsl;
    a.d3d9_render_state = D3d9RenderState;
    a.d3d9_set_projection = D3d9SetProjection;
    a.d3d9_remake_written_textures = D3d9RemakeWrittenTextures;
    a.d3d9_set_transform = D3d9SetTransform;
    a.replaying = Replaying;
    a.capture_on_draw = CaptureOnDraw;
    a.capture_note = CaptureNote;
    a.replay_on_note = ReplayOnNote;
    a.d3d9_on_copy = D3d9OnCopy;
    a.draw_vertex_buffer = DrawVertexBuffer;
    a.d3d9_fixed_transform = D3d9FixedTransform;
    a.pointer_elsewhere = PointerElsewhere;
    a.d3d9_skinned_meshes = D3d9SkinnedMeshes;
    return a;
}
const RecompModApi g_api = MakeApi();

std::filesystem::path ModsDir() {
    if (const char* named = std::getenv("RECOMP_MOD_LIBS_DIR"); named && *named) return named;
    return app::ExeDir() / "mods";
}

#ifdef _WIN32
constexpr const char* kExt = ".dll";
#else
constexpr const char* kExt = ".so";
#endif

// one library: kept if its recomp_mod_init says so
bool LoadOne(const std::filesystem::path& path) {
    std::string name = path.filename().string();
#ifdef _WIN32
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) {
        LOGR("[MOD] %s: could not load (%lu)", name.c_str(), GetLastError());
        return false;
    }
    auto init = reinterpret_cast<RecompModInitFn>(reinterpret_cast<void (*)()>(GetProcAddress(m, "recomp_mod_init")));
    auto unload = [&] { FreeLibrary(m); };
#else
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        LOGR("[MOD] %s: could not load (%s)", name.c_str(), dlerror());
        return false;
    }
    auto init = reinterpret_cast<RecompModInitFn>(dlsym(m, "recomp_mod_init"));
    auto unload = [&] { dlclose(m); };
#endif
    if (!init) {
        LOGR("[MOD] %s: no recomp_mod_init, not a mod", name.c_str());
        unload();
        return false;
    }
    bool keep = init(&g_api) != 0;
    if (!keep) unload();
    LOGR("[MOD] %s: %s", name.c_str(), keep ? "loaded" : "not for this game, unloaded");
    return keep;
}

}  // namespace

void Load() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* e = std::getenv("RECOMP_MOD_LIBS"); e && e[0] == '0') return;
    std::filesystem::path dir = ModsDir();
    std::error_code ec;
    std::vector<std::filesystem::path> found;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (e.is_regular_file(ec) && ext == kExt && found.size() < kMaxMods) found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());
    int kept = 0;
    for (const auto& p : found) kept += LoadOne(p);
    g_shaders_made = true;
    if (!found.empty()) LOGR("[MOD] %d of %zu in %s loaded", kept, found.size(), dir.string().c_str());
}

void Frame() {
    for (const FrameHook& h : g_frame) h.fn(h.user);
}

const D3d9Hooks& D3d9() { return g_d3d9; }

bool HooksDraws() { return !g_d3d9.draw.empty(); }

void SetRenderer(Renderer* r) { g_renderer = r; }

}  // namespace mods
