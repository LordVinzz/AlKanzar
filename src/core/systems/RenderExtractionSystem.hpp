#pragma once

#include "core/app/FrameData.hpp"
#include "core/lighting/MaterialLibrary.hpp"
#include "core/editor/SelectionModel.hpp"
#include "TaskScheduler.hpp"
#include "core/ecs/World.hpp"

namespace core {

class RenderExtractionSystem {
public:
    /**
     * Extracts render-ready frame data from world state.
     * Parallel extraction is frame-bound and waits for each phase before presenting.
     */
    void extract(
        const World& world,
        const MaterialLibrary& materials,
        const SelectionModel& selection,
        FrameSceneData& outFrame,
        TaskScheduler& scheduler,
        bool useParallel = false
    ) const;
};

}  // namespace core
