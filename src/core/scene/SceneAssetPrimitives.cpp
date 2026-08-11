#include "SceneAssetDetail.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace core::scene_asset_detail {
namespace {

bool parsePrimitiveShape(
    std::string_view token,
    ScenePrimitiveShape& outShape,
    std::string* error,
    std::string_view path
) {
    if (token == "Plane") {
        outShape = ScenePrimitiveShape::Plane;
        return true;
    }
    if (token == "Box") {
        outShape = ScenePrimitiveShape::Box;
        return true;
    }
    return fail(error, std::string(path) + ".shape", "contains an unknown primitive shape");
}

bool parseMaterialPreset(
    std::string_view token,
    SceneMaterialPreset& outMaterial,
    std::string* error,
    std::string_view path
) {
    if (token == "Soil") {
        outMaterial = SceneMaterialPreset::Soil;
        return true;
    }
    if (token == "Rock") {
        outMaterial = SceneMaterialPreset::Rock;
        return true;
    }
    if (token == "Wood") {
        outMaterial = SceneMaterialPreset::Wood;
        return true;
    }
    return fail(error, std::string(path) + ".material", "contains an unknown material preset");
}

bool parseRenderLayer(
    std::string_view token,
    render::RenderLayer& outLayer,
    std::string* error,
    std::string_view path
) {
    if (token == "Ground") {
        outLayer = render::RenderLayer::Ground;
        return true;
    }
    if (token == "Geometry") {
        outLayer = render::RenderLayer::Geometry;
        return true;
    }
    if (token == "Actors") {
        outLayer = render::RenderLayer::Actors;
        return true;
    }
    return fail(error, std::string(path) + ".layer", "contains an unknown render layer");
}

}  // namespace

bool parsePrimitiveObject(
    lua_State* state,
    int objectIndex,
    int parametersIndex,
    SceneBlueprint& blueprint,
    std::string* error,
    std::string_view path,
    std::uint32_t version
) {
    if (version < 2u) {
        return fail(error, path, "Primitive objects require SCN V2");
    }
    if (!validateStringFields(
            state,
            parametersIndex,
            {"type", "id", "name", "shape", "material", "layer"},
            error,
            path)) {
        return false;
    }

    ScenePrimitiveBlueprint primitive{};
    std::string shapeToken{};
    std::string materialToken{};
    std::string layerToken{"Geometry"};
    if (!readSceneObjectIdentity(
            state,
            objectIndex,
            parametersIndex,
            "Primitive",
            version,
            blueprint,
            primitive.id,
            primitive.parentId,
            error,
            path) ||
        !readStringField(state, parametersIndex, "name", primitive.name, true, error, path) ||
        !readStringField(state, parametersIndex, "shape", shapeToken, true, error, path) ||
        !readStringField(state, parametersIndex, "material", materialToken, true, error, path) ||
        !readStringField(state, parametersIndex, "layer", layerToken, false, error, path) ||
        !readTransform(state, objectIndex, primitive.transform, error, path) ||
        !parsePrimitiveShape(shapeToken, primitive.shape, error, path) ||
        !parseMaterialPreset(materialToken, primitive.material, error, path) ||
        !parseRenderLayer(layerToken, primitive.layer, error, path)) {
        return false;
    }
    if (primitive.name.empty() ||
        primitive.transform.scale.x <= 0.0f ||
        primitive.transform.scale.y <= 0.0f ||
        primitive.transform.scale.z <= 0.0f) {
        return fail(error, path, "primitive name and transform scale must be positive");
    }
    blueprint.primitives.push_back(std::move(primitive));
    return true;
}

void appendLegacyScenePrimitives(SceneBlueprint& blueprint) {
    std::unordered_set<SceneObjectId> reservedIds(
        blueprint.objectOrder.begin(),
        blueprint.objectOrder.end()
    );
    const auto allocateId = [&](std::string_view prefix) {
        SceneObjectId candidate(prefix);
        for (std::size_t suffix = 1u; !reservedIds.insert(candidate).second; ++suffix) {
            candidate = std::string(prefix) + "_" + std::to_string(suffix);
        }
        return candidate;
    };

    ScenePrimitiveBlueprint ground{};
    ground.id = allocateId("ground");
    ground.name = "Ground";
    ground.shape = ScenePrimitiveShape::Plane;
    ground.material = SceneMaterialPreset::Soil;
    ground.layer = render::RenderLayer::Ground;
    ground.transform.scale = glm::vec3(
        blueprint.groundHalfExtent * 2.0f,
        1.0f,
        blueprint.groundHalfExtent * 2.0f
    );

    const auto wall = [&](std::string_view id, std::string name, float x, SceneMaterialPreset material) {
        ScenePrimitiveBlueprint result{};
        result.id = allocateId(id);
        result.name = std::move(name);
        result.shape = ScenePrimitiveShape::Box;
        result.material = material;
        result.layer = render::RenderLayer::Geometry;
        result.transform.position = glm::vec3(x, blueprint.wallHeight * 0.5f, 0.0f);
        result.transform.scale = glm::vec3(
            blueprint.wallThickness,
            blueprint.wallHeight,
            blueprint.wallLength * 2.0f
        );
        return result;
    };

    ScenePrimitiveBlueprint frustumBox{};
    frustumBox.id = allocateId("frustum_test_box");
    frustumBox.name = "Frustum Test Box";
    frustumBox.material = SceneMaterialPreset::Wood;
    frustumBox.transform.position = glm::vec3(-9.5f, 0.75f, -14.0f);
    frustumBox.transform.scale = glm::vec3(1.5f);

    ScenePrimitiveBlueprint occlusionBox{};
    occlusionBox.id = allocateId("occlusion_test_box");
    occlusionBox.name = "Occlusion Test Box";
    occlusionBox.material = SceneMaterialPreset::Rock;
    occlusionBox.transform.position = glm::vec3(-3.0f, 0.75f, -8.5f);
    occlusionBox.transform.scale = glm::vec3(1.5f);

    blueprint.primitives = {
        std::move(ground),
        wall("wall_a", "Wall A", -blueprint.wallOffset, SceneMaterialPreset::Rock),
        wall("wall_b", "Wall B", blueprint.wallOffset, SceneMaterialPreset::Wood),
        std::move(frustumBox),
        std::move(occlusionBox),
    };
    std::vector<SceneObjectId> migratedOrder{};
    migratedOrder.reserve(blueprint.primitives.size() + blueprint.objectOrder.size());
    for (const ScenePrimitiveBlueprint& primitive : blueprint.primitives) {
        migratedOrder.push_back(primitive.id);
    }
    migratedOrder.insert(
        migratedOrder.end(),
        blueprint.objectOrder.begin(),
        blueprint.objectOrder.end()
    );
    blueprint.objectOrder = std::move(migratedOrder);
}

}  // namespace core::scene_asset_detail
