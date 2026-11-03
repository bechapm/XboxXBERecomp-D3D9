#pragma once
#include <cstdint>
#include <string>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot; the game code window starts it earlier
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

// keyboard and mouse: controller 1, beside any pad on that port, while the game window is in front (keyboard_mouse)
// the controls: the settings file's [controls] (toml::table, or null) over GameInfo::controls over the built-in keys
void LoadControls(const void* table);
void SaveControls(void* table);  // every control, as the settings file has them
// what the keyboard and mouse alone held at controller 1's last poll: GameInfo::pad tells a key from the pad's button
Pad KeyboardMouse();
// direct mouse aim: under mouse_look the game takes the mouse's movement itself, in counts, and the right stick gets
// none of it (a stick is a speed; a game that turns its camera by the counts aims as a PC game does)
void MouseDirect(bool on);
bool MouseTake(float* dx, float* dy);  // counts since the last take, right and down; false if nothing moved
// window thread, from the game window's messages
void Window(void* hwnd);  // the window is up: its mouse movement comes as WM_INPUT under mouse_look
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // true: handled

// the mouse as a pointer over the game's picture, for a game's own menus (docs/controls.md, "Pointer")
struct Pointer {
    bool inside = false;   // over the picture, the window in front and the pointer wanted
    bool shown = false;    // wanted, and the mouse used since a key or a pad was
    float x = 0, y = 0;    // across and down the picture, 0 to 1 inside it, past them in its bars
    float aspect = 4.0f / 3.0f;  // the picture's width over its height, as shown
    uint32_t moved = 0;    // counts the pointer's moves
    uint8_t held = 0;      // buttons: 1 left, 2 right, 4 middle, 8 x1, 16 x2
    uint32_t presses[5] = {};  // counts each button's presses, the order of held's bits
    int32_t wheel = 0;     // notches turned while wanted, up positive; a running total
};
Pointer GetPointer();  // any thread
// any thread: a menu wants the pointer, or no longer does. Each caller pairs its own calls, and the pointer is wanted
// while any caller wants it. While wanted, mouse_look lets the pointer go and the mouse's buttons and wheel are the
// pointer's, not the controls'; a button held when the want ends stays the pointer's until let go
void PointerWant(bool on);
enum class Device : uint8_t { pad, keyboard, mouse };
Device LastUsed();  // the last controller 1 used: a key, the mouse or a pad
// window thread: the cursor is to be hidden over the client area (mouse_look holds the mouse, or the pointer is wanted
// and a key or pad was used since the mouse moved). The window's WM_SETCURSOR asks this before ImGui, which would
// otherwise show its arrow there whatever the game is doing
bool CursorHidden();
std::string KeyName(int vk);  // a Windows virtual key as the keyboard calls it ("Enter", "Esc"), "" for none
std::string ControlName(const char* control);  // the first key or mouse button bound to a control, "" for none

}  // namespace input
