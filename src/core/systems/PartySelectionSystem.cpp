#include "PartySelectionSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/app/FrameData.hpp"
#include "core/ecs/World.hpp"
#include "core/presentation/CharacterPresentation.hpp"
#include "core/simulation/CharacterComponents.hpp"
#include "PartySelectionModel.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

namespace {

constexpr float kMinimumProjectedExtentPixels = 2.0f;

std::optional<glm::vec2> projectToScreen(
    const glm::vec3& point,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight
) {
    const glm::vec4 clip = camera.projection * camera.view * glm::vec4(point, 1.0f);
    if (clip.w <= 1.0e-5f) {
        return std::nullopt;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2(
        (ndc.x + 1.0f) * 0.5f * static_cast<float>(viewportWidth),
        (1.0f - ndc.y) * 0.5f * static_cast<float>(viewportHeight)
    );
}

void includePoint(ScreenSelectionRect& bounds, bool& hasPoint, const glm::vec2& point) {
    if (!hasPoint) {
        bounds = ScreenSelectionRect{point.x, point.y, point.x, point.y};
        hasPoint = true;
        return;
    }
    bounds.minX = std::min(bounds.minX, point.x);
    bounds.minY = std::min(bounds.minY, point.y);
    bounds.maxX = std::max(bounds.maxX, point.x);
    bounds.maxY = std::max(bounds.maxY, point.y);
}

void includeWorldBounds(
    ScreenSelectionRect& screenBounds,
    bool& hasPoint,
    const render::Bounds3& worldBounds,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight
) {
    const std::array<float, 2> xs{worldBounds.min.x, worldBounds.max.x};
    const std::array<float, 2> ys{worldBounds.min.y, worldBounds.max.y};
    const std::array<float, 2> zs{worldBounds.min.z, worldBounds.max.z};
    for (const float x : xs) {
        for (const float y : ys) {
            for (const float z : zs) {
                const std::optional<glm::vec2> projected = projectToScreen(
                    glm::vec3(x, y, z),
                    camera,
                    viewportWidth,
                    viewportHeight
                );
                if (projected.has_value()) {
                    includePoint(screenBounds, hasPoint, *projected);
                }
            }
        }
    }
}

std::optional<ScreenSelectionRect> characterScreenBounds(
    EntityId character,
    const World& world,
    const FrameSceneData& frame,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight
) {
    ScreenSelectionRect screenBounds{};
    bool hasPoint = false;
    for (const FrameRenderable& renderable : frame.renderables) {
        if (!renderable.visible || !renderable.hasWorldBounds ||
            world.characterOwnerEntity(renderable.entity) != character) {
            continue;
        }
        includeWorldBounds(
            screenBounds,
            hasPoint,
            renderable.worldBounds,
            camera,
            viewportWidth,
            viewportHeight
        );
    }

    if (!hasPoint) {
        for (const FrameGroundIndicator& indicator : frame.groundIndicators) {
            if (indicator.owner != character) {
                continue;
            }
            const std::optional<glm::vec2> projected = projectToScreen(
                indicator.center,
                camera,
                viewportWidth,
                viewportHeight
            );
            if (projected.has_value()) {
                includePoint(screenBounds, hasPoint, *projected);
            }
            break;
        }
    }

    if (!hasPoint) {
        return std::nullopt;
    }
    if (screenBounds.maxX - screenBounds.minX < kMinimumProjectedExtentPixels) {
        screenBounds.minX -= kMinimumProjectedExtentPixels;
        screenBounds.maxX += kMinimumProjectedExtentPixels;
    }
    if (screenBounds.maxY - screenBounds.minY < kMinimumProjectedExtentPixels) {
        screenBounds.minY -= kMinimumProjectedExtentPixels;
        screenBounds.maxY += kMinimumProjectedExtentPixels;
    }
    return screenBounds;
}

}  // namespace

bool PartySelectionSystem::isControllablePartyMember(
    const World& world,
    EntityId entity
) const {
    return world.isAlive(entity) && world.characters.contains(entity) &&
        isActivePlayerPartyMember(
            world.characterControllers.tryGet(entity),
            world.partyMembers.tryGet(entity)
        );
}

std::vector<EntityId> PartySelectionSystem::orderedActivePartyMembers(
    const World& world
) const {
    std::vector<EntityId> members{};
    members.reserve(std::min(world.partyMembers.size(), kMaximumPartySize));
    for (const EntityId entity : world.partyMembers.entities()) {
        if (isControllablePartyMember(world, entity)) {
            members.push_back(entity);
        }
    }
    std::sort(members.begin(), members.end(), [&world](EntityId lhs, EntityId rhs) {
        const PartyMemberComponent& lhsMember = world.partyMembers.get(lhs);
        const PartyMemberComponent& rhsMember = world.partyMembers.get(rhs);
        if (lhsMember.slot != rhsMember.slot) {
            return lhsMember.slot < rhsMember.slot;
        }
        if (lhs.index != rhs.index) {
            return lhs.index < rhs.index;
        }
        return lhs.generation < rhs.generation;
    });
    if (members.size() > kMaximumPartySize) {
        members.resize(kMaximumPartySize);
    }
    return members;
}

bool isPartySelectionDrag(
    int startX,
    int startY,
    int currentX,
    int currentY,
    int thresholdPixels
) {
    const std::int64_t dx = static_cast<std::int64_t>(currentX) - startX;
    const std::int64_t dy = static_cast<std::int64_t>(currentY) - startY;
    const std::int64_t threshold = std::max(thresholdPixels, 0);
    return dx * dx + dy * dy >= threshold * threshold;
}

ScreenSelectionRect makeScreenSelectionRect(
    int startX,
    int startY,
    int currentX,
    int currentY,
    int viewportWidth,
    int viewportHeight
) {
    const float width = static_cast<float>(std::max(viewportWidth, 0));
    const float height = static_cast<float>(std::max(viewportHeight, 0));
    return ScreenSelectionRect{
        std::clamp(static_cast<float>(std::min(startX, currentX)), 0.0f, width),
        std::clamp(static_cast<float>(std::min(startY, currentY)), 0.0f, height),
        std::clamp(static_cast<float>(std::max(startX, currentX)), 0.0f, width),
        std::clamp(static_cast<float>(std::max(startY, currentY)), 0.0f, height),
    };
}

std::vector<EntityId> PartySelectionSystem::selectOwnedCharacters(
    const World& world,
    const FrameSceneData& frame,
    const render::CameraMatrices& camera,
    int viewportWidth,
    int viewportHeight,
    const ScreenSelectionRect& selectionRect
) const {
    std::vector<EntityId> selected{};
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        return selected;
    }

    for (const EntityId entity : orderedActivePartyMembers(world)) {
        const std::optional<ScreenSelectionRect> bounds = characterScreenBounds(
            entity,
            world,
            frame,
            camera,
            viewportWidth,
            viewportHeight
        );
        if (bounds.has_value() && selectionRect.intersects(*bounds)) {
            selected.push_back(entity);
        }
    }
    return selected;
}

void PartySelectionSystem::syncGroundIndicatorSelection(
    const World& world,
    const PartySelectionModel& selection,
    FrameSceneData& frame
) const {
    for (FrameGroundIndicator& indicator : frame.groundIndicators) {
        const CharacterComponent* character = world.characters.tryGet(indicator.owner);
        if (character == nullptr) {
            continue;
        }
        if (isControllablePartyMember(world, indicator.owner)) {
            indicator.color = characterGroundIndicatorColor(
                CharacterAffiliation::Player,
                selection.contains(indicator.owner)
            );
        } else {
            indicator.color = characterGroundIndicatorColor(character->affiliation);
        }
    }
}

void PartySelectionSystem::pruneInvalidSelection(
    const World& world,
    PartySelectionModel& selection
) const {
    std::vector<EntityId> valid{};
    valid.reserve(selection.selected().size());
    for (const EntityId entity : selection.selected()) {
        if (isControllablePartyMember(world, entity)) {
            valid.push_back(entity);
        }
    }
    selection.setSelection(std::move(valid));
}

}  // namespace core
