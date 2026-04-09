#pragma once

#include "TimeContext.hpp"
#include "World.hpp"

namespace core {

class LightSystem {
public:
    void update(World& world, const TimeContext& timeContext) const;
};

}  // namespace core
