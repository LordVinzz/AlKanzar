#pragma once

#include "core/app/FrameData.hpp"
#include "core/ecs/World.hpp"

namespace core::render_extraction_detail {

bool resolveAnimatedSelectionBounds(
    const World& world,
    EntityId selected,
    const FrameSceneData& frame,
    render::Bounds3& outBounds
);

void applyAnimatedRenderableState(
    const World& world,
    EntityId entity,
    FrameRenderable& frameRenderable,
    FrameSceneData& outFrame
);

void populateSelectionSkeletonDebug(
    const World& world,
    EntityId selected,
    FrameSceneData& outFrame
);

}  // namespace core::render_extraction_detail
