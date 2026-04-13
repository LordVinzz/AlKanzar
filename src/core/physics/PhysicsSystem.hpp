#pragma once

#include "core/app/TimeContext.hpp"
#include "core/ecs/World.hpp"
#include "core/systems/TaskScheduler.hpp"

namespace core {

class PhysicsSystem {
public:
    /**
     * Simulates rigidbody-vs-rigidbody collisions for entities that also own a box or sphere collider.
     * Parallel mode uses the task scheduler for contact generation and applies the final body state on the main thread.
     */
    void update(
        World& world,
        const TimeContext& timeContext,
        TaskScheduler& scheduler,
        bool useParallel = true
    ) const;
};

}  // namespace core
