#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "core/ecs/Entity.hpp"
#include "render/resources/Profiling.hpp"

namespace render {
struct Material;
}

namespace core {

struct EngineServices;
struct ProfilerTraceCapture;

[[nodiscard]] std::string editorEntityLabel(const EngineServices& services, EntityId entity);
[[nodiscard]] std::string editorSelectionLabel(const EngineServices& services);
[[nodiscard]] std::string editorHierarchyLabel(const EngineServices& services, EntityId entity);
void notifyEditorTransformChanged(EngineServices& services, EntityId entity);
void notifyEditorLightChanged(EngineServices& services, EntityId entity);
void notifyEditorMaterialChanged(EngineServices& services, EntityId entity);
[[nodiscard]] std::filesystem::path defaultProfilerExportPath(const ProfilerTraceCapture& capture);
void sortProfilerCounters(std::vector<const render::FrameCounterRecord*>& counters);
void drawProfilerCounterTable(
    const char* tableId,
    const std::vector<const render::FrameCounterRecord*>& counters
);
[[nodiscard]] std::size_t editorHierarchyChildCount(const EngineServices& services, EntityId entity);

void drawSceneHierarchyWindow(EngineServices& services);
void drawAnimationInspector(EngineServices& services, EntityId selected);
void drawNavMeshWindow(EngineServices& services);
void drawEditorMainWindow(EngineServices& services);
void drawProfilerWindow(EngineServices& services);
void drawInspectorWindow(EngineServices& services);
void drawTextureBrowser(EngineServices& services, EntityId entity, render::Material& material);

}  // namespace core
