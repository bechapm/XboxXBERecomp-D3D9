#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// A place's own background: recordings a game's hooks loaded, played under the title's sounds by its mixer
// (xdk/dsound.cpp). A bed is a loop that plays on, as loud and as dull as the game last said, each ear reading it half a
// loop apart so it is heard all round and not inside the head; a shot is one recording played once, to one side or the
// other, and answered by the room round the listener (audio/acoustics.h) as the title's sounds are. Nothing here makes a
// sound of its own: silence until a game adds recordings.
namespace audio::beds {

struct Sample {
    std::vector<int16_t> pcm;  // mono
    uint32_t rate = 48000;
};

// any thread: a loop, silent until Set. Its id, 0 for an empty sample or kBeds already made
constexpr uint32_t kBeds = 16;
uint32_t Add(std::shared_ptr<const Sample> sample);
// any thread, as often as it changes: how loud (linear, 0 none) and how dull (0 as recorded, 1 only what is under
// 250 Hz, as through a building's walls) the bed plays; it gets there over a second or two
void Set(uint32_t id, float level, float dull);
// any thread: the sample once. pan -1 left to 1 right; `room`, how much of it the room round the listener answers
constexpr uint32_t kShots = 8;
void Shot(std::shared_ptr<const Sample> sample, float level, float pan, float dull, float room);
void Clear();  // every bed and shot let go

// the mixer's thread: this block of them added to `stereo` (frames of left, right) and to what the room hears (frames)
void Mix(float* stereo, float* room, uint32_t frames);

}  // namespace audio::beds
