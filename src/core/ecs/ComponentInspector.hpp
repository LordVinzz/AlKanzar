#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "core/editor/Command.hpp"
#include "core/editor/CommandHistory.hpp"

namespace core {

struct EngineServices;
struct EntityId;

template <typename TSnapshot, typename ApplyFn, typename DrawFn>
void editComponentSnapshot(
    const char* itemId,
    const std::string& label,
    const std::string& mergeKey,
    TSnapshot currentValue,
    ApplyFn applyFn,
    DrawFn drawFn,
    CommandHistory& history,
    bool mergeable = true
) {
    static std::unordered_map<ImGuiID, TSnapshot> snapshots;
    const TSnapshot before = currentValue;
    TSnapshot edited = currentValue;
    const bool changed = drawFn(edited);
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemActivated()) {
        snapshots[id] = before;
    }
    if (changed) {
        applyFn(edited);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const auto it = snapshots.find(id);
        history.execute(std::make_unique<SnapshotCommand<TSnapshot>>(
            label, mergeKey, it != snapshots.end() ? it->second : before,
            edited, applyFn, mergeable
        ));
        snapshots.erase(id);
    } else if (!ImGui::IsItemActive()) {
        snapshots.erase(id);
    }
}

void notifyTransformChanged(EngineServices& services, EntityId entity);
void notifyLightChanged(EngineServices& services, EntityId entity);
void notifyMaterialChanged(EngineServices& services, EntityId entity);
void drawMaterialInspector(EngineServices& services, EntityId entity);

}  // namespace core
