#pragma once

#include "FrameData.hpp"
#include "MaterialLibrary.hpp"
#include "SelectionModel.hpp"
#include "World.hpp"

namespace core {

class RenderExtractionSystem {
public:
    void extract(
        const World& world,
        const MaterialLibrary& materials,
        const SelectionModel& selection,
        FrameSceneData& outFrame
    ) const;
};

}  // namespace core
