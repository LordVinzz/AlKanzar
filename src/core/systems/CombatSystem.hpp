#pragma once

#include "core/app/TimeContext.hpp"
#include "core/ecs/World.hpp"

namespace core {

class CombatSystem {
public:
    void update(World& world, const TimeContext& time) const;
};

}  // namespace core
