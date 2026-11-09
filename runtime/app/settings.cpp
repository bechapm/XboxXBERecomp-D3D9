// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/acoustics.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    s.mouse_look = kGame.mouse_look;
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        input::LoadControls(nullptr);
        ApplyAcoustics();
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.renderer = t["renderer"].value_or(s.renderer);
    s.keyboard_mouse = t["keyboard_mouse"].value_or(s.keyboard_mouse);
    s.mouse_look = t["mouse_look"].value_or(s.mouse_look);
    s.mouse_sensitivity = std::clamp(t["mouse_sensitivity"].value_or(s.mouse_sensitivity), 1u, 100u);
    s.aspect_ratio = t["aspect_ratio"].value_or(s.aspect_ratio);
    s.acoustics = t["acoustics"].value_or(s.acoustics);
    s.acoustics_strength = std::min(t["acoustics_strength"].value_or(s.acoustics_strength), 100u);
    input::LoadControls(t["controls"].as_table());
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    ApplyAcoustics();
}

void ApplyAcoustics() { audio::acoustics::Configure(Get().acoustics, Get().acoustics_strength / 100.0f); }

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip},
                  {"renderer", s.renderer},
                  {"keyboard_mouse", s.keyboard_mouse},
                  {"mouse_look", s.mouse_look},
                  {"mouse_sensitivity", int64_t(s.mouse_sensitivity)},
                  {"aspect_ratio", s.aspect_ratio},
                  {"acoustics", s.acoustics},
                  {"acoustics_strength", int64_t(s.acoustics_strength)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    toml::table controls;
    input::SaveControls(&controls);
    t.insert("controls", std::move(controls));
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

std::vector<Mode> DisplayModes() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    std::vector<Mode> modes;
    if (!GetMonitorInfoW(mon, &mi)) return modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        // the desktop's colour depth; a monitor's portrait modes are the same modes turned
        if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (std::none_of(modes.begin(), modes.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
            modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    });
    return modes;
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

// Output for every frame, declared here for PictureAspect
Mode CachedOutput();

float PictureAspect() {
    if (!kGame.hor_plus) return Widescreen() ? 16.0f / 9 : 4.0f / 3;
    const std::string& a = Get().aspect_ratio;
    for (const Aspect& k : kAspects)
        if (a == k.name) return k.value;
    // "screen", or a shape this build does not know: the output's
    Mode m = CachedOutput();
    float v = m.height ? float(m.width) / float(m.height) : 16.0f / 9;
    return std::clamp(v, 4.0f / 3, 32.0f / 9);
}

float HorPlus() { return kGame.hor_plus ? PictureAspect() * 3 / 4 : 1.0f; }

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

static std::atomic<bool> g_size_at_start{false};
bool SizeAtStart() { return g_size_at_start; }
void SetSizeAtStart(bool at_start) { g_size_at_start = at_start; }

float RenderScale(uint32_t guest_height) {
    float h = CachedOutput().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
