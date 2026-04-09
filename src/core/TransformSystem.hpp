#pragma once

#include "World.hpp"

namespace core {

class TransformSystem {
public:
    void update(World& world) const;
};

}  // namespace core
