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
#include "core/ecs/ComponentRegistry.hpp"
#include "render/resources/StaticGltfModel.hpp"


namespace core {

const char* navSelectionPreviewLabel(const std::vector<EntityId>& targets, const EngineServices& services) {
    if (targets.empty()) {
        return "No Nav Sources";
    }
    const NavSourceComponent* first = services.world.navSources.tryGet(targets.front());
    if (first == nullptr) {
        return "No Nav Sources";
    }
    for (std::size_t index = 1; index < targets.size(); ++index) {
        const NavSourceComponent* current = services.world.navSources.tryGet(targets[index]);
        if (current == nullptr || current->effectiveTag != first->effectiveTag) {
            return "Mixed";
        }
    }
    return navSourceTagName(first->effectiveTag);
}

void drawNavMeshWindow(EngineServices& services) {
    if (!services.editorSession.navMeshWindowVisible) {
        services.editorSession.navMeshWindowFocusRequested = false;
        return;
    }

    if (services.editorSession.navMeshWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_FirstUseEver);

    const bool wasOpen = services.editorSession.navMeshWindowVisible;
    bool open = wasOpen;
    if (!ImGui::Begin("NavMesh", &open)) {
        ImGui::End();
        services.editorSession.navMeshWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.navMeshWindowFocusRequested = false;
        return;
    }

    ImGui::Text("Asset: %s", services.navigation.assetPath.c_str());
    ImGui::Text(
        "%zu polygons | %zu runtime cells",
        services.navigation.asset.polygons.size(),
        services.navigation.bakedCells.size()
    );
    if (!services.navigation.statusMessage.empty()) {
        const ImVec4 color = services.navigation.statusIsError
            ? ImVec4(0.90f, 0.32f, 0.32f, 1.0f)
            : ImVec4(0.35f, 0.78f, 0.42f, 1.0f);
        ImGui::TextColored(color, "%s", services.navigation.statusMessage.c_str());
    }
    if (!services.navigation.exactPathfindingWarning.empty()) {
        ImGui::TextColored(
            ImVec4(0.95f, 0.68f, 0.22f, 1.0f),
            "%s",
            services.navigation.exactPathfindingWarning.c_str()
        );
    }

    const auto bakeRuntime = [&services]() {
        std::string error{};
        if (services.navigationSystem.rebuildRuntime(services.navigation, &error)) {
            services.navigation.statusMessage = "Baked navmesh (" +
                std::to_string(services.navigation.bakedCells.size()) + " cells).";
            services.navigation.statusIsError = false;
        } else {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        }
    };

    if (ImGui::Button("Save")) {
        std::string error{};
        if (services.navigationSystem.saveAsset(services.navigation, &error)) {
            services.navigation.statusMessage = "Saved navmesh asset.";
            services.navigation.statusIsError = false;
        } else {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        if (!services.navigationSystem.initializeScene(services.currentScene, services.world, services.navigation)) {
            services.navigation.statusMessage = services.navigation.statusMessage.empty()
                ? "Failed to reload navmesh asset."
                : services.navigation.statusMessage;
            services.navigation.statusIsError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Bake")) {
        bakeRuntime();
    }
    ImGui::SameLine();
    if (ImGui::Button("Generate From Tags")) {
        std::string error{};
        if (!services.navigationSystem.generateFromTags(services.world, services.navigation, &error)) {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        services.navigation.asset.polygons.clear();
        services.navigation.asset.links.clear();
        services.navigationSystem.rebuildRuntime(services.navigation);
        services.navigation.statusMessage = "Cleared navmesh polygons and links.";
        services.navigation.statusIsError = false;
    }

    bool overlayVisible = services.editorSession.navMeshOverlayVisible;
    if (ImGui::Checkbox("Show Nav Overlay", &overlayVisible)) {
        setPersistedEditorSessionFlag(services.editorSession.navMeshOverlayVisible, overlayVisible);
    }
    ImGui::SameLine();
    bool polygonWireframeVisible = services.editorSession.navMeshPolygonWireframeVisible;
    if (ImGui::Checkbox("Red Polygon Wireframe", &polygonWireframeVisible)) {
        setPersistedEditorSessionFlag(
            services.editorSession.navMeshPolygonWireframeVisible,
            polygonWireframeVisible
        );
    }

    ImGui::Checkbox("Editor Click Moves Character", &services.navigation.editor.testMoveMode);
    if (services.navigation.editor.testMoveMode) {
        services.navigation.editor.polygonCaptureActive = false;
    }
    if (ImGui::DragFloat(
        "Minimum Generated Triangle Area",
        &services.navigation.asset.minimumRuntimeCellArea,
        0.001f,
        0.0f,
        100.0f,
        "%.4f",
        ImGuiSliderFlags_AlwaysClamp
    )) {
        services.navigation.asset.minimumRuntimeCellArea =
            std::max(services.navigation.asset.minimumRuntimeCellArea, 0.0f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 disables the constraint. Regenerate From Tags after changing this value."
        );
    }
    if (ImGui::DragFloat(
        "Maximum Polygon Edge Length",
        &services.navigation.asset.maximumPolygonEdgeLength,
        0.1f,
        0.0f,
        1000.0f,
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp
    )) {
        services.navigation.asset.maximumPolygonEdgeLength = std::max(
            services.navigation.asset.maximumPolygonEdgeLength,
            0.0f
        );
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 disables the limit. Regenerate From Tags after changing this value."
        );
    }

    ImGui::SeparatorText("Tagging");
    if (services.selection.current().has_value()) {
        const std::vector<EntityId> targets =
            services.navigationSystem.collectRenderableSelectionTargets(
                services.world,
                services.selection.current()->entity
            );
        ImGui::Text("%d target renderables", static_cast<int>(targets.size()));
        const char* preview = navSelectionPreviewLabel(targets, services);
        if (ImGui::BeginCombo("Applied Tag", preview)) {
            for (const NavSourceTag tag : {NavSourceTag::Walkable, NavSourceTag::Blocking, NavSourceTag::Ignored}) {
                const bool selected = std::string_view(preview) == navSourceTagName(tag);
                if (ImGui::Selectable(navSourceTagName(tag), selected)) {
                    for (EntityId entity : targets) {
                        services.navigationSystem.applyTagOverride(services.world, services.navigation, entity, tag);
                    }
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextUnformatted("Select a renderable or parent node to edit nav source tags.");
    }

    ImGui::SeparatorText("Manual Polygon");
    ImGui::DragFloat("Capture Elevation", &services.navigation.editor.polygonCaptureElevation, 0.05f);
    if (!services.navigation.editor.polygonCaptureActive) {
        if (ImGui::Button("Start Polygon Capture")) {
            services.navigation.editor.testMoveMode = false;
            services.navigation.editor.polygonCaptureActive = true;
            services.navigation.editor.polygonCaptureVertices.clear();
        }
    } else if (ImGui::Button("Stop Polygon Capture")) {
        services.navigation.editor.polygonCaptureActive = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Commit Polygon")) {
        std::string error{};
        if (!services.navigationSystem.commitCapturedPolygon(services.navigation, &error)) {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        } else {
            services.navigation.statusMessage = "Committed manual polygon.";
            services.navigation.statusIsError = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel Capture")) {
        services.navigationSystem.clearCapturedPolygon(services.navigation);
    }
    ImGui::Text("%d captured vertices", static_cast<int>(services.navigation.editor.polygonCaptureVertices.size()));

    ImGui::SeparatorText("Manual Link");
    std::vector<int> polygonIds{};
    polygonIds.reserve(services.navigation.asset.polygons.size());
    for (const NavPolygon& polygon : services.navigation.asset.polygons) {
        polygonIds.push_back(polygon.id);
    }
    const auto polygonLabel = [&services](int polygonId) {
        const auto polygonIndex = services.navigation.polygonIndexById.find(polygonId);
        if (polygonIndex == services.navigation.polygonIndexById.end()) {
            return std::string{"<none>"};
        }
        const NavPolygon& polygon = services.navigation.asset.polygons[polygonIndex->second];
        return std::string("Polygon ") + std::to_string(polygon.id) + " @ " + std::to_string(polygon.elevationY);
    };
    if (ImGui::BeginCombo("Source Polygon", polygonLabel(services.navigation.editor.pendingLinkFromPolygonId).c_str())) {
        for (int polygonId : polygonIds) {
            const bool selected = polygonId == services.navigation.editor.pendingLinkFromPolygonId;
            if (ImGui::Selectable(polygonLabel(polygonId).c_str(), selected)) {
                services.navigation.editor.pendingLinkFromPolygonId = polygonId;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Target Polygon", polygonLabel(services.navigation.editor.pendingLinkToPolygonId).c_str())) {
        for (int polygonId : polygonIds) {
            const bool selected = polygonId == services.navigation.editor.pendingLinkToPolygonId;
            if (ImGui::Selectable(polygonLabel(polygonId).c_str(), selected)) {
                services.navigation.editor.pendingLinkToPolygonId = polygonId;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Seed Link Endpoints")) {
        std::string error{};
        if (!services.navigationSystem.seedPendingLink(
                services.navigation,
                services.navigation.editor.pendingLinkFromPolygonId,
                services.navigation.editor.pendingLinkToPolygonId,
                &error)) {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        }
    }
    ImGui::DragFloat3("From Point", glm::value_ptr(services.navigation.editor.pendingLinkFromPoint), 0.05f);
    ImGui::DragFloat3("To Point", glm::value_ptr(services.navigation.editor.pendingLinkToPoint), 0.05f);
    ImGui::Checkbox("Bidirectional", &services.navigation.editor.pendingLinkBidirectional);
    if (ImGui::Button("Add Link")) {
        std::string error{};
        if (!services.navigationSystem.commitPendingLink(services.navigation, &error)) {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        } else {
            services.navigation.statusMessage = "Added manual nav link.";
            services.navigation.statusIsError = false;
        }
    }

    bool navDirty = false;
    bool deletedPolygon = false;
    ImGui::SeparatorText("Polygons");
    for (std::size_t polygonIndex = 0; polygonIndex < services.navigation.asset.polygons.size(); ++polygonIndex) {
        NavPolygon& polygon = services.navigation.asset.polygons[polygonIndex];
        ImGui::PushID(static_cast<int>(polygon.id));
        if (ImGui::TreeNodeEx("polygon", ImGuiTreeNodeFlags_DefaultOpen, "Polygon %d", polygon.id)) {
            navDirty |= ImGui::DragFloat("Elevation", &polygon.elevationY, 0.05f);
            for (std::size_t vertexIndex = 0; vertexIndex < polygon.verticesXZ.size(); ++vertexIndex) {
                ImGui::PushID(static_cast<int>(vertexIndex));
                navDirty |= ImGui::DragFloat2("Vertex", glm::value_ptr(polygon.verticesXZ[vertexIndex]), 0.05f);
                ImGui::SameLine();
                if (polygon.verticesXZ.size() > 3u && ImGui::Button("Delete Vertex")) {
                    polygon.verticesXZ.erase(polygon.verticesXZ.begin() + static_cast<std::ptrdiff_t>(vertexIndex));
                    navDirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Add Vertex")) {
                const glm::vec2 seed = polygon.verticesXZ.empty() ? glm::vec2(0.0f) : polygon.verticesXZ.back();
                polygon.verticesXZ.push_back(seed + glm::vec2(0.5f, 0.0f));
                navDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Polygon")) {
                const int deletedId = polygon.id;
                services.navigation.asset.polygons.erase(
                    services.navigation.asset.polygons.begin() + static_cast<std::ptrdiff_t>(polygonIndex)
                );
                services.navigation.asset.links.erase(
                    std::remove_if(
                        services.navigation.asset.links.begin(),
                        services.navigation.asset.links.end(),
                        [deletedId](const NavLink& link) {
                            return link.fromPolygonId == deletedId || link.toPolygonId == deletedId;
                        }
                    ),
                    services.navigation.asset.links.end()
                );
                navDirty = true;
                deletedPolygon = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        if (deletedPolygon) {
            break;
        }
    }

    bool deletedLink = false;
    ImGui::SeparatorText("Links");
    for (std::size_t linkIndex = 0; linkIndex < services.navigation.asset.links.size(); ++linkIndex) {
        NavLink& link = services.navigation.asset.links[linkIndex];
        ImGui::PushID(static_cast<int>(link.id));
        if (ImGui::TreeNodeEx("link", ImGuiTreeNodeFlags_DefaultOpen, "Link %d", link.id)) {
            navDirty |= ImGui::DragFloat3("From", glm::value_ptr(link.fromPoint), 0.05f);
            navDirty |= ImGui::DragFloat3("To", glm::value_ptr(link.toPoint), 0.05f);
            navDirty |= ImGui::Checkbox("Bidirectional##link", &link.bidirectional);
            if (ImGui::Button("Delete Link")) {
                services.navigation.asset.links.erase(
                    services.navigation.asset.links.begin() + static_cast<std::ptrdiff_t>(linkIndex)
                );
                navDirty = true;
                deletedLink = true;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        if (deletedLink) {
            break;
        }
    }

    if (navDirty) {
        std::string error{};
        if (!services.navigationSystem.rebuildRuntime(services.navigation, &error)) {
            services.navigation.statusMessage = error;
            services.navigation.statusIsError = true;
        }
    }

    ImGui::End();

    services.editorSession.navMeshWindowVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.navMeshWindowFocusRequested = false;
}

}  // namespace core
