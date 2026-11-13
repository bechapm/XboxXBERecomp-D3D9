// A place's background (beds.h): loops and single recordings mixed under the title's sounds
#include "audio/beds.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace audio::beds {
namespace {

constexpr float kOut = 48000.0f;
constexpr float kFollow = 0.00002f;  // a bed's level and dullness move this much of the way a frame: about a second

// two one pole low passes in a row, whose corner falls from 18 kHz to 250 Hz as `dull` goes from 0 to 1
struct Dull {
    float a[2] = {}, b[2] = {};  // left and right
    static float Pole(float dull) {
        float hz = 18000.0f * std::pow(250.0f / 18000.0f, std::clamp(dull, 0.0f, 1.0f));
        return 1 - std::exp(-2 * 3.14159265f * hz / kOut);
    }
    float Run(int ear, float x, float pole) {
        a[ear] += pole * (x - a[ear]);
        b[ear] += pole * (a[ear] - b[ear]);
        return b[ear];
    }
};

// a sample read at the output's rate, between two of its frames; `loop` wraps
float Read(const Sample& s, double at, bool loop) {
    size_t n = s.pcm.size();
    size_t i = size_t(at);
    float t = float(at - double(i));
    size_t j = i + 1;
    if (loop) {
        i %= n;
        j %= n;
    } else if (j >= n) {
        j = n - 1;
    }
    return (float(s.pcm[i]) + (float(s.pcm[j]) - float(s.pcm[i])) * t) / 32768.0f;
}

struct Bed {
    std::shared_ptr<const Sample> sample;
    double at = 0;
    float level = 0, dull = 0;        // as heard now
    float want = 0, want_dull = 0;    // as last set
    Dull filter;
};
struct Once {
    std::shared_ptr<const Sample> sample;
    double at = 0;
    float level = 0, pan = 0, pole = 1, room = 0;
    Dull filter;
};

std::mutex g_mutex;
Bed g_beds[kBeds];
uint32_t g_made = 0;
Once g_shots[kShots];

}  // namespace

uint32_t Add(std::shared_ptr<const Sample> sample) {
    if (!sample || sample->pcm.size() < 2 || !sample->rate) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_made >= kBeds) return 0;
    g_beds[g_made] = Bed{};
    g_beds[g_made].sample = std::move(sample);
    return ++g_made;
}

void Set(uint32_t id, float level, float dull) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!id || id > g_made) return;
    Bed& b = g_beds[id - 1];
    b.want = std::isfinite(level) ? std::clamp(level, 0.0f, 4.0f) : 0.0f;
    b.want_dull = std::isfinite(dull) ? std::clamp(dull, 0.0f, 1.0f) : 0.0f;
}

void Shot(std::shared_ptr<const Sample> sample, float level, float pan, float dull, float room) {
    if (!sample || sample->pcm.size() < 2 || !sample->rate || !(level > 0)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Once& o : g_shots) {
        if (o.sample) continue;
        o = Once{};
        o.sample = std::move(sample);
        o.level = std::min(level, 4.0f);
        o.pan = std::clamp(pan, -1.0f, 1.0f);
        o.pole = Dull::Pole(dull);
        o.room = std::clamp(room, 0.0f, 1.0f);
        return;
    }
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Bed& b : g_beds) b = Bed{};
    for (Once& o : g_shots) o = Once{};
    g_made = 0;
}

void Mix(float* stereo, float* room, uint32_t frames) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t k = 0; k < g_made; k++) {
        Bed& b = g_beds[k];
        if (!b.sample || (b.want <= 0 && b.level < 1e-5f)) {
            b.level = 0;
            continue;
        }
        const Sample& s = *b.sample;
        double step = double(s.rate) / kOut, n = double(s.pcm.size());
        float held = Dull::Pole(b.dull);  // the corner moves a block at a time
        b.dull += (b.want_dull - b.dull) * std::min(1.0f, kFollow * float(frames));
        for (uint32_t i = 0; i < frames; i++) {
            b.level += (b.want - b.level) * kFollow;
            // the right ear half a loop on: the same recording, heard all round
            stereo[i * 2] += b.filter.Run(0, Read(s, b.at, true), held) * b.level;
            stereo[i * 2 + 1] += b.filter.Run(1, Read(s, b.at + n / 2, true), held) * b.level;
            b.at += step;
            if (b.at >= n) b.at -= n;
        }
    }
    for (Once& o : g_shots) {
        if (!o.sample) continue;
        const Sample& s = *o.sample;
        double step = double(s.rate) / kOut, n = double(s.pcm.size());
        float left = std::sqrt(0.5f * (1 - o.pan)), right = std::sqrt(0.5f * (1 + o.pan));
        for (uint32_t i = 0; i < frames && o.at < n - 1; i++) {
            float x = o.filter.Run(0, Read(s, o.at, false), o.pole) * o.level;
            stereo[i * 2] += x * left;
            stereo[i * 2 + 1] += x * right;
            if (room) room[i] += x * o.room;
            o.at += step;
        }
        if (o.at >= n - 1) o = Once{};
    }
}

}  // namespace audio::beds
