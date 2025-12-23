#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <SDL_opengl.h>

#include <string_view>

#include "render/RenderEngine.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

int main(int argc, char** argv) {
    bool runRenderTest = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "--test" || arg == "-t") && i + 1 < argc) {
            if (std::string_view{argv[i + 1]} == "RenderEngine") {
                runRenderTest = true;
            }
        }
    }

    // Thread pool async
    spdlog::init_thread_pool(8192, 1);

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "AlKanzar.log", false
    );
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

    std::vector<spdlog::sink_ptr> sinks {
        console_sink,
        file_sink
    };

    auto logger = std::make_shared<spdlog::async_logger>(
        "AlKanzar",
        sinks.begin(),
        sinks.end(),
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest
    );

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    spdlog::set_level(spdlog::level::trace);

    // Flush auto
    spdlog::flush_on(spdlog::level::err);
    spdlog::flush_every(std::chrono::seconds(1));

    spdlog::info("AlKanzar: checking SDL2 and OpenGL availability...");

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        spdlog::error("SDL2 not available: {}", SDL_GetError());
        spdlog::shutdown();
        return 1;
    }

    SDL_version compiled{};
    SDL_VERSION(&compiled);
    SDL_version linked{};
    SDL_GetVersion(&linked);

    spdlog::info(
        "SDL2 compiled {}.{}.{} | linked {}.{}.{}",
        compiled.major, compiled.minor, compiled.patch,
        linked.major, linked.minor, linked.patch
    );

    bool glAvailable = SDL_GL_LoadLibrary(nullptr) == 0;
    if (glAvailable) {
        spdlog::info("OpenGL library loaded successfully via SDL.");
    } else {
        spdlog::error("OpenGL not available: {}", SDL_GetError());
    }

    if (runRenderTest && glAvailable) {
        render::RenderEngine engine(1280, 720);
        engine.run();
    }

    if (glAvailable) {
        SDL_GL_UnloadLibrary();
    }

    SDL_Quit();
    spdlog::shutdown();
    return glAvailable ? 0 : 2;
}
