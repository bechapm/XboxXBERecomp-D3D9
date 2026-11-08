// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// XBE_AUDIO_CAPTURE=<file.wav>: everything the mixer sends out, as 32 bit float stereo, so a test run can be listened to
// afterwards (it is written whether or not XBE_MUTE silences the speakers). The header is brought up to date every
// second, so a run that is stopped still leaves a file that plays
struct Capture {
    FILE* f = nullptr;
    uint32_t frames = 0;
    void Header() {
        uint32_t data = frames * 8, fmt[4] = {0x00020003u, kRate, kRate * 8, 0x00200008u};
        uint32_t head[5] = {0x46464952u, 36 + data, 0x45564157u, 0x20746D66u, 16};  // RIFF, WAVE, "fmt "
        uint32_t tail[2] = {0x61746164u, data};                                        // "data"
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(head, 4, 5, f);
        std::fwrite(fmt, 4, 4, f);
        std::fwrite(tail, 4, 2, f);
        std::fseek(f, 0, SEEK_END);
    }
    void Open() {
        const char* path = std::getenv("XBE_AUDIO_CAPTURE");
        if (!path || !*path) return;
        f = std::fopen(path, "wb");
        if (!f) {
            LOGE("audio: %s could not be written", path);
            return;
        }
        Header();
        LOGI("audio: the mix goes to %s", path);
    }
    void Write(const float* b, uint32_t n) {
        if (!f) return;
        std::fwrite(b, 8, n, f);
        if ((frames + n) / kRate != frames / kRate) {
            frames += n;
            Header();
            std::fflush(f);
        } else {
            frames += n;
        }
    }
};

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (SUCCEEDED(hr)) hr = xa->CreateMasteringVoice(&master);
        if (SUCCEEDED(hr)) hr = xa->CreateSourceVoice(&voice, &wf);
        if (FAILED(hr)) {
            LOGE("audio: XAudio2 unavailable (%08X), no sound", unsigned(hr));
            return;
        }
        if (std::getenv("XBE_MUTE")) master->SetVolume(0.0f);  // test runs: everything runs, nothing is heard
        voice->Start();
        Capture capture;
        capture.Open();
        constexpr uint32_t kFrames = kRate / 100;
        constexpr uint32_t kMinQueued = 3;
        constexpr uint32_t kMaxQueued = 10;
        // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
        constexpr uint32_t kSlots = kMaxQueued + 1;
        std::vector<float> bufs(kSlots * kFrames * 2);
        uint32_t depth = kMinQueued;
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
            if (n && !st.BuffersQueued && depth < kMaxQueued) {
                depth++;
                LOGI("audio: output starved, queueing %u ms", depth * 10);
            }
            if (st.BuffersQueued >= depth) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            capture.Write(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
