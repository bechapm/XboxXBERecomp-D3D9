// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report; the keyboard
// and mouse on controller 1
#include "core/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "core/testdrive.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

// keyboard and mouse

// the console's controls: 0-7 Pad::buttons bits, 8-15 Pad::analog, 16-23 stick halves
constexpr const char* kControls[] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "lthumb", "rthumb",
    "a", "b", "x", "y", "black", "white", "ltrigger", "rtrigger",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right", "rstick_up", "rstick_down", "rstick_left", "rstick_right"};
constexpr int kControlCount = int(std::size(kControls));
constexpr int kAnalogFirst = 8, kStickFirst = 16;
constexpr size_t kMaxSources = 4;

// the keys DirectXbox-Recomp has kept for its titles: arrows are the dpad, Z X A S the face buttons
constexpr std::pair<const char*, const char*> kBuiltIn[] = {
    {"a", "key:Z"}, {"b", "key:X"}, {"x", "key:A"}, {"y", "key:S"}, {"black", "key:W"}, {"white", "key:Q"},
    {"ltrigger", "key:E"}, {"rtrigger", "key:R"}, {"start", "key:RETURN"}, {"back", "key:BACK"},
    {"dpad_up", "key:UP"}, {"dpad_down", "key:DOWN"}, {"dpad_left", "key:LEFT"}, {"dpad_right", "key:RIGHT"}};

// the names DirectXbox-Recomp's input files use, so one file's bindings read the same in both
constexpr std::pair<const char*, int> kKeys[] = {
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"RETURN", VK_RETURN},
    {"ENTER", VK_RETURN}, {"BACK", VK_BACK}, {"BACKSPACE", VK_BACK}, {"SPACE", VK_SPACE}, {"TAB", VK_TAB},
    {"ESCAPE", VK_ESCAPE}, {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"CTRL", VK_CONTROL},
    {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"ALT", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
    {"CAPSLOCK", VK_CAPITAL}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
    {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11},
    {"F12", VK_F12}, {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2},
    {"NUMPAD3", VK_NUMPAD3}, {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6},
    {"NUMPAD7", VK_NUMPAD7}, {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9}, {"MULTIPLY", VK_MULTIPLY},
    {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"DECIMAL", VK_DECIMAL}, {"DIVIDE", VK_DIVIDE},
    {"SEMICOLON", VK_OEM_1}, {"EQUALS", VK_OEM_PLUS}, {"COMMA", VK_OEM_COMMA}, {"MINUS", VK_OEM_MINUS},
    {"PERIOD", VK_OEM_PERIOD}, {"SLASH", VK_OEM_2}, {"GRAVE", VK_OEM_3}, {"LBRACKET", VK_OEM_4},
    {"BACKSLASH", VK_OEM_5}, {"RBRACKET", VK_OEM_6}, {"QUOTE", VK_OEM_7}};
// mouse: sources above the key codes, the wheel's two ways last
constexpr const char* kMouse[] = {"left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"};
constexpr int kMouseVk[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
constexpr int kMouseFirst = 0x100, kWheelUp = kMouseFirst + 5, kWheelDown = kMouseFirst + 6;

std::vector<int> g_controls[kControlCount];  // sources, under g_lock
HWND g_hwnd = nullptr;
bool g_captured = false;  // mouse_look holds the pointer in the window, window thread

// the pointer (input.h): wanted by a game's menu, which takes the mouse's buttons and wheel from the controls
constexpr UINT kPointerSync = WM_APP + 0x40;  // to the window thread: the capture and the cursor follow the want
std::atomic<int> g_wants{0};       // callers wanting the pointer now
std::atomic<bool> g_want{false};  // any of them
std::atomic<bool> g_cursor_elsewhere{false};  // CursorElsewhere: an overlay has the cursor
std::atomic<bool> g_keys_elsewhere{false};    // KeysElsewhere: an overlay has the keyboard
bool g_keys_blocked[256] = {};                // keys an overlay took, held back until let go; the polling thread's
bool g_shown = false;           // under g_lock, as the rest of the pointer's state
Device g_last = Device::keyboard;
uint32_t g_moved = 0, g_presses[5] = {};
int32_t g_wheel_total = 0, g_wheel_part = 0;  // whole notches, and what a fine wheel turned toward the next
uint8_t g_blocked = 0;          // mouse buttons the pointer keeps from the controls until they are let go
LPARAM g_pos = -1;              // the last WM_MOUSEMOVE's client position
bool g_pad_active = false;      // controller 1's pad was in use at its last poll

// the mouse buttons as the player has them: Windows' left and right swapped back when they are swapped
uint8_t MouseHeld() {
    uint8_t held = 0;
    for (int i = 0; i < 5; i++)
        if (GetAsyncKeyState(kMouseVk[i]) & 0x8000) held |= uint8_t(1 << i);
    if (GetSystemMetrics(SM_SWAPBUTTON)) held = uint8_t((held & ~3) | (held & 1) << 1 | (held & 2) >> 1);
    return held;
}

// a source from its text, 0 when it names nothing
int Source(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (text.rfind("KEY:", 0) == 0) {
        std::string k = text.substr(4);
        if (k.size() > 1 && k[0] == '#') {
            int vk = std::atoi(k.c_str() + 1);
            return vk > 0 && vk < kMouseFirst ? vk : 0;
        }
        if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
        for (auto [name, vk] : kKeys)
            if (k == name) return vk;
        return 0;
    }
    if (text.rfind("MOUSE:", 0) == 0) {
        for (int i = 0; i < int(std::size(kMouse)); i++) {
            std::string m = kMouse[i];
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            if (text.substr(6) == m) return kMouseFirst + i;
        }
    }
    return 0;
}

std::string SourceName(int src) {
    if (src >= kMouseFirst) return std::string("mouse:") + kMouse[src - kMouseFirst];
    if ((src >= 'A' && src <= 'Z') || (src >= '0' && src <= '9')) return std::string("key:") + char(src);
    for (auto [name, vk] : kKeys)
        if (vk == src) return std::string("key:") + name;
    return "key:#" + std::to_string(src);
}

int ControlIndex(std::string_view name) {
    for (int i = 0; i < kControlCount; i++)
        if (name == kControls[i]) return i;
    return -1;
}

// "key:E mouse:left" into a control's sources; a control named again starts over
void Bind(std::vector<int>* controls, const char* control, const std::vector<std::string>& sources) {
    int c = ControlIndex(control);
    if (c < 0) {
        LOGE("input: no control %s", control);
        return;
    }
    controls[c].clear();
    for (const std::string& text : sources) {
        if (text.empty() || text == "none") continue;
        int src = Source(text);
        if (!src) LOGE("input: %s: no key or mouse button %s", control, text.c_str());
        else if (controls[c].size() < kMaxSources) controls[c].push_back(src);
    }
}

std::vector<std::string> Words(const char* text) {
    std::vector<std::string> words;
    std::istringstream in(text ? text : "");
    for (std::string w; in >> w;) words.push_back(w);
    return words;
}

// the wheel: each notch a press held long enough for a game reading its pads once a frame, then let go
constexpr uint64_t kWheelPressMs = 50, kWheelGapMs = 34;
constexpr int kWheelMaxPending = 4;
struct WheelWay {
    int pending = 0;
    bool down = false;
    uint64_t until = 0;
};
WheelWay g_wheel[2];  // up, down; under g_lock

bool WheelHeld(WheelWay& w, uint64_t now) {
    if (now >= w.until) {
        if (w.down) {
            w.down = false;
            w.until = now + kWheelGapMs;
        } else if (w.pending > 0) {
            w.pending--;
            w.down = true;
            w.until = now + kWheelPressMs;
        }
    }
    return w.down;
}

// mouse look: the stick pushed in proportion to the mouse's speed, worked out again every 12 ms whatever the game's
// polling rate and averaged with the period before, as DirectXbox-Recomp's mouse_look does
constexpr auto kLookPeriod = std::chrono::milliseconds(12);
constexpr float kLookFullScale = 100000.0f;  // counts a second for a full push at sensitivity 1
std::atomic<int> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<bool> g_direct{false};  // MouseDirect: the game takes the counts, the stick none
std::chrono::steady_clock::time_point g_look_at;
float g_look_speed[2] = {}, g_look_out[2] = {};  // under g_lock

void LookAxes(float out[2]) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - g_look_at;
    if (elapsed >= kLookPeriod) {
        // the first look after the game stopped reading spreads what came over a quarter second
        double secs = std::min(std::chrono::duration<double>(elapsed).count(), 0.25);
        float moved[2] = {float(g_mouse_dx.exchange(0)), -float(g_mouse_dy.exchange(0))};  // screen y grows down
        float sens = float(app::Get().mouse_sensitivity);
        for (int i = 0; i < 2; i++) {
            float speed = float(moved[i] / secs);
            float push = (speed + g_look_speed[i]) * 0.5f * sens / kLookFullScale;
            g_look_out[i] = std::fabs(push) < 0.0005f ? 0.0f : std::clamp(push, -1.0f, 1.0f);
            g_look_speed[i] = speed;
        }
        g_look_at = now;
    }
    out[0] = g_look_out[0];
    out[1] = g_look_out[1];
}

Pad g_kbm;  // the keyboard and mouse's part of controller 1 at its last poll, under g_lock

// keys only reach the game while its window is in front, mouse buttons while the pointer is over it too
void ReadKeyboardMouse(Pad& p) {
    p.connected = true;
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) return;
    POINT pt;
    bool over = g_captured || (GetCursorPos(&pt) && WindowFromPoint(pt) == g_hwnd);
    uint64_t now = SDL_GetTicks();
    bool wheel[2] = {WheelHeld(g_wheel[0], now), WheelHeld(g_wheel[1], now)};
    // a button the pointer took stays its own until let go: the click that closed a menu does not fire
    uint8_t mouse = MouseHeld();
    if (g_want) g_blocked = 0x1F;
    else g_blocked &= mouse;
    int sticks[4] = {};  // left x, left y, right x, right y
    for (int c = 0; c < kControlCount; c++) {
        bool held = false;
        for (int src : g_controls[c]) {
            if (src == kWheelUp || src == kWheelDown) held |= wheel[src - kWheelUp];
            else if (src >= kMouseFirst) held |= over && (mouse & ~g_blocked & (1 << (src - kMouseFirst)));
            else if (src > 0 && src < 256) {
                // a key the overlay took stays its own until let go: typing in it does not walk the player after
                bool down = (GetAsyncKeyState(src) & 0x8000) != 0;
                if (g_keys_elsewhere || g_keys_blocked[src]) g_keys_blocked[src] = down || g_keys_elsewhere;
                else held |= down;
            }
        }
        if (!held) continue;
        if (c < kAnalogFirst) p.buttons |= uint16_t(1 << c);
        else if (c < kStickFirst) p.analog[c - kAnalogFirst] = 0xFF;
        else {
            int half = c - kStickFirst;  // up, down, left, right of the left stick, then the right's
            int stick = half / 4 * 2;
            int way = half % 4;
            sticks[stick + (way < 2 ? 1 : 0)] += way == 0 || way == 3 ? 1 : -1;
        }
    }
    // a key pushes its half of the stick all the way
    int16_t* axes[4] = {&p.lx, &p.ly, &p.rx, &p.ry};
    for (int i = 0; i < 4; i++)
        if (sticks[i]) *axes[i] = sticks[i] > 0 ? int16_t(32767) : int16_t(-32768);
    if (app::Get().mouse_look && g_captured && !g_direct) {
        float look[2];
        LookAxes(look);
        for (int i = 0; i < 2; i++) {
            int v = std::clamp(int(*axes[2 + i]) + int(std::lround(look[i] * 32767.0f)), -32768, 32767);
            *axes[2 + i] = int16_t(v);
        }
    }
}

// a button held on either; a stick pushed by keys or the mouse is theirs, else the pad's
void Merge(Pad& p, const Pad& kbm) {
    p.connected = true;
    p.buttons |= kbm.buttons;
    for (int i = 0; i < 8; i++) p.analog[i] = std::max(p.analog[i], kbm.analog[i]);
    if (kbm.lx) p.lx = kbm.lx;
    if (kbm.ly) p.ly = kbm.ly;
    if (kbm.rx) p.rx = int16_t(std::clamp(int(p.rx) + int(kbm.rx), -32768, 32767));
    if (kbm.ry) p.ry = int16_t(std::clamp(int(p.ry) + int(kbm.ry), -32768, 32767));
}

void SyncCursor();

// mouse_look keeps the pointer hidden in the client area while the window is in front
void Capture(bool on) {
    on = on && g_hwnd && app::Get().keyboard_mouse && app::Get().mouse_look && !g_want;
    if (on) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
    } else if (g_captured) {
        ClipCursor(nullptr);
    }
    g_captured = on;
    SyncCursor();
}

// window thread: the cursor over the client area, hidden while mouse_look holds it or a key or pad was used last
bool Hidden() {
    std::lock_guard lock(g_lock);
    return g_captured || (g_want && !g_shown);
}

// window thread: the window thread's cursor display count follows Hidden(), below 0 while hidden and 0 or more while
// not. A cursor set by anyone else in between (ImGui's frame, an overlay hooking the window, the class cursor) then
// stays hidden too: on its own, SetCursor(nullptr) lasted only until the next of those, and the cursor flickered as the
// mouse moved. Set outright rather than stepped, as an overlay (RTX Remix's menu) moves the count itself
void SyncCursor() {
    if (g_cursor_elsewhere) return;
    if (Hidden()) {
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

bool SetClientCursor() {
    if (g_cursor_elsewhere) return false;  // left to the overlay
    SyncCursor();
    SetCursor(Hidden() ? nullptr : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
    return true;
}

bool OverClient() {
    POINT pt;
    if (!GetCursorPos(&pt) || WindowFromPoint(pt) != g_hwnd) return false;
    ScreenToClient(g_hwnd, &pt);
    RECT r;
    GetClientRect(g_hwnd, &r);
    return PtInRect(&r, pt) != 0;
}

// a key or pad was used: the pointer hides until the mouse moves
void Used(Device d) {
    bool hide;
    {
        std::lock_guard lock(g_lock);
        g_last = d;
        hide = g_shown;
        g_shown = false;
    }
    if (hide && g_want && g_hwnd) PostMessageW(g_hwnd, kPointerSync, 0, 0);
}

std::string MouseName(int i) {
    constexpr const char* kNames[] = {"Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5", "Wheel Up",
                                      "Wheel Down"};
    return kNames[i];
}

}  // namespace

void LoadControls(const void* table) {
    std::vector<int> controls[kControlCount];
    for (auto [control, keys] : kBuiltIn) Bind(controls, control, Words(keys));
    for (auto [control, keys] : app::kGame.controls) Bind(controls, control, Words(keys));
    if (const auto* t = static_cast<const toml::table*>(table)) {
        for (auto&& [key, node] : *t) {
            std::vector<std::string> sources;
            if (const auto* list = node.as_array()) {
                for (auto&& e : *list)
                    if (auto v = e.value<std::string>()) sources.push_back(*v);
            } else if (auto v = node.value<std::string>()) {
                sources = Words(v->c_str());
            }
            Bind(controls, std::string(key.str()).c_str(), sources);
        }
    }
    std::lock_guard lock(g_lock);
    for (int c = 0; c < kControlCount; c++) g_controls[c] = std::move(controls[c]);
}

void SaveControls(void* table) {
    auto& t = *static_cast<toml::table*>(table);
    std::lock_guard lock(g_lock);
    for (int c = 0; c < kControlCount; c++) {
        toml::array sources;
        for (int src : g_controls[c]) sources.push_back(SourceName(src));
        t.insert(kControls[c], std::move(sources));
    }
}

Pad KeyboardMouse() {
    std::lock_guard lock(g_lock);
    return g_kbm;
}

void MouseDirect(bool on) {
    if (on && !g_direct) LOGI("input: the game turns by the mouse itself; the right stick is left to the pad");
    g_direct = on;
}

bool MouseTake(float* dx, float* dy) {
    int x = g_mouse_dx.exchange(0), y = g_mouse_dy.exchange(0);
    if (dx) *dx = float(x);
    if (dy) *dy = float(y);
    return x || y;
}

void Window(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    if (!app::Get().keyboard_mouse) return;
    LOGI("input: keyboard and mouse on controller 1%s", app::Get().mouse_look ? ", the mouse on its right stick" : "");
    if (!app::Get().mouse_look) return;
    // raw movement, not the pointer's: the pointer stops at the window's edge
    RAWINPUTDEVICE rid{0x01, 0x02, 0, g_hwnd};  // generic desktop, mouse
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) LOGE("input: no raw mouse input (%lu)", GetLastError());
    if (GetForegroundWindow() == g_hwnd) Capture(true);  // shown before this, its activation went by
}

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (hwnd != g_hwnd || !app::Get().keyboard_mouse) return false;
    switch (msg) {
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (g_captured && GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size,
                                              sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_dx += ri.data.mouse.lLastX;
                g_mouse_dy += ri.data.mouse.lLastY;
                if (ri.data.mouse.lLastX || ri.data.mouse.lLastY) {
                    std::lock_guard lock(g_lock);
                    g_last = Device::mouse;
                }
            }
            return false;  // DefWindowProc cleans up after WM_INPUT
        }
        case WM_MOUSEWHEEL: {
            if (g_want) {
                std::lock_guard lock(g_lock);
                g_wheel_part += GET_WHEEL_DELTA_WPARAM(wp);
                g_wheel_total += g_wheel_part / WHEEL_DELTA;
                g_wheel_part %= WHEEL_DELTA;
                g_last = Device::mouse;
                *result = 0;
                return true;
            }
            int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            std::lock_guard lock(g_lock);
            g_last = Device::mouse;
            WheelWay& w = g_wheel[notches > 0 ? 0 : 1];
            w.pending = std::min(w.pending + std::abs(notches), kWheelMaxPending);
            *result = 0;
            return true;
        }
        case WM_MOUSEMOVE: {
            bool show = false;
            {
                std::lock_guard lock(g_lock);
                if (lp == g_pos) return false;  // Windows sends a move when nothing moved, after a click for one
                g_pos = lp;
                g_last = Device::mouse;
                if (g_want && !g_captured) {
                    g_moved++;
                    show = !g_shown;
                    g_shown = true;
                }
            }
            if (show) SetClientCursor();
            return false;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK: {
            int b = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK   ? 0
                    : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ? 1
                    : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK ? 2
                                                                       : 2 + GET_XBUTTON_WPARAM(wp);
            bool show = false;
            {
                std::lock_guard lock(g_lock);
                g_last = Device::mouse;
                if (g_want && !g_captured && b < 5) {
                    g_presses[b]++;
                    show = !g_shown;
                    g_shown = true;
                }
            }
            if (show) SetClientCursor();
            return false;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            Used(Device::keyboard);
            if (g_want && OverClient()) SetClientCursor();
            return false;
        case kPointerSync:
            Capture(GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd));
            if (OverClient()) SetClientCursor();
            *result = 0;
            return true;
        case WM_ACTIVATE:
            Capture(LOWORD(wp) != WA_INACTIVE && !HIWORD(wp));
            return false;
        case WM_SIZE:
        case WM_MOVE:
            if (g_captured) Capture(true);
            return false;
        case WM_SETCURSOR:
            if (LOWORD(lp) != HTCLIENT) return false;
            *result = TRUE;
            return SetClientCursor();
    }
    return false;
}

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    if (port == 0) {
        // a button, or a stick or trigger past half way, where none was: the pad is in use
        bool active = p.buttons || std::abs(int(p.lx)) > 0x4000 || std::abs(int(p.ly)) > 0x4000 ||
                      std::abs(int(p.rx)) > 0x4000 || std::abs(int(p.ry)) > 0x4000;
        for (uint8_t a : p.analog) active |= a > 0x80;
        if (active && !g_pad_active) Used(Device::pad);
        g_pad_active = active;
    }
    if (port == 0 && app::Get().keyboard_mouse) {
        std::lock_guard lock(g_lock);
        g_kbm = Pad{};
        ReadKeyboardMouse(g_kbm);
        Merge(p, g_kbm);
    }
    testdrive::Pad(port, p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

Pointer GetPointer() {
    Pointer ptr;
    bool want = g_want && g_hwnd && app::Get().keyboard_mouse;
    POINT pt{};
    bool over = want && !g_captured && GetForegroundWindow() == g_hwnd && GetCursorPos(&pt) &&
                WindowFromPoint(pt) == g_hwnd && ScreenToClient(g_hwnd, &pt);
    // the picture letterboxed in the client area, as the renderers show it (D3D9Backend::FrameRect)
    RECT rc{};
    if (g_hwnd) GetClientRect(g_hwnd, &rc);
    float w = float(std::max<LONG>(rc.right - rc.left, 1));
    float h = float(std::max<LONG>(rc.bottom - rc.top, 1));
    ptr.aspect = app::PictureAspect();
    float vw = std::min(w, h * ptr.aspect);
    float vh = vw / ptr.aspect;
    ptr.x = (float(pt.x) - (w - vw) / 2) / vw;
    ptr.y = (float(pt.y) - (h - vh) / 2) / vh;
    std::lock_guard lock(g_lock);
    ptr.inside = over && ptr.x >= 0 && ptr.x < 1 && ptr.y >= 0 && ptr.y < 1;
    ptr.shown = want && g_shown;
    ptr.moved = g_moved;
    ptr.held = over ? MouseHeld() : 0;
    std::copy(std::begin(g_presses), std::end(g_presses), ptr.presses);
    ptr.wheel = g_wheel_total;
    return ptr;
}

void PointerWant(bool on) {
    int n = on ? ++g_wants : --g_wants;
    if (n < 0) g_wants = n = 0;  // a let go with no want before it
    on = n > 0;
    if (g_want.exchange(on) == on) return;
    {
        std::lock_guard lock(g_lock);
        g_wheel[0] = g_wheel[1] = WheelWay{};  // notches turned for the controls do not arrive in the menu, or after it
        g_wheel_part = 0;
        if (on) g_shown = false;  // until the mouse moves
    }
    if (g_hwnd) PostMessageW(g_hwnd, kPointerSync, 0, 0);
}

bool CursorHidden() { return !g_cursor_elsewhere && Hidden(); }

void KeysElsewhere(bool on) { g_keys_elsewhere = on; }

void CursorElsewhere(bool on) {
    if (g_cursor_elsewhere.exchange(on) == on) return;
    if (g_hwnd) PostMessageW(g_hwnd, kPointerSync, 0, 0);  // the SDK's rule again when it ends
}

Device LastUsed() {
    std::lock_guard lock(g_lock);
    return g_last;
}

std::string KeyName(int vk) {
    for (int i = 0; i < 5; i++)
        if (vk == kMouseVk[i]) return MouseName(i);
    UINT sc = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC_EX);  // 0xE0 in the high byte for the keys left of the numbers
    if (!sc) return "";
    wchar_t name[64];
    int n = GetKeyNameTextW(LONG((sc & 0xFF) << 16 | ((sc & 0xFF00) ? 1 << 24 : 0)), name, int(std::size(name)));
    if (n <= 0) return "";
    char out[256];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, n, out, int(sizeof(out)), nullptr, nullptr);
    return std::string(out, std::max(len, 0));
}

std::string ControlName(const char* control) {
    int c = ControlIndex(control ? control : "");
    int src;
    {
        std::lock_guard lock(g_lock);
        if (c < 0 || g_controls[c].empty()) return "";
        src = g_controls[c][0];
    }
    return src >= kMouseFirst ? MouseName(src - kMouseFirst) : KeyName(src);
}

}  // namespace input
