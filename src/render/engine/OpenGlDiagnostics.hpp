#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#pragma once

#include <SDL_opengl.h>

#include <algorithm>
#include <string_view>

#include <spdlog/spdlog.h>

namespace render::gl_diagnostics {

inline const char* errorName(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM:
            return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:
            return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:
            return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY:
            return "GL_OUT_OF_MEMORY";
        default:
            return "GL_UNKNOWN_ERROR";
    }
}

inline void beginCheckedOperation(
    std::string_view operation,
    std::string_view resourcePath
) {
    for (GLenum error = glGetError(); error != GL_NO_ERROR;
         error = glGetError()) {
        spdlog::warn(
            "OpenGL diagnostic: pre-existing {} (0x{:04X}) before '{}' "
            "for texture '{}'",
            errorName(error),
            error,
            operation,
            resourcePath
        );
    }
}

[[nodiscard]] inline bool reportErrors(
    std::string_view operation,
    std::string_view resourcePath
) {
    bool success = true;
    for (GLenum error = glGetError(); error != GL_NO_ERROR;
         error = glGetError()) {
        success = false;
        spdlog::error(
            "OpenGL texture error: operation='{}' path='{}' error={} "
            "(0x{:04X})",
            operation,
            resourcePath,
            errorName(error),
            error
        );
    }
    return success;
}

[[nodiscard]] inline bool validateBoundTexture2D(
    std::string_view resourcePath,
    int expectedWidth,
    int expectedHeight,
    int expectedMipLevels = 1
) {
    beginCheckedOperation("validate GL_TEXTURE_2D", resourcePath);
    const int mipLevelCount = std::max(expectedMipLevels, 1);
    int levelWidth = expectedWidth;
    int levelHeight = expectedHeight;
    for (int level = 0; level < mipLevelCount; ++level) {
        GLint width = 0;
        GLint height = 0;
        GLint internalFormat = 0;
        glGetTexLevelParameteriv(
            GL_TEXTURE_2D,
            level,
            GL_TEXTURE_WIDTH,
            &width
        );
        glGetTexLevelParameteriv(
            GL_TEXTURE_2D,
            level,
            GL_TEXTURE_HEIGHT,
            &height
        );
        glGetTexLevelParameteriv(
            GL_TEXTURE_2D,
            level,
            GL_TEXTURE_INTERNAL_FORMAT,
            &internalFormat
        );
        if (!reportErrors("query GL_TEXTURE_2D mip level", resourcePath)) {
            return false;
        }
        if (width != levelWidth || height != levelHeight ||
            internalFormat == 0) {
            spdlog::error(
                "OpenGL texture error: path='{}' has invalid mip level {} "
                "(expected={}x{}, actual={}x{}, internal_format=0x{:04X})",
                resourcePath,
                level,
                levelWidth,
                levelHeight,
                width,
                height,
                internalFormat
            );
            return false;
        }
        levelWidth = std::max(levelWidth / 2, 1);
        levelHeight = std::max(levelHeight / 2, 1);
    }
    return true;
}

inline const char* framebufferStatusName(GLenum status) {
    switch (status) {
        case GL_FRAMEBUFFER_COMPLETE:
            return "GL_FRAMEBUFFER_COMPLETE";
        case GL_FRAMEBUFFER_UNDEFINED:
            return "GL_FRAMEBUFFER_UNDEFINED";
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
            return "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER";
        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
            return "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER";
        case GL_FRAMEBUFFER_UNSUPPORTED:
            return "GL_FRAMEBUFFER_UNSUPPORTED";
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
            return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
        case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
            return "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS";
        default:
            return "GL_FRAMEBUFFER_UNKNOWN_STATUS";
    }
}

[[nodiscard]] inline bool validateFramebuffer(std::string_view resourcePath) {
    beginCheckedOperation("validate framebuffer", resourcePath);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (!reportErrors("glCheckFramebufferStatus", resourcePath)) {
        return false;
    }
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error(
            "OpenGL framebuffer error: path='{}' status={} (0x{:04X})",
            resourcePath,
            framebufferStatusName(status),
            status
        );
        return false;
    }
    return true;
}

}  // namespace render::gl_diagnostics
