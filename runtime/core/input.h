#pragma once
#include <cstdint>

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

}  // namespace input
