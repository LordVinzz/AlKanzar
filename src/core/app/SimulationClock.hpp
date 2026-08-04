#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core {

struct SimulationClockConfig {
    double fixedStepSeconds{1.0 / 60.0};
    double maxFrameSeconds{0.25};
    std::uint32_t maxStepsPerFrame{8u};
};

class SimulationClock {
public:
    explicit SimulationClock(SimulationClockConfig config = {})
        : config_(config) {}

    double advance(double frameSeconds, bool paused, double timeScale) {
        const double safeFrameSeconds = std::clamp(frameSeconds, 0.0, config_.maxFrameSeconds);
        if (paused || timeScale <= 0.0) {
            return safeFrameSeconds;
        }

        accumulatorSeconds_ = std::min(
            accumulatorSeconds_ + safeFrameSeconds * timeScale,
            config_.fixedStepSeconds * static_cast<double>(config_.maxStepsPerFrame)
        );
        return safeFrameSeconds;
    }

    [[nodiscard]] bool hasPendingStep() const {
        return accumulatorSeconds_ + 1e-9 >= config_.fixedStepSeconds;
    }

    bool consumeStep() {
        if (!hasPendingStep()) {
            return false;
        }
        accumulatorSeconds_ = std::max(0.0, accumulatorSeconds_ - config_.fixedStepSeconds);
        ++tickCount_;
        return true;
    }

    [[nodiscard]] double fixedStepSeconds() const { return config_.fixedStepSeconds; }
    [[nodiscard]] double interpolationAlpha() const {
        return std::clamp(accumulatorSeconds_ / config_.fixedStepSeconds, 0.0, 1.0);
    }
    [[nodiscard]] std::uint64_t tickCount() const { return tickCount_; }

private:
    SimulationClockConfig config_{};
    double accumulatorSeconds_{0.0};
    std::uint64_t tickCount_{0u};
};

}  // namespace core
