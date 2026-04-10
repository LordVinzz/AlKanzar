#pragma once

#include "core/app/TimeContext.hpp"
#include "core/ecs/World.hpp"
#include "core/systems/TaskScheduler.hpp"

namespace core {

class AnimationSystem {
public:
    void update(World& world, const TimeContext& time, TaskScheduler& scheduler, bool useParallel = true) const;
};

}  // namespace core
