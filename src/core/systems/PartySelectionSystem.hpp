#pragma once

#include <vector>

#include "core/ecs/Entity.hpp"

namespace render {
struct CameraMatrices;
}

namespace core {

struct FrameSceneData;
class World;
class PartySelectionModel;

struct ScreenSelectionRect {
    float minX{0.0f};
    float minY{0.0f};
    float maxX{0.0f};
    float maxY{0.0f};

    [[nodiscard]] bool intersects(const ScreenSelectionRect& other) const {
        return maxX >= other.minX && other.maxX >= minX &&
            maxY >= other.minY && other.maxY >= minY;
    }
};

[[nodiscard]] bool isPartySelectionDrag(
    int startX,
    int startY,
    int currentX,
    int currentY,
    int thresholdPixels = 6
);

[[nodiscard]] ScreenSelectionRect makeScreenSelectionRect(
    int startX,
    int startY,
    int currentX,
    int currentY,
    int viewportWidth,
    int viewportHeight
);

class PartySelectionSystem {
public:
    [[nodiscard]] std::vector<EntityId> selectOwnedCharacters(
        const World& world,
        const FrameSceneData& frame,
        const render::CameraMatrices& camera,
        int viewportWidth,
        int viewportHeight,
        const ScreenSelectionRect& selectionRect
    ) const;

    void syncGroundIndicatorSelection(
        const World& world,
        const PartySelectionModel& selection,
        FrameSceneData& frame
    ) const;
};

}  // namespace core
