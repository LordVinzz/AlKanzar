#include "RenderEngine.hpp"

namespace render {

MeshBuffer* RenderEngine::createSceneMesh(const Mesh& mesh) {
    auto buffer = std::make_unique<MeshBuffer>();
    if (!buffer->upload(mesh)) {
        return nullptr;
    }

    MeshBuffer* raw = buffer.get();
    sceneMeshes_.push_back(std::move(buffer));
    return raw;
}

MeshHandle RenderEngine::uploadMesh(const Mesh& mesh) {
    MeshBuffer* uploaded = createSceneMesh(mesh);
    if (!uploaded) {
        return {};
    }
    return MeshHandle{sceneMeshes_.size() - 1u};
}

void RenderEngine::uploadJointMatrices(const std::vector<glm::mat4>& jointMatrices) {
    const GLsizeiptr bufferSize = static_cast<GLsizeiptr>(jointMatrices.size() * sizeof(glm::mat4));
    if (bufferSize == 0) {
        jointMatrixBufferSize_ = 0;
        return;
    }

    if (jointMatrixBuffer_ == 0) {
        glGenBuffers(1, &jointMatrixBuffer_);
    }
    if (jointMatrixTexture_ == 0) {
        glGenTextures(1, &jointMatrixTexture_);
    }

    glBindBuffer(GL_TEXTURE_BUFFER, jointMatrixBuffer_);
    if (jointMatrixBufferSize_ != bufferSize) {
        glBufferData(GL_TEXTURE_BUFFER, bufferSize, jointMatrices.data(), GL_DYNAMIC_DRAW);
        jointMatrixBufferSize_ = bufferSize;
    } else {
        glBufferSubData(GL_TEXTURE_BUFFER, 0, bufferSize, jointMatrices.data());
    }

    glBindTexture(GL_TEXTURE_BUFFER, jointMatrixTexture_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, jointMatrixBuffer_);
}

std::shared_ptr<Texture> RenderEngine::registerTexture(const std::shared_ptr<Texture>& texture) {
    return resourceRegistry_.registerTexture(texture);
}

std::shared_ptr<Sampler> RenderEngine::registerSampler(const std::shared_ptr<Sampler>& sampler) {
    return resourceRegistry_.registerSampler(sampler);
}

std::vector<std::shared_ptr<Texture>> RenderEngine::textureCatalog(TextureSemantic preferredSemantic) const {
    return resourceRegistry_.textureCatalog(preferredSemantic);
}

void* RenderEngine::texturePreviewId(const std::shared_ptr<Texture>& texture) {
    return resourceRegistry_.texturePreviewId(texture);
}

}  // namespace render

