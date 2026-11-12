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

// any thread, every frame it applies: where the sounds are heard from, in the sounds' space (as SetSoundSpace), in place
// of the listener the game gave DirectSound: its place, the way it faces and its top. A game whose sounds follow its
// player's head moves the hearing to the camera on the screen this way (a cutscene's, a security camera's); `on` false
// gives it back to the game's listener. Hearing() is the mixer's: true, and the pose, while it is moved
void SetHearing(bool on, const float pos[3], const float front[3], const float top[3]);
bool Hearing(float pos[3], float front[3], float top[3]);

// on, and how much of the blocking is heard, 0 to 1 (the settings file's acoustics and acoustics_strength; XBE_ACOUSTICS
// 0 or 1 overrides on; XBE_ACOUSTICS_ALTERNATE=<seconds> switches it on and off that often, to hear the difference in one
// test run)
void Configure(bool on, float strength);
bool On();
bool Available();  // Steam Audio loaded, or not tried yet

// Headphones: a mono 3D sound's two ears come from a head's filter (Steam Audio's HRTF) for the direction it is heard
// from, in place of a mixer's own left and right, so above, below and behind sound as such. Needs no level and is
// apart from the walls (the settings file's acoustics_headphones; XBE_HEADPHONES 0 or 1 overrides)
void SetHeadphones(bool on);
bool Headphones();  // on, and the head is ready
// The mixer, a block of a mono sound (480 frames at 48 kHz) that is `d` from the listener in the listener's own space (x
// right, y up, z ahead, the game's units): its left and right ear. Within half a metre it moves into the head, heard
// less and less from a direction, as a player's own gun is. `image` tells a sound's hearings apart: 0 the sound itself, 1
// its way round (Way). false: no head, and the mixer places it as it always has
bool Ears(uint32_t sound, uint32_t image, const float d[3], const float* in, float* left, float* right, uint32_t frames);

// The mixer, for a 3D sound at `pos` heard at `listener` (the game's units): its gain in the low, middle and high band,
// 1 where nothing is in the way. The rays run on a thread of their own, so this is the last result, a frame or two
// behind; false when acoustics are off or have no level, and the sound is left as it is.
bool Gains(uint32_t sound, const float pos[3], const float listener[3], float out[3]);
void Forget(uint32_t sound);  // the sound is gone

// The way round. A sound the walls block is also heard by the shortest way round them, through a doorway or round a
// corner, from where that way reaches the listener: quieter by its length and duller by its bends. The ways are Steam
// Audio's baked paths between points it is given all over the level's floors, worked out in a few seconds after each
// SetScene on a thread of their own (until then, and for a level with no floors, sounds are only muffled). On with the
// acoustics unless switched off (the settings file's acoustics_paths; XBE_ACOUSTICS_PATHS 0 or 1 overrides)
void SetPaths(bool on);
// The mixer, for a mono 3D sound it has asked Gains about: where the sound seems to come from by its way round, in the
// space its position was given in and as far off as the way is long, and how much of each band arrives that way next
// to what a sound that far off in the open would give (nothing while the sound is in plain view). false: no way
bool Way(uint32_t sound, float pos[3], float gain[3]);

// The room. Every mono 3D sound is also heard as the room round the listener answers it: its echoes off the level's own
// surfaces, each from where it comes, and the reverberation after them, as long and as bright as the room is large and
// hard. Steam Audio traces the room from where the listener stands ten times a second on a thread of its own (a trace
// is a few hundredths of a second, so nothing is kept in a file), and the mixer runs what the sounds send through the
// last trace: the first half second as traced, the rest as a reverb of the length the trace measured. On headphones it
// is heard all round through the head's filter. `level` 0 to 1 is how much of it is heard (the settings file's
// acoustics_reverb and acoustics_reverb_level; XBE_ACOUSTICS_REVERB 0 or 1 overrides). Needs the acoustics on and a
// level
void SetRoom(bool on, float level);
bool Room();  // wanted and able: the mixer sends to it, and a title's own reverb stands down
// A recording's own echo. Many of a title's sounds were recorded or mastered with a room's ring on them (a shot's
// second of hall), which with the room above is two rooms at once. `kept` 0 to 1 is how much of that ring a 3D sound
// keeps (the settings file's acoustics_tails; 1 leaves every sound as recorded): the mixer lets a sound's quiet end
// die away sooner, the real room's answer takes its place. Only while the room is heard
void SetTails(float kept);
float Tails();  // 1 while the room is not heard
// The mixer, every block: `send` (480 frames, mono) is what the 3D sounds send to the room, `front` and `top` the
// listener's in the sounds' space. The room's answer is added to `left` and `right`. false: nothing was added
bool RoomAnswer(const float* send, const float front[3], const float top[3], float* left, float* right, uint32_t frames);

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
    uint32_t ways = 0;                 // of those, the ones also heard by a way round
    uint32_t probes = 0;               // the points the ways run between; 0: none worked out (yet)
    bool room = false;                 // the room round the listener is traced and heard (SetRoom)
    float room_ms = 0;                 // how long its last trace took
    float test = -1;                   // the test sound's middle band heard, 0 to 1; -1: none
    bool test_placed = false;
    bool test_way = false;             // the test sound is also heard by a way round
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
