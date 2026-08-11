#include "SceneAsset.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "SceneAssetSerializationDetail.hpp"
#include "core/content/ContentFileHeader.hpp"

namespace core {
namespace {

using SceneObject = std::variant<
    const ScenePrimitiveBlueprint*,
    const ModelInstanceBlueprint*,
    const DirectionalLightBlueprint*,
    const LightVolumeBlueprint*,
    const PointLightBlueprint*,
    const SpotLightBlueprint*
>;

using namespace scene_asset_serialization_detail;

bool replaceFileAtomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::error_code& error
) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        error.clear();
        return true;
    }
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

const SceneObjectId& objectId(const SceneObject& object) {
    return std::visit([](const auto* value) -> const SceneObjectId& { return value->id; }, object);
}

std::optional<SceneObjectId> objectParentId(const SceneObject& object) {
    return std::visit([](const auto* value) -> std::optional<SceneObjectId> {
        using Value = std::remove_cv_t<std::remove_pointer_t<decltype(value)>>;
        if constexpr (std::is_same_v<Value, DirectionalLightBlueprint>) {
            return std::nullopt;
        } else {
            return value->parentId;
        }
    }, object);
}

const char* layerToken(render::RenderLayer layer) {
    switch (layer) {
        case render::RenderLayer::Ground: return "Ground";
        case render::RenderLayer::Geometry: return "Geometry";
        case render::RenderLayer::Actors: return "Actors";
    }
    return "Geometry";
}

const char* primitiveShapeToken(ScenePrimitiveShape shape) {
    switch (shape) {
        case ScenePrimitiveShape::Plane: return "Plane";
        case ScenePrimitiveShape::Box: return "Box";
    }
    return "Box";
}

const char* materialPresetToken(SceneMaterialPreset material) {
    switch (material) {
        case SceneMaterialPreset::Soil: return "Soil";
        case SceneMaterialPreset::Rock: return "Rock";
        case SceneMaterialPreset::Wood: return "Wood";
    }
    return "Rock";
}

const char* modelMaterialProfileToken(SceneModelMaterialProfile profile) {
    switch (profile) {
        case SceneModelMaterialProfile::Imported: return "Imported";
        case SceneModelMaterialProfile::House: return "House";
    }
    return "Imported";
}

void appendVec3(std::ostream& output, const glm::vec3& value) {
    output << "{ x = " << value.x << ", y = " << value.y << ", z = " << value.z << " }";
}

void appendTransform(
    std::ostream& output,
    std::string_view variable,
    const TransformComponent& transform
) {
    output << variable << ".transform({\n    position = ";
    appendVec3(output, transform.position);
    output << ",\n    rotation = ";
    appendVec3(output, transform.rotationDeg);
    output << ",\n    scale = ";
    appendVec3(output, transform.scale);
    output << ",\n})\n";
}

void appendModel(std::ostream& output, std::string_view variable, const ModelInstanceBlueprint& model) {
    output << variable << " = Create({\n"
           << "    type = \"Model\",\n"
           << "    id = " << quoteLuaString(model.id) << ",\n"
           << "    name = " << quoteLuaString(model.name) << ",\n"
           << "    asset = " << quoteLuaString(model.path) << ",\n"
           << "    layer = " << quoteLuaString(layerToken(model.layer)) << ",\n"
           << "    material_profile = "
           << quoteLuaString(modelMaterialProfileToken(model.materialProfile)) << ",\n"
           << "})\n";
    appendTransform(output, variable, model.transform);
    if (model.fitToFootprint) {
        output << variable << ".fit_to_footprint(" << model.footprint << ")\n";
    }
    if (model.character.has_value()) {
        appendCharacterLua(output, variable, *model.character);
    }
}

void appendPrimitive(
    std::ostream& output,
    std::string_view variable,
    const ScenePrimitiveBlueprint& primitive
) {
    output << variable << " = Create({\n"
           << "    type = \"Primitive\",\n"
           << "    id = " << quoteLuaString(primitive.id) << ",\n"
           << "    name = " << quoteLuaString(primitive.name) << ",\n"
           << "    shape = " << quoteLuaString(primitiveShapeToken(primitive.shape)) << ",\n"
           << "    material = " << quoteLuaString(materialPresetToken(primitive.material)) << ",\n"
           << "    layer = " << quoteLuaString(layerToken(primitive.layer)) << ",\n"
           << "})\n";
    appendTransform(output, variable, primitive.transform);
}

void appendDirectionalLight(
    std::ostream& output,
    std::string_view variable,
    const DirectionalLightBlueprint& light
) {
    output << variable << " = Create({\n"
           << "    type = \"DirectionalLight\",\n"
           << "    id = " << quoteLuaString(light.id) << ",\n"
           << "    name = " << quoteLuaString(light.name) << ",\n"
           << "    direction = ";
    appendVec3(output, light.direction);
    output << ",\n    color = ";
    appendVec3(output, light.color);
    output << ",\n    intensity = " << light.intensity << ",\n})\n";
}

void appendLightVolume(
    std::ostream& output,
    std::string_view variable,
    const LightVolumeBlueprint& volume
) {
    output << variable << " = Create({\n"
           << "    type = \"LightVolume\",\n"
           << "    id = " << quoteLuaString(volume.id) << ",\n"
           << "    name = " << quoteLuaString(volume.name) << ",\n"
           << "    half_extents = ";
    appendVec3(output, volume.halfExtents);
    output << ",\n})\n";
    appendTransform(output, variable, volume.transform);
}

void appendPointLight(std::ostream& output, std::string_view variable, const PointLightBlueprint& light) {
    output << variable << " = Create({\n"
           << "    type = \"PointLight\",\n"
           << "    id = " << quoteLuaString(light.id) << ",\n"
           << "    name = " << quoteLuaString(light.name) << ",\n"
           << "    radius = " << light.radius << ",\n"
           << "    color = ";
    appendVec3(output, light.color);
    output << ",\n    intensity = " << light.intensity << ",\n"
           << "    phase = " << light.phase << ",\n"
           << "    movable = " << (light.isMovable ? "true" : "false") << ",\n"
           << "    casts_shadow = " << (light.castsShadow ? "true" : "false") << ",\n"
           << "    shadow_bias_min = " << light.shadowBiasMin << ",\n"
           << "    shadow_bias_slope = " << light.shadowBiasSlope << ",\n"
           << "})\n";
    appendTransform(output, variable, light.transform);
}

void appendSpotLight(std::ostream& output, std::string_view variable, const SpotLightBlueprint& light) {
    output << variable << " = Create({\n"
           << "    type = \"SpotLight\",\n"
           << "    id = " << quoteLuaString(light.id) << ",\n"
           << "    name = " << quoteLuaString(light.name) << ",\n"
           << "    radius = " << light.radius << ",\n"
           << "    color = ";
    appendVec3(output, light.color);
    output << ",\n    intensity = " << light.intensity << ",\n    target = ";
    appendVec3(output, light.target);
    output << ",\n    inner_angle = " << light.innerAngle << ",\n"
           << "    outer_angle = " << light.outerAngle << ",\n"
           << "    phase = " << light.phase << ",\n"
           << "    movable = " << (light.isMovable ? "true" : "false") << ",\n"
           << "    casts_shadow = " << (light.castsShadow ? "true" : "false") << ",\n"
           << "    shadow_bias_min = " << light.shadowBiasMin << ",\n"
           << "    shadow_bias_slope = " << light.shadowBiasSlope << ",\n"
           << "})\n";
    appendTransform(output, variable, light.transform);
}

void appendObject(std::ostream& output, std::string_view variable, const SceneObject& object) {
    std::visit([&](const auto* value) {
        using Value = std::remove_cv_t<std::remove_pointer_t<decltype(value)>>;
        if constexpr (std::is_same_v<Value, ScenePrimitiveBlueprint>) {
            appendPrimitive(output, variable, *value);
        } else if constexpr (std::is_same_v<Value, ModelInstanceBlueprint>) {
            appendModel(output, variable, *value);
        } else if constexpr (std::is_same_v<Value, DirectionalLightBlueprint>) {
            appendDirectionalLight(output, variable, *value);
        } else if constexpr (std::is_same_v<Value, LightVolumeBlueprint>) {
            appendLightVolume(output, variable, *value);
        } else if constexpr (std::is_same_v<Value, PointLightBlueprint>) {
            appendPointLight(output, variable, *value);
        } else {
            appendSpotLight(output, variable, *value);
        }
    }, object);
}

bool orderedObjects(
    const SceneBlueprint& blueprint,
    std::vector<SceneObject>& outObjects,
    std::string* error
) {
    std::vector<SceneObject> declared{};
    for (const ScenePrimitiveBlueprint& object : blueprint.primitives) declared.emplace_back(&object);
    for (const ModelInstanceBlueprint& object : blueprint.models) declared.emplace_back(&object);
    if (blueprint.directionalLight.has_value()) declared.emplace_back(&*blueprint.directionalLight);
    for (const LightVolumeBlueprint& object : blueprint.lightVolumes) declared.emplace_back(&object);
    for (const PointLightBlueprint& object : blueprint.pointLights) declared.emplace_back(&object);
    for (const SpotLightBlueprint& object : blueprint.spotLights) declared.emplace_back(&object);

    std::unordered_map<SceneObjectId, SceneObject> byId{};
    for (const SceneObject& object : declared) {
        if (objectId(object).empty() || !byId.emplace(objectId(object), object).second) {
            if (error != nullptr) *error = "Every SCN V2 object must have a unique non-empty id.";
            return false;
        }
    }
    std::unordered_set<SceneObjectId> emitted{};
    for (const SceneObjectId& id : blueprint.objectOrder) {
        const auto object = byId.find(id);
        if (object == byId.end() || !emitted.insert(id).second) {
            if (error != nullptr) *error = "Scene objectOrder contains an unknown or duplicate id: " + id;
            return false;
        }
        outObjects.push_back(object->second);
    }
    for (const SceneObject& object : declared) {
        if (emitted.insert(objectId(object)).second) outObjects.push_back(object);
    }
    return true;
}

}  // namespace

bool serializeSceneAsset(const SceneBlueprint& blueprint, std::string& outBytes, std::string* error) {
    if (error != nullptr) error->clear();
    std::vector<SceneObject> objects{};
    if (!orderedObjects(blueprint, objects, error)) return false;

    std::array<char, kContentFileHeaderSize> header{};
    if (!encodeTextContentFileHeader(kSceneAssetVersion, kSceneContentType, header, error)) return false;

    std::ostringstream output{};
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output << "\n-- AlKanzar SCN V2 -- generated deterministically; safe to edit by hand.\n\n"
           << "scene = Create({\n"
           << "    type = \"Scene\",\n"
           << "    navmesh = " << quoteLuaString(blueprint.navMeshAssetPath) << ",\n"
           << "})\n\n";

    std::unordered_map<SceneObjectId, std::string> variables{};
    for (std::size_t index = 0u; index < objects.size(); ++index) {
        const std::string variable = "object_" + std::to_string(index + 1u);
        variables.emplace(objectId(objects[index]), variable);
        appendObject(output, variable, objects[index]);
        output << "\n";
    }
    for (const SceneObject& object : objects) {
        const std::optional<SceneObjectId> parentId = objectParentId(object);
        if (!parentId.has_value()) continue;
        const auto parentVariable = variables.find(*parentId);
        if (parentVariable == variables.end()) {
            if (error != nullptr) *error = "Scene object parent id does not exist: " + *parentId;
            return false;
        }
        output << variables.at(objectId(object)) << ".parent(" << parentVariable->second << ")\n";
    }
    if (!objects.empty()) output << "\n";
    for (const SceneObject& object : objects) {
        output << "scene.add(" << variables.at(objectId(object)) << ")\n";
    }
    output << "scene.build()\n";

    const std::string serialized = output.str();
    SceneBlueprint witness{};
    std::string validationError{};
    if (!parseSceneAsset(serialized, witness, &validationError, "serialized scene")) {
        if (error != nullptr) *error = "Refusing to save invalid SCN V2 output: " + validationError;
        return false;
    }
    outBytes = serialized;
    return true;
}

bool saveSceneAsset(
    const std::filesystem::path& path,
    const SceneBlueprint& blueprint,
    std::string* error
) {
    std::string bytes{};
    if (!serializeSceneAsset(blueprint, bytes, error)) return false;
    if (path.empty()) {
        if (error != nullptr) *error = "Scene save path is empty.";
        return false;
    }

    std::error_code filesystemError{};
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
    }
    if (filesystemError) {
        if (error != nullptr) *error = "Could not create scene directory: " + filesystemError.message();
        return false;
    }

    std::filesystem::path temporary{};
    bool reservedTemporary = false;
    for (unsigned int suffix = 0u; suffix < 1000u; ++suffix) {
        temporary = path;
        temporary += ".tmp." + std::to_string(suffix);
        if (!std::filesystem::exists(temporary, filesystemError)) {
            reservedTemporary = !filesystemError;
            break;
        }
    }
    if (!reservedTemporary || filesystemError) {
        if (error != nullptr) *error = "Could not reserve an adjacent temporary scene file.";
        return false;
    }

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, filesystemError);
        if (error != nullptr) *error = "Could not write the temporary scene file.";
        return false;
    }
    if (!replaceFileAtomically(temporary, path, filesystemError)) {
        std::error_code cleanupError{};
        std::filesystem::remove(temporary, cleanupError);
        if (error != nullptr) *error = "Could not atomically replace the scene: " + filesystemError.message();
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

}  // namespace core
