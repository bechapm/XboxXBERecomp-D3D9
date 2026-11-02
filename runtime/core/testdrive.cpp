#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "core/testdrive.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"

namespace testdrive {

namespace {

struct Press {
    double at;
    std::string buttons;
    double len;
};

std::mutex g_lock;
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_snap{false};
std::vector<Press> g_script;
std::vector<Press> g_live;  // presses from the cmd file, under g_lock
int g_snaps = 0;

std::filesystem::path Dir() {
    static const std::filesystem::path dir = [] {
        const char* v = std::getenv("XBE_TEST_DIR");
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

bool LoadScript() {
    const char* p = std::getenv("XBE_PAD_SCRIPT");
    if (FILE* f = p ? std::fopen(p, "r") : nullptr) {
        char line[200];
        while (std::fgets(line, sizeof line, f)) {
            char b[128];
            double t, len = 8;
            if (line[0] != '#' && std::sscanf(line, "%lf %127s %lf", &t, b, &len) >= 2) g_script.push_back({t, b, len});
        }
        std::fclose(f);
        LOGI("testdrive: pad script %s, %zu presses", p, g_script.size());
    }
    return true;
}

// a key's virtual key code: a letter or digit, F1 to F24, a few names, or #<code>
int KeyCode(const std::string& k) {
    if (k.size() > 1 && k[0] == '#') return std::atoi(k.c_str() + 1);
    if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
    if (k.size() >= 2 && k[0] == 'F' && std::atoi(k.c_str() + 1) >= 1 && std::atoi(k.c_str() + 1) <= 24)
        return 0x70 + std::atoi(k.c_str() + 1) - 1;
    static const std::pair<const char*, int> kNames[] = {
        {"RETURN", 0x0D}, {"ESCAPE", 0x1B}, {"SPACE", 0x20},  {"TAB", 0x09},   {"BACK", 0x08},  {"LEFT", 0x25},
        {"UP", 0x26},     {"RIGHT", 0x27},  {"DOWN", 0x28},   {"LSHIFT", 0xA0}, {"LCTRL", 0xA2}, {"DELETE", 0x2E}};
    for (auto& [n, vk] : kNames)
        if (k == n) return vk;
    return 0;
}

// each name joined by +
template <class F>
void Each(const std::string& s, F f) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        if (e > at) f(s.substr(at, e - at));
        at = e + 1;
    }
}

void Apply(const std::string& s, input::Pad& p) {
    Each(s, [&](const std::string& b) {
        if (b == "UP") p.buttons |= 1;
        else if (b == "DOWN") p.buttons |= 2;
        else if (b == "LEFT") p.buttons |= 4;
        else if (b == "RIGHT") p.buttons |= 8;
        else if (b == "START") p.buttons |= 16;
        else if (b == "BACK") p.buttons |= 32;
        else if (b == "L3") p.buttons |= 64;
        else if (b == "R3") p.buttons |= 128;
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
        else if (b == "BLACK") p.analog[4] = 255;
        else if (b == "WHITE") p.analog[5] = 255;
        else if (b == "LT") p.analog[6] = 255;
        else if (b == "RT") p.analog[7] = 255;
        else if (b == "LS") p.ly = 32767;
        else if (b == "LSD") p.ly = -32767;
        else if (b == "LSL") p.lx = -32767;
        else if (b == "LSR") p.lx = 32767;
        else if (b == "RS") p.ry = 32767;
        else if (b == "RSD") p.ry = -32767;
        else if (b == "RSL") p.rx = -32767;
        else if (b == "RSR") p.rx = 32767;
    });
}

bool Active(const Press& x, double t) { return t >= x.at && t < x.at + x.len; }

// the folder's cmd and snap files
void ReadDir() {
    if (Dir().empty()) return;
    std::filesystem::path cmd = Dir() / "cmd", snap = Dir() / "snap";
    std::error_code ec;
    if (std::filesystem::exists(cmd, ec)) {
        if (FILE* f = std::fopen(cmd.string().c_str(), "r")) {
            char b[128] = {};
            double len = 3;
            int n = std::fscanf(f, "%127s %lf", b, &len);
            std::fclose(f);
            std::filesystem::remove(cmd, ec);
            if (n >= 1) {
                std::lock_guard lock(g_lock);
                g_live.push_back({double(g_frames.load()), b, len});
                LOGI("testdrive: frame %llu, %s for %.0f frames", (unsigned long long)g_frames.load(), b, len);
            }
        }
    }
    if (std::filesystem::exists(snap, ec)) {
        std::filesystem::remove(snap, ec);
        g_snap = true;
    }
}

}  // namespace

bool On() {
    static const bool on = std::getenv("XBE_PAD_SCRIPT") || std::getenv("XBE_TEST_DIR");
    return on;
}

uint64_t Frames() { return g_frames.load(); }

void Pad(int port, input::Pad& p) {
    if (port != 0 || !On()) return;
    static const bool loaded = LoadScript();
    (void)loaded;
    double t = double(g_frames.load());
    bool any = false;
    std::lock_guard lock(g_lock);
    for (const Press& x : g_script)
        if (Active(x, t)) Apply(x.buttons, p), any = true;
    for (const Press& x : g_live)
        if (Active(x, t)) Apply(x.buttons, p), any = true;
    if (any) p.connected = true;
}

bool KeyHeld(int vk) {
    if (!On()) return false;
    double t = double(g_frames.load());
    bool held = false;
    auto test = [&](const Press& x) {
        if (!Active(x, t)) return;
        Each(x.buttons, [&](const std::string& b) {
            if (b.rfind("KEY:", 0) == 0 && KeyCode(b.substr(4)) == vk) held = true;
        });
    };
    std::lock_guard lock(g_lock);
    for (const Press& x : g_script) test(x);
    for (const Press& x : g_live) test(x);
    return held;
}

void Presented() {
    if (!On()) return;
    uint64_t n = ++g_frames;
    if (n % 2 == 0) ReadDir();  // every other frame: a file system check per frame is enough for scripts
    std::lock_guard lock(g_lock);
    std::erase_if(g_live, [&](const Press& x) { return double(n) >= x.at + x.len + 1; });
}

bool SnapDue() { return g_snap.load(); }

void Snap(uint32_t width, uint32_t height, const uint8_t* rows, uint32_t pitch) {
    g_snap = false;
    if (Dir().empty() || !width || !height) return;
    std::filesystem::path path = Dir() / ("snap_" + std::to_string(++g_snaps) + ".bmp");
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return;
    uint32_t row = width * 3, padded = (row + 3) & ~3u, image = padded * height;
    uint8_t h[54] = {'B', 'M'};
    auto put = [&](int at, uint32_t v) { std::memcpy(h + at, &v, 4); };
    put(2, 54 + image);
    put(10, 54);
    put(14, 40);
    put(18, width);
    put(22, height);  // bottom up
    h[26] = 1;
    h[28] = 24;
    put(34, image);
    std::fwrite(h, 1, sizeof h, f);
    std::vector<uint8_t> line(padded, 0);
    for (uint32_t y = height; y-- > 0;) {
        const uint8_t* src = rows + size_t(y) * pitch;
        for (uint32_t x = 0; x < width; x++) std::memcpy(&line[x * 3], src + x * 4, 3);
        std::fwrite(line.data(), 1, padded, f);
    }
    std::fclose(f);
    LOGI("testdrive: frame %llu saved as %s", (unsigned long long)g_frames.load(), path.filename().string().c_str());
}

}  // namespace testdrive
