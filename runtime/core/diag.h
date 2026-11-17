#pragma once
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// XBE_D3D9_SOLID set: Direct3D 9's pixel shaders draw magenta, which shows where its draws land
inline bool D3d9Solid() {
    static const bool on = std::getenv("XBE_D3D9_SOLID") != nullptr;
    return on;
}

// XBE_D3D9_FIXED_TRANSFORM: 1 draws programs that are only a plain transform by fixed function, 0 never, whatever a mod
// asked; unset (-1), as a mod asked
inline int D3d9FixedTransform() {
    static const int on = [] {
        const char* e = std::getenv("XBE_D3D9_FIXED_TRANSFORM");
        return e && *e ? (e[0] != '0' ? 1 : 0) : -1;
    }();
    return on;
}

// XBE_D3D9_SKIP_PROGRAMS: vertex programs, by the hashes the D3D9 camera report prints, comma separated, whose draws
// are left out: which of a frame's draws does something can be found by leaving it out
inline bool D3d9SkipProgram(uint64_t hash) {
    static const std::vector<uint64_t> list = [] {
        std::vector<uint64_t> l;
        const char* e = std::getenv("XBE_D3D9_SKIP_PROGRAMS");
        while (e && *e) {
            char* end;
            uint64_t v = std::strtoull(e, &end, 16);
            if (end == e) break;
            l.push_back(v);
            e = *end == ',' ? end + 1 : end;
        }
        return l;
    }();
    for (uint64_t h : list)
        if (h == hash) return true;
    return false;
}

}  // namespace diag
