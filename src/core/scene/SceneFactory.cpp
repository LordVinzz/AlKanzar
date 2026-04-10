#include "SceneFactory.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include <SDL.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include "core/transform/TransformMath.hpp"
#include "render/resources/Geometry.hpp"
#include "render/engine/RenderEngine.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
};

void pushVertex(const Vertex& vertex, render::Mesh& outMesh) {
    outMesh.positions.push_back(vertex.position);
    outMesh.normals.push_back(vertex.normal);
    outMesh.colors.push_back(vertex.color);
    if (outMesh.uvSets.size() < 2) {
        outMesh.uvSets.resize(2);
    }
    outMesh.uvSets[0].push_back(vertex.uv0);
    outMesh.uvSets[1].push_back(vertex.uv1);
}

void addQuad(const std::array<Vertex, 4>& verts, render::Mesh& outMesh) {
    const unsigned int base = static_cast<unsigned int>(outMesh.positions.size());
    for (const auto& vertex : verts) {
        pushVertex(vertex, outMesh);
    }
    outMesh.indices.insert(outMesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void addBox(
    const glm::vec3& minCorner,
    const glm::vec3& maxCorner,
    const glm::vec4& color,
    render::Mesh& outMesh
) {
    const float minX = minCorner.x;
    const float minY = minCorner.y;
    const float minZ = minCorner.z;
    const float maxX = maxCorner.x;
    const float maxY = maxCorner.y;
    const float maxZ = maxCorner.z;

    const std::array<Vertex, 4> rightFace{{
        {glm::vec3(maxX, minY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(rightFace, outMesh);

    const std::array<Vertex, 4> leftFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(leftFace, outMesh);

    const std::array<Vertex, 4> topFace{{
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(topFace, outMesh);

    const std::array<Vertex, 4> bottomFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(bottomFace, outMesh);

    const std::array<Vertex, 4> frontFace{{
        {glm::vec3(minX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, maxZ), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
    }};
    addQuad(frontFace, outMesh);

    const std::array<Vertex, 4> backFace{{
        {glm::vec3(minX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), color},
        {glm::vec3(minX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, maxY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), color},
        {glm::vec3(maxX, minY, minZ), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), color},
    }};
    addQuad(backFace, outMesh);
}

render::Bounds3 computeMeshBounds(const render::Mesh& mesh) {
    render::Bounds3 bounds{};
    if (mesh.positions.empty()) {
        return bounds;
    }

    bounds.min = mesh.positions.front();
    bounds.max = bounds.min;
    for (const glm::vec3& position : mesh.positions) {
        bounds.min = glm::min(bounds.min, position);
        bounds.max = glm::max(bounds.max, position);
    }
    return bounds;
}

render::Bounds3 computeModelBounds(const render::GltfModelData& model) {
    render::Bounds3 bounds{};
    bool hasBounds = false;
    for (const auto& section : model.sections) {
        if (section.mesh.positions.empty()) {
            continue;
        }
        glm::mat4 nodeMatrix(1.0f);
        if (section.nodeIndex >= 0 && section.nodeIndex < static_cast<int>(model.nodes.size())) {
            nodeMatrix = model.nodes[static_cast<std::size_t>(section.nodeIndex)].bindGlobalMatrix;
        }

        render::Bounds3 sectionBounds{};
        bool hasSectionBounds = false;
        for (const glm::vec3& position : section.mesh.positions) {
            const glm::vec3 worldPosition = glm::vec3(nodeMatrix * glm::vec4(position, 1.0f));
            if (!hasSectionBounds) {
                sectionBounds.min = worldPosition;
                sectionBounds.max = worldPosition;
                hasSectionBounds = true;
                continue;
            }
            sectionBounds.min = glm::min(sectionBounds.min, worldPosition);
            sectionBounds.max = glm::max(sectionBounds.max, worldPosition);
        }
        if (!hasSectionBounds) {
            continue;
        }
        if (!hasBounds) {
            bounds = sectionBounds;
            hasBounds = true;
            continue;
        }
        bounds.min = glm::min(bounds.min, sectionBounds.min);
        bounds.max = glm::max(bounds.max, sectionBounds.max);
    }
    return bounds;
}

core::TransformComponent transformFromNodeTransform(const render::NodeTransform& transform) {
    return core::TransformComponent{
        transform.translation,
        glm::degrees(glm::eulerAngles(transform.rotation)),
        transform.scale
    };
}

bool fitModelToFootprint(render::GltfModelData& model, float targetFootprint) {
    const render::Bounds3 bounds = computeModelBounds(model);
    const glm::vec3 size = bounds.max - bounds.min;
    const float footprint = std::max(size.x, size.z);
    if (footprint <= 1.0e-4f) {
        return false;
    }

    const float scale = targetFootprint / footprint;
    for (int rootNodeIndex : model.sceneRootNodes) {
        if (rootNodeIndex < 0 || rootNodeIndex >= static_cast<int>(model.nodes.size())) {
            continue;
        }
        model.nodes[static_cast<std::size_t>(rootNodeIndex)].localTransform.scale *= glm::vec3(scale);
    }
    render::refreshModelBindPose(model);
    return true;
}

std::string assetRootPath(const char* subdir) {
    char* basePath = SDL_GetBasePath();
    std::string root = basePath ? basePath : "";
    if (basePath) {
        SDL_free(basePath);
    }
    return root + subdir;
}

}  // namespace

namespace core {

bool SceneFactory::buildScene(
    const SceneBlueprint& blueprint,
    World& world,
    MaterialLibrary& materials,
    render::RenderEngine& renderer
) const {
    world.clear();
    materials.clear();
    world.lightVolumes.emplace_back(glm::vec3(-100.0f), glm::vec3(100.0f));

    const std::string texturesRoot = assetRootPath("textures/");
    const auto soilBase = renderer.registerTexture(render::loadTextureFromFile(
        texturesRoot + "soil.jpg",
        "SoilBaseColor",
        true,
        render::TextureSemantic::BaseColor
    ));
    const auto woodBase = renderer.registerTexture(render::loadTextureFromFile(
        texturesRoot + "wood.jpg",
        "WoodBaseColor",
        true,
        render::TextureSemantic::BaseColor
    ));
    const auto rockBase = renderer.registerTexture(render::loadTextureFromFile(
        texturesRoot + "rock.jpg",
        "RockBaseColor",
        true,
        render::TextureSemantic::BaseColor
    ));

    const auto soilNormal = soilBase ? renderer.registerTexture(render::generateNormalTexture(*soilBase, "SoilNormal")) : nullptr;
    const auto soilOrm = soilBase ? renderer.registerTexture(render::generateMetallicRoughnessTexture(*soilBase, "SoilORM", 0.0f, 0.10f)) : nullptr;
    const auto soilAo = soilBase ? renderer.registerTexture(render::generateOcclusionTexture(*soilBase, "SoilAO")) : nullptr;
    const auto soilHeight = soilBase ? renderer.registerTexture(render::generateHeightTexture(*soilBase, "SoilHeight")) : nullptr;

    const auto woodNormal = woodBase ? renderer.registerTexture(render::generateNormalTexture(*woodBase, "WoodNormal", 5.0f)) : nullptr;
    const auto woodOrm = woodBase ? renderer.registerTexture(render::generateMetallicRoughnessTexture(*woodBase, "WoodORM", 0.0f, -0.08f)) : nullptr;
    const auto woodAo = woodBase ? renderer.registerTexture(render::generateOcclusionTexture(*woodBase, "WoodAO")) : nullptr;
    const auto woodHeight = woodBase ? renderer.registerTexture(render::generateHeightTexture(*woodBase, "WoodHeight")) : nullptr;

    const auto rockNormal = rockBase ? renderer.registerTexture(render::generateNormalTexture(*rockBase, "RockNormal", 6.0f)) : nullptr;
    const auto rockOrm = rockBase ? renderer.registerTexture(render::generateMetallicRoughnessTexture(*rockBase, "RockORM", 0.0f, 0.14f)) : nullptr;
    const auto rockAo = rockBase ? renderer.registerTexture(render::generateOcclusionTexture(*rockBase, "RockAO")) : nullptr;
    const auto rockHeight = rockBase ? renderer.registerTexture(render::generateHeightTexture(*rockBase, "RockHeight")) : nullptr;

    const auto applyTextureSet = [&renderer](
        render::Material& material,
        const std::shared_ptr<render::Texture>& baseColor,
        const std::shared_ptr<render::Texture>& normal,
        const std::shared_ptr<render::Texture>& orm,
        const std::shared_ptr<render::Texture>& ao,
        const std::shared_ptr<render::Texture>& height,
        const glm::vec2& uvScale,
        float clearcoatFactor,
        float clearcoatRoughness,
        float detailNormalScale,
        float heightScale
    ) {
        render::UVTransform transform{};
        transform.scale = uvScale;
        material.baseColor = render::makeTextureRef(baseColor, renderer.defaultSampler(), 0, transform);
        material.normal = render::makeTextureRef(normal, renderer.defaultSampler(), 0, transform);
        material.metallicRoughness.texture = render::makeTextureRef(orm, renderer.defaultSampler(), 0, transform);
        material.ao = render::makeTextureRef(ao, renderer.defaultSampler(), 0, transform);
        material.height.texture = render::makeTextureRef(height, renderer.defaultSampler(), 0, transform);
        material.detailNormal.texture = render::makeTextureRef(normal, renderer.defaultSampler(), 0, transform);
        material.clearcoat.factor = clearcoatFactor;
        material.clearcoat.roughness = clearcoatRoughness;
        material.detailNormal.scale = detailNormalScale;
        material.height.scale = heightScale;
    };

    const auto createProceduralMaterial = [&renderer](
        const std::string& name,
        const std::shared_ptr<render::Texture>& baseColor,
        const std::shared_ptr<render::Texture>& normal,
        const std::shared_ptr<render::Texture>& orm,
        const std::shared_ptr<render::Texture>& ao,
        const std::shared_ptr<render::Texture>& height,
        const glm::vec3& tint,
        const glm::vec2& uvScale,
        float clearcoatFactor,
        float clearcoatRoughness,
        float detailNormalScale
    ) {
        auto material = std::make_shared<render::Material>();
        material->name = name;
        material->baseColorFactor = tint;
        material->clearcoat.factor = clearcoatFactor;
        material->clearcoat.roughness = clearcoatRoughness;
        material->detailNormal.scale = detailNormalScale;
        material->height.scale = 0.03f;

        render::UVTransform transform{};
        transform.scale = uvScale;
        material->baseColor = render::makeTextureRef(baseColor, renderer.defaultSampler(), 0, transform);
        material->normal = render::makeTextureRef(normal, renderer.defaultSampler(), 0, transform);
        material->metallicRoughness.texture = render::makeTextureRef(orm, renderer.defaultSampler(), 0, transform);
        material->ao = render::makeTextureRef(ao, renderer.defaultSampler(), 0, transform);
        material->height.texture = render::makeTextureRef(height, renderer.defaultSampler(), 0, transform);
        material->detailNormal.texture = render::makeTextureRef(normal, renderer.defaultSampler(), 0, transform);
        return material;
    };

    const MaterialHandle groundMaterial = materials.add(createProceduralMaterial(
        "Ground Soil",
        soilBase,
        soilNormal,
        soilOrm,
        soilAo,
        soilHeight,
        glm::vec3(0.92f, 0.96f, 0.90f),
        glm::vec2(120.0f, 120.0f),
        0.0f,
        0.0f,
        0.85f
    ));
    const MaterialHandle wallRockMaterial = materials.add(createProceduralMaterial(
        "Wall Rock",
        rockBase,
        rockNormal,
        rockOrm,
        rockAo,
        rockHeight,
        glm::vec3(0.86f, 0.80f, 0.76f),
        glm::vec2(3.5f, 1.4f),
        0.0f,
        0.0f,
        1.15f
    ));
    const MaterialHandle wallWoodMaterial = materials.add(createProceduralMaterial(
        "Wall Wood",
        woodBase,
        woodNormal,
        woodOrm,
        woodAo,
        woodHeight,
        glm::vec3(0.98f, 0.94f, 0.86f),
        glm::vec2(2.5f, 1.25f),
        0.20f,
        0.25f,
        0.45f
    ));

    render::Mesh groundMesh;
    groundMesh.uvSets.resize(2);
    addQuad(
        {{
            {glm::vec3(-blueprint.groundHalfExtent, 0.0f, -blueprint.groundHalfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(-blueprint.groundHalfExtent, 0.0f, blueprint.groundHalfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(blueprint.groundHalfExtent, 0.0f, blueprint.groundHalfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
            {glm::vec3(blueprint.groundHalfExtent, 0.0f, -blueprint.groundHalfExtent), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), glm::vec4(1.0f)},
        }},
        groundMesh
    );

    render::Mesh wallAMesh;
    render::Mesh wallBMesh;
    addBox(
        glm::vec3(-blueprint.wallThickness * 0.5f, 0.0f, -blueprint.wallLength),
        glm::vec3(blueprint.wallThickness * 0.5f, blueprint.wallHeight, blueprint.wallLength),
        glm::vec4(1.0f),
        wallAMesh
    );
    addBox(
        glm::vec3(-blueprint.wallThickness * 0.5f, 0.0f, -blueprint.wallLength),
        glm::vec3(blueprint.wallThickness * 0.5f, blueprint.wallHeight, blueprint.wallLength),
        glm::vec4(1.0f),
        wallBMesh
    );

    const render::MeshHandle groundMeshHandle = renderer.uploadMesh(groundMesh);
    const render::MeshHandle wallAMeshHandle = renderer.uploadMesh(wallAMesh);
    const render::MeshHandle wallBMeshHandle = renderer.uploadMesh(wallBMesh);
    if (!groundMeshHandle.valid() || !wallAMeshHandle.valid() || !wallBMeshHandle.valid()) {
        spdlog::error("SceneFactory: failed to upload procedural scene meshes");
        return false;
    }

    auto createRenderableEntity = [&world](
        const std::string& name,
        const TransformComponent& transform,
        const render::Bounds3& bounds,
        render::MeshHandle mesh,
        MaterialHandle material,
        render::RenderLayer layer
    ) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{name});
        world.transforms.emplace(entity, transform);
        world.bounds.emplace(entity, BoundsComponent{bounds});
        world.visibilities.emplace(entity, VisibilityComponent{true});
        world.renderables.emplace(entity, RenderableComponent{mesh, material, layer});
        world.markTransformsDirty(entity);
        return entity;
    };

    createRenderableEntity(
        "Ground",
        TransformComponent{},
        computeMeshBounds(groundMesh),
        groundMeshHandle,
        groundMaterial,
        render::RenderLayer::Ground
    );
    createRenderableEntity(
        "Wall A",
        TransformComponent{glm::vec3(-blueprint.wallOffset, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
        computeMeshBounds(wallAMesh),
        wallAMeshHandle,
        wallRockMaterial,
        render::RenderLayer::Geometry
    );
    createRenderableEntity(
        "Wall B",
        TransformComponent{glm::vec3(blueprint.wallOffset, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
        computeMeshBounds(wallBMesh),
        wallBMeshHandle,
        wallWoodMaterial,
        render::RenderLayer::Geometry
    );

    const std::string modelRoot = assetRootPath("models/");
    for (const ModelInstanceBlueprint& modelBlueprint : blueprint.models) {
        auto modelAsset = std::make_shared<render::GltfModelData>();
        if (!render::loadGltfModel(modelRoot + modelBlueprint.path, *modelAsset)) {
            spdlog::error("SceneFactory: failed to load model '{}'", modelBlueprint.path);
            return false;
        }

        if (modelBlueprint.fitToFootprint && !fitModelToFootprint(*modelAsset, modelBlueprint.footprint)) {
            spdlog::error("SceneFactory: failed to fit model '{}'", modelBlueprint.path);
            return false;
        }

        if (modelBlueprint.name == "House") {
            for (auto& section : modelAsset->sections) {
                if (!section.material) {
                    section.material = std::make_shared<render::Material>();
                    section.material->name = section.name;
                }

                std::string lowered = section.material->name;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });

                if (lowered.find("wood") != std::string::npos) {
                    applyTextureSet(*section.material, woodBase, woodNormal, woodOrm, woodAo, woodHeight, glm::vec2(1.6f), 0.22f, 0.20f, 0.40f, 0.02f);
                } else if (lowered.find("stone") != std::string::npos) {
                    applyTextureSet(*section.material, rockBase, rockNormal, rockOrm, rockAo, rockHeight, glm::vec2(1.5f), 0.0f, 0.0f, 1.15f, 0.04f);
                } else if (lowered.find("plaster") != std::string::npos) {
                    applyTextureSet(*section.material, soilBase, soilNormal, soilOrm, soilAo, soilHeight, glm::vec2(1.1f), 0.03f, 0.45f, 0.35f, 0.015f);
                } else if (lowered.find("roof") != std::string::npos) {
                    applyTextureSet(*section.material, rockBase, rockNormal, rockOrm, rockAo, rockHeight, glm::vec2(2.3f), 0.0f, 0.0f, 0.95f, 0.03f);
                    section.material->baseColorFactor *= glm::vec3(1.05f, 0.82f, 0.72f);
                } else if (lowered.find("window") != std::string::npos) {
                    applyTextureSet(*section.material, woodBase, woodNormal, woodOrm, woodAo, woodHeight, glm::vec2(1.2f), 0.12f, 0.18f, 0.25f, 0.01f);
                    section.material->baseColorFactor *= glm::vec3(0.88f, 0.92f, 1.0f);
                }
            }
        }

        const EntityId rootEntity = world.createEntity();
        world.names.emplace(rootEntity, NameComponent{modelBlueprint.name});
        world.transforms.emplace(rootEntity, modelBlueprint.transform);
        world.visibilities.emplace(rootEntity, VisibilityComponent{true});
        if (modelAsset->animated()) {
            world.animatedModels.emplace(rootEntity, AnimatedModelComponent{modelAsset});
        }
        world.markTransformsDirty(rootEntity);

        for (std::size_t sectionIndex = 0; sectionIndex < modelAsset->sections.size(); ++sectionIndex) {
            const auto& section = modelAsset->sections[sectionIndex];
            const render::MeshHandle meshHandle = renderer.uploadMesh(section.mesh);
            if (!meshHandle.valid()) {
                spdlog::warn("SceneFactory: skipped model section '{}'", section.name);
                continue;
            }

            auto material = section.material ? section.material : std::make_shared<render::Material>();
            if (material->name.empty()) {
                material->name = section.name;
            }
            const MaterialHandle materialHandle = materials.add(material);

            const EntityId sectionEntity = world.createEntity();
            world.names.emplace(sectionEntity, NameComponent{modelBlueprint.name + " / " + section.name});
            world.parents.emplace(sectionEntity, ParentComponent{rootEntity});
            if (section.nodeIndex >= 0 && section.nodeIndex < static_cast<int>(modelAsset->nodes.size())) {
                render::NodeTransform nodeTransform{};
                if (render::decomposeNodeTransform(
                        modelAsset->nodes[static_cast<std::size_t>(section.nodeIndex)].bindGlobalMatrix,
                        nodeTransform
                    )) {
                    world.transforms.emplace(sectionEntity, transformFromNodeTransform(nodeTransform));
                }
            }
            world.bounds.emplace(sectionEntity, BoundsComponent{computeMeshBounds(section.mesh)});
            world.visibilities.emplace(sectionEntity, VisibilityComponent{true});
            world.renderables.emplace(sectionEntity, RenderableComponent{meshHandle, materialHandle, modelBlueprint.layer});
            if (section.skinIndex >= 0 && modelAsset->animated()) {
                world.skinnedRenderables.emplace(sectionEntity, SkinnedRenderableComponent{
                    rootEntity,
                    section.skinIndex,
                    section.nodeIndex,
                    static_cast<int>(sectionIndex)
                });
            }
            world.markTransformsDirty(sectionEntity);
        }
    }

    for (const PointLightBlueprint& lightBlueprint : blueprint.pointLights) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{lightBlueprint.name});
        world.transforms.emplace(entity, lightBlueprint.transform);
        world.pointLights.emplace(entity, PointLightComponent{
            lightBlueprint.radius,
            lightBlueprint.color,
            lightBlueprint.intensity,
            lightBlueprint.phase,
            lightBlueprint.isMovable,
            lightBlueprint.castsShadow,
            lightBlueprint.shadowBiasMin,
            lightBlueprint.shadowBiasSlope
        });
        world.markTransformsDirty(entity);
        world.markLightsDirty(entity);
    }

    for (const SpotLightBlueprint& lightBlueprint : blueprint.spotLights) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{lightBlueprint.name});
        world.transforms.emplace(entity, lightBlueprint.transform);
        world.spotLights.emplace(entity, SpotLightComponent{
            lightBlueprint.radius,
            lightBlueprint.color,
            lightBlueprint.intensity,
            lightBlueprint.target,
            lightBlueprint.innerAngle,
            lightBlueprint.outerAngle,
            lightBlueprint.phase,
            lightBlueprint.isMovable,
            lightBlueprint.castsShadow,
            lightBlueprint.shadowBiasMin,
            lightBlueprint.shadowBiasSlope
        });
        world.markTransformsDirty(entity);
        world.markLightsDirty(entity);
    }

    return true;
}

}  // namespace core
