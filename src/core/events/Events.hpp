#pragma once

#include <optional>
#include <variant>

#include "core/ecs/Entity.hpp"
#include "core/editor/SelectionModel.hpp"
#include "render/engine/RenderTypes.hpp"

namespace core {

struct QuitRequestedEvent {};
struct EnterEditorEvent {};
struct ExitEditorEvent {};
struct EnterGameplayEvent {};
struct ToggleEditorEvent {};
struct ToggleFreeCameraEvent {};
struct ToggleLightDebugEvent {};
struct ToggleSimulationPauseEvent {};
struct AdjustSimulationSpeedEvent {
    int direction{0};
};
struct UndoRequestedEvent {};
struct RedoRequestedEvent {};

struct DebugViewSelectedEvent {
    render::DebugView view{render::DebugView::Final};
};

struct ShadowCascadeStepEvent {
    int delta{0};
};

struct WindowResizedEvent {
    int width{0};
    int height{0};
};

struct ViewportWheelEvent {
    int delta{0};
};

struct ViewportPanEvent {
    int dx{0};
    int dy{0};
};

struct ViewportClickedEvent {
    int x{0};
    int y{0};
};

struct EditorSelectionChangedEvent {
    std::optional<SelectionTarget> selection{};
};

struct PartySelectionChangedEvent {
    std::optional<EntityId> leader{};
};

struct TransformChangedEvent {
    EntityId entity{};
};

struct LightChangedEvent {
    EntityId entity{};
};

struct MaterialChangedEvent {
    EntityId entity{};
};

using AppEvent = std::variant<
    QuitRequestedEvent,
    EnterEditorEvent,
    ExitEditorEvent,
    EnterGameplayEvent,
    ToggleEditorEvent,
    ToggleFreeCameraEvent,
    ToggleLightDebugEvent,
    ToggleSimulationPauseEvent,
    AdjustSimulationSpeedEvent,
    UndoRequestedEvent,
    RedoRequestedEvent,
    DebugViewSelectedEvent,
    ShadowCascadeStepEvent,
    WindowResizedEvent,
    ViewportWheelEvent,
    ViewportPanEvent,
    ViewportClickedEvent,
    EditorSelectionChangedEvent,
    PartySelectionChangedEvent,
    TransformChangedEvent,
    LightChangedEvent,
    MaterialChangedEvent>;

}  // namespace core
