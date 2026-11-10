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
    X(iplSourceGetOutputs) X(iplHRTFCreate) X(iplBinauralEffectCreate) X(iplBinauralEffectRelease)                \
    X(iplBinauralEffectReset) X(iplBinauralEffectApply) X(iplSceneRetain) X(iplProbeBatchCreate)                 \
    X(iplProbeBatchRelease) X(iplProbeBatchAddProbe) X(iplProbeBatchCommit) X(iplPathBakerBake)                  \
    X(iplSimulatorAddProbeBatch) X(iplSimulatorRemoveProbeBatch) X(iplSimulatorRunPathing)                       \
    X(iplSimulatorRunReflections) X(iplReflectionEffectCreate) X(iplReflectionEffectApply)                       \
    X(iplReflectionEffectReset) X(iplAmbisonicsDecodeEffectCreate) X(iplAmbisonicsDecodeEffectApply)             \
    X(iplAmbisonicsDecodeEffectReset)
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
// Steam Audio's transmission is a wall's own (a wood wall passes 1.4% of the middle band, concrete 0.2%), and all of
// it lies under kFloor: a listening check found a wood wall and a concrete one heard alike (31% and 30%, 10 Nov 2026).
// Raised to this power a thin wall stands clear of a thick one: wood 18%, glass 29%, concrete 8% over the floor
constexpr float kThin = 0.4f;
// The ways round (Way). Points 1.5 m over every floor, this far apart; fewer than kMaxProbes, or they are spread wider
constexpr float kProbeSpacing = 1.5f;
constexpr uint32_t kProbeOpen = 3;   // under the open sky, this many times as far apart, or more:
constexpr float kProbeOpenFar = 12.0f;  // up to this, where the points along a diagonal still join (kVisRange)
constexpr float kProbeHeight = 1.5f;
constexpr float kProbeClear = 0.3f;  // metres of room a point needs over it
// The work of joining them, and the memory it takes while it runs, grow with the square of their number: 5,800 points
// took 4.6 GB for 9 s on a PC (10 Nov 2026), 2,000 take about 0.6 GB for one
constexpr size_t kMaxProbes = 2000;
// two points are joined when half of 4 x 4 rays between balls of half a metre round them pass (a doorway 1.2 m wide
// joins the points either side of it), up to 20 m apart; a way is at most 100 m long
constexpr int kVisSamples = 4;
constexpr float kVisRadius = 0.5f;
constexpr float kVisThreshold = 0.5f;
constexpr float kVisRange = 20.0f;
constexpr float kPathRange = 100.0f;
constexpr float kWayFollow = 0.15f;  // a way's place and level follow the latest result more slowly: ways switch
// a sound with a way round keeps this much of kFloor through the wall itself: the way carries it now, and it should be
// heard from the doorway, not from behind the wall
constexpr float kWayFloor = 0.3f;

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
    // its way round (Way): where it reaches the listener from, in the level, as far off as it is long, and how much of
    // each band it brings; then what the mixer was given, following those
    bool way = false;
    float way_from[3] = {};
    float way_gain[3] = {};
    bool way_placed = false;
    float way_at[3] = {};
    float way_heard[3] = {};
    float detour = 0;  // 1 while it has a way round, following: kFloor gives way to kWayFloor of it
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
uint32_t g_probes = 0;  // points the level's ways run between, 0 until they are worked out
bool g_test = false;   // a test sound is placed (PlaceTestSound)
float g_test_at[3] = {};  // where, in the level, before the level's raise
constexpr uint32_t kTestSound = 0xFFFFFFFEu;  // its id among the sounds

void ToLevel(const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = g_origin[i] + in[0] * g_axes[i] + in[1] * g_axes[3 + i] + in[2] * g_axes[6 + i];
}

// Headphones (Ears). The head is Steam Audio's own, made by the thread when headphones are first asked for; each sound's
// filter is made, run and let go under g_ears_mutex, by the mixer's thread but for Forget
constexpr uint32_t kFrame = 480;        // frames in a block: 10 ms, what the output asks for
constexpr float kHeadNear = 0.5f;       // metres: nearer than this a sound moves into the head
constexpr int64_t kEarRestMs = 60;      // a filter not run for this long starts clean: the sound began again
constexpr int64_t kEarStaleMs = 2000;   // and one not run for this long is let go
std::atomic<bool> g_headphones{false};
std::atomic<IPLContext> g_context{nullptr};
std::atomic<IPLHRTF> g_hrtf{nullptr};
std::atomic<float> g_scale{1.0f};  // metres in one unit, as g_meters, for the mixer's thread
struct Ear {
    IPLBinauralEffect effect = nullptr;
    int64_t used = 0;
};
std::mutex g_ears_mutex;
std::unordered_map<uint64_t, Ear> g_ears;  // by sound and image
int64_t g_ears_swept = 0;

// The room (RoomAnswer). 4096 rays of 32 bounces from the listener, for two seconds of sound: the first 200 ms, the early
// echoes, are kept as traced (first order Ambisonics), the rest is a reverb of the length the trace measured (Steam
// Audio's hybrid), so the two can be set apart: the reverb's level is the hybrid's tail EQ
constexpr int kRoomRays = 4096;
constexpr int kRoomBounces = 32;
constexpr int kRoomDiffuse = 32;
constexpr float kRoomSeconds = 2.0f;
// traced: Steam Audio's reverb, which follows, is fitted to the traced part's end and came out 30 dB short after 100 ms
constexpr float kRoomTraced = 0.2f;
constexpr int kRoomIr = int(kRoomSeconds * 48000);
// between traces: four a second follow a listener at a run (a room's echo changes over metres, not steps), and at 100
// the traces took a third of a processor on a PC (12 ms each on four threads, 10 Nov 2026)
constexpr int64_t kRoomRestMs = 250;
// How loud the room answers. Traced as it is, a room's answer to a sound made at the listener has the room's size in
// it and the floor under the listener's feet: a bare concrete room of 300 m3 answers 9 dB over the sound itself, most
// of it the floor's and ceiling's first echo, and a hall of 45,000 m3 1 dB over, 94% of it the floor. A game's mix has
// no room for the first, and a step's echo in the second was lost under its music (both heard in play, 10 Nov 2026).
// So: surfaces within kRoomNear of the listener give no first echo (Steam Audio's irradianceMinDistance, which drops
// them: measured), since the sounds answered are not at the listener's feet. And each trace is measured, an impulse
// run through it for a second, and its two parts fitted apart. The reverberation, later than 80 ms, is brought to
// kRoomWet of the sound for a room that rings kRoomRing seconds, and more or less as it rings longer or shorter,
// whatever its size, as a game's reverb presets are (their level is how loud a reverb starts, so its energy goes with
// its length). The early echoes are raised by no more than kRoomEarlyRaise and to no more than kRoomEarly of the
// sound: a carpeted room, nearly all early echo, stays dry. One gain for both, tried first, let a nave's
// reverberation swing 10 dB from one place in it to the next by how near its walls were (10 Nov 2026). Neither part
// is raised under the open sky (kRoomSky): a few faint echoes off a hut and distant cliffs came up 10 to 15 dB to
// stand in for a reverberation there is not, and how much of the answer comes late does not tell the open air from a
// nave (11% to 25% outdoors, 27% to 90% in a nave, 13% in a crypt)
constexpr float kRoomNear = 3.0f;    // metres
constexpr float kRoomWet = -4.0f;    // dB at level 1
constexpr float kRoomRing = 1.5f;    // seconds
constexpr float kRoomEarly = -3.0f;  // dB at level 1
constexpr float kRoomEarlyRaise = 12.0f;  // dB
constexpr float kRoomFitMin = -24.0f, kRoomFitMax = 30.0f;
constexpr float kRoomRingMin = 0.3f, kRoomRingMax = 6.0f;  // seconds a room is taken to ring, for the fit
// Steam Audio's first order binaural decode adds its virtual speakers' heads up alike at low frequencies: next to the
// speaker decode a room came out 11 dB louder under 250 Hz, 4 dB at 1 kHz and 5 dB quieter at 2 kHz (measured, 10 Nov
// 2026), and a footstep's echo in a nave was heard as a thump under 1 kHz and no tail. A shelf takes the lows back down
constexpr float kRoomHeadLow = -9.0f;    // dB, under the shelf
constexpr float kRoomHeadShelf = 700.0f; // Hz, its middle
// How open the place is: rays from the listener toward kRoomSky directions over and round it (the zenith, and rings
// of 8 at 15 and 50 degrees up), that meet nothing within kRoomSkyFar. Open at kRoomOpenNone of them no longer has its
// echo raised at all, and at kRoomOpenAll and under, all of it. A nave or a crypt is closed in every direction
constexpr int kRoomSky = 17;
constexpr float kRoomSkyFar = 50.0f;  // metres
constexpr float kRoomOpenAll = 0.12f, kRoomOpenNone = 0.4f;
constexpr int kRoomFitBlocks = 100;  // of 10 ms
constexpr int kRoomFitEarly = 8;
constexpr int64_t kRoomFitMs = 2000; // measured again this often, or when the listener has moved kRoomFitMove metres
constexpr float kRoomFitMove = 1.5f;
constexpr float kRoomSpeakers = 0.47f;  // Steam Audio's speaker decode of a room is 6.6 dB over its head's (measured)
std::atomic<bool> g_room{false};
std::atomic<float> g_room_level{0.5f};
std::atomic<bool> g_room_traced{false};  // the level in use has a trace
std::atomic<float> g_room_ms{0};
std::atomic<float> g_room_fit{0};   // the gain that fits the last trace measured, at level 1: its early echoes
std::atomic<float> g_room_tail{1};  // and its reverberation's, next to that
std::atomic<IPLSource> g_room_source{nullptr};  // the listener, in the room's own simulator; made once, never let go
std::atomic<IPLReflectionEffect> g_room_effect{nullptr};
std::atomic<IPLAmbisonicsDecodeEffect> g_room_decode{nullptr};
std::condition_variable g_room_wake;
std::optional<IPLScene> g_room_scene;  // under g_mutex: a level for the room's thread, its own hold on it; null, none

std::atomic<bool> g_paths{[] {
    const char* v = std::getenv("XBE_ACOUSTICS_PATHS");
    return !(v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0));
}()};
const size_t g_max_probes = [] {  // XBE_ACOUSTICS_POINTS=<n>: another most, to weigh the ways against the memory
    const char* v = std::getenv("XBE_ACOUSTICS_POINTS");
    long n = v ? std::atol(v) : 0;
    return n >= 100 && n <= 6000 ? size_t(n) : kMaxProbes;
}();
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
    bool head_tried = false;
    IPLProbeBatch batch = nullptr;  // the level's ways, once worked out
    uint64_t level = 0;             // counts the levels built
};

// a level's ways worked out, for the thread to take up if the level is still the one
struct Baked {
    IPLProbeBatch batch;
    uint64_t level;
    uint32_t probes;
};
std::optional<Baked> g_baked;  // under g_mutex

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
    ss.flags = IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING);
    ss.sceneType = s.type;
    ss.maxNumOcclusionSamples = kSamples;
    ss.maxOrder = 1;
    ss.numVisSamples = kVisSamples;
    ss.samplingRate = 48000;
    ss.frameSize = 480;
    if (g_ipl.iplSimulatorCreate(s.context, &ss, &s.simulator) != IPL_STATUS_SUCCESS) {
        LOGR("acoustics: Steam Audio's simulator did not start, sounds play as before");
        return false;
    }
    LOGR("acoustics: Steam Audio %u.%u.%u, %s rays", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR,
         STEAMAUDIO_VERSION_PATCH, s.type == IPL_SCENETYPE_EMBREE ? "Embree" : "built-in");
    g_context = s.context;
    return true;
}

// the head for headphones, Steam Audio's own, once
void MakeHead(Simulation& s) {
    if (s.head_tried) return;
    s.head_tried = true;
    IPLAudioSettings as{48000, IPLint32(kFrame)};
    IPLHRTFSettings hs{};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_NONE;
    IPLHRTF hrtf = nullptr;
    if (g_ipl.iplHRTFCreate(s.context, &as, &hs, &hrtf) != IPL_STATUS_SUCCESS) {
        LOGR("acoustics: Steam Audio's head did not load, headphones sound as before");
        return;
    }
    g_hrtf = hrtf;
    LOGR("acoustics: Steam Audio's head is ready, for headphones and the room");
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
    if (s.batch) {  // the last level's ways go with it
        g_ipl.iplSimulatorRemoveProbeBatch(s.simulator, s.batch);
        g_ipl.iplProbeBatchRelease(&s.batch);
    }
    s.level++;
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

// Points for the level's ways: on a grid, kProbeHeight over every surface that lies flat enough to stand on and has
// room above it, in metres. A level's floors and ceilings both lie flat, and which side of a triangle is up is not
// known, so a ceiling with nothing over it gets points too: they join nothing inside and cost only their share of the
// work. Where nothing lies over a point, the open air or the top of a roof, the points are kProbeOpen times as far
// apart: the ways that matter turn at doorways and corridors, and there the points have to stand close enough for one
// either side of a doorway (at 7.5 m apart a corner in Siberia's dam lost its way, 10 Nov 2026). So the open air's are
// spread first, to `wide`, and then `spacing`, of the points under a roof, is widened until there are no more than
// kMaxProbes; `roofed` is how many of them are
std::vector<IPLSphere> Probes(const Scene& level, float& spacing, float& wide, size_t& roofed) {
    std::vector<IPLSphere> probes;
    size_t tris = level.triangles.size() / 3, points = level.vertices.size() / 3;
    if (!tris || !points) return probes;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < points; i++)
        for (int k = 0; k < 3; k++) {
            float v = level.vertices[i * 3 + k] * level.meters;
            if (!std::isfinite(v)) return probes;
            lo[k] = std::min(lo[k], v);
            hi[k] = std::max(hi[k], v);
        }
    struct Hit {
        uint32_t column;
        float y;
        bool flat;
    };
    std::vector<Hit> hits;
    for (spacing = kProbeSpacing;;) {
        double across = std::ceil((hi[0] - lo[0]) / spacing), deep = std::ceil((hi[2] - lo[2]) / spacing);
        if (across * deep > 4e6) {  // a level too wide for this grid
            spacing *= 1.3f;
            continue;
        }
        uint32_t nx = uint32_t(std::max(across, 1.0)), nz = uint32_t(std::max(deep, 1.0));
        hits.clear();
        for (size_t t = 0; t < tris; t++) {
            float p[3][3];
            bool whole = true;
            for (int k = 0; k < 3 && whole; k++) {
                uint32_t v = level.triangles[t * 3 + k];
                whole = v < points;
                for (int c = 0; c < 3 && whole; c++) p[k][c] = level.vertices[size_t(v) * 3 + c] * level.meters;
            }
            if (!whole) continue;
            float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 0) || std::fabs(n[1]) < 1e-3f * length) continue;  // none, or upright: over no grid point
            bool flat = std::fabs(n[1]) > 0.7f * length;
            float x0 = std::min({p[0][0], p[1][0], p[2][0]}), x1 = std::max({p[0][0], p[1][0], p[2][0]});
            float z0 = std::min({p[0][2], p[1][2], p[2][2]}), z1 = std::max({p[0][2], p[1][2], p[2][2]});
            int i0 = std::max(int(std::ceil((x0 - lo[0]) / spacing - 0.5f)), 0);
            int i1 = std::min(int(std::floor((x1 - lo[0]) / spacing - 0.5f)), int(nx) - 1);
            int j0 = std::max(int(std::ceil((z0 - lo[2]) / spacing - 0.5f)), 0);
            int j1 = std::min(int(std::floor((z1 - lo[2]) / spacing - 0.5f)), int(nz) - 1);
            for (int j = j0; j <= j1; j++)
                for (int i = i0; i <= i1; i++) {
                    // the grid point inside the triangle seen from above, and the triangle's height there
                    float x = lo[0] + (i + 0.5f) * spacing - p[0][0], z = lo[2] + (j + 0.5f) * spacing - p[0][2];
                    float u = (x * e2[2] - z * e2[0]) / -n[1], w = (z * e1[0] - x * e1[2]) / -n[1];
                    if (u < 0 || w < 0 || u + w > 1) continue;
                    hits.push_back({uint32_t(j) * nx + uint32_t(i), p[0][1] + u * e1[1] + w * e2[1], flat});
                }
        }
        std::sort(hits.begin(), hits.end(),
                  [](const Hit& a, const Hit& b) { return a.column != b.column ? a.column < b.column : a.y < b.y; });
        // under the open sky (and on top of a roof) one point in `open` each way, reaching as far
        auto place = [&](uint32_t open) {
            probes.clear();
            roofed = 0;
            for (size_t h = 0; h < hits.size(); h++) {
                if (!hits[h].flat) continue;
                bool room = true, roof = false;
                for (size_t o = h + 1; o < hits.size() && hits[o].column == hits[h].column && room && !roof; o++) {
                    float over = hits[o].y - hits[h].y;
                    if (over > kProbeHeight + kProbeClear) roof = true;
                    else room = over <= 0.05f;
                }
                if (!room) continue;
                uint32_t i = hits[h].column % nx, j = hits[h].column / nx;
                if (!roof && (i % open != open / 2 || j % open != open / 2)) continue;
                float x = lo[0] + (i + 0.5f) * spacing, z = lo[2] + (j + 0.5f) * spacing;
                probes.push_back({{x, hits[h].y + kProbeHeight, z}, roof ? spacing : spacing * open});
                roofed += roof;
            }
        };
        // the open air's points are spread first, as far as they still join (kProbeOpenFar), and further where they
        // alone would take half the points there may be; only then the rest
        uint32_t most = std::max(uint32_t(kProbeOpenFar / spacing), 1u);
        for (uint32_t open = std::min(kProbeOpen, most);; open++) {
            place(open);
            wide = spacing * open;
            if (probes.size() <= g_max_probes) return probes;
            if (open >= most && (probes.size() - roofed) * 2 <= g_max_probes) break;
        }
        // the roofed points' number falls with the square of the spacing while the open air's stays: the spacing that
        // should just fit, and a little over
        float fits = std::sqrt(float(roofed) / float(g_max_probes - (probes.size() - roofed)));
        spacing *= std::clamp(1.03f * fits, 1.05f, 1.5f);
    }
}

void IPLCALL BakeProgress(IPLfloat32, void*) {}

// A level's ways worked out, on a thread of its own: it takes seconds, and the rays for the sounds go on meanwhile.
// `scene` is the thread's own hold on the level
void Bake(IPLContext context, IPLScene scene, Scene level, uint64_t number) {
    int64_t began = NowMs();
    float spacing = 0, wide = 0;
    size_t roofed = 0;
    std::vector<IPLSphere> probes = Probes(level, spacing, wide, roofed);
    IPLProbeBatch batch = nullptr;
    if (!probes.empty() && g_ipl.iplProbeBatchCreate(context, &batch) == IPL_STATUS_SUCCESS) {
        for (const IPLSphere& probe : probes) g_ipl.iplProbeBatchAddProbe(batch, probe);
        g_ipl.iplProbeBatchCommit(batch);
        IPLPathBakeParams params{};
        params.scene = scene;
        params.probeBatch = batch;
        params.identifier.type = IPL_BAKEDDATATYPE_PATHING;
        params.identifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
        params.numSamples = kVisSamples;
        params.radius = kVisRadius;
        params.threshold = kVisThreshold;
        params.visRange = kVisRange;
        params.pathRange = kPathRange;
        params.numThreads = int(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u));
        g_ipl.iplPathBakerBake(context, &params, BakeProgress, nullptr);
    }
    g_ipl.iplSceneRelease(&scene);
    LOGR("acoustics: %s: ways round between %zu points, %zu under a roof %.1f m apart and the rest %.1f m, worked out "
         "in %lld ms",
         level.name, probes.size(), roofed, spacing, wide, static_cast<long long>(NowMs() - began));
    if (!batch) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_baked) g_ipl.iplProbeBatchRelease(&g_baked->batch);  // a level since replaced, never taken up
    g_baked = Baked{batch, number, uint32_t(probes.size())};
    g_wake.notify_one();
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

constexpr IPLSimulationFlags kBoth = IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_PATHING);

// The shelf on a binaural room (kRoomHeadLow), one per ear: first order, unity above it
struct Shelf {
    float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    Shelf() {
        float g = std::pow(10.0f, kRoomHeadLow / 20), c = 2.0f * 48000;
        float pole = 2 * 3.14159265f * kRoomHeadShelf / std::sqrt(g), zero = pole * g;  // middle at kRoomHeadShelf
        b0 = (c + zero) / (c + pole);
        b1 = (zero - c) / (c + pole);
        a1 = (pole - c) / (c + pole);
    }
    void Reset() { x1 = y1 = 0; }
    void Run(float* x, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            float y = b0 * x[i] + b1 * x1 - a1 * y1;
            x1 = x[i];
            y1 = y;
            x[i] = y;
        }
    }
};

// The room's thread: a simulator of its own with the listener as its one source, so a trace never holds up the rays for
// the sounds. Started when a room is first wanted; the levels reach it as the rays' thread builds them
// A trace measured: an impulse through it and through the head, the energy of what comes back in the first second at
// an ear next to the impulse itself, in dB, and how much of it comes later than 80 ms. The gains that fit it, its
// early echoes' (returned) and its reverberation's next to them (`tail`), by how `open` the place is (kRoomWet)
struct Fitted {
    float gain = 1, tail = 1, answered = -120, late = 0, early_db = 0, tail_db = 0;
};
Fitted Fit(IPLReflectionEffect probe, IPLAmbisonicsDecodeEffect ears, IPLSource source, float open) {
    Fitted fit;
    IPLSimulationOutputs out{};
    g_ipl.iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
    IPLReflectionEffectParams params = out.reflections;
    params.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    params.numChannels = 4;
    params.irSize = kRoomIr;
    static float impulse[kFrame], ambi[4][kFrame], heard[2][kFrame];  // the room's thread alone
    float* in_channels[1] = {impulse};
    float* ambi_channels[4] = {ambi[0], ambi[1], ambi[2], ambi[3]};
    float* ear_channels[2] = {heard[0], heard[1]};
    IPLAudioBuffer in{1, IPLint32(kFrame), in_channels};
    IPLAudioBuffer room{4, IPLint32(kFrame), ambi_channels};
    IPLAudioBuffer two{2, IPLint32(kFrame), ear_channels};
    IPLAmbisonicsDecodeEffectParams dp{};
    dp.order = 1;
    dp.hrtf = g_hrtf;
    dp.orientation.right = {1, 0, 0};
    dp.orientation.up = {0, 1, 0};
    dp.orientation.ahead = {0, 0, -1};
    dp.binaural = IPL_TRUE;
    // what comes after 80 ms is in two parts: the traced part's own echoes, to kRoomTraced, and Steam Audio's reverb
    // after it. (Measured once: an effect run again on the same trace is silent)
    g_ipl.iplReflectionEffectReset(probe);
    g_ipl.iplAmbisonicsDecodeEffectReset(ears);
    Shelf shelf[2];
    const int traced_blocks = int(kRoomTraced * 100);
    double first = 0, rest = 0, traced = 0;
    for (int b = 0; b < kRoomFitBlocks; b++) {
        std::fill(impulse, impulse + kFrame, 0.0f);
        if (!b) impulse[0] = 1;
        g_ipl.iplReflectionEffectApply(probe, &params, &in, &room, nullptr);
        g_ipl.iplAmbisonicsDecodeEffectApply(ears, &dp, &room, &two);
        shelf[0].Run(heard[0], kFrame);
        shelf[1].Run(heard[1], kFrame);
        double sum = 0;
        for (uint32_t i = 0; i < kFrame; i++)
            sum += 0.5 * (double(heard[0][i]) * heard[0][i] + double(heard[1][i]) * heard[1][i]);
        (b < kRoomFitEarly ? first : rest) += sum;
        if (b >= kRoomFitEarly && b < traced_blocks) traced += sum;
    }
    double all = first + rest;
    if (!std::isfinite(all) || all < 1e-12) return fit;
    fit.answered = float(10 * std::log10(all));
    fit.late = float(rest / all);
    // The reverberation as it starts: what came after 80 ms, and what 80 ms of its own decay had already taken (60 dB
    // in the time it rings, the three bands' mean). A dead room's is then small, not small and late
    float rings = (params.reverbTimes[0] + params.reverbTimes[1] + params.reverbTimes[2]) / 3;
    rings = std::isfinite(rings) ? std::clamp(rings, kRoomRingMin, kRoomRingMax) : kRoomRingMin;
    float reverberation = float(10 * std::log10(std::max(rest, 1e-12))) + 60 * (kRoomFitEarly * 0.01f) / rings;
    float echoes = float(10 * std::log10(std::max(first, 1e-12)));
    float wet = kRoomWet + 10 * std::log10(rings / kRoomRing);
    float early = std::min(kRoomEarly - echoes, kRoomEarlyRaise);
    float late = wet - reverberation;
    // under the open sky neither is raised (cut, as a too loud room is, they still are)
    float closed = std::clamp((kRoomOpenNone - open) / (kRoomOpenNone - kRoomOpenAll), 0.0f, 1.0f);
    closed = closed * closed * (3 - 2 * closed);
    if (early > 0) early *= closed;
    if (late > 0) late *= closed;
    late = std::clamp(late, kRoomFitMin, kRoomFitMax);
    // The reverb's own level next to the early gain (`tail`) brings what comes after 80 ms to `late`. The traced part's
    // own echoes after 80 ms come with the early gain, and in a small room they are most of its reverberation: there
    // the reverb keeps at least the level Steam Audio gave it, so the room still rings on, and the early echoes are
    // held down instead
    double want = rest * std::pow(10.0, late / 10);
    double reverb = std::max(rest - traced, 0.0), gain2 = std::pow(10.0, std::clamp(early, kRoomFitMin, kRoomFitMax) / 10);
    double tail2 = reverb > 1e-12 ? (want - gain2 * traced) / (gain2 * reverb) : 1.0;
    if (tail2 < 1) {
        tail2 = 1;
        gain2 = std::min(gain2, want / std::max(traced + reverb, 1e-12));
    }
    early = std::clamp(float(10 * std::log10(gain2)), kRoomFitMin, kRoomFitMax);
    fit.early_db = early;
    fit.tail_db = late;
    fit.gain = std::pow(10.0f, early / 20);
    fit.tail = float(std::sqrt(std::min(tail2, 1e4)));
    return fit;
}

void RunRoom(IPLContext context, IPLSceneType type) {
    IPLSimulationSettings ss{};
    ss.flags = IPLSimulationFlags(IPL_SIMULATIONFLAGS_REFLECTIONS | IPL_SIMULATIONFLAGS_DIRECT);
    ss.sceneType = type;
    ss.reflectionType = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    ss.maxNumRays = kRoomRays;
    ss.numDiffuseSamples = kRoomDiffuse;
    ss.maxDuration = kRoomSeconds;
    ss.maxOrder = 1;
    // the listener twice, one for the mixer and one to measure (a trace serves one effect only), and the sky's points
    ss.maxNumSources = 2 + kRoomSky;
    ss.numThreads = int(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
    ss.samplingRate = 48000;
    ss.frameSize = IPLint32(kFrame);
    IPLSimulator simulator = nullptr;
    IPLSource source = nullptr, twin = nullptr;
    IPLSourceSettings so{};
    so.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    IPLAudioSettings as{48000, IPLint32(kFrame)};
    IPLReflectionEffectSettings rs{};
    rs.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    rs.irSize = kRoomIr;
    rs.numChannels = 4;
    IPLReflectionEffect effect = nullptr, probe = nullptr;  // the mixer's, and one to measure a trace with (Fit)
    IPLAmbisonicsDecodeEffect probe_ears = nullptr;
    if (g_ipl.iplSimulatorCreate(context, &ss, &simulator) != IPL_STATUS_SUCCESS ||
        g_ipl.iplSourceCreate(simulator, &so, &source) != IPL_STATUS_SUCCESS ||
        g_ipl.iplSourceCreate(simulator, &so, &twin) != IPL_STATUS_SUCCESS ||
        g_ipl.iplReflectionEffectCreate(context, &as, &rs, &effect) != IPL_STATUS_SUCCESS ||
        g_ipl.iplReflectionEffectCreate(context, &as, &rs, &probe) != IPL_STATUS_SUCCESS) {
        LOGR("acoustics: Steam Audio's room did not start, sounds have no echo of their rooms");
        return;
    }
    g_ipl.iplSourceAdd(source, simulator);
    g_ipl.iplSourceAdd(twin, simulator);
    // how open the place is (kRoomSky): points far off over and round the listener, and whether a ray reaches them
    IPLSource sky[kRoomSky] = {};
    float sky_dir[kRoomSky][3] = {{0, 1, 0}};
    IPLSourceSettings sky_settings{};
    sky_settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
    for (int k = 0; k < kRoomSky; k++) {
        if (k) {
            float up = (k <= 8 ? 15.0f : 50.0f) * 3.14159265f / 180, round = float(k % 8) * 3.14159265f / 4;
            sky_dir[k][0] = std::cos(up) * std::cos(round);
            sky_dir[k][1] = std::sin(up);
            sky_dir[k][2] = std::cos(up) * std::sin(round);
        }
        if (g_ipl.iplSourceCreate(simulator, &sky_settings, &sky[k]) == IPL_STATUS_SUCCESS)
            g_ipl.iplSourceAdd(sky[k], simulator);
    }
    g_ipl.iplSimulatorCommit(simulator);
    IPLScene scene = nullptr;
    bool said = false;
    int64_t fitted = 0;  // when the trace was last measured, 0: not for this level
    float fitted_at[3] = {};
    for (;;) {
        std::optional<IPLScene> next;
        float listener[3], meters;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_room_wake.wait_for(lock, std::chrono::milliseconds(kRoomRestMs));
            next.swap(g_room_scene);
            std::memcpy(listener, g_listener, sizeof listener);
            meters = g_meters;
        }
        if (next) {
            g_room_traced = false;  // the mixer fades the last level's room out
            if (*next) {
                g_ipl.iplSimulatorSetScene(simulator, *next);
                g_ipl.iplSimulatorCommit(simulator);
            }
            if (scene) g_ipl.iplSceneRelease(&scene);
            scene = *next;
            said = false;
            fitted = 0;
        }
        if (!scene || !g_room || !g_on) continue;
        if (!g_room_decode.load()) {  // the head decodes it, made by the rays' thread
            IPLHRTF hrtf = g_hrtf;
            if (!hrtf) continue;
            IPLAmbisonicsDecodeEffectSettings ds{};
            ds.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            ds.hrtf = hrtf;
            ds.maxOrder = 1;
            IPLAmbisonicsDecodeEffect decode = nullptr;
            if (g_ipl.iplAmbisonicsDecodeEffectCreate(context, &as, &ds, &decode) != IPL_STATUS_SUCCESS ||
                g_ipl.iplAmbisonicsDecodeEffectCreate(context, &as, &ds, &probe_ears) != IPL_STATUS_SUCCESS) {
                LOGR("acoustics: Steam Audio's room has no decoder, sounds have no echo of their rooms");
                return;
            }
            g_room_decode = decode;
            g_room_effect = effect;
            g_room_source = source;
        }
        // traced from the listener to the listener: the room as a sound made where the listener stands is answered
        IPLSimulationSharedInputs shared{};
        shared.listener = At(listener, meters);
        shared.numRays = kRoomRays;
        shared.numBounces = kRoomBounces;
        shared.duration = kRoomSeconds;
        shared.order = 1;
        shared.irradianceMinDistance = kRoomNear;
        IPLSimulationInputs in{};
        in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        in.source = shared.listener;
        in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
        in.hybridReverbTransitionTime = kRoomTraced;
        in.hybridReverbOverlapPercent = 0.25f;
        int64_t began = NowMs();
        g_ipl.iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
        g_ipl.iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);
        g_ipl.iplSourceSetInputs(twin, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);
        g_ipl.iplSimulatorRunReflections(simulator);
        float took = float(NowMs() - began);
        g_room_ms = took;
        // measured before the mixer first hears a level's room, then as the listener moves on
        float moved = 0;
        for (int k = 0; k < 3; k++) moved += (listener[k] - fitted_at[k]) * (listener[k] - fitted_at[k]);
        int64_t now = NowMs();
        if (!fitted || now - fitted >= kRoomFitMs || std::sqrt(moved) * meters >= kRoomFitMove) {
            // how open: the share of the sky's rays that meet nothing
            IPLSimulationSharedInputs at{};
            at.listener = shared.listener;
            g_ipl.iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_DIRECT, &at);
            int rays = 0;
            for (int k = 0; k < kRoomSky; k++) {
                if (!sky[k]) continue;
                IPLSimulationInputs ray{};
                ray.flags = IPL_SIMULATIONFLAGS_DIRECT;
                ray.directFlags = IPL_DIRECTSIMULATIONFLAGS_OCCLUSION;
                ray.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
                ray.source = shared.listener;
                ray.source.origin.x += sky_dir[k][0] * kRoomSkyFar;
                ray.source.origin.y += sky_dir[k][1] * kRoomSkyFar;
                ray.source.origin.z += sky_dir[k][2] * kRoomSkyFar;
                g_ipl.iplSourceSetInputs(sky[k], IPL_SIMULATIONFLAGS_DIRECT, &ray);
                rays++;
            }
            g_ipl.iplSimulatorRunDirect(simulator);
            float open = 0;
            for (int k = 0; k < kRoomSky; k++) {
                if (!sky[k]) continue;
                IPLSimulationOutputs seen{};
                g_ipl.iplSourceGetOutputs(sky[k], IPL_SIMULATIONFLAGS_DIRECT, &seen);
                open += seen.direct.occlusion;
            }
            open = rays ? open / float(rays) : 0;
            Fitted fit = Fit(probe, probe_ears, twin, open);
            g_room_fit = fit.gain;
            g_room_tail = fit.tail;
            fitted = now;
            std::memcpy(fitted_at, listener, sizeof fitted_at);
            IPLSimulationOutputs out{};
            g_ipl.iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
            if (!said)
                LOGR("acoustics: the room round the listener traced in %.0f ms, its reverb %.2f %.2f %.2f s", took,
                     out.reflections.reverbTimes[0], out.reflections.reverbTimes[1], out.reflections.reverbTimes[2]);
            if (!said || g_log)
                LOGR("acoustics: the room answers %.1f dB, %.0f%% of it late, %.0f%% open; its echoes raised %.1f dB and "
                     "its reverberation %.1f dB (reverb %.2f %.2f %.2f s, measured in %lld ms)",
                     fit.answered, fit.late * 100, open * 100, fit.early_db, fit.tail_db, out.reflections.reverbTimes[0],
                     out.reflections.reverbTimes[1], out.reflections.reverbTimes[2],
                     static_cast<long long>(NowMs() - now));
            said = true;
        }
        g_room_traced = true;
    }
}

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
    bool room_started = false;
    for (;;) {
        if (g_headphones || g_room) MakeHead(s);
        if (g_room && !room_started) {
            room_started = true;
            std::thread(RunRoom, s.context, s.type).detach();
        }
        std::optional<Scene> level;
        std::optional<Baked> taken;
        float meters;
        jobs.clear();
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait_for(lock, kStep);
            level.swap(g_pending);
            taken.swap(g_baked);
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
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_scene = built;
                g_meters = meters = level->meters;
                g_scale = meters;
                g_raise = level->raise;
                g_probes = 0;
                // the room's thread takes the level too, with a hold of its own (one it never took is let go)
                if (g_room_scene && *g_room_scene) g_ipl.iplSceneRelease(&*g_room_scene);
                g_room_scene = built ? g_ipl.iplSceneRetain(s.scene) : nullptr;
                g_room_wake.notify_one();
                for (auto& [id, sound] : g_sounds) {
                    std::fill(sound.target, sound.target + 3, 1.0f);
                    sound.way = false;
                }
            }
            if (built)  // whether or not the ways are switched on now: they can be while the level is played
                std::thread(Bake, s.context, g_ipl.iplSceneRetain(s.scene), std::move(*level), s.level).detach();
        }
        if (taken) {  // the ways are in: for this level, or one since replaced
            if (taken->level == s.level && s.scene && !s.batch) {
                s.batch = taken->batch;
                g_ipl.iplSimulatorAddProbeBatch(s.simulator, s.batch);
                g_ipl.iplSimulatorCommit(s.simulator);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_probes = taken->probes;
            } else {
                g_ipl.iplProbeBatchRelease(&taken->batch);
            }
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
            ss.flags = kBoth;
            IPLSource source = nullptr;
            if (g_ipl.iplSourceCreate(s.simulator, &ss, &source) != IPL_STATUS_SUCCESS) continue;
            g_ipl.iplSourceAdd(source, s.simulator);
            s.sources[j.id] = source;
            changed = true;
        }
        if (changed) g_ipl.iplSimulatorCommit(s.simulator);
        // with the level's ways worked out, each sound's way round is looked up as well
        bool ways = s.batch && g_paths;
        IPLSimulationFlags flags = ways ? kBoth : IPL_SIMULATIONFLAGS_DIRECT;
        IPLSimulationSharedInputs shared{};
        shared.listener = At(listener, meters);
        g_ipl.iplSimulatorSetSharedInputs(s.simulator, flags, &shared);
        for (const Job& j : jobs) {
            auto it = s.sources.find(j.id);
            if (j.beside || it == s.sources.end()) continue;
            IPLSimulationInputs in{};
            in.flags = flags;
            in.directFlags =
                IPLDirectSimulationFlags(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
            in.source = At(j.pos, meters);
            in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
            in.occlusionRadius = kRadius;
            in.numOcclusionSamples = kSamples;
            in.numTransmissionRays = kSurfaces;
            if (ways) {
                in.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_PATHING;
                in.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_DYNAMIC;
                in.pathingProbes = s.batch;
                in.visRadius = kVisRadius;
                in.visThreshold = kVisThreshold;
                in.visRange = kVisRange;
                in.pathingOrder = 1;
            }
            g_ipl.iplSourceSetInputs(it->second, flags, &in);
        }
        g_ipl.iplSimulatorRunDirect(s.simulator);
        if (ways) g_ipl.iplSimulatorRunPathing(s.simulator);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Job& j : jobs) {
            auto sound = g_sounds.find(j.id);
            if (sound == g_sounds.end()) continue;
            float* target = sound->second.target;
            sound->second.way = false;
            auto it = s.sources.find(j.id);
            if (j.beside || it == s.sources.end()) {
                std::fill(target, target + 3, 1.0f);
                continue;
            }
            IPLSimulationOutputs out{};
            g_ipl.iplSourceGetOutputs(it->second, flags, &out);
            float open = std::clamp(out.direct.occlusion, 0.0f, 1.0f);
            for (int b = 0; b < 3; b++)
                target[b] = open + (1 - open) * std::pow(std::clamp(out.direct.transmission[b], 0.0f, 1.0f), kThin);
            // The way round arrives as first order Ambisonics (W, Y to the left, Z up, X ahead, N3D) of a sound
            // fallen off as 1 / metres along it: its length is in W, where it comes from in the other three. It
            // brings what the walls took from the sound, less what its bends take
            const float* sh = ways ? out.pathing.shCoeffs : nullptr;
            constexpr float kW = 0.2820948f;
            if (!sh || !(sh[0] > 1e-4f * kW) || target[1] > 0.98f) continue;
            float from[3] = {-sh[1], sh[2], -sh[3]};
            float length = std::sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
            if (!(length > 0.2f * sh[0])) continue;  // from all round: no doorway to hear it from
            float along = kW / sh[0] / meters;  // the way's length in the level's units
            // what the bends leave of each band, next to the band they leave most of: the way's length already makes
            // it quieter, and Steam Audio's own loss at a bend on top of that was more than a doorway sounds like
            const float* eq = out.pathing.eqCoeffs;
            float most = std::max({eq[0], eq[1], eq[2], 1e-6f});
            Sound& so = sound->second;
            for (int k = 0; k < 3; k++) {
                so.way_from[k] = listener[k] + from[k] / length * along;
                so.way_gain[k] = (1 - target[k]) * std::clamp(eq[k] / most, 0.0f, 1.0f);  // what the walls took
            }
            so.way = true;
        }
        if (g_log && NowMs() - said >= g_log) {
            said = NowMs();
            uint32_t blocked = 0, n = 0;
            for (const auto& [id, sound] : g_sounds) blocked += sound.target[1] < 0.5f;
            LOGR("acoustics: listener %.2f %.2f %.2f, %zu sounds, %u behind walls; space at %.2f %.2f %.2f, axes %.3f %.3f "
                 "%.3f  %.3f %.3f %.3f  %.3f %.3f %.3f",
                 listener[0], listener[1], listener[2], g_sounds.size(), blocked, g_origin[0], g_origin[1], g_origin[2],
                 g_axes[0], g_axes[1], g_axes[2], g_axes[3], g_axes[4], g_axes[5], g_axes[6], g_axes[7], g_axes[8]);
            for (const auto& [id, sound] : g_sounds) {
                if (n++ >= 8) break;
                LOGR("acoustics: sound %08X given %.2f %.2f %.2f, in the level %.2f %.2f %.2f, mid %.2f", id,
                     sound.raw[0], sound.raw[1], sound.raw[2], sound.pos[0], sound.pos[1], sound.pos[2],
                     sound.target[1]);
                if (sound.way)
                    LOGR("acoustics: sound %08X also by a way round, from %.2f %.2f %.2f, bands %.2f %.2f %.2f", id,
                         sound.way_from[0], sound.way_from[1], sound.way_from[2], sound.way_gain[0], sound.way_gain[1],
                         sound.way_gain[2]);
            }
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

void SetHeadphones(bool on) {
    Env("XBE_HEADPHONES", on);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on != g_headphones) LOGR("acoustics: headphones %s", on ? "on" : "off");
    g_headphones = on;
    if (on && g_state >= 0) {
        Start();
        g_wake.notify_one();
    }
}

bool Headphones() { return g_headphones && g_hrtf.load() != nullptr; }

bool Ears(uint32_t sound, uint32_t image, const float d[3], const float* in, float* left, float* right, uint32_t frames) {
    IPLHRTF hrtf = g_hrtf;
    IPLContext context = g_context;
    if (!hrtf || !context || frames != kFrame) return false;
    std::lock_guard<std::mutex> lock(g_ears_mutex);
    int64_t now = NowMs();
    uint64_t id = uint64_t(sound) << 1 | (image & 1);
    Ear& ear = g_ears[id];
    if (!ear.effect) {
        IPLAudioSettings as{48000, IPLint32(kFrame)};
        IPLBinauralEffectSettings es{hrtf};
        if (g_ipl.iplBinauralEffectCreate(context, &as, &es, &ear.effect) != IPL_STATUS_SUCCESS) {
            g_ears.erase(id);
            return false;
        }
    } else if (now - ear.used > kEarRestMs) {
        g_ipl.iplBinauralEffectReset(ear.effect);
    }
    ear.used = now;
    float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    IPLBinauralEffectParams params{};
    // Steam Audio looks down -z
    params.direction = length > 1e-5f ? IPLVector3{d[0] / length, d[1] / length, -d[2] / length} : IPLVector3{0, 0, -1};
    params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    params.spatialBlend = std::isfinite(length) ? std::clamp(length * float(g_scale) / kHeadNear, 0.0f, 1.0f) : 0.0f;
    params.hrtf = hrtf;
    float* source = const_cast<float*>(in);
    float* ears[2] = {left, right};
    IPLAudioBuffer from{1, IPLint32(frames), &source};
    IPLAudioBuffer to{2, IPLint32(frames), ears};
    g_ipl.iplBinauralEffectApply(ear.effect, &params, &from, &to);
    if (now - g_ears_swept > kEarStaleMs) {
        g_ears_swept = now;
        for (auto it = g_ears.begin(); it != g_ears.end();) {
            if (now - it->second.used <= kEarStaleMs) {
                ++it;
                continue;
            }
            g_ipl.iplBinauralEffectRelease(&it->second.effect);
            it = g_ears.erase(it);
        }
    }
    return true;
}
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
    sound.detour += ((sound.way && g_paths && g_probes ? 1.0f : 0.0f) - sound.detour) * kWayFollow;
    for (int b = 0; b < 3; b++) {
        float floor = kFloor[b] * (1 - (1 - kWayFloor) * sound.detour);
        float kept = floor + (1 - floor) * sound.target[b];
        float want = 1 - strength * (1 - kept);
        sound.heard[b] += (want - sound.heard[b]) * kFollow;
        out[b] = sound.heard[b];
    }
    return true;
}

void SetPaths(bool on) {
    Env("XBE_ACOUSTICS_PATHS", on);
    g_paths = on;
}

bool Way(uint32_t id, float pos[3], float gain[3]) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sounds.find(id);
    if (it == g_sounds.end() || !g_on || g_state < 0 || !g_scene) return false;
    Sound& sound = it->second;
    bool way = sound.way && g_paths && g_probes;
    float strength = way && Heard(NowMs()) ? float(g_strength) : 0.0f;
    if (way) {
        // where it comes from follows the latest result too: a way that switches doorways swings across
        for (int k = 0; k < 3; k++) {
            if (!sound.way_placed) sound.way_at[k] = sound.way_from[k];
            sound.way_at[k] += (sound.way_from[k] - sound.way_at[k]) * kWayFollow;
        }
        sound.way_placed = true;
    }
    bool any = false;
    for (int b = 0; b < 3; b++) {
        float want = way ? strength * sound.way_gain[b] : 0.0f;
        sound.way_heard[b] += (want - sound.way_heard[b]) * kWayFollow;
        if (sound.way_heard[b] < 1e-4f) sound.way_heard[b] = 0;
        gain[b] = sound.way_heard[b];
        any = any || gain[b] > 0;
    }
    if (!any) {
        sound.way_placed = false;
        return false;
    }
    // the level's point taken back into the sounds' space, as TestSound does
    float d[3] = {sound.way_at[0] - g_origin[0], sound.way_at[1] - g_origin[1], sound.way_at[2] - g_origin[2]};
    for (int i = 0; i < 3; i++) pos[i] = d[0] * g_axes[i * 3] + d[1] * g_axes[i * 3 + 1] + d[2] * g_axes[i * 3 + 2];
    return true;
}

void SetRoom(bool on, float level) {
    Env("XBE_ACOUSTICS_REVERB", on);
    g_room_level = std::clamp(level, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on != g_room) LOGR("acoustics: the room's echo %s", on ? "on" : "off");
    g_room = on;
    g_wake.notify_one();
}

bool Room() { return g_room && g_on && g_room_effect.load() != nullptr; }

Shelf g_room_shelf[2];  // the mixer's thread alone

bool RoomAnswer(const float* send, const float front[3], const float top[3], float* left, float* right, uint32_t frames) {
    static float s_level = 0;   // the mixer's thread alone: the level the last block ended at
    static bool s_ran = false;  // the effect holds sound since its last reset
    IPLReflectionEffect effect = g_room_effect;
    IPLAmbisonicsDecodeEffect decode = g_room_decode;
    IPLSource source = g_room_source;
    IPLHRTF hrtf = g_hrtf;
    if (!effect || !decode || !source || !hrtf || frames != kFrame) return false;
    bool on = g_room && g_on && g_room_traced && Heard(NowMs());
    // the trace's fit is followed over a few blocks: it steps as the listener walks from one room into another
    static float s_fit = 0, s_tail = 1;
    float fit = g_room_fit, tail = g_room_tail;
    s_fit = s_level == 0 ? fit : s_fit + (fit - s_fit) * 0.05f;
    s_tail = s_level == 0 ? tail : s_tail + (tail - s_tail) * 0.05f;
    float want = on ? g_room_level * s_fit : 0.0f;
    if (want == 0 && s_level == 0) {
        if (s_ran) {  // switched off, or a new level: the next room starts without the last one's tail
            g_ipl.iplReflectionEffectReset(effect);
            g_ipl.iplAmbisonicsDecodeEffectReset(decode);
            g_room_shelf[0].Reset();
            g_room_shelf[1].Reset();
            s_ran = false;
        }
        return false;
    }
    // the listener's right, up and ahead in the level, from the sounds' space as the mixer has them
    float axes[9];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::memcpy(axes, g_axes, sizeof axes);
    }
    float side[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2],
                     top[0] * front[1] - top[1] * front[0]};
    float r[3], u[3], f[3];
    for (int i = 0; i < 3; i++) {
        r[i] = side[0] * axes[i] + side[1] * axes[3 + i] + side[2] * axes[6 + i];
        u[i] = top[0] * axes[i] + top[1] * axes[3 + i] + top[2] * axes[6 + i];
        f[i] = front[0] * axes[i] + front[1] * axes[3 + i] + front[2] * axes[6 + i];
    }
    IPLSimulationOutputs out{};
    g_ipl.iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
    IPLReflectionEffectParams params = out.reflections;
    params.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    params.numChannels = 4;
    params.irSize = kRoomIr;
    for (float& eq : params.eq) eq *= s_tail;  // the reverberation's own level (Fit)
    static float ambi[4][kFrame], ears[2][kFrame];
    float* in_channels[1] = {const_cast<float*>(send)};
    float* ambi_channels[4] = {ambi[0], ambi[1], ambi[2], ambi[3]};
    float* ear_channels[2] = {ears[0], ears[1]};
    IPLAudioBuffer in{1, IPLint32(frames), in_channels};
    IPLAudioBuffer room{4, IPLint32(frames), ambi_channels};
    IPLAudioBuffer heard{2, IPLint32(frames), ear_channels};
    g_ipl.iplReflectionEffectApply(effect, &params, &in, &room, nullptr);
    // The trace is in the level's axes: W, then the sound's direction along -x, +y and -z (Ambisonics' left, up and
    // ahead as Steam Audio lays them). Turned to the listener's own left, up and ahead here, so the decoder needs no
    // orientation and a level of either handedness is heard the right way round
    for (uint32_t i = 0; i < frames; i++) {
        float d[3] = {-ambi[1][i], ambi[2][i], -ambi[3][i]};
        ambi[1][i] = -(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
        ambi[2][i] = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        ambi[3][i] = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    }
    bool head = g_headphones;
    IPLAmbisonicsDecodeEffectParams dp{};
    dp.order = 1;
    dp.hrtf = hrtf;
    dp.orientation.right = {1, 0, 0};
    dp.orientation.up = {0, 1, 0};
    dp.orientation.ahead = {0, 0, -1};
    dp.binaural = head ? IPL_TRUE : IPL_FALSE;
    g_ipl.iplAmbisonicsDecodeEffectApply(decode, &dp, &room, &heard);
    if (head) {
        g_room_shelf[0].Run(ears[0], frames);
        g_room_shelf[1].Run(ears[1], frames);
    }
    s_ran = true;
    float level = s_level, step = (want - s_level) / float(frames), scale = head ? 1.0f : kRoomSpeakers;
    static double s_sent = 0, s_answered = 0;  // XBE_ACOUSTICS_LOG: what went in and what came back, squared and summed
    static uint32_t s_blocks = 0;
    for (uint32_t i = 0; i < frames; i++) {
        level += step;
        float l = ears[0][i] * level * scale, r = ears[1][i] * level * scale;
        left[i] += l;
        right[i] += r;
        s_sent += double(send[i]) * send[i];
        s_answered += 0.5 * (double(l) * l + double(r) * r);
    }
    s_level = want;
    if (g_log && ++s_blocks >= uint32_t(g_log / 10)) {
        double n = double(s_blocks) * frames;
        LOGR("acoustics: the room: sent %.1f dB, answered %.1f dB an ear (level %.2f, reverb %.2f %.2f %.2f s)",
             10 * std::log10(s_sent / n + 1e-12), 10 * std::log10(s_answered / n + 1e-12), want,
             params.reverbTimes[0], params.reverbTimes[1], params.reverbTimes[2]);
        s_sent = s_answered = 0;
        s_blocks = 0;
    }
    return true;
}

void Forget(uint32_t id) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sounds.erase(id);
    }
    std::lock_guard<std::mutex> lock(g_ears_mutex);
    for (uint64_t image = 0; image < 2; image++) {
        auto ear = g_ears.find(uint64_t(id) << 1 | image);
        if (ear == g_ears.end()) continue;
        g_ipl.iplBinauralEffectRelease(&ear->second.effect);
        g_ears.erase(ear);
    }
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
    st.probes = g_probes;
    st.room = st.on && g_room && g_room_traced && g_room_effect.load() != nullptr;
    st.room_ms = g_room_ms;
    st.test_placed = g_test;
    st.heard = st.on && Heard(NowMs());
    int64_t now = NowMs();
    for (const auto& [id, sound] : g_sounds) {
        if (now - sound.seen > 200) continue;
        if (id == kTestSound) {
            st.test = sound.heard[1];
            st.test_way = sound.way && g_paths && g_probes;
            continue;
        }
        st.sounds++;
        st.blocked += sound.target[1] < 0.5f;
        st.ways += sound.way;
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
