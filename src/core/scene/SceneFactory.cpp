#include "SceneFactory.hpp"
#include "SceneMeshFactory.hpp"
#include "SceneModelFactory.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <SDL.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include "core/simulation/CharacterSimulation.hpp"
#include "core/transform/TransformMath.hpp"
#include "render/resources/Geometry.hpp"
#include "render/engine/RenderEngine.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {

bool SceneFactory::buildScene(
    const SceneBlueprint& blueprint,
    World& world,
    render::RenderEngine& renderer
) const {
    world.clear();

    const std::string texturesRoot = scene_detail::assetRootPath("textures/");
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

    const std::shared_ptr<render::Material> groundMaterial = createProceduralMaterial(
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
    );
    const std::shared_ptr<render::Material> wallRockMaterial = createProceduralMaterial(
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
    );
    const std::shared_ptr<render::Material> wallWoodMaterial = createProceduralMaterial(
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
    );

    auto createRenderableEntity = [&world](
        const std::string& name,
        const TransformComponent& transform,
        const render::Mesh& meshData,
        const render::Bounds3& bounds,
        render::MeshHandle mesh,
        const std::shared_ptr<render::Material>& material,
        render::RenderLayer layer
    ) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{name});
        world.transforms.emplace(entity, transform);
        world.bounds.emplace(entity, BoundsComponent{bounds});
        world.visibilities.emplace(entity, VisibilityComponent{true});
        world.renderables.emplace(entity, RenderableComponent{mesh, layer});
        world.materials.emplace(entity, MaterialComponent{material});
        world.navSourceGeometry.emplace(entity, NavSourceGeometryComponent{std::make_shared<render::Mesh>(meshData)});
        world.markTransformsDirty(entity);
        return entity;
    };

    const auto materialForPreset = [&](SceneMaterialPreset preset) {
        switch (preset) {
            case SceneMaterialPreset::Soil: return groundMaterial;
            case SceneMaterialPreset::Rock: return wallRockMaterial;
            case SceneMaterialPreset::Wood: return wallWoodMaterial;
        }
        return wallRockMaterial;
    };
    for (const ScenePrimitiveBlueprint& primitive : blueprint.primitives) {
        const render::Mesh mesh = primitive.shape == ScenePrimitiveShape::Plane
            ? SceneMeshFactory::createPlane(glm::vec4(1.0f))
            : SceneMeshFactory::createBox(
                glm::vec3(-0.5f),
                glm::vec3(0.5f),
                glm::vec4(1.0f)
            );
        const render::MeshHandle meshHandle = renderer.uploadMesh(mesh);
        if (!meshHandle.valid()) {
            spdlog::error("SceneFactory: failed to upload primitive '{}'", primitive.id);
            return false;
        }
        const EntityId entity = createRenderableEntity(
            primitive.name,
            primitive.transform,
            mesh,
            SceneMeshFactory::computeBounds(mesh),
            meshHandle,
            materialForPreset(primitive.material),
            primitive.layer
        );
        world.authoredSceneObjects.emplace(entity, AuthoredSceneObjectComponent{primitive.id});
    }

    for (const LightVolumeBlueprint& volumeBlueprint : blueprint.lightVolumes) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{volumeBlueprint.name});
        world.authoredSceneObjects.emplace(entity, AuthoredSceneObjectComponent{volumeBlueprint.id});
        world.transforms.emplace(entity, volumeBlueprint.transform);
        world.lightVolumes.emplace(entity, LightVolumeComponent{volumeBlueprint.halfExtents});
        world.markTransformsDirty(entity);
        world.markLightsDirty(entity);
    }

    const std::string modelRoot = scene_detail::assetRootPath("models/");
    for (const ModelInstanceBlueprint& modelBlueprint : blueprint.models) {
        auto modelAsset = std::make_shared<render::GltfModelData>();
        if (!render::loadGltfModel(modelRoot + modelBlueprint.path, *modelAsset)) {
            spdlog::error("SceneFactory: failed to load model '{}'", modelBlueprint.path);
            return false;
        }

        if (modelBlueprint.fitToFootprint &&
            !scene_detail::fitModelToFootprint(*modelAsset, modelBlueprint.footprint)) {
            spdlog::error("SceneFactory: failed to fit model '{}'", modelBlueprint.path);
            return false;
        }

        if (modelBlueprint.materialProfile == SceneModelMaterialProfile::House) {
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
        world.authoredSceneObjects.emplace(rootEntity, AuthoredSceneObjectComponent{modelBlueprint.id});
        world.transforms.emplace(rootEntity, modelBlueprint.transform);
        world.visibilities.emplace(rootEntity, VisibilityComponent{true});
        if (modelAsset->animated()) {
            world.animatedModels.emplace(rootEntity, AnimatedModelComponent{modelAsset});
        }
        if (modelBlueprint.character.has_value()) {
            CharacterBlueprint character = *modelBlueprint.character;
            normalizeCharacterComponents(
                character.character,
                character.abilities,
                character.skills,
                character.vitals
            );
            world.characters.emplace(rootEntity, character.character);
            world.characterControllers.emplace(rootEntity, character.controller);
            if (character.partyMember.has_value()) {
                world.partyMembers.emplace(rootEntity, *character.partyMember);
            }
            world.abilityScores.emplace(rootEntity, character.abilities);
            world.skillRanks.emplace(rootEntity, character.skills);
            world.characterVitals.emplace(rootEntity, character.vitals);
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
            const EntityId sectionEntity = world.createEntity();
            world.names.emplace(sectionEntity, NameComponent{modelBlueprint.name + " / " + section.name});
            world.parents.emplace(sectionEntity, ParentComponent{rootEntity});
            if (section.nodeIndex >= 0 && section.nodeIndex < static_cast<int>(modelAsset->nodes.size())) {
                render::NodeTransform nodeTransform{};
                if (render::decomposeNodeTransform(
                        modelAsset->nodes[static_cast<std::size_t>(section.nodeIndex)].bindGlobalMatrix,
                        nodeTransform
                    )) {
                    world.transforms.emplace(
                        sectionEntity,
                        scene_detail::transformFromNodeTransform(nodeTransform)
                    );
                }
            }
            world.bounds.emplace(sectionEntity, BoundsComponent{SceneMeshFactory::computeBounds(section.mesh)});
            world.visibilities.emplace(sectionEntity, VisibilityComponent{true});
            world.renderables.emplace(sectionEntity, RenderableComponent{meshHandle, modelBlueprint.layer});
            world.materials.emplace(sectionEntity, MaterialComponent{material});
            world.navSourceGeometry.emplace(sectionEntity, NavSourceGeometryComponent{std::make_shared<render::Mesh>(section.mesh)});
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

    if (blueprint.directionalLight.has_value()) {
        const DirectionalLightBlueprint& lightBlueprint = *blueprint.directionalLight;
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{lightBlueprint.name});
        world.authoredSceneObjects.emplace(entity, AuthoredSceneObjectComponent{lightBlueprint.id});
        world.directionalLights.emplace(entity, DirectionalLightComponent{
            lightBlueprint.direction,
            lightBlueprint.color,
            lightBlueprint.intensity
        });
        world.markLightsDirty(entity);
    }

    for (const PointLightBlueprint& lightBlueprint : blueprint.pointLights) {
        const EntityId entity = world.createEntity();
        world.names.emplace(entity, NameComponent{lightBlueprint.name});
        world.authoredSceneObjects.emplace(entity, AuthoredSceneObjectComponent{lightBlueprint.id});
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
        world.authoredSceneObjects.emplace(entity, AuthoredSceneObjectComponent{lightBlueprint.id});
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
