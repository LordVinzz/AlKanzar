#pragma once

#include "TaskScheduler.hpp"
#include "TimeContext.hpp"
#include "World.hpp"

namespace core {

class LightSystem {
public:
    /**
     * Updates light runtime state and volume assignments for the current frame.
     * Parallel mode runs in phases and waits between them before the frame can continue.
     */
    void update(
        World& world,
        const TimeContext& timeContext,
        TaskScheduler& scheduler,
        bool useParallel = true
    ) const;
};

}  // namespace core
