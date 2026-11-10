// Sound through the level (acoustics.h): Steam Audio's direct path, occlusion and transmission by material, run for
// every 3D sound the mixer is playing, on a thread of its own. Loaded from phonon.dll at run time, so a build or a
// player without it is unchanged.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "audio/acoustics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "../../third_party/steamaudio/include/phonon.h"
#include "core/log.h"

namespace audio::acoustics {

namespace {

// Steam Audio's entry points, read from the library
#define IPL_FUNCS(X)                                                                                              \
    X(iplContextCreate) X(iplContextRelease) X(iplEmbreeDeviceCreate) X(iplSceneCreate) X(iplSceneRelease)      \
    X(iplSceneCommit) X(iplStaticMeshCreate) X(iplStaticMeshRelease) X(iplStaticMeshAdd) X(iplSimulatorCreate)    \
    X(iplSimulatorSetScene) X(iplSimulatorCommit) X(iplSimulatorSetSharedInputs) X(iplSimulatorRunDirect)        \
    X(iplSourceCreate) X(iplSourceRelease) X(iplSourceAdd) X(iplSourceRemove) X(iplSourceSetInputs)               \
    X(iplSourceGetOutputs)
struct Api {
#define IPL_POINTER(name) decltype(&::name) name = nullptr;
    IPL_FUNCS(IPL_POINTER)
#undef IPL_POINTER
};
Api g_ipl;

static_assert(sizeof(Material) == sizeof(IPLMaterial), "Material is IPLMaterial");

constexpr auto kStep = std::chrono::milliseconds(15);  // a run of the rays, about once a frame
constexpr int64_t kStaleMs = 1000;                     // a sound not mixed for this long is let go
constexpr float kNear = 2.0f;                          // metres: a sound this close to the listener is never blocked,
                                                       // the listener's own steps and shots among them
// metres: a sound is a ball, partly hidden behind an edge, so a sound by a doorway or round a corner fades in rather
// than switching, the nearest thing to sound bending round an edge before paths through doorways (raised from 0.3 on
// 10 Nov 2026: a player heard the muffling come on too hard). Keep it under the game's raise plus a little, or the
// floor hides part of every sound
constexpr float kRadius = 0.6f;
constexpr int kSamples = 32;                           // points in that ball
constexpr int kSurfaces = 3;                           // walls between counted for transmission
// the least of each band a blocked sound keeps (low, middle, high): a sound behind a thick wall stays a dull sound
// rather than none, so a game's warnings and footsteps still carry. Tuned by ear: 0.25 0.12 0.06 (-12, -18 and -24 dB)
// was too strong in play (10 Nov 2026); now -6, -10.5 and -16.5 dB
constexpr float kFloor[3] = {0.5f, 0.3f, 0.15f};
constexpr float kFollow = 0.3f;  // each mix block moves this much of the way to the latest result, about 30 ms

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Sound {
    float pos[3] = {};  // in the level's space
    float raw[3] = {};  // as the mixer gave it
    bool placed = false;
    int64_t seen = 0;
    float target[3] = {1, 1, 1};  // the rays' last result
    float heard[3] = {1, 1, 1};   // what the mixer was given, following target
};

std::mutex g_mutex;  // the members below
std::condition_variable g_wake;
std::unordered_map<uint32_t, Sound> g_sounds;
std::optional<Scene> g_pending;  // a level for the thread to build
bool g_scene = false;            // a level is built and in use
float g_meters = 1;
float g_raise = 0;  // the level's Scene::raise
bool g_started = false;
float g_listener[3] = {};  // in the level's space, as the mixer last gave it
float g_origin[3] = {}, g_axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the sounds' space in the level (SetSoundSpace)
bool g_test = false;   // a test sound is placed (PlaceTestSound)
float g_test_at[3] = {};  // where, in the level, before the level's raise
constexpr uint32_t kTestSound = 0xFFFFFFFEu;  // its id among the sounds

void ToLevel(const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = g_origin[i] + in[0] * g_axes[i] + in[1] * g_axes[3 + i] + in[2] * g_axes[6 + i];
}

std::atomic<bool> g_on{false};
std::atomic<float> g_strength{1.0f};
std::atomic<int> g_state{0};  // 0 not tried, 1 loaded, -1 unavailable
int64_t g_alternate = [] {  // XBE_ACOUSTICS_ALTERNATE: milliseconds on, then as many off
    const char* v = std::getenv("XBE_ACOUSTICS_ALTERNATE");
    return v ? int64_t(std::atof(v) * 1000) : 0;
}();
std::atomic<int64_t> g_alternate_set{-1};  // Alternate's, when it has been called
// XBE_ACOUSTICS_LOG=1: where the listener is, the sounds' space and how many sounds are blocked, every five seconds; a
// number above 1 is the milliseconds between
int64_t g_log = [] {
    const char* v = std::getenv("XBE_ACOUSTICS_LOG");
    int64_t ms = v ? std::atoll(v) : 0;
    return ms <= 0 ? 0 : ms == 1 ? 5000 : ms;
}();

bool Env(const char* name, bool& value) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    value = !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0);
    return true;
}

bool LoadPhonon() {
#ifdef _WIN32
    HMODULE lib = ::LoadLibraryW(L"phonon.dll");
    auto find = [lib](const char* name) { return reinterpret_cast<void*>(GetProcAddress(lib, name)); };
#else
    void* lib = dlopen("libphonon.so", RTLD_NOW | RTLD_LOCAL);
    auto find = [lib](const char* name) { return dlsym(lib, name); };
#endif
    if (!lib) {
        LOGR("acoustics: Steam Audio (phonon) is not beside the executable, sounds play as before");
        return false;
    }
#define IPL_FIND(name)                                                        \
    g_ipl.name = reinterpret_cast<decltype(g_ipl.name)>(find(#name));         \
    if (!g_ipl.name) {                                                        \
        LOGR("acoustics: Steam Audio has no %s, sounds play as before", #name); \
        return false;                                                         \
    }
    IPL_FUNCS(IPL_FIND)
#undef IPL_FIND
    return true;
}

// what the thread owns
struct Simulation {
    IPLContext context = nullptr;
    IPLEmbreeDevice embree = nullptr;
    IPLSceneType type = IPL_SCENETYPE_DEFAULT;
    IPLSimulator simulator = nullptr;
    IPLScene scene = nullptr;
    IPLStaticMesh mesh = nullptr;
    std::unordered_map<uint32_t, IPLSource> sources;
};

void IPLCALL Log(IPLLogLevel level, const char* message) {
    if (level == IPL_LOGLEVEL_ERROR || level == IPL_LOGLEVEL_WARNING) LOGR("acoustics: Steam Audio: %s", message);
}

bool Open(Simulation& s) {
    if (!LoadPhonon()) return false;
    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = Log;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (g_ipl.iplContextCreate(&cs, &s.context) != IPL_STATUS_SUCCESS) {
        LOGR("acoustics: Steam Audio did not start, sounds play as before");
        return false;
    }
    IPLEmbreeDeviceSettings es{};
    if (g_ipl.iplEmbreeDeviceCreate(s.context, &es, &s.embree) == IPL_STATUS_SUCCESS) s.type = IPL_SCENETYPE_EMBREE;
    IPLSimulationSettings ss{};
    ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
    ss.sceneType = s.type;
    ss.maxNumOcclusionSamples = kSamples;
    ss.samplingRate = 48000;
    ss.frameSize = 480;
    if (g_ipl.iplSimulatorCreate(s.context, &ss, &s.simulator) != IPL_STATUS_SUCCESS) {
        LOGR("acoustics: Steam Audio's simulator did not start, sounds play as before");
        return false;
    }
    LOGR("acoustics: Steam Audio %u.%u.%u, %s rays", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR,
         STEAMAUDIO_VERSION_PATCH, s.type == IPL_SCENETYPE_EMBREE ? "Embree" : "built-in");
    return true;
}

// the level built and handed to the simulator, the one before let go; false for an empty level
bool Build(Simulation& s, const Scene& level) {
    IPLScene scene = nullptr;
    IPLStaticMesh mesh = nullptr;
    size_t tris = level.triangles.size() / 3;
    if (tris && !level.materials.empty()) {
        IPLSceneSettings ss{};
        ss.type = s.type;
        ss.embreeDevice = s.embree;
        std::vector<IPLVector3> vertices(level.vertices.size() / 3);
        for (size_t i = 0; i < vertices.size(); i++)
            vertices[i] = {level.vertices[i * 3] * level.meters, level.vertices[i * 3 + 1] * level.meters,
                           level.vertices[i * 3 + 2] * level.meters};
        std::vector<IPLTriangle> triangles(tris);
        std::vector<IPLint32> material(tris);
        for (size_t t = 0; t < tris; t++) {
            for (int k = 0; k < 3; k++) triangles[t].indices[k] = IPLint32(level.triangles[t * 3 + k]);
            material[t] = t < level.material.size() ? IPLint32(level.material[t]) : 0;
            if (material[t] < 0 || size_t(material[t]) >= level.materials.size()) material[t] = 0;
        }
        std::vector<IPLMaterial> materials(level.materials.size());
        std::memcpy(materials.data(), level.materials.data(), materials.size() * sizeof(IPLMaterial));
        IPLStaticMeshSettings ms{};
        ms.numVertices = IPLint32(vertices.size());
        ms.numTriangles = IPLint32(tris);
        ms.numMaterials = IPLint32(materials.size());
        ms.vertices = vertices.data();
        ms.triangles = triangles.data();
        ms.materialIndices = material.data();
        ms.materials = materials.data();
        if (g_ipl.iplSceneCreate(s.context, &ss, &scene) != IPL_STATUS_SUCCESS ||
            g_ipl.iplStaticMeshCreate(scene, &ms, &mesh) != IPL_STATUS_SUCCESS) {
            LOGR("acoustics: %s: Steam Audio could not take the level, sounds play as before", level.name);
            if (scene) g_ipl.iplSceneRelease(&scene);
            scene = nullptr;
        } else {
            g_ipl.iplStaticMeshAdd(mesh, scene);
            g_ipl.iplSceneCommit(scene);
        }
    }
    g_ipl.iplSimulatorSetScene(s.simulator, scene);
    g_ipl.iplSimulatorCommit(s.simulator);
    if (s.mesh) g_ipl.iplStaticMeshRelease(&s.mesh);
    if (s.scene) g_ipl.iplSceneRelease(&s.scene);
    s.mesh = mesh;
    s.scene = scene;
    if (scene)
        LOGR("acoustics: %s: %zu triangles, %zu materials", level.name, tris, level.materials.size());
    return scene != nullptr;
}

IPLCoordinateSpace3 At(const float p[3], float meters) {
    IPLCoordinateSpace3 c{};
    c.right = {1, 0, 0};
    c.up = {0, 1, 0};
    c.ahead = {0, 0, -1};
    c.origin = {p[0] * meters, p[1] * meters, p[2] * meters};
    return c;
}

struct Job {
    uint32_t id;
    float pos[3];
    bool beside;
};

void Run() {
    Simulation s;
    if (!Open(s)) {
        g_state = -1;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.reset();
        return;
    }
    g_state = 1;
    float listener[3] = {};
    std::vector<Job> jobs;
    int64_t said = NowMs();
    for (;;) {
        std::optional<Scene> level;
        float meters;
        jobs.clear();
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait_for(lock, kStep);
            level.swap(g_pending);
            meters = g_meters;
            int64_t now = NowMs();
            std::memcpy(listener, g_listener, sizeof listener);
            for (auto it = g_sounds.begin(); it != g_sounds.end();) {
                if (now - it->second.seen > kStaleMs) {
                    it = g_sounds.erase(it);
                    continue;
                }
                const Sound& sound = it->second;
                float d[3] = {sound.pos[0] - listener[0], sound.pos[1] - listener[1], sound.pos[2] - listener[2]};
                bool beside = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * meters < kNear;
                jobs.push_back({it->first, {sound.pos[0], sound.pos[1], sound.pos[2]}, beside});
                ++it;
            }
        }
        if (level) {
            bool built = Build(s, *level);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_scene = built;
            g_meters = meters = level->meters;
            g_raise = level->raise;
            for (auto& [id, sound] : g_sounds) std::fill(sound.target, sound.target + 3, 1.0f);
        }
        // sources follow the sounds playing
        bool changed = false;
        for (auto it = s.sources.begin(); it != s.sources.end();) {
            bool playing = std::any_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.id == it->first; });
            if (playing) {
                ++it;
                continue;
            }
            g_ipl.iplSourceRemove(it->second, s.simulator);
            g_ipl.iplSourceRelease(&it->second);
            it = s.sources.erase(it);
            changed = true;
        }
        if (!s.scene || !g_on) {
            if (changed) g_ipl.iplSimulatorCommit(s.simulator);
            continue;
        }
        for (const Job& j : jobs) {
            if (j.beside || s.sources.count(j.id)) continue;
            IPLSourceSettings ss{};
            ss.flags = IPL_SIMULATIONFLAGS_DIRECT;
            IPLSource source = nullptr;
            if (g_ipl.iplSourceCreate(s.simulator, &ss, &source) != IPL_STATUS_SUCCESS) continue;
            g_ipl.iplSourceAdd(source, s.simulator);
            s.sources[j.id] = source;
            changed = true;
        }
        if (changed) g_ipl.iplSimulatorCommit(s.simulator);
        IPLSimulationSharedInputs shared{};
        shared.listener = At(listener, meters);
        g_ipl.iplSimulatorSetSharedInputs(s.simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
        for (const Job& j : jobs) {
            auto it = s.sources.find(j.id);
            if (j.beside || it == s.sources.end()) continue;
            IPLSimulationInputs in{};
            in.flags = IPL_SIMULATIONFLAGS_DIRECT;
            in.directFlags =
                IPLDirectSimulationFlags(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
            in.source = At(j.pos, meters);
            in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            in.occlusionRadius = kRadius;
            in.numOcclusionSamples = kSamples;
            in.numTransmissionRays = kSurfaces;
            g_ipl.iplSourceSetInputs(it->second, IPL_SIMULATIONFLAGS_DIRECT, &in);
        }
        g_ipl.iplSimulatorRunDirect(s.simulator);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Job& j : jobs) {
            auto sound = g_sounds.find(j.id);
            if (sound == g_sounds.end()) continue;
            float* target = sound->second.target;
            auto it = s.sources.find(j.id);
            if (j.beside || it == s.sources.end()) {
                std::fill(target, target + 3, 1.0f);
                continue;
            }
            IPLSimulationOutputs out{};
            g_ipl.iplSourceGetOutputs(it->second, IPL_SIMULATIONFLAGS_DIRECT, &out);
            float open = std::clamp(out.direct.occlusion, 0.0f, 1.0f);
            for (int b = 0; b < 3; b++) target[b] = open + (1 - open) * std::clamp(out.direct.transmission[b], 0.0f, 1.0f);
        }
        if (g_log && NowMs() - said >= g_log) {
            said = NowMs();
            uint32_t blocked = 0, n = 0;
            for (const auto& [id, sound] : g_sounds) blocked += sound.target[1] < 0.5f;
            LOGR("acoustics: listener %.2f %.2f %.2f, %zu sounds, %u behind walls; space at %.2f %.2f %.2f, axes %.3f %.3f "
                 "%.3f  %.3f %.3f %.3f  %.3f %.3f %.3f",
                 listener[0], listener[1], listener[2], g_sounds.size(), blocked, g_origin[0], g_origin[1], g_origin[2],
                 g_axes[0], g_axes[1], g_axes[2], g_axes[3], g_axes[4], g_axes[5], g_axes[6], g_axes[7], g_axes[8]);
            for (const auto& [id, sound] : g_sounds)
                if (n++ < 8)
                    LOGR("acoustics: sound %08X given %.2f %.2f %.2f, in the level %.2f %.2f %.2f, mid %.2f", id,
                         sound.raw[0], sound.raw[1], sound.raw[2], sound.pos[0], sound.pos[1], sound.pos[2],
                         sound.target[1]);
        }
    }
}

// on the first need: the thread, which loads the library
void Start() {
    if (g_started) return;
    g_started = true;
    std::thread(Run).detach();
}

}  // namespace

void SetScene(Scene scene) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state < 0) return;
    g_pending = std::move(scene);
    if (g_on) Start();
    g_wake.notify_one();
}

void SetSoundSpace(const float origin[3], const float axes[9]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(g_origin, origin, sizeof g_origin);
    std::memcpy(g_axes, axes, sizeof g_axes);
}

void Configure(bool on, float strength) {
    Env("XBE_ACOUSTICS", on);
    g_strength = std::clamp(strength, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on != g_on) LOGR("acoustics: %s", on ? "on" : "off");
    g_on = on;
    if (on && g_pending) Start();
}

bool On() { return g_on; }
bool Available() { return g_state >= 0; }

// on now: XBE_ACOUSTICS_ALTERNATE's half of its period, said when it turns
bool Heard(int64_t now) {
    if (int64_t set = g_alternate_set; set >= 0) g_alternate = set;
    if (g_alternate <= 0) return true;
    static int s_half = -1;
    int half = int(now / g_alternate % 2);
    if (half != s_half) LOGR("acoustics: %s (alternating)", half ? "off" : "on");
    s_half = half;
    return half == 0;
}

bool Gains(uint32_t id, const float pos[3], const float listener[3], float out[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);  // every sound is kept track of, on or off, for Sources
    Sound& sound = g_sounds[id];
    // placed when it moves (a hundredth of a unit), then left where it was put
    if (!sound.placed || std::fabs(pos[0] - sound.raw[0]) + std::fabs(pos[1] - sound.raw[1]) +
                                 std::fabs(pos[2] - sound.raw[2]) > 0.01f) {
        std::memcpy(sound.raw, pos, sizeof sound.raw);
        ToLevel(pos, sound.pos);
        sound.pos[1] += g_raise;
        sound.placed = true;
    }
    ToLevel(listener, g_listener);
    sound.seen = NowMs();
    if (!g_on || g_state < 0 || !g_scene) {
        std::fill(sound.heard, sound.heard + 3, 1.0f);  // switched back on, it eases in from whole
        return false;
    }
    // in the off half of an alternation the sound follows back to whole, as it follows the rays: the change is ramped,
    // and what it was given (Now()'s test sound included) is what it is heard at, not the last muffled value
    float strength = Heard(sound.seen) ? float(g_strength) : 0.0f;
    for (int b = 0; b < 3; b++) {
        float kept = kFloor[b] + (1 - kFloor[b]) * sound.target[b];
        float want = 1 - strength * (1 - kept);
        sound.heard[b] += (want - sound.heard[b]) * kFollow;
        out[b] = sound.heard[b];
    }
    return true;
}

void Forget(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sounds.erase(id);
}

void PlaceTestSound() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < 3; i++) g_test_at[i] = g_origin[i];
    g_test_at[1] -= g_raise;  // Gains raises it again: the sound sits at the eye
    g_test = true;
    g_sounds.erase(kTestSound);
    LOGR("acoustics: test sound at %.2f %.2f %.2f", g_test_at[0], g_test_at[1] + g_raise, g_test_at[2]);
}

void RemoveTestSound() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_test = false;
    g_sounds.erase(kTestSound);
}

bool TestSound(float pos[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_test) return false;
    // the level's point taken back into the sounds' space: its axes are at right angles, so the turn is their transpose
    float d[3] = {g_test_at[0] - g_origin[0], g_test_at[1] - g_origin[1], g_test_at[2] - g_origin[2]};
    for (int i = 0; i < 3; i++) pos[i] = d[0] * g_axes[i * 3] + d[1] * g_axes[i * 3 + 1] + d[2] * g_axes[i * 3 + 2];
    return true;
}

void Alternate(float seconds) {
    g_alternate_set = int64_t(std::max(seconds, 0.0f) * 1000);
    LOGR("acoustics: alternating every %.1f s", seconds);
}

float Alternating() {
    int64_t set = g_alternate_set;
    return float(set >= 0 ? set : g_alternate) / 1000.0f;
}

Status Now() {
    Status st;
    st.loaded = g_state > 0;
    st.on = g_on;
    std::lock_guard<std::mutex> lock(g_mutex);
    st.level = g_scene;
    st.test_placed = g_test;
    st.heard = st.on && Heard(NowMs());
    int64_t now = NowMs();
    for (const auto& [id, sound] : g_sounds) {
        if (now - sound.seen > 200) continue;
        if (id == kTestSound) {
            st.test = sound.heard[1];
            continue;
        }
        st.sounds++;
        st.blocked += sound.target[1] < 0.5f;
    }
    return st;
}

std::vector<Source> Sources() {
    std::vector<Source> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    int64_t now = NowMs();
    bool on = g_on && g_state >= 0 && g_scene;
    for (auto it = g_sounds.begin(); it != g_sounds.end();) {
        const Sound& sound = it->second;
        if (now - sound.seen > kStaleMs && !g_started) {  // the thread lets go of stale sounds; until it runs, here
            it = g_sounds.erase(it);
            continue;
        }
        if (now - sound.seen <= 200) {
            Source src{};
            std::memcpy(src.pos, sound.pos, sizeof src.pos);
            float d[3] = {sound.pos[0] - g_listener[0], sound.pos[1] - g_listener[1], sound.pos[2] - g_listener[2]};
            src.beside = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * g_meters < kNear;
            src.through = on ? sound.heard[1] : 1.0f;
            src.test = it->first == kTestSound;
            out.push_back(src);
        }
        ++it;
    }
    return out;
}

}  // namespace audio::acoustics
