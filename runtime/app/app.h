#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    std::string renderer = "d3d12";        // "d3d9" draws with Direct3D 9, for D3D9 tools such as RTX Remix
    bool keyboard_mouse = true;      // controller 1 is the keyboard and mouse too, bound in [controls]
    bool mouse_look = false;         // the mouse turns controller 1's right stick; GameInfo::mouse_look by default
    uint32_t mouse_sensitivity = 80;  // 1 to 100
    // the picture's shape for a game that widens its own view (GameInfo::hor_plus): "screen" the output's, or one of
    // kAspects; another game is shown at its console's 4:3 or 16:9 whatever this says
    std::string aspect_ratio = "screen";
    // sound through the level (audio/acoustics.h), for a game that hands over its level: 3D sounds muffled by what lies
    // between them and the listener, by material, as much as acoustics_strength says (0 to 100)
    bool acoustics = false;
    uint32_t acoustics_strength = 100;
    // headphones: 3D sounds placed all round the listener, above and behind too, by a head's filter (audio/acoustics.h)
    bool acoustics_headphones = false;
    // with acoustics: a sound the walls block is also heard by its way round them, from the doorway or the corner
    bool acoustics_paths = true;
};
// the shapes aspect_ratio takes besides "screen", width over height
struct Aspect {
    const char* name;
    float value;
};
inline constexpr Aspect kAspects[] = {{"4:3", 4.0f / 3}, {"16:10", 16.0f / 10}, {"16:9", 16.0f / 9}, {"21:9", 64.0f / 27},
                                      {"32:9", 32.0f / 9}};

Settings& Get();
void Load();
void ApplyAcoustics();  // acoustics and acoustics_strength to the mixer, after a change
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
std::vector<Mode> DisplayModes();  // the game window's monitor's modes, width then height ascending, no repeats
Mode Output();
float RenderScale(uint32_t guest_height);
// the renderer keeps the picture's size from the start until the next: a tool's d3d9.dll draws (RTX Remix), which takes
// the screen by the back buffer's size. A settings page says so beside what changes it (render scale, resolution, shape)
bool SizeAtStart();
void SetSizeAtStart(bool at_start);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// the shape the frame is shown at, width over height: a game that widens its own view (GameInfo::hor_plus) is shown at
// the aspect_ratio setting, at least 4:3 and at most 32:9; any other at 16:9 when Widescreen, else 4:3. Any thread
float PictureAspect();
// how many times wider than the console's 4:3 the picture is for a game that widens its own view, 1 for any other: the
// game widens its camera by it, and its 2D laid out for 4:3 is placed across it (gpu/picture.h)
float HorPlus();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // keyboard and mouse defaults: control and sources, "a" = "key:E mouse:left", over the built-in keys (input.cpp)
    std::vector<std::pair<const char*, const char*>> controls;
    bool mouse_look;  // the mouse_look setting's default
    // the game widens its own camera by HorPlus and places its 2D (gpu::SetTwoDPlacement), so its picture takes any
    // shape (aspect_ratio) rather than the console's 4:3 or 16:9
    bool hor_plus;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
