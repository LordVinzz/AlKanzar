#pragma once

#include "TaskScheduler.hpp"
#include "World.hpp"

namespace core {

class TransformSystem {
public:
    /**
     * Rebuilds dirty world transforms for the current frame.
     * Parallel mode is frame-bound and still blocks on completion before rendering continues.
     */
    void update(World& world, TaskScheduler& scheduler, bool useParallel = true) const;
};

}  // namespace core
