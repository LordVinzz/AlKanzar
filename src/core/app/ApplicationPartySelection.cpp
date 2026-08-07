#include "Application.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "core/scene/Camera.hpp"

namespace core {

bool Application::moveAgentToViewportPosition(EntityId agent, int x, int y) {
    if (!agent.valid() || !services_.world.navAgents.contains(agent)) {
        return false;
    }

    const render::CameraMatrices camera = computeCameraMatrices(
        services_.camera,
        services_.renderer.width(),
        services_.renderer.height()
    );
    std::optional<NavHitResult> hit{};
    {
        ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Hit Test");
        hit = services_.navigationSystem.hitTest(
            services_.navigation,
            camera,
            services_.renderer.width(),
            services_.renderer.height(),
            x,
            y
        );
    }
    if (!hit.has_value()) {
        return false;
    }

    ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Path Request");
    return services_.navigationSystem.requestAgentDestination(
        services_.world,
        services_.navigation,
        services_.scheduler,
        agent,
        hit->position
    );
}

bool Application::moveSelectedPartyToViewportPosition(int x, int y) {
    if (services_.partySelection.selected().empty()) {
        return false;
    }

    const render::CameraMatrices camera = computeCameraMatrices(
        services_.camera,
        services_.renderer.width(),
        services_.renderer.height()
    );
    std::optional<NavHitResult> hit{};
    {
        ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Group Hit Test");
        hit = services_.navigationSystem.hitTest(
            services_.navigation,
            camera,
            services_.renderer.width(),
            services_.renderer.height(),
            x,
            y
        );
    }
    if (!hit.has_value()) {
        return false;
    }

    ALKANZAR_PROFILE_SCOPE(services_.profiler, "Navigation Group Path Requests");
    const PartyMoveOrderResult result = services_.partyOrderSystem.issueMoveOrder(
        services_.world,
        services_.navigation,
        services_.navigationSystem,
        services_.scheduler,
        services_.partySelection.selected(),
        hit->position
    );
    return result.requestedCount > 0u;
}

void Application::finishPartySelectionDrag(int x, int y) {
    PartySelectionDragSession& drag = services_.input.partySelectionDrag;
    if (!drag.active) {
        return;
    }
    drag.update(x, y);

    const int startX = drag.startX;
    const int startY = drag.startY;
    const int currentX = drag.currentX;
    const int currentY = drag.currentY;
    const bool dragged = isPartySelectionDrag(startX, startY, currentX, currentY);
    drag.reset();

    if (!modeSession_.capabilities().acceptsPartySelection || services_.renderer.wantsMouse()) {
        return;
    }

    const int viewportWidth = services_.renderer.width();
    const int viewportHeight = services_.renderer.height();
    const render::CameraMatrices camera = computeCameraMatrices(
        services_.camera,
        viewportWidth,
        viewportHeight
    );

    if (dragged) {
        const ScreenSelectionRect rectangle = makeScreenSelectionRect(
            startX,
            startY,
            currentX,
            currentY,
            viewportWidth,
            viewportHeight
        );
        std::vector<EntityId> selected = services_.partySelectionSystem.selectOwnedCharacters(
            services_.world,
            services_.frame,
            camera,
            viewportWidth,
            viewportHeight,
            rectangle
        );
        services_.partySelection.setSelection(std::move(selected));
        return;
    }

    std::optional<EntityId> picked = services_.pickingSystem.pick(
        services_.frame,
        camera,
        viewportWidth,
        viewportHeight,
        currentX,
        currentY,
        false
    );
    if (picked.has_value()) {
        const EntityId owner = services_.world.characterOwnerEntity(*picked);
        if (services_.partySelectionSystem.isControllablePartyMember(
                services_.world,
                owner
            )) {
            services_.partySelection.setSelection({owner});
            return;
        }
    }

    moveSelectedPartyToViewportPosition(currentX, currentY);
}

void Application::syncPartySelectionPresentation() {
    services_.partySelectionSystem.pruneInvalidSelection(
        services_.world,
        services_.partySelection
    );
    services_.partySelectionSystem.syncGroundIndicatorSelection(
        services_.world,
        services_.partySelection,
        services_.frame
    );
    services_.frame.partySelectionMarquee = FramePartySelectionMarquee{};
    const PartySelectionDragSession& drag = services_.input.partySelectionDrag;
    if (!modeSession_.capabilities().acceptsPartySelection || !drag.active ||
        !isPartySelectionDrag(drag.startX, drag.startY, drag.currentX, drag.currentY)) {
        return;
    }

    const ScreenSelectionRect rectangle = makeScreenSelectionRect(
        drag.startX,
        drag.startY,
        drag.currentX,
        drag.currentY,
        services_.renderer.width(),
        services_.renderer.height()
    );
    services_.frame.partySelectionMarquee.visible = true;
    services_.frame.partySelectionMarquee.min = glm::vec2(rectangle.minX, rectangle.minY);
    services_.frame.partySelectionMarquee.max = glm::vec2(rectangle.maxX, rectangle.maxY);
}

}  // namespace core
