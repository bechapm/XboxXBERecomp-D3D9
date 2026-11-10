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
    X(iplSimulatorAddProbeBatch) X(iplSimulatorRemoveProbeBatch) X(iplSimulatorRunPathing)
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
// The ways round (Way). Points 1.5 m over every floor, this far apart; fewer than kMaxProbes, or they are spread wider
constexpr float kProbeSpacing = 1.5f;
constexpr float kProbeHeight = 1.5f;
constexpr float kProbeClear = 0.3f;  // metres of room a point needs over it
constexpr size_t kMaxProbes = 6000;
// two points are joined when half of 4 x 4 rays between balls of half a metre round them pass (a doorway 1.2 m wide
// joins the points either side of it), up to 20 m apart; a way is at most 150 m long
constexpr int kVisSamples = 4;
constexpr float kVisRadius = 0.5f;
constexpr float kVisThreshold = 0.5f;
constexpr float kVisRange = 20.0f;
constexpr float kPathRange = 150.0f;
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

std::atomic<bool> g_paths{[] {
    const char* v = std::getenv("XBE_ACOUSTICS_PATHS");
    return !(v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0));
}()};
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
    LOGR("acoustics: headphones: 3D sounds through Steam Audio's head");
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
// work. `spacing` is widened until there are fewer than kMaxProbes
std::vector<IPLSphere> Probes(const Scene& level, float& spacing) {
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
    for (spacing = kProbeSpacing;; spacing *= 1.3f) {
        double across = std::ceil((hi[0] - lo[0]) / spacing), deep = std::ceil((hi[2] - lo[2]) / spacing);
        if (across * deep > 4e6) continue;  // a level too wide for this grid
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
        probes.clear();
        for (size_t h = 0; h < hits.size(); h++) {
            if (!hits[h].flat) continue;
            bool room = true;
            for (size_t o = h + 1; o < hits.size() && hits[o].column == hits[h].column && room; o++) {
                float over = hits[o].y - hits[h].y;
                if (over > kProbeHeight + kProbeClear) break;
                room = over <= 0.05f;
            }
            if (!room) continue;
            float x = lo[0] + (hits[h].column % nx + 0.5f) * spacing, z = lo[2] + (hits[h].column / nx + 0.5f) * spacing;
            probes.push_back({{x, hits[h].y + kProbeHeight, z}, spacing});
        }
        if (probes.size() <= kMaxProbes) return probes;
    }
}

void IPLCALL BakeProgress(IPLfloat32, void*) {}

// A level's ways worked out, on a thread of its own: it takes seconds, and the rays for the sounds go on meanwhile.
// `scene` is the thread's own hold on the level
void Bake(IPLContext context, IPLScene scene, Scene level, uint64_t number) {
    int64_t began = NowMs();
    float spacing = 0;
    std::vector<IPLSphere> probes = Probes(level, spacing);
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
    LOGR("acoustics: %s: ways round between %zu points %.1f m apart, worked out in %lld ms", level.name, probes.size(),
         spacing, static_cast<long long>(NowMs() - began));
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
        if (g_headphones) MakeHead(s);
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
            for (int b = 0; b < 3; b++) target[b] = open + (1 - open) * std::clamp(out.direct.transmission[b], 0.0f, 1.0f);
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
