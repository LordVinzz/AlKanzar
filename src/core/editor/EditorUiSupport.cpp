#include "core/editor/EditorUi.hpp"
#include "core/editor/EditorUiCommands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/editor/ComponentRegistry.hpp"
#include "render/resources/StaticGltfModel.hpp"


namespace core {
namespace {

const char* affiliationLabel(CharacterAffiliation affiliation) {
    switch (affiliation) {
        case CharacterAffiliation::Player:
            return "Player";
        case CharacterAffiliation::FriendlyNpc:
            return "Friendly NPC";
        case CharacterAffiliation::HostileNpc:
            return "Hostile NPC";
    }
    return "Character";
}

}  // namespace

std::string editorEntityLabel(const EngineServices& services, EntityId entity) {
    if (const NameComponent* name = services.world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

void sortProfilerCounters(std::vector<const render::FrameCounterRecord*>& counters) {
    std::sort(counters.begin(), counters.end(), [](const render::FrameCounterRecord* lhs, const render::FrameCounterRecord* rhs) {
        if (lhs->name != rhs->name) {
            return lhs->name < rhs->name;
        }
        return lhs->value < rhs->value;
    });
}

void drawProfilerCounterTable(
    const char* tableId,
    const std::vector<const render::FrameCounterRecord*>& counters
) {
    if (!ImGui::BeginTable(
            tableId,
            2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }

    ImGui::TableSetupColumn("Metric");
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    for (const render::FrameCounterRecord* counter : counters) {
        const std::string_view name(counter->name);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(counter->name.c_str());
        ImGui::TableSetColumnIndex(1);
        if (name.ends_with(" Enabled")) {
            ImGui::TextUnformatted(counter->value != 0 ? "Enabled" : "Disabled");
        } else if (name.ends_with(" Dirty")) {
            ImGui::TextUnformatted(counter->value != 0 ? "Dirty" : "Clean");
        } else {
            ImGui::Text("%lld", static_cast<long long>(counter->value));
        }
    }
    ImGui::EndTable();
}

std::string editorSelectionLabel(const EngineServices& services) {
    if (!services.editorSelection.current().has_value()) {
        return "None";
    }

    return editorEntityLabel(services, services.editorSelection.current()->entity);
}

std::string editorHierarchyLabel(const EngineServices& services, EntityId entity) {
    std::string label = editorEntityLabel(services, entity);
    if (const CharacterComponent* character = services.world.characters.tryGet(entity)) {
        label += " [";
        label += affiliationLabel(character->affiliation);
        label += "]";
    } else if (services.world.directionalLights.contains(entity)) {
        label += " [Directional Light]";
    } else if (services.world.pointLights.contains(entity)) {
        label += " [Point Light]";
    } else if (services.world.spotLights.contains(entity)) {
        label += " [Spot Light]";
    } else if (services.world.renderables.contains(entity)) {
        label += " [Mesh]";
    } else if (services.world.transforms.contains(entity)) {
        label += " [Group]";
    } else {
        label += " [Node]";
    }
    return label;
}

void notifyEditorTransformChanged(EngineServices& services, EntityId entity) {
    services.world.markTransformsDirty(entity);
    services.events.publish(TransformChangedEvent{entity});
}

void notifyEditorLightChanged(EngineServices& services, EntityId entity) {
    services.world.markLightsDirty(entity);
    services.events.publish(LightChangedEvent{entity});
}

void notifyEditorMaterialChanged(EngineServices& services, EntityId entity) {
    services.events.publish(MaterialChangedEvent{entity});
}

std::filesystem::path defaultProfilerExportPath(const ProfilerTraceCapture& capture) {
    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &timestamp);
#else
    localtime_r(&timestamp, &localTime);
#endif

    std::ostringstream filename;
    filename << "alkanzar-profile-s" << capture.sessionId;
    if (!capture.frames.empty()) {
        filename << "-f" << capture.frames.back().frameNumber;
    }
    filename << "-" << std::put_time(&localTime, "%Y%m%d-%H%M%S") << ".pftrace";
    return std::filesystem::path("captures") / filename.str();
}

}  // namespace core
