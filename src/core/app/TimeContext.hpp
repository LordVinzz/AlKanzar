#pragma once

#include <cstdint>

namespace core {

struct TimeContext {
    float totalSeconds{0.0f};
    float deltaSeconds{0.0f};
    float frameDeltaSeconds{0.0f};
    float interpolationAlpha{0.0f};
    float timeScale{1.0f};
    std::uint64_t simulationTick{0u};
    bool paused{false};
};

}  // namespace core
