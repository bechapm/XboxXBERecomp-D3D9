// the mixer pulled every 10 ms by the clock, its output dropped
#include <chrono>
#include <thread>
#include <vector>

#include "audio/audio.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        std::vector<float> b(kRate / 100 * kChannels);
        for (;;) {
            render(b.data(), kRate / 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}

bool IsSurround() { return false; }
bool IsMono() { return false; }
void SetOutputMode(uint32_t) {}

}  // namespace audio
