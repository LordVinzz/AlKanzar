#include "RenderEngine.hpp"

#include <limits>

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "RenderPaths.hpp"

namespace render {

void RenderEngine::pollOcclusionQueries() {
    for (auto& [entity, state] : occlusionQueryStates_) {
        (void)entity;
        if (!state.queryInFlight || state.queryId == 0) {
            continue;
        }

        GLuint available = 0;
        glGetQueryObjectuiv(state.queryId, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) {
            continue;
        }

        GLuint passed = 0;
        glGetQueryObjectuiv(state.queryId, GL_QUERY_RESULT, &passed);
        if (passed != 0) {
            state.lastVisible = true;
            state.occludedFrameStreak = 0;
        } else {
            state.lastVisible = false;
            if (state.occludedFrameStreak < std::numeric_limits<std::uint8_t>::max()) {
                state.occludedFrameStreak++;
            }
        }
        state.hasLastResult = true;
        state.queryInFlight = false;
        recycleOcclusionQuery(state.queryId);
        state.queryId = 0;
    }
}

void RenderEngine::cleanupOcclusionStates() {
    constexpr std::uint64_t kMaxInactiveFrames = 3u;
    for (auto it = occlusionQueryStates_.begin(); it != occlusionQueryStates_.end();) {
        if (renderFrameNumber_ > it->second.lastFrameTouched &&
            renderFrameNumber_ - it->second.lastFrameTouched > kMaxInactiveFrames) {
            if (it->second.queryId != 0) {
                recycleOcclusionQuery(it->second.queryId);
            }
            it = occlusionQueryStates_.erase(it);
            continue;
        }
        ++it;
    }
}

void RenderEngine::recycleOcclusionQuery(GLuint queryId) {
    if (queryId != 0) {
        freeOcclusionQueries_.push_back(queryId);
    }
}

void RenderEngine::issueOcclusionQueries(const RenderPathContext& context) {
    if (occlusionShader_.id() == 0 || !occlusionBoundsMesh_.valid()) {
        latestOcclusionCullStats_.queryIssued = 0u;
        return;
    }

    renderPath_->prepareOcclusionQueryPass(context);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    const glm::mat4 viewProjection = context.camera.projection * context.camera.view;
    occlusionShader_.use();
    glUniform4f(occlusionColorLocation_, 1.0f, 1.0f, 1.0f, 1.0f);

    std::uint32_t issued = 0u;
    for (const RenderSceneObjectView& object : context.scene.objects) {
        if (!object.visible || !object.frustumVisible || !object.hasWorldBounds || object.layer == RenderLayer::Ground) {
            continue;
        }

        OcclusionQueryState& state = occlusionQueryStates_[object.entity];
        state.lastFrameTouched = renderFrameNumber_;
        if (state.queryInFlight) {
            continue;
        }

        if (state.queryId == 0) {
            if (!freeOcclusionQueries_.empty()) {
                state.queryId = freeOcclusionQueries_.back();
                freeOcclusionQueries_.pop_back();
            } else {
                glGenQueries(1, &state.queryId);
            }
        }
        if (state.queryId == 0) {
            continue;
        }

        glm::mat4 model(1.0f);
        const glm::vec3 center = (object.worldBounds.min + object.worldBounds.max) * 0.5f;
        const glm::vec3 extents = glm::max((object.worldBounds.max - object.worldBounds.min) * 0.5f, glm::vec3(0.01f));
        model = glm::translate(model, center);
        model = glm::scale(model, extents * 2.0f);
        const glm::mat4 mvp = viewProjection * model;
        glUniformMatrix4fv(occlusionMvpLocation_, 1, GL_FALSE, glm::value_ptr(mvp));

        glBeginQuery(GL_ANY_SAMPLES_PASSED, state.queryId);
        occlusionBoundsMesh_.draw();
        glEndQuery(GL_ANY_SAMPLES_PASSED);

        state.queryInFlight = true;
        issued++;
    }

    sceneView_.occlusionCullStats.queryIssued = issued;
    latestOcclusionCullStats_.queryIssued = issued;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, context.width, context.height);
}

}  // namespace render

