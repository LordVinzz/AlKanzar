#pragma once

#include <SDL.h>

#include "AppState.hpp"
#include "EngineServices.hpp"

namespace core {

class Application {
public:
    Application(int width, int height);

    void run();

private:
    void transitionTo(AppMode mode);
    void translateSdlEvent(const SDL_Event& event);
    void bindEventHandlers();
    void updateFreeCameraControls();
    void releaseFreeCameraMouse();

    EngineServices services_;
    BootstrapState bootstrapState_{};
    GameplayState gameplayState_{};
    EditorState editorState_{};
    ShutdownState shutdownState_{};
    IAppState* currentState_{nullptr};
    AppMode currentMode_{AppMode::Shutdown};
};

}  // namespace core
