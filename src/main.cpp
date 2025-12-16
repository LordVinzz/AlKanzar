#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <SDL_opengl.h>
#include <iostream>

int main() {
    std::cout << "AlKanzar: checking SDL2 and OpenGL availability...\n";

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL2 not available: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_version compiled{};
    SDL_VERSION(&compiled);
    SDL_version linked{};
    SDL_GetVersion(&linked);
    std::cout << "SDL2 compiled against "
              << static_cast<int>(compiled.major) << '.'
              << static_cast<int>(compiled.minor) << '.'
              << static_cast<int>(compiled.patch)
              << " | linked "
              << static_cast<int>(linked.major) << '.'
              << static_cast<int>(linked.minor) << '.'
              << static_cast<int>(linked.patch)
              << "\n";

    bool glAvailable = SDL_GL_LoadLibrary(nullptr) == 0;
    if (glAvailable) {
        std::cout << "OpenGL library loaded successfully via SDL.\n";
        SDL_GL_UnloadLibrary();
    } else {
        std::cerr << "OpenGL not available: " << SDL_GetError() << "\n";
    }

    SDL_Quit();
    return glAvailable ? 0 : 2;
}
