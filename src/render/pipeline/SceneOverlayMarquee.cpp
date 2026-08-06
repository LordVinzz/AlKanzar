#include "SceneOverlayRenderer.hpp"

#include <vector>

#include <glm/ext/matrix_clip_space.hpp>

namespace render {

void SceneOverlayRenderer::renderPartySelectionMarquee(
    const RenderSceneView& scene,
    int width,
    int height
) const {
    const core::FramePartySelectionMarquee& marquee = scene.partySelectionMarquee;
    if (!marquee.visible || width <= 0 || height <= 0 || debugColorShader_.id() == 0) {
        return;
    }

    const std::vector<glm::vec3> vertices{
        glm::vec3(marquee.min.x, marquee.min.y, 0.0f),
        glm::vec3(marquee.max.x, marquee.min.y, 0.0f),
        glm::vec3(marquee.max.x, marquee.max.y, 0.0f),
        glm::vec3(marquee.min.x, marquee.max.y, 0.0f),
    };

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    drawLineVertices(
        vertices,
        glm::ortho(0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, -1.0f, 1.0f),
        glm::mat4(1.0f),
        marquee.color,
        GL_LINE_LOOP
    );

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthMask(GL_TRUE);
}

}  // namespace render
