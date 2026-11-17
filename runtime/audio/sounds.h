#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// A game's own sounds: PCM its hooks made or loaded themselves, played by the title's mixer as a 3D voice among the
// title's own (xdk/dsound.cpp), so they are placed, rolled off and heard through the level (audio/acoustics.h) the
// same way. Nothing plays before the title has made its first DirectSound buffer or stream.
namespace audio::sounds {

struct Sample {
    std::vector<int16_t> pcm;  // mono
    uint32_t rate = 48000;
};

struct Shot {
    float pos[3] = {};      // where, in the space the title gives DirectSound its 3D positions in
    bool relative = false;  // pos is from the listener, turned with it (DS3DMODE_HEADRELATIVE)
    float volume = 1;       // linear
    float pitch = 1;        // the sample's rate times this
    float min_distance = 1, max_distance = 1e9f;  // whole within min, min / distance past it, no quieter past max
};

// any thread: the sample played once from its start. Its id while it plays; 0 with no mixer running, an empty sample
// or kVoices already playing
constexpr uint32_t kVoices = 32;
uint32_t Play(std::shared_ptr<const Sample> sample, const Shot& shot);
void Stop(uint32_t id);  // cut short; an id that has ended is ignored

}  // namespace audio::sounds
