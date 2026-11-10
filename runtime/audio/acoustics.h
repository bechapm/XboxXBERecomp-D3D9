#pragma once
#include <cstdint>
#include <vector>

// Sound through the level: a game hands over its level's surfaces and what each is made of, and every 3D sound is
// heard through what lies between it and the listener, by band, as much as each surface lets through. The rays are
// Steam Audio's (phonon.dll beside the executable, loaded when first needed); without it, with no level, or switched
// off, every sound plays exactly as before.
namespace audio::acoustics {

// a surface: the fraction of sound it absorbs, scatters and lets through, low, middle and high band (400 Hz, 2.5 kHz,
// 15 kHz), as Steam Audio's IPLMaterial
struct Material {
    float absorption[3];
    float scattering;
    float transmission[3];
};
// Steam Audio's own table of materials
inline constexpr Material kGeneric{{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};
inline constexpr Material kBrick{{0.03f, 0.04f, 0.07f}, 0.05f, {0.015f, 0.015f, 0.015f}};
inline constexpr Material kConcrete{{0.05f, 0.07f, 0.08f}, 0.05f, {0.015f, 0.002f, 0.001f}};
inline constexpr Material kCeramic{{0.01f, 0.02f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}};
inline constexpr Material kGravel{{0.60f, 0.70f, 0.80f}, 0.05f, {0.031f, 0.012f, 0.008f}};
inline constexpr Material kCarpet{{0.24f, 0.69f, 0.73f}, 0.05f, {0.020f, 0.005f, 0.003f}};
inline constexpr Material kGlass{{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}};
inline constexpr Material kPlaster{{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}};
inline constexpr Material kWood{{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}};
inline constexpr Material kMetal{{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}};
inline constexpr Material kRock{{0.13f, 0.20f, 0.24f}, 0.05f, {0.015f, 0.002f, 0.001f}};

// a level: triangles in the space of the game's 3D sound positions
struct Scene {
    std::vector<float> vertices;      // x y z
    std::vector<uint32_t> triangles;  // three vertex indices each
    std::vector<uint32_t> material;   // each triangle's index into materials
    std::vector<Material> materials;
    float meters = 1;  // metres in one unit
    float raise = 0;   // units every sound is raised (+y) before the rays: a game that puts sounds at its floor sets it
    const char* name = "";
};
// any thread: the level sounds are heard through from now on, replacing the last; an empty scene, none
void SetScene(Scene scene);

// any thread, as often as it changes: where the game's 3D sound positions are in the level. A sound at x y z (the
// listener's space, as the game hands DirectSound its positions) is at origin + x * axes[0..2] + y * axes[3..5] +
// z * axes[6..8] among the scene's triangles. A game that moves its sounds with the camera rather than its listener
// says so here every frame; until it does, the two spaces are the same. A sound is placed in the level when its position
// changes and stays there while the space moves, as a game that sets a sound once means it to
void SetSoundSpace(const float origin[3], const float axes[9]);

// on, and how much of the blocking is heard, 0 to 1 (the settings file's acoustics and acoustics_strength; XBE_ACOUSTICS
// 0 or 1 overrides on; XBE_ACOUSTICS_ALTERNATE=<seconds> switches it on and off that often, to hear the difference in one
// test run)
void Configure(bool on, float strength);
bool On();
bool Available();  // Steam Audio loaded, or not tried yet

// The mixer, for a 3D sound at `pos` heard at `listener` (the game's units): its gain in the low, middle and high band,
// 1 where nothing is in the way. The rays run on a thread of their own, so this is the last result, a frame or two
// behind; false when acoustics are off or have no level, and the sound is left as it is.
bool Gains(uint32_t sound, const float pos[3], const float listener[3], float out[3]);
void Forget(uint32_t sound);  // the sound is gone

// Hearing it work (a game's developer tools). A test sound left where the listener's space is now (the game's eye),
// made by the mixer and heard through the walls as any 3D sound; placing it again moves it
void PlaceTestSound();
void RemoveTestSound();
bool TestSound(float pos[3]);  // the mixer: where it is, in the game's space as its sounds are given; false: none
// the effect switched off and on every `seconds` (0 stops), as XBE_ACOUSTICS_ALTERNATE does
void Alternate(float seconds);
float Alternating();
struct Status {
    bool loaded = false;  // Steam Audio is running
    bool level = false;   // a level is in use
    bool on = false;      // switched on
    bool heard = false;   // on, and not in the off half of Alternate
    uint32_t sounds = 0, blocked = 0;  // 3D sounds playing, and those more than half blocked
    float test = -1;                   // the test sound's middle band heard, 0 to 1; -1: none
    bool test_placed = false;
};
Status Now();
// the 3D sounds mixed in the last fifth of a second, where they are in the level (as the rays see them, raise and
// all), for a game's debug view; with acoustics off too
struct Source {
    float pos[3];
    float through;  // the middle band heard, 0 to 1 (1 while switched off)
    bool beside;    // within 2 m of the listener: never muffled
    bool test;      // the test sound
};
std::vector<Source> Sources();

}  // namespace audio::acoustics
