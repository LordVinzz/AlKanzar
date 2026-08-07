#pragma once

#include <SDL.h>

#include "AppState.hpp"
#include "EngineServices.hpp"

namespace core {

class Application {
public:
    Application(int width, int height, AppMode startupMode = AppMode::Gameplay);

    void run();

private:
    void transitionTo(AppMode mode);
    void translateSdlEvent(const SDL_Event& event);
    void bindEventHandlers();
    void updateFreeCameraControls();
    void releaseFreeCameraMouse();
    void finishPartySelectionDrag(int x, int y);
    void syncPartySelectionPresentation();
    bool moveAgentToViewportPosition(EntityId agent, int x, int y);
    bool moveSelectedPartyToViewportPosition(int x, int y);

    EngineServices services_;
    AppStateCollection states_{};
    AppModeSession modeSession_{};
    IAppState* currentState_{nullptr};
};

}  // namespace core
