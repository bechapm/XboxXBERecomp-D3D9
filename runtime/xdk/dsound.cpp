// DirectSound (XDK 4721, 4831 and 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "audio/acoustics.h"
#include "audio/audio.h"
#include "audio/beds.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/dsp_image.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_Pause
#define SYM_IDirectSoundBuffer_Pause FFFFFF0D
#define HAS_IDirectSoundBuffer_Pause 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF0E
#define HAS_IDirectSoundBuffer_SetEG 0
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

// DSMIXBIN numbers the host mixes by name
enum MixBin : uint32_t {
    kFrontLeft,
    kFrontRight,
    kCentre,
    kSub,
    kBackLeft,
    kBackRight,
    kCrosstalkFrontLeft,
    kCrosstalkFrontRight,
    kCrosstalkBackLeft,
    kCrosstalkBackRight,
    kFirstSend,  // effect sends from here up
};
constexpr uint32_t kBins = dsp::kBins;
constexpr uint32_t kInvalidCall = 0x88780032u;  // DSERR_INVALIDCALL
constexpr uint32_t kMaxChannels = 8;
// rows of a voice's mix: its channels, then a mono 3D voice's two ears
constexpr uint32_t kLeftEar = kMaxChannels;
constexpr uint32_t kRightEar = kMaxChannels + 1;
constexpr uint32_t kRows = kMaxChannels + 2;
constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples

// gain of each row on each mix bin
struct Gains {
    float bin[kRows][kBins];
};

// A mono 3D sound's second hearing, by its way round the walls (audio/acoustics.h, Way): the last block's level in each
// band, and the two low passes that split the sound into them
struct Detour {
    float band[3] = {0, 0, 0};
    float split[2] = {};
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    Gains gain = {};  // the last block's gains, the next block ramps from them
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples and its recent samples
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    bool head = false;  // the last block's ears came from the head's filter (audio/acoustics.h, Ears)
    Detour way;         // its second hearing, by the way round the walls (audio/acoustics.h, Way)
    float send = 0;     // the last block's level to the room round the listener (audio/acoustics.h, Room)
    // sound through the level (audio/acoustics.h): the last block's low, middle and high gains, and each channel's
    // low passes that split it into those bands and its deep part's level (Muffled)
    bool walls = false;
    float band[3] = {1, 1, 1};
    float split[kMaxChannels][4] = {};
    float body = -1;  // seconds of it before the echo recorded into it, none known under 0 (SetRecordedTails)
    // ADPCM block cache: the block read last and the one read before it, a frame between two blocks reads both
    int64_t block[2] = {-1, -1};
    uint8_t last = 0;  // decoded[last] holds block[last]
    int16_t decoded[2][65 * 8];
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
// where the sounds are heard from: the title's listener (IDirectSound_SetPosition, SetOrientation), or where the game
// moved the hearing (acoustics::SetHearing), taken as each block starts
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
float g_ds_listener[3] = {}, g_ds_front[3] = {0, 0, 1}, g_ds_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
// IDirectSound_SetMixBinHeadroom: the voice processor halves a bin per step, 1 for every bin but the last by default
uint8_t g_bin_headroom[kBins] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
struct Segment {
    uint32_t at;
    uint32_t bytes;
};
std::vector<Segment> g_effect_states;  // each effect's state segment of the downloaded image

void ForgetBlocks(Voice& v) {
    v.block[0] = -1;
    v.block[1] = -1;
}

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// the decoded ADPCM block at guest address at, decoded once while it stays in the cache
const int16_t* DecodedBlock(Voice& v, int64_t at) {
    for (uint8_t k = 0; k < 2; k++) {
        if (v.block[k] != at) continue;
        v.last = k;
        return v.decoded[k];
    }
    uint8_t k = v.last ^ 1;  // the slot not read last
    DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded[k]);
    v.block[k] = at;
    v.last = k;
    return v.decoded[k];
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        return DecodedBlock(v, at)[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// the console's stereo downmix of the speaker bins: centre and back pair at -3 dB, the sub dropped
constexpr float kFold = 0.707f;

// A voice with no speaker bin and no effects image plays its loudest bin on the front pair; with speaker bins the
// crosstalk bins join their speakers and effect sends are dropped. Without the title's image nothing reads them.
void RouteWithoutImage(const Voice& v, Gains& g) {
    bool speakers = false;
    for (uint32_t s = 0; s < v.nbins; s++) speakers |= v.bin[s] < kFirstSend && v.bin[s] != kSub;
    if (!v.nbins) speakers = true;
    for (auto& row : g.bin) {
        if (!speakers) {
            float loudest = *std::max_element(row, row + kBins);
            std::fill(row, row + kBins, 0.0f);
            row[kFrontLeft] = loudest;
            row[kFrontRight] = loudest;
            continue;
        }
        row[kFrontLeft] += row[kCrosstalkFrontLeft];
        row[kFrontRight] += row[kCrosstalkFrontRight];
        row[kBackLeft] += row[kCrosstalkBackLeft];
        row[kBackRight] += row[kCrosstalkBackRight];
        std::fill(row + kCrosstalkFrontLeft, row + kBins, 0.0f);
    }
}

// The library gives every hardware voice (two channels) a run of n / voices slots. A stereo voice plays its left
// channel on even slots and its right on odd ones.
uint32_t ChannelOfSlot(const Voice& v, uint32_t voice, uint32_t slot) {
    uint32_t voice_channels = std::min<uint32_t>(v.channels - voice * 2, 2);
    return voice * 2 + slot % voice_channels;
}

// each channel's level on each bin; the voice processor divides a bin by 2^headroom, which the image may undo
Gains SlotGains(const Voice& v) {
    static const uint8_t kSpeakerBins[6] = {0, 1, 2, 3, 4, 5};  // no mix bins given: the speakers in order
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t voices = (channels - 1) / 2 + 1;
    const uint8_t* bin = v.nbins ? v.bin : kSpeakerBins;
    uint32_t n = v.nbins ? v.nbins : std::min<uint32_t>(voices * 2, 6);
    uint32_t per = std::max<uint32_t>(n / voices, 1);
    bool image = dsp::Loaded();
    Gains g = {};
    for (uint32_t k = 0; k < voices; k++) {
        for (uint32_t s = k * per; s < (k + 1) * per && s < n; s++) {
            uint32_t b = bin[s];
            float level = Db(v.bin_mb[b]) * v.volume * v.headroom;
            if (image) level = std::ldexp(level, -g_bin_headroom[b]);
            g.bin[ChannelOfSlot(v, k, s)][b] += level;
        }
    }
    return g;
}

// A 3D voice as the library's 3D pass sees it. Its levels have no left/right term: a mono voice's two ears differ
// by the interaural delay, and on a stereo console the back crosstalk pair is silent.
struct Place {
    bool on = false;
    float att = 1;      // distance, every bin
    float front = 1;    // crosstalk front pair
    float centre = 1;   // centre bin
    float azimuth = 0;  // degrees, plus to the right
    float elevation = 0;
    float d[3] = {0, 0, 0};  // from the listener, in the listener's own space: right, up, ahead
};

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the library's octant estimate of the angle of along from across: 45 degrees where they are equal
float OctantAngle(float along, float across) {
    float a = std::fabs(along);
    float s = std::fabs(across);
    if (a > s) return 45 * s / a;
    if (s > 0) return 90 - 45 * a / s;
    return 0;
}

float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float deg = OctantAngle(ahead, side);
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float deg = OctantAngle(flat, up);
    return up < 0 ? -deg : deg;
}

// interaural delay in samples by azimuth in steps of 3 degrees at elevation 0, the far ear's HRTF delay
float Itd(float azimuth) {
    static const uint8_t kDelay[61] = {0,  2,  3,  4,  4,  5,  6,  9,  10, 11, 12, 12, 13, 14, 17, 18,
                                       19, 20, 21, 21, 22, 23, 24, 25, 26, 28, 29, 29, 31, 30, 30, 31,
                                       30, 28, 27, 28, 27, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
                                       14, 13, 13, 12, 11, 8,  7,  6,  6,  3,  2,  2,  0};
    constexpr int kStep = 3;
    if (!(std::fabs(azimuth) <= 180)) return 0;
    int deg = int(std::fabs(azimuth) + kStep / 2.0f) / kStep * kStep;
    float delay = kDelay[deg / kStep];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre shares the front with it below 45 degrees: 0.7071 cos^2(2a) and
// 0.8409 cos(2a) by whole degrees of azimuth and elevation
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// distance attenuation, 1.0 inside the min distance: min / distance, or the voice's rolloff curve
float Attenuation(const Voice& v, float dist) {
    if (dist <= v.min_dist) return 1;
    if (v.curve.empty()) return v.min_dist / std::min(dist, v.max_dist);
    if (v.max_dist <= v.min_dist) return 1;
    // points spread evenly from min to max distance, 1.0 at min
    float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
    size_t i = std::min(size_t(at), v.curve.size() - 1);
    float from = i ? v.curve[i - 1] : 1.0f;
    return from + (v.curve[i] - from) * (at - i);
}

Place Locate(const Voice& v) {
    static const float kAhead[3] = {0, 0, 1};
    static const float kRight[3] = {1, 0, 0};
    static const float kUp[3] = {0, 1, 0};
    Place at;
    if (!v.is3d || v.mode == 2) return at;
    at.on = true;
    bool relative = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !relative; i++) d[i] -= g_listener[i];
    at.att = Attenuation(v, std::sqrt(Dot(d, d)));
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    const float* front = relative ? kAhead : g_front;
    const float* side = relative ? kRight : right;
    const float* top = relative ? kUp : g_top;
    at.azimuth = Azimuth(d, front, side);
    at.elevation = Elevation(d, front, side, top);
    at.d[0] = Dot(d, side);
    at.d[1] = Dot(d, top);
    at.d[2] = Dot(d, front);
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kCentre) ShareCentre(at);
    return at;
}

float BinScale(const Place& at, uint32_t b) {
    if (!at.on) return 1;
    if (b == kCentre) return at.att * at.centre;
    if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) return at.att * at.front;
    if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) return 0;
    return at.att;
}

// a mono 3D voice's crosstalk bins play its ears: the left bins the left ear, the right bins the right one
void SplitEars(Gains& g) {
    static const uint32_t kLeft[2] = {kCrosstalkFrontLeft, kCrosstalkBackLeft};
    static const uint32_t kRight[2] = {kCrosstalkFrontRight, kCrosstalkBackRight};
    for (int k = 0; k < 2; k++) {
        std::swap(g.bin[0][kLeft[k]], g.bin[kLeftEar][kLeft[k]]);
        std::swap(g.bin[0][kRight[k]], g.bin[kRightEar][kRight[k]]);
    }
}

Gains Levels(const Voice& v, const Place& at) {
    Gains g = SlotGains(v);
    for (auto& row : g.bin) {
        for (uint32_t b = 0; b < kBins; b++) row[b] *= BinScale(at, b);
    }
    if (at.on && v.channels == 1) SplitEars(g);
    if (!dsp::Loaded()) RouteWithoutImage(v, g);
    // a sound placed at NaN goes silent alone; its NaN gain would silence the whole mix
    for (auto& row : g.bin) {
        for (float& gain : row) {
            if (!std::isfinite(gain)) gain = 0;
        }
    }
    return g;
}

// a row's level on one bin, ramped across the block: levels follow the game once a frame, stepped a fast turn clicks
struct Lane {
    uint32_t row;
    uint32_t bin;
    float gain;
    float step;
};

// the levels the block can hear; one zero at both ends of its ramp adds nothing to the mix
std::vector<Lane> AudibleLanes(const Gains& from, const Gains& to, uint32_t frames) {
    std::vector<Lane> lanes;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t b = 0; b < kBins; b++) {
            float a = from.bin[r][b];
            float z = to.bin[r][b];
            if (a != 0 || z != 0) lanes.push_back({r, b, a, (z - a) / float(frames)});
        }
    }
    return lanes;
}

// the voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
};

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, looping or stopping at the end of its region; none once stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
    uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
    if (f >= end) {
        if (!v.looping || end <= start) {
            v.playing = false;
            v.pos = 0;
            return std::nullopt;
        }
        v.pos = start + std::fmod(v.pos - start, double(end - start));
        f = uint32_t(v.pos);
    }
    if (!v.data) return std::nullopt;
    uint32_t base = v.data + v.play_start;
    Cursor at = {base, f, base, f};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping) {
        at.next_f = start;
    }
    return at;
}

// a 3D voice's place in the listener's world: a head relative one turned and moved by the listener
void WorldPosition(const Voice& v, float out[3]) {
    if (v.mode != 1) {
        std::memcpy(out, v.p, sizeof v.p);
        return;
    }
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    for (int i = 0; i < 3; i++) out[i] = g_listener[i] + right[i] * v.p[0] + g_top[i] * v.p[1] + g_front[i] * v.p[2];
}

// what the level lets through to a 3D voice, low, middle and high band (audio/acoustics.h); false: it plays as it is
bool Walls(const Voice& v, const Place& at, float out[3]) {
    if (!at.on) return false;
    float pos[3];
    WorldPosition(v, pos);
    return audio::acoustics::Gains(v.obj, pos, g_listener, out);
}

// a channel split at 800 Hz and 8 kHz by two one pole low passes at the output rate, 1 - exp(-2 pi f / 48000), the bands
// Steam Audio's materials are given in; the three bands add up to the sample again
constexpr float kLowSplit = 0.0994231f;
constexpr float kHighSplit = 0.6490802f;
float Through(float x, float* split, const float* gain) {
    split[0] += kLowSplit * (x - split[0]);
    split[1] += kHighSplit * (x - split[1]);
    return gain[0] * split[0] + gain[1] * (split[1] - split[0]) + gain[2] * (x - split[1]);
}

// The same for a sound heard through walls, with what is under 120 Hz apart: a wall stops less of a sound the deeper
// it is (6 dB less an octave, the mass law), so the deep part keeps kDeep times what the low band keeps, and a sound
// with much of it, an explosion, a shot, a giant's step, is a rumble behind a wall where it was a bang (Ben, 12 Nov
// 2026: a mine behind a closed door was only quieter and a little duller). The more of the deep part there is (its
// level followed, from kRumbleFrom to kRumbleFull of full scale), the more it is raised, up to kRumble, by as much as
// the wall takes of the middle band: a person's step, with next to none, gains nothing. It never keeps more than
// kDeepMost of what the sound had, and only as much over the low band's share as the sound's way round leaves (`spare`,
// 1 with no way): with twice the deep part and a way through a doorway beside it, a mine behind a wall was 6 dB louder
// under 120 Hz than with no wall (12 Nov 2026). state: the two splits, the deep part and its level
constexpr float kDeepSplit = 0.0155858f;  // 120 Hz
constexpr float kDeep = 6.0f;             // 2.7 octaves under the low band's middle
constexpr float kRumble = 2.0f, kRumbleFrom = 0.03f, kRumbleFull = 0.2f;
constexpr float kDeepMost = 0.8f;
float Muffled(float x, float* state, const float* gain, float spare) {
    state[0] += kLowSplit * (x - state[0]);
    state[1] += kHighSplit * (x - state[1]);
    state[2] += kDeepSplit * (state[0] - state[2]);
    float deep = state[2], level = std::fabs(deep);
    state[3] += (level > state[3] ? 0.01f : 0.0002f) * (level - state[3]);  // up in 2 ms, down in 100
    float big = std::clamp((state[3] - kRumbleFrom) / (kRumbleFull - kRumbleFrom), 0.0f, 1.0f);
    float taken = 1 - std::min(gain[1], 1.0f);
    float most = std::max(kDeepMost, gain[0]);
    float raised = std::min(most, gain[0] * (1 + (kDeep - 1) * taken) * (1 + (kRumble - 1) * big * taken));
    float keeps = gain[0] + std::max(raised - gain[0], 0.0f) * std::clamp(spare, 0.0f, 1.0f);
    return keeps * deep + gain[0] * (state[0] - deep) + gain[1] * (state[1] - state[0]) + gain[2] * (x - state[1]);
}

// A recording's own echo let die sooner (audio/acoustics.h, SetRecordedTails). A sound's key: FNV-1a over its buffer's
// length in bytes, its rate, channels and format tag (32 bits each, low byte first), then the buffer's first 2048
// bytes as the title gave them, 31 bits of it. And how much of a sound is heard `seconds` into it, when all after
// `body` is the recording's echo: whole to there, then falling kTailFall dB a second with nothing kept, none with all
constexpr float kTailFall = 60.0f;
uint32_t SampleKey(const Voice& v) {
    if (!v.data || !v.bytes) return 0;
    uint32_t h = 2166136261u;
    auto byte = [&](uint8_t x) { h = (h ^ x) * 16777619u; };
    for (uint32_t field : {v.bytes, v.rate, uint32_t(v.channels), uint32_t(v.tag)})
        for (int k = 0; k < 4; k++) byte(uint8_t(field >> (8 * k)));
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(v.data));
    for (uint32_t i = 0; i < std::min(v.bytes, 2048u); i++) byte(p[i]);
    return h & 0x7FFFFFFFu;
}
float TailGain(float seconds, float body, float kept) {
    if (seconds <= body) return 1.0f;
    return std::pow(10.0f, -(seconds - body) * kTailFall * (1 - kept) / 20.0f);
}

// A flat background stream heard from far points in the level (audio/acoustics.h, SetFar): the stream's last two
// seconds, and each point's hearing of it, a third of a second after the one before so the points are not one sound
// in the middle of the head. The output thread alone renders
constexpr uint32_t kFarRing = audio::kRate * 2;
constexpr uint32_t kFarLater = audio::kRate * 33 / 100;
struct FarPoint {
    float split[4] = {};
    float band[3] = {1, 1, 1};
    float left = 0, right = 0, send = 0;
};
FarPoint g_far[audio::acoustics::kFar];
float g_far_pos[audio::acoustics::kFar][3];
float g_far_open[audio::acoustics::kFar];
// what a covered point keeps, low, middle and high: one wall of the level's commonest (12 Nov 2026, Training Ground)
constexpr float kFarCovered[3] = {0.19f, 0.08f, 0.06f};
constexpr float kFarFollow = 0.02f;  // a point's cover moves this much of the way a block: half a second
float g_far_reach = 1;
int g_far_n = 0;  // this block's points
std::vector<float> g_far_ring;
uint32_t g_far_at = 0;
bool g_far_fed = false;

// What the mono 3D sounds send to the room round the listener this block (audio/acoustics.h, Room), and the last
// detour's sound for it: the output thread alone renders
std::vector<float> g_room_send;
std::vector<float> g_way_mono;

// a voice's own level, apart from where it is: its volume on the loudest of its bins, as SlotGains has it
float OwnLevel(const Voice& v) {
    bool image = dsp::Loaded();
    float most = 0;
    if (!v.nbins) most = image ? std::ldexp(1.0f, -g_bin_headroom[kFrontLeft]) : 1.0f;
    for (uint32_t s = 0; s < v.nbins; s++) {
        float level = Db(v.bin_mb[v.bin[s]]);
        if (image) level = std::ldexp(level, -g_bin_headroom[v.bin[s]]);
        most = std::max(most, level);
    }
    float level = most * v.volume * v.headroom;
    return std::isfinite(level) ? level : 0.0f;
}

// The room's answer never clips the mix: where the two speakers and the answer together would pass full scale, the
// answer gives way within a millisecond and comes back over a fifth of a second
struct Duck {
    float gain = 1;
};
void AddAnswer(Duck& duck, const float* bins, const float* left, const float* right, float* out, uint32_t frames) {
    constexpr float kTop = 0.98f;
    float need = 1;
    for (uint32_t i = 0; i < frames; i++) {
        const float* b = bins + size_t(i) * kBins;
        float dry[2] = {b[kFrontLeft] + kFold * (b[kCentre] + b[kBackLeft]), b[kFrontRight] + kFold * (b[kCentre] + b[kBackRight])};
        float wet[2] = {std::fabs(left[i]), std::fabs(right[i])};
        for (int c = 0; c < 2; c++) {
            float room = std::max(kTop - std::fabs(dry[c]), 0.0f);
            if (wet[c] > room) need = std::min(need, room / wet[c]);
        }
    }
    constexpr uint32_t kDown = 48;  // frames to give way in
    float from = duck.gain, to = std::min(need, std::min(1.0f, duck.gain + float(frames) / (0.2f * audio::kRate)));
    for (uint32_t i = 0; i < frames; i++) {
        float t = to < from ? std::min(1.0f, float(i + 1) / kDown) : float(i + 1) / float(frames);
        float gain = from + (to - from) * t;
        const float* b = bins + size_t(i) * kBins;
        out[i * 2] = std::clamp(b[kFrontLeft] + kFold * (b[kCentre] + b[kBackLeft]) + left[i] * gain, -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(b[kFrontRight] + kFold * (b[kCentre] + b[kBackRight]) + right[i] * gain, -1.0f, 1.0f);
    }
    duck.gain = to;
}

// A mono 3D sound heard a second time, by its way round the walls: `dry` (the sound before the walls dulled it) comes
// from where the way reaches the listener, at `level` times the bands the way leaves, the level of a sound as far off as
// the way is long next to the sound's own. Added to its two ears, which the head's filter makes on headphones and a pan
// otherwise. `gain` is zero once the way is gone: the block fades it out
void MixDetour(Detour& way, uint32_t sound, const float from[3], const float gain[3], float level, const float* dry,
               float* rows, uint32_t frames) {
    static std::vector<float> ear[2];  // the output thread alone renders
    std::vector<float>& mono = g_way_mono;
    mono.resize(frames);
    float band[3], step[3];
    for (int b = 0; b < 3; b++) {
        band[b] = way.band[b];
        float to = std::isfinite(level) ? gain[b] * level : 0.0f;
        step[b] = (to - band[b]) / float(frames);
        way.band[b] = to;
    }
    for (uint32_t i = 0; i < frames; i++) {
        for (int b = 0; b < 3; b++) band[b] += step[b];
        mono[i] = Through(dry[i], way.split, band);
    }
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float d[3] = {from[0] - g_listener[0], from[1] - g_listener[1], from[2] - g_listener[2]};
    float local[3] = {Dot(d, right), Dot(d, g_top), Dot(d, g_front)};
    ear[0].resize(frames);
    ear[1].resize(frames);
    if (!audio::acoustics::Headphones() ||
        !audio::acoustics::Ears(sound, 1, local, mono.data(), ear[0].data(), ear[1].data(), frames)) {
        // no head: by level, both ears whole straight ahead as a sound's own ears are
        float pan = std::sin(Azimuth(d, g_front, right) * 3.14159265f / 180);  // -1 left, 1 right
        float left = 1.41421356f * std::cos((pan + 1) * 0.78539816f), rightg = 1.41421356f * std::sin((pan + 1) * 0.78539816f);
        for (uint32_t i = 0; i < frames; i++) {
            ear[0][i] = mono[i] * left;
            ear[1][i] = mono[i] * rightg;
        }
    }
    for (uint32_t i = 0; i < frames; i++) {
        rows[size_t(i) * kRows + kLeftEar] += ear[0][i];
        rows[size_t(i) * kRows + kRightEar] += ear[1][i];
    }
}

// adds the voice to the bins, frames of kBins interleaved, its levels ramped across the block
void MixVoice(Voice& v, float* bins, uint32_t frames) {
    Place at = Locate(v);
    // a voice the level stops filtering is ramped back to every band whole over one block, then left alone
    float walls[3] = {1, 1, 1};
    bool through = Walls(v, at, walls);
    bool filter = through || v.walls;
    float band[3], band_step[3];
    for (int b = 0; b < 3; b++) {
        band[b] = v.band[b];
        band_step[b] = (walls[b] - v.band[b]) / float(frames);
    }
    Gains target = Levels(v, at);
    float itd = Itd(at.azimuth);
    if (!v.has_gain) {
        v.gain = target;
        v.itd = itd;
    }
    v.has_gain = true;
    std::vector<Lane> lanes = AudibleLanes(v.gain, target, frames);
    bool ears = at.on && v.channels == 1;
    // headphones: the two ears come from a head's filter for the sound's direction, in place of the delay between them
    bool head = ears && audio::acoustics::Headphones();
    float itd_step = (itd - v.itd) / float(frames);
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t read = ears ? 1u : 0u;  // the channels a lane plays, a bit each
    for (const Lane& lane : lanes) {
        if (lane.row < kMaxChannels) read |= 1u << lane.row;
    }
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    v.block[v.last ^ 1] = -1;  // the title may have rewritten that block's memory since the last mix
    // the block's rows are read first and mixed after, the head's filter between: the output thread alone renders
    static std::vector<float> rows;
    rows.assign(size_t(frames) * kRows, 0.0f);
    // its way round the walls, a second hearing of the sound as it was before they dulled it
    float way_from[3] = {}, way_gain[3] = {0, 0, 0};
    bool way = ears && through && audio::acoustics::Way(v.obj, way_from, way_gain);
    bool detour = way || v.way.band[0] != 0 || v.way.band[1] != 0 || v.way.band[2] != 0;
    static std::vector<float> dry;
    if (detour) dry.assign(frames, 0.0f);
    // a placed sound known to end in a recorded echo loses some of it while the room answers it (TailGain)
    float kept = at.on && !v.stream && !v.looping && v.body >= 0 ? audio::acoustics::Tails() : 1.0f;
    uint32_t got = 0;
    for (uint32_t i = 0; i < frames; i++) {
        std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
        if (!cursor) break;
        float t = float(v.pos - cursor->f);
        float* row = &rows[size_t(i) * kRows];
        for (uint32_t c = 0; c < channels; c++) {
            if (!(read >> c & 1)) continue;
            float x = Sample(v, cursor->data, cursor->f, int(c));
            row[c] = x + (Sample(v, cursor->next_data, cursor->next_f, int(c)) - x) * t;
        }
        if (kept < 1) {
            float gain = TailGain(float(cursor->f) / float(v.rate), v.body, kept);
            for (uint32_t c = 0; c < channels; c++) row[c] *= gain;
        }
        if (detour) dry[i] = row[0];
        if (filter) {
            for (int b = 0; b < 3; b++) band[b] += band_step[b];
            for (uint32_t c = 0; c < channels; c++)
                if (read >> c & 1) row[c] = Muffled(row[c], v.split[c], band, 1 - v.way.band[0]);
        }
        if (ears) {
            v.itd += itd_step;
            PushEar(v, row[0]);
            row[kLeftEar] = EarAt(v, v.itd);
            row[kRightEar] = EarAt(v, -v.itd);
        }
        v.pos += rate;
        got = i + 1;
    }
    if (got && (head || v.head)) {
        static std::vector<float> mono, left, right;
        mono.resize(frames);
        left.resize(frames);
        right.resize(frames);
        for (uint32_t i = 0; i < frames; i++) mono[i] = rows[size_t(i) * kRows];
        if (audio::acoustics::Ears(v.obj, 0, at.d, mono.data(), left.data(), right.data(), frames)) {
            // a block that turns the head on or off fades from one pair of ears to the other
            float mix = v.head ? 1.0f : 0.0f;
            float mix_step = ((head ? 1.0f : 0.0f) - mix) / float(frames);
            for (uint32_t i = 0; i < frames; i++) {
                float* row = &rows[size_t(i) * kRows];
                mix += mix_step;
                row[kLeftEar] += (left[i] - row[kLeftEar]) * mix;
                row[kRightEar] += (right[i] - row[kRightEar]) * mix;
            }
            got = frames;  // the filter rings on a little past a sound's last frame
        } else {
            head = false;
        }
    }
    if (got && detour) {
        // as loud as the sound would be that far off, next to how loud its levels already make it
        float d[3] = {way_from[0] - g_listener[0], way_from[1] - g_listener[1], way_from[2] - g_listener[2]};
        // (the ears' own level: its distance, and the share of it a sound ahead leaves to the centre speaker)
        float own = at.att * at.front;
        float level = way && own > 1e-6f ? Attenuation(v, std::sqrt(Dot(d, d))) / own : 0.0f;
        MixDetour(v.way, v.obj, way_from, way_gain, level, dry.data(), rows.data(), frames);
        got = frames;
    } else if (detour) {
        v.way = Detour{};  // the sound ended: its next play starts without the last one's way
    }
    // The room hears it too: what the walls left of it and what its way round brings, each as loud as it is heard.
    // (Sent louder than heard, at the root of its distance's level as a real room's even reverberation would have
    // it, a level's many far sounds raised the whole mix 8 dB, 10 Nov 2026)
    float send = ears && audio::acoustics::Room() ? OwnLevel(v) : 0.0f;
    if (got && (send != 0 || v.send != 0) && g_room_send.size() >= frames) {
        float direct = std::max(at.att, 0.0f), round = got == frames && detour && way ? at.att * at.front : 0.0f;
        float level = v.send, step = (send - v.send) / float(frames);
        for (uint32_t i = 0; i < got; i++) {
            level += step;
            g_room_send[i] += (rows[size_t(i) * kRows] * direct + (round != 0 ? g_way_mono[i] * round : 0.0f)) * level;
        }
    }
    v.send = got ? send : 0.0f;
    // a mono stream or loop with no place, while the game has far points for one: kept for them (MixFar), not played
    // flat (Training Ground's distant battle is a looped buffer, not a stream: 12 Nov 2026)
    bool far_off = g_far_n > 0 && (v.stream || v.looping) && v.channels == 1 && !at.on && !g_far_fed;
    if (far_off) {
        float own = OwnLevel(v);
        if (g_far_ring.size() != kFarRing) g_far_ring.assign(kFarRing, 0.0f);
        for (uint32_t i = 0; i < frames; i++)
            g_far_ring[(g_far_at + i) % kFarRing] = i < got ? rows[size_t(i) * kRows] * own : 0.0f;
        g_far_fed = true;
        got = 0;
    }
    for (uint32_t i = 0; i < got; i++) {
        const float* row = &rows[size_t(i) * kRows];
        float* frame = bins + size_t(i) * kBins;
        for (Lane& lane : lanes) {
            lane.gain += lane.step;
            frame[lane.bin] += row[lane.row] * lane.gain;
        }
    }
    v.head = head;
    v.gain = target;
    v.itd = itd;
    std::memcpy(v.band, walls, sizeof walls);
    v.walls = through;
}

// The acoustics test sound (audio/acoustics.h, PlaceTestSound): made here, a low buzz under bursts of hiss, so a wall's
// dulling is plain to hear. Panned by its direction, full within 3 m and halved with each doubling past, and heard
// through the walls as any 3D voice
struct TestVoice {
    uint32_t n = 0;           // frames made
    uint32_t noise = 0x9E3779B9u;
    float phase = 0;
    float split[4] = {};
    float band[3] = {1, 1, 1};
    float left = 0, right = 0;
    float head = 0;  // how much of it the head's filter places (headphones), ramped as its levels are
    Detour way;      // its way round the walls
};
TestVoice g_test;
constexpr uint32_t kTestSound = 0xFFFFFFFEu;  // acoustics.cpp's id for it

void MixTest(float* bins, uint32_t frames) {
    float pos[3];
    if (!audio::acoustics::TestSound(pos)) {
        g_test.left = g_test.right = 0;
        return;
    }
    float d[3] = {pos[0] - g_listener[0], pos[1] - g_listener[1], pos[2] - g_listener[2]};
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float att = std::min(1.0f, 3.0f / std::max(std::sqrt(Dot(d, d)), 0.01f));
    float pan = std::sin(Azimuth(d, g_front, right) * 3.14159265f / 180);  // -1 left, 1 right
    float left = att * std::cos((pan + 1) * 0.78539816f), rightg = att * std::sin((pan + 1) * 0.78539816f);
    float walls[3] = {1, 1, 1};
    audio::acoustics::Gains(kTestSound, pos, g_listener, walls);
    float band_step[3], left_step = (left - g_test.left) / float(frames), right_step = (rightg - g_test.right) / float(frames);
    for (int b = 0; b < 3; b++) band_step[b] = (walls[b] - g_test.band[b]) / float(frames);
    constexpr uint32_t kPeriod = audio::kRate * 6 / 10, kBurst = audio::kRate * 35 / 100, kFade = audio::kRate / 200;
    static std::vector<float> mono, ear[2], dry, both;
    mono.resize(frames);
    dry.resize(frames);
    for (uint32_t i = 0; i < frames; i++, g_test.n++) {
        uint32_t at = g_test.n % kPeriod;
        float env = at >= kBurst ? 0.0f : std::min({1.0f, float(at) / kFade, float(kBurst - at) / kFade});
        g_test.noise ^= g_test.noise << 13;
        g_test.noise ^= g_test.noise >> 17;
        g_test.noise ^= g_test.noise << 5;
        float hiss = float(int32_t(g_test.noise)) / 2147483648.0f;
        g_test.phase += 110.0f / audio::kRate;
        if (g_test.phase >= 1) g_test.phase -= 1;
        float x = 0.2f * hiss * env + 0.1f * (2 * g_test.phase - 1);
        for (int b = 0; b < 3; b++) g_test.band[b] += band_step[b];
        dry[i] = x;
        mono[i] = Muffled(x, g_test.split, g_test.band, 1 - g_test.way.band[0]);
    }
    // its way round the walls, as a voice's: made as two ears at the level of a sound that far off, added below
    float way_from[3] = {}, way_gain[3] = {0, 0, 0};
    bool way = audio::acoustics::Way(kTestSound, way_from, way_gain);
    bool detour = way || g_test.way.band[0] != 0 || g_test.way.band[1] != 0 || g_test.way.band[2] != 0;
    if (detour) {
        float wd[3] = {way_from[0] - g_listener[0], way_from[1] - g_listener[1], way_from[2] - g_listener[2]};
        float far = way ? 0.70710678f * std::min(1.0f, 3.0f / std::max(std::sqrt(Dot(wd, wd)), 0.01f)) : 0.0f;
        both.assign(size_t(frames) * kRows, 0.0f);
        MixDetour(g_test.way, kTestSound, way_from, way_gain, far, dry.data(), both.data(), frames);
    }
    bool room = audio::acoustics::Room() && g_room_send.size() >= frames;
    // headphones: from its direction through the head's filter, at the level a sound straight ahead is panned to
    bool head = audio::acoustics::Headphones() || g_test.head > 0;
    if (head) {
        ear[0].resize(frames);
        ear[1].resize(frames);
        float local[3] = {Dot(d, right), Dot(d, g_top), Dot(d, g_front)};
        head = audio::acoustics::Ears(kTestSound, 0, local, mono.data(), ear[0].data(), ear[1].data(), frames);
    }
    float head_step = ((head && audio::acoustics::Headphones() ? 1.0f : 0.0f) - g_test.head) / float(frames);
    float level = att * 0.70710678f;
    for (uint32_t i = 0; i < frames; i++) {
        g_test.left += left_step;
        g_test.right += right_step;
        g_test.head += head_step;
        float l = mono[i] * g_test.left, r = mono[i] * g_test.right;
        if (head) {
            l += (ear[0][i] * level - l) * g_test.head;
            r += (ear[1][i] * level - r) * g_test.head;
        }
        if (detour) {
            l += both[size_t(i) * kRows + kLeftEar];
            r += both[size_t(i) * kRows + kRightEar];
        }
        if (room) g_room_send[i] += mono[i] * level + (detour ? g_way_mono[i] : 0.0f);
        float* frame = bins + size_t(i) * kBins;
        frame[kFrontLeft] += l;
        frame[kFrontRight] += r;
    }
    if (!head) g_test.head = 0;
    std::memcpy(g_test.band, walls, sizeof walls);
}

// The far points' hearings of the stream kept for them, after every voice: panned by direction, as loud as each is
// near, dulled by its cover as the game gives it and sent to the room
void MixFar(float* bins, uint32_t frames) {
    if (g_far_ring.size() != kFarRing) return;
    if (!g_far_fed)  // no stream this block: what the later points have yet to hear plays out
        for (uint32_t i = 0; i < frames; i++) g_far_ring[(g_far_at + i) % kFarRing] = 0;
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    bool room = audio::acoustics::Room() && g_room_send.size() >= frames;
    for (int k = 0; k < audio::acoustics::kFar; k++) {
        FarPoint& f = g_far[k];
        float left = 0, rightg = 0, send = 0, walls[3] = {1, 1, 1};
        if (k < g_far_n) {
            const float* pos = g_far_pos[k];
            float d[3] = {pos[0] - g_listener[0], pos[1] - g_listener[1], pos[2] - g_listener[2]};
            float att = std::min(2.0f, g_far_reach / std::max(std::sqrt(Dot(d, d)), 1.0f)) / std::sqrt(float(g_far_n));
            float pan = std::sin(Azimuth(d, g_front, right) * 3.14159265f / 180);  // -1 left, 1 right
            left = att * std::cos((pan + 1) * 0.78539816f);
            rightg = att * std::sin((pan + 1) * 0.78539816f);
            send = room ? att * 0.70710678f : 0.0f;
            for (int b = 0; b < 3; b++) {
                float want = kFarCovered[b] + (1 - kFarCovered[b]) * g_far_open[k];
                walls[b] = f.band[b] + kFarFollow * (want - f.band[b]);
            }
        } else if (f.left == 0 && f.right == 0) {
            continue;
        }
        float left_step = (left - f.left) / float(frames), right_step = (rightg - f.right) / float(frames);
        float send_step = (send - f.send) / float(frames), band_step[3];
        for (int b = 0; b < 3; b++) band_step[b] = (walls[b] - f.band[b]) / float(frames);
        uint32_t from = g_far_at + kFarRing - uint32_t(k) * kFarLater;
        for (uint32_t i = 0; i < frames; i++) {
            for (int b = 0; b < 3; b++) f.band[b] += band_step[b];
            f.left += left_step;
            f.right += right_step;
            f.send += send_step;
            float x = Muffled(g_far_ring[(from + i) % kFarRing], f.split, f.band, 1);
            float* frame = bins + size_t(i) * kBins;
            frame[kFrontLeft] += x * f.left;
            frame[kFrontRight] += x * f.right;
            if (room) g_room_send[i] += x * f.send;
        }
        f.left = left;
        f.right = rightg;
        f.send = send;
        std::memcpy(f.band, walls, sizeof walls);
    }
    g_far_at = (g_far_at + frames) % kFarRing;
    g_far_fed = false;
}

void Render(float* out, uint32_t frames) {
    static std::vector<float> bins, answer[2];  // the output thread alone renders
    bins.assign(size_t(frames) * kBins, 0.0f);
    g_room_send.assign(frames, 0.0f);
    answer[0].assign(frames, 0.0f);
    answer[1].assign(frames, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!audio::acoustics::Hearing(g_listener, g_front, g_top)) {
        std::memcpy(g_listener, g_ds_listener, sizeof g_listener);
        std::memcpy(g_front, g_ds_front, sizeof g_front);
        std::memcpy(g_top, g_ds_top, sizeof g_top);
    }
    g_far_n = audio::acoustics::Far(g_far_pos, g_far_open, g_far_reach);
    for (auto& [obj, v] : g_voices) {
        if (v.paused || v.sync || (!v.stream && !v.playing)) continue;
        MixVoice(v, bins.data(), frames);
    }
    MixFar(bins.data(), frames);
    MixTest(bins.data(), frames);
    // XBE_ACOUSTICS_LOG: where in the chain the mix is, once a second (a nave fell silent with its room on, 11 Nov 2026)
    static const bool s_trace = [] {
        const char* v = std::getenv("XBE_ACOUSTICS_LOG");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    static uint32_t s_blocks = 0;
    static float s_peak[5] = {};  // voices, after the effects image, sent to the room, the room's answer, out
    static uint32_t s_voices = 0;
    auto peak = [&](int at, const float* x, size_t n, size_t stride) {
        for (size_t i = 0; i < n; i++) s_peak[at] = std::max(s_peak[at], std::fabs(x[i * stride]));
    };
    if (s_trace) {
        for (auto& [obj, v] : g_voices) s_voices += !v.paused && !v.sync && (v.stream || v.playing);
        peak(0, bins.data(), size_t(frames) * kBins, 1);  // every bin: 3D voices reach the speakers through the image
    }
    // the place's background (audio/beds.h): its single sounds are answered by the room too
    static std::vector<float> beds;
    beds.assign(size_t(frames) * 2, 0.0f);
    audio::beds::Mix(beds.data(), g_room_send.data(), frames);
    // the level's own rooms answer the 3D sounds in place of the title's reverb, added to the two speakers as heard
    dsp::StandDown(audio::acoustics::Room());
    if (dsp::Loaded()) dsp::Run(bins.data(), frames);
    static Duck duck;
    bool answered =
        audio::acoustics::RoomAnswer(g_room_send.data(), g_front, g_top, answer[0].data(), answer[1].data(), frames);
    if (answered) {
        AddAnswer(duck, bins.data(), answer[0].data(), answer[1].data(), out, frames);
    } else {
        duck = Duck{};
        for (uint32_t i = 0; i < frames; i++) {
            const float* b = &bins[size_t(i) * kBins];
            out[i * 2] = std::clamp(b[kFrontLeft] + kFold * (b[kCentre] + b[kBackLeft]), -1.0f, 1.0f);
            out[i * 2 + 1] = std::clamp(b[kFrontRight] + kFold * (b[kCentre] + b[kBackRight]), -1.0f, 1.0f);
        }
    }
    for (size_t i = 0; i < size_t(frames) * 2; i++) out[i] = std::clamp(out[i] + beds[i], -1.0f, 1.0f);
    if (s_trace) {
        peak(1, &bins[kFrontLeft], frames, kBins);
        peak(1, &bins[kFrontRight], frames, kBins);
        peak(2, g_room_send.data(), frames, 1);
        peak(3, answer[0].data(), frames, 1);
        peak(3, answer[1].data(), frames, 1);
        peak(4, out, size_t(frames) * 2, 1);
        if (++s_blocks >= 100) {
            auto db = [](float x) { return 20 * std::log10(std::max(x, 1e-10f)); };
            LOGR("dsound: the last second's peaks: voices %.1f dB (%u voices a block), after the effects image %.1f, sent "
                 "to the room %.1f, its answer %.1f (giving way to %.2f), out %.1f",
                 db(s_peak[0]), s_voices / s_blocks, db(s_peak[1]), db(s_peak[2]), db(s_peak[3]), duck.gain,
                 db(s_peak[4]));
            s_blocks = s_voices = 0;
            std::fill(std::begin(s_peak), std::end(s_peak), 0.0f);
        }
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

// the effect's state segment when offset and size lie inside it, else 0
uint32_t EffectState(uint32_t effect, uint32_t offset, uint32_t size) {
    if (effect >= g_effect_states.size()) return 0;
    const Segment& state = g_effect_states[effect];
    if (offset > state.bytes || size > state.bytes - offset) return 0;
    return state.at;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx), SYM(IDirectSoundBuffer_Pause), SYM(IDirectSoundBuffer_SetEG),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
// (this, effect, offset, data, size, flags): writes the effect's state
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(state + a[2]), HostPtr(a[3]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
// (this, mix bin, headroom)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < kBins) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// (this, DSI3DL2LISTENER, apply): room, room HF, reflections and reverb are integers
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    float p[12];
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(a[1] + i * 4);
        bool integer = i < 2 || i == 5 || i == 7;
        p[i] = integer ? float(int32_t(v)) : F32(v);
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    dsp::SetListener(p);
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_ds_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_ds_front[i] = F32(a[1 + i]);
        g_ds_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// The descriptor keeps the image's effect list so the title finds its indices. State segments hold the image's plain
// state, which the host effects read (xdk/dsp_image.cpp); code, Y memory and scratch point at zeroed memory.
// loc: DSEFFECTIMAGELOC, the I3DL2 reverb's index first
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name, uint32_t loc) {
    constexpr int kStateSegment = 1;
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    std::vector<Segment> states;
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            uint32_t at = u32(m + k * 8);
            bool inside = at <= img.size() && size <= img.size() - at;
            if (k == kStateSegment && inside) std::memcpy(HostPtr(seg), img.data() + at, size);
            if (k == kStateSegment) states.push_back({seg, size});
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    int32_t reverb = loc ? int32_t(R32(loc)) : -1;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_effect_states = states;
    std::vector<uint32_t> addresses;
    for (const Segment& s : states) addresses.push_back(s.at);
    dsp::Load(img, t, addresses, reverb);
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str(), a[1]);
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory", a[3]);
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): reads the effect's state
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(a[3]), HostPtr(state + a[2]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    audio::acoustics::Forget(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(Play, 4) {
    v.has_gain = false;  // a new sound does not ramp from the last one's levels
    v.body = audio::acoustics::RecordedBody(SampleKey(v));
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags): stops now, the release envelope is not reproduced
BUFFER(StopEx, 4) {
    v.playing = false;
    return 0;
}
// (this, flags): 0 resumes, 1 pauses, 2 holds until IDirectSound_SynchPlayback
BUFFER(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
// envelopes are not played
BUFFER(SetEG, 2) { return 0; }
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
BUFFER(SetOutputBuffer, 2) { return 0; }  // the voice keeps its own mix bins
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    ForgetBlocks(v);
    return 0;
}

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(a[1], obj);
    return 0;
}

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    ForgetBlocks(v);
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    audio::acoustics::Forget(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus, 2) {
    // DSSTREAMSTATUS: READY (XMEDIASTATUS_ACCEPT_INPUT) in bit 0, PLAYING 0x10000 and PAUSED 0x20000 above it.
    // TimeSplitters 2 lifts its story intro's fade once its music tests (status & 0x30000) == 0x10000
    uint32_t status = v.packets.size() < v.max_packets ? 1u : 0u;
    if (v.paused) status |= 0x20000;
    else if (!v.packets.empty()) status |= 0x10000;
    W32(a[1], status);
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    if (!v.active) v.has_gain = false;  // after going quiet a stream starts its levels fresh
    v.active = true;
    v.ending = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    FlushStream(v);
    return 0;
}
STREAM(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// the library's own writes voice registers a host stream does not have; envelopes are not played
HLE_STDCALL(IDirectSoundStream_SetEG, 2) { return 0; }
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }
