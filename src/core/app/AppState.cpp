#include "AppState.hpp"

#include <algorithm>
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

#include "core/editor/Command.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "EngineServices.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {

namespace {

template <typename TSnapshot, typename ApplyFn, typename DrawFn>
void editSnapshotCommand(
    const char* itemId,
    const std::string& label,
    const std::string& mergeKey,
    TSnapshot currentValue,
    ApplyFn applyFn,
    DrawFn drawFn,
    CommandHistory& history,
    bool mergeable = true
) {
    static std::unordered_map<ImGuiID, TSnapshot> snapshotById;

    const TSnapshot before = currentValue;
    TSnapshot edited = currentValue;
    const bool changed = drawFn(edited);
    const ImGuiID imguiId = ImGui::GetItemID();

    if (ImGui::IsItemActivated()) {
        snapshotById[imguiId] = before;
    }

    if (changed) {
        applyFn(edited);
    }

    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const auto it = snapshotById.find(imguiId);
        const TSnapshot committedBefore = it != snapshotById.end() ? it->second : before;
        history.execute(std::make_unique<SnapshotCommand<TSnapshot>>(
            label,
            mergeKey,
            committedBefore,
            edited,
            applyFn,
            mergeable
        ));
        snapshotById.erase(imguiId);
    } else if (!ImGui::IsItemActive()) {
        snapshotById.erase(imguiId);
    }
}

std::string entityLabel(const EngineServices& services, EntityId entity) {
    if (const NameComponent* name = services.world.names.tryGet(entity)) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

void sortCounterPointers(std::vector<const render::FrameCounterRecord*>& counters) {
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

std::string selectionLabel(const EngineServices& services) {
    if (!services.selection.current().has_value()) {
        return "None";
    }

    return entityLabel(services, *services.selection.current());
}

std::string hierarchyLabel(const EngineServices& services, EntityId entity) {
    std::string label = entityLabel(services, entity);
    if (services.world.pointLights.contains(entity)) {
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

void notifyTransformChanged(EngineServices& services, EntityId entity) {
    services.world.markTransformsDirty(entity);
    services.events.publish(TransformChangedEvent{entity});
}

void notifyLightChanged(EngineServices& services, EntityId entity) {
    services.world.markLightsDirty(entity);
    services.events.publish(LightChangedEvent{entity});
}

void notifyMaterialChanged(EngineServices& services, MaterialHandle materialHandle) {
    services.materials.notifyChanged(materialHandle);
    services.events.publish(MaterialChangedEvent{materialHandle});
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

struct SceneHierarchyData {
    std::vector<EntityId> roots{};
    std::vector<std::vector<EntityId>> childrenByParent{};
};

std::size_t sceneHierarchyChildCount(const SceneHierarchyData& hierarchy, EntityId entity) {
    if (entity.index >= hierarchy.childrenByParent.size()) {
        return 0u;
    }
    return hierarchy.childrenByParent[entity.index].size();
}

SceneHierarchyData buildSceneHierarchy(const EngineServices& services) {
    SceneHierarchyData hierarchy{};
    const World& world = services.world;

    std::vector<EntityId> entities{};
    std::vector<bool> seen(std::max(world.transformCache_.size(), world.lightRuntime_.size()), false);

    auto ensureIndex = [&](std::size_t index) {
        if (seen.size() <= index) {
            seen.resize(index + 1u, false);
        }
        if (hierarchy.childrenByParent.size() <= index) {
            hierarchy.childrenByParent.resize(index + 1u);
        }
    };

    auto addEntity = [&](EntityId entity) {
        if (!entity.valid() || !world.isAlive(entity)) {
            return;
        }
        ensureIndex(entity.index);
        if (seen[entity.index]) {
            return;
        }
        seen[entity.index] = true;
        entities.push_back(entity);
    };

    auto collectEntities = [&](const auto& store) {
        for (EntityId entity : store.entities()) {
            addEntity(entity);
        }
    };

    collectEntities(world.names);
    collectEntities(world.transforms);
    collectEntities(world.visibilities);
    collectEntities(world.renderables);
    collectEntities(world.pointLights);
    collectEntities(world.spotLights);

    for (EntityId child : world.parents.entities()) {
        addEntity(child);
        const ParentComponent& parent = world.parents.get(child);
        addEntity(parent.parent);
    }

    const auto compareEntitiesByLabel = [&](EntityId lhs, EntityId rhs) {
        const std::string lhsLabel = entityLabel(services, lhs);
        const std::string rhsLabel = entityLabel(services, rhs);
        if (lhsLabel != rhsLabel) {
            return lhsLabel < rhsLabel;
        }
        return lhs.index < rhs.index;
    };

    std::sort(entities.begin(), entities.end(), compareEntitiesByLabel);

    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr || !parent->parent.valid() || !world.isAlive(parent->parent)) {
            continue;
        }

        ensureIndex(parent->parent.index);
        hierarchy.childrenByParent[parent->parent.index].push_back(entity);
    }

    for (auto& children : hierarchy.childrenByParent) {
        std::sort(children.begin(), children.end(), compareEntitiesByLabel);
    }

    for (EntityId entity : entities) {
        const ParentComponent* parent = world.parents.tryGet(entity);
        if (parent == nullptr ||
            !parent->parent.valid() ||
            !world.isAlive(parent->parent) ||
            parent->parent.index >= seen.size() ||
            !seen[parent->parent.index]) {
            hierarchy.roots.push_back(entity);
        }
    }

    return hierarchy;
}

void selectHierarchyEntity(EngineServices& services, EntityId entity) {
    services.selection.set(entity);
    services.editorSession.activeInspectorTab = InspectorTab::Selection;
    services.editorSession.textureBrowserFocusRequested = true;
}

void drawTransformControls(
    EngineServices& services,
    EntityId entity,
    const std::string& moveLabel,
    const std::string& rotateLabel,
    const std::string& scaleLabel,
    const std::string& mergeKeyPrefix
) {
    if (TransformComponent* transform = services.world.transforms.tryGet(entity)) {
        editSnapshotCommand<TransformComponent>(
            "Position",
            moveLabel,
            mergeKeyPrefix + "-position-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
            },
            services.commands
        );
        editSnapshotCommand<TransformComponent>(
            "Rotation",
            rotateLabel,
            mergeKeyPrefix + "-rotation-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    notifyTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                return ImGui::DragFloat3("Rotation", glm::value_ptr(edited.rotationDeg), 0.5f);
            },
            services.commands
        );
        editSnapshotCommand<TransformComponent>(
            "Scale",
            scaleLabel,
            mergeKeyPrefix + "-scale-" + std::to_string(entity.index),
            *transform,
            [&services, entity](const TransformComponent& snapshot) {
                if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                    *target = snapshot;
                    target->scale = glm::max(target->scale, glm::vec3(0.01f));
                    notifyTransformChanged(services, entity);
                }
            },
            [](TransformComponent& edited) {
                const bool changed = ImGui::DragFloat3("Scale", glm::value_ptr(edited.scale), 0.02f);
                if (changed) {
                    edited.scale = glm::max(edited.scale, glm::vec3(0.01f));
                }
                return changed;
            },
            services.commands
        );
    }
}

void drawVisibilityControl(EngineServices& services, EntityId entity) {
    if (!services.world.visibilities.contains(entity)) {
        return;
    }

    bool visible = services.world.visibilities.get(entity).visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        const bool before = services.world.visibilities.get(entity).visible;
        services.commands.execute(std::make_unique<SnapshotCommand<bool>>(
            "Toggle Visibility",
            std::string{},
            before,
            visible,
            [&services, entity](const bool& snapshot) {
                if (VisibilityComponent* target = services.world.visibilities.tryGet(entity)) {
                    target->visible = snapshot;
                    notifyTransformChanged(services, entity);
                }
            },
            false
        ));
    }
}

std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool skeletonSubtreeMatchesSearch(
    const render::GltfModelData& model,
    const render::SkinData& skin,
    const std::string& needle,
    int skeletonIndex
) {
    if (skeletonIndex < 0 || skeletonIndex >= static_cast<int>(skin.skeletonNodeIndices.size())) {
        return false;
    }
    const int nodeIndex = skin.skeletonNodeIndices[static_cast<std::size_t>(skeletonIndex)];
    const std::string nodeName =
        nodeIndex >= 0 && nodeIndex < static_cast<int>(model.nodes.size())
            ? model.nodes[static_cast<std::size_t>(nodeIndex)].name
            : std::string{"<invalid>"};
    if (needle.empty() || lowercaseCopy(nodeName).find(needle) != std::string::npos) {
        return true;
    }
    for (std::size_t childIndex = 0; childIndex < skin.skeletonParentIndices.size(); ++childIndex) {
        if (skin.skeletonParentIndices[childIndex] == skeletonIndex &&
            skeletonSubtreeMatchesSearch(model, skin, needle, static_cast<int>(childIndex))) {
            return true;
        }
    }
    return false;
}

void drawSkeletonInspectorNode(
    const render::GltfModelData& model,
    const render::SkinData& skin,
    const AnimatedModelComponent& animation,
    const std::vector<glm::mat4>& skinMatrices,
    const glm::mat4& ownerWorldMatrix,
    const std::string& searchNeedle,
    int skeletonIndex
) {
    if (skeletonIndex < 0 || skeletonIndex >= static_cast<int>(skin.skeletonNodeIndices.size()) ||
        !skeletonSubtreeMatchesSearch(model, skin, searchNeedle, skeletonIndex)) {
        return;
    }
    const int nodeIndex = skin.skeletonNodeIndices[static_cast<std::size_t>(skeletonIndex)];
    const int jointIndex = skin.skeletonJointIndices[static_cast<std::size_t>(skeletonIndex)];
    const std::string nodeName =
        nodeIndex >= 0 && nodeIndex < static_cast<int>(model.nodes.size())
            ? model.nodes[static_cast<std::size_t>(nodeIndex)].name
            : std::string{"<invalid>"};

    bool hasVisibleChildren = false;
    for (std::size_t childIndex = 0; childIndex < skin.skeletonParentIndices.size(); ++childIndex) {
        if (skin.skeletonParentIndices[childIndex] == skeletonIndex &&
            skeletonSubtreeMatchesSearch(model, skin, searchNeedle, static_cast<int>(childIndex))) {
            hasVisibleChildren = true;
            break;
        }
    }

    ImGui::PushID(skeletonIndex);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasVisibleChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    const bool open = ImGui::TreeNodeEx(
        "joint",
        flags,
        "%s",
        nodeName.c_str()
    );
    if (open) {
        const int parentIndex = skin.skeletonParentIndices[static_cast<std::size_t>(skeletonIndex)];
        const std::string parentName =
            parentIndex >= 0 &&
                skin.skeletonNodeIndices[static_cast<std::size_t>(parentIndex)] >= 0 &&
                skin.skeletonNodeIndices[static_cast<std::size_t>(parentIndex)] < static_cast<int>(model.nodes.size())
            ? model.nodes[static_cast<std::size_t>(skin.skeletonNodeIndices[static_cast<std::size_t>(parentIndex)])].name
            : std::string{"<root>"};
        ImGui::Text(
            "Parent: %s",
            parentIndex >= 0 ? parentName.c_str() : "<root>"
        );

        if (nodeIndex >= 0 && nodeIndex < static_cast<int>(animation.globalNodeMatrices.size())) {
            const glm::vec3 position = glm::vec3(
                ownerWorldMatrix *
                animation.globalNodeMatrices[static_cast<std::size_t>(nodeIndex)] *
                glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)
            );
            ImGui::Text("World Pos: %.3f %.3f %.3f", position.x, position.y, position.z);
        }

        if (jointIndex >= 0 && static_cast<std::size_t>(jointIndex) < skinMatrices.size()) {
            const glm::mat4& matrix = skinMatrices[static_cast<std::size_t>(jointIndex)];
            ImGui::Text(
                "[%.3f %.3f %.3f %.3f]",
                matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]
            );
            ImGui::Text(
                "[%.3f %.3f %.3f %.3f]",
                matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]
            );
            ImGui::Text(
                "[%.3f %.3f %.3f %.3f]",
                matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]
            );
            ImGui::Text(
                "[%.3f %.3f %.3f %.3f]",
                matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]
            );
        } else {
            ImGui::TextUnformatted("No skin matrix (non-joint node)");
        }

        for (std::size_t childIndex = 0; childIndex < skin.skeletonParentIndices.size(); ++childIndex) {
            if (skin.skeletonParentIndices[childIndex] == skeletonIndex) {
                drawSkeletonInspectorNode(
                    model,
                    skin,
                    animation,
                    skinMatrices,
                    ownerWorldMatrix,
                    searchNeedle,
                    static_cast<int>(childIndex)
                );
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void drawAnimationInspector(EngineServices& services, EntityId selected) {
    const EntityId owner = services.world.animationOwnerEntity(selected);
    if (!owner.valid()) {
        return;
    }

    AnimatedModelComponent* animation = services.world.animatedModels.tryGet(owner);
    if (animation == nullptr || !animation->model) {
        return;
    }

    const render::GltfModelData& model = *animation->model;
    if (model.skins.empty() && model.animations.empty()) {
        return;
    }

    int inspectedSkinIndex = services.editorSession.animationInspectorSkinIndex;
    if (const SkinnedRenderableComponent* skinned = services.world.skinnedRenderables.tryGet(selected);
        skinned != nullptr && skinned->animationOwner == owner) {
        inspectedSkinIndex = skinned->skinIndex;
    }
    inspectedSkinIndex = std::clamp(inspectedSkinIndex, 0, std::max(static_cast<int>(model.skins.size()) - 1, 0));
    services.editorSession.animationInspectorSkinIndex = inspectedSkinIndex;

    ImGui::Separator();
    ImGui::TextUnformatted("Animation");
    if (model.animations.empty()) {
        ImGui::TextUnformatted("No animation clips. Showing bind pose.");
    } else {
        int selectedClip = animation->nextClip >= 0 ? animation->nextClip : animation->currentClip;
        selectedClip = std::clamp(selectedClip, 0, static_cast<int>(model.animations.size()) - 1);
        const char* preview = model.animations[static_cast<std::size_t>(selectedClip)].name.c_str();
        if (ImGui::BeginCombo("Clip", preview)) {
            for (std::size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
                const bool isSelected = static_cast<int>(clipIndex) == selectedClip;
                if (ImGui::Selectable(model.animations[clipIndex].name.c_str(), isSelected)) {
                    animation->requestedClip = static_cast<int>(clipIndex);
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button(animation->playing ? "Pause" : "Play")) {
            animation->playing = !animation->playing;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &animation->loop);
        ImGui::DragFloat("Speed", &animation->speed, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Blend Duration", &animation->blendDuration, 0.01f, 0.0f, 2.0f);

        if (animation->currentClip >= 0 && animation->currentClip < static_cast<int>(model.animations.size())) {
            const render::AnimationClip& currentClip = model.animations[static_cast<std::size_t>(animation->currentClip)];
            float currentTime = animation->currentTime;
            if (currentClip.duration > 0.0f &&
                ImGui::SliderFloat("Time", &currentTime, 0.0f, currentClip.duration)) {
                animation->currentTime = currentTime;
            }
            ImGui::Text("Current Clip: %s", currentClip.name.c_str());
        }
        if (animation->nextClip >= 0 && animation->nextClip < static_cast<int>(model.animations.size())) {
            const render::AnimationClip& nextClip = model.animations[static_cast<std::size_t>(animation->nextClip)];
            const float blendProgress = animation->blendDuration <= 0.0f
                ? 1.0f
                : std::clamp(animation->blendElapsed / animation->blendDuration, 0.0f, 1.0f);
            ImGui::Text("Next Clip: %s", nextClip.name.c_str());
            ImGui::ProgressBar(blendProgress, ImVec2(-1.0f, 0.0f));
        }
    }

    if (!model.skins.empty()) {
        if (model.skins.size() > 1u) {
            const char* preview = model.skins[static_cast<std::size_t>(inspectedSkinIndex)].name.c_str();
            if (ImGui::BeginCombo("Skin", preview)) {
                for (std::size_t skinIndex = 0; skinIndex < model.skins.size(); ++skinIndex) {
                    const bool isSelected = static_cast<int>(skinIndex) == inspectedSkinIndex;
                    if (ImGui::Selectable(model.skins[skinIndex].name.c_str(), isSelected)) {
                        inspectedSkinIndex = static_cast<int>(skinIndex);
                        services.editorSession.animationInspectorSkinIndex = inspectedSkinIndex;
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        const render::SkinData& skin = model.skins[static_cast<std::size_t>(inspectedSkinIndex)];
        const std::vector<glm::mat4>* skinMatrices = nullptr;
        if (inspectedSkinIndex >= 0 && inspectedSkinIndex < static_cast<int>(animation->skinJointMatrices.size())) {
            skinMatrices = &animation->skinJointMatrices[static_cast<std::size_t>(inspectedSkinIndex)];
        }
        glm::mat4 ownerWorldMatrix(1.0f);
        if (owner.index < services.world.transformCache_.size()) {
            ownerWorldMatrix = services.world.transformCache_[owner.index].worldMatrix;
        }
        ImGui::Checkbox("Show Skeleton Overlay", &animation->showSkeletonOverlay);
        ImGui::Text("Skeleton: %s", skin.name.c_str());
        ImGui::InputTextWithHint(
            "##SkeletonSearch",
            "Search bones...",
            services.editorSession.animationSkeletonSearch.data(),
            static_cast<int>(services.editorSession.animationSkeletonSearch.size())
        );

        const std::string searchNeedle = lowercaseCopy(services.editorSession.animationSkeletonSearch.data());
        if (ImGui::BeginChild("SkeletonInspector", ImVec2(0.0f, 260.0f), true)) {
            for (std::size_t skeletonIndex = 0; skeletonIndex < skin.skeletonParentIndices.size(); ++skeletonIndex) {
                if (skin.skeletonParentIndices[skeletonIndex] < 0) {
                    drawSkeletonInspectorNode(
                        model,
                        skin,
                        *animation,
                        skinMatrices != nullptr ? *skinMatrices : std::vector<glm::mat4>{},
                        ownerWorldMatrix,
                        searchNeedle,
                        static_cast<int>(skeletonIndex)
                    );
                }
            }
        }
        ImGui::EndChild();
    }
}

void drawSceneHierarchyNode(EngineServices& services, const SceneHierarchyData& hierarchy, EntityId entity) {
    static const std::vector<EntityId> emptyChildren{};

    const std::vector<EntityId>& children = entity.index < hierarchy.childrenByParent.size()
        ? hierarchy.childrenByParent[entity.index]
        : emptyChildren;
    const bool isSelected = services.selection.current().has_value() && *services.selection.current() == entity;

    ImGui::PushID(static_cast<int>(entity.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    }

    const bool open = ImGui::TreeNodeEx("node", flags, "%s", hierarchyLabel(services, entity).c_str());
    if (ImGui::IsItemClicked()) {
        selectHierarchyEntity(services, entity);
    }

    if (open && !children.empty()) {
        for (EntityId child : children) {
            drawSceneHierarchyNode(services, hierarchy, child);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void drawSceneHierarchyWindow(EngineServices& services) {
    if (!services.editorSession.sceneHierarchyVisible) {
        services.editorSession.sceneHierarchyFocusRequested = false;
        return;
    }

    if (services.editorSession.sceneHierarchyFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);

    const bool wasOpen = services.editorSession.sceneHierarchyVisible;
    bool open = wasOpen;
    if (ImGui::Begin("Scene Hierarchy", &open)) {
        const SceneHierarchyData hierarchy = buildSceneHierarchy(services);
        if (hierarchy.roots.empty()) {
            ImGui::TextUnformatted("No scene entities available.");
        } else if (ImGui::TreeNodeEx(
                       "CurrentScene",
                       ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                       "Current Scene")) {
            for (EntityId root : hierarchy.roots) {
                drawSceneHierarchyNode(services, hierarchy, root);
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    services.editorSession.sceneHierarchyVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.sceneHierarchyFocusRequested = false;
}

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
    if (!services.navigation.statusMessage.empty()) {
        const ImVec4 color = services.navigation.statusIsError
            ? ImVec4(0.90f, 0.32f, 0.32f, 1.0f)
            : ImVec4(0.35f, 0.78f, 0.42f, 1.0f);
        ImGui::TextColored(color, "%s", services.navigation.statusMessage.c_str());
    }

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
    ImGui::Checkbox("Editor Click Moves Character", &services.navigation.editor.testMoveMode);
    if (services.navigation.editor.testMoveMode) {
        services.navigation.editor.polygonCaptureActive = false;
    }

    ImGui::SeparatorText("Tagging");
    if (services.selection.current().has_value()) {
        const std::vector<EntityId> targets =
            services.navigationSystem.collectRenderableSelectionTargets(services.world, *services.selection.current());
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

void drawEditorMainWindow(EngineServices& services) {
    if (!services.editorSession.mainWindowVisible) {
        services.editorSession.mainWindowFocusRequested = false;
        return;
    }

    if (services.editorSession.mainWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_FirstUseEver);

    bool open = services.editorSession.mainWindowVisible;
    if (ImGui::Begin("Editor", &open)) {
        ImGui::TextUnformatted("Window Toggles");
        ImGui::Separator();

        bool sceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
        if (ImGui::Checkbox("Scene Hierarchy", &sceneHierarchyVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.sceneHierarchyVisible, sceneHierarchyVisible);
            if (!sceneHierarchyVisible) {
                services.editorSession.sceneHierarchyFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+S");

        bool inspectorVisible = services.editorSession.inspectorWindowVisible;
        if (ImGui::Checkbox("Inspector", &inspectorVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.inspectorWindowVisible, inspectorVisible);
            if (!inspectorVisible) {
                services.editorSession.inspectorWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+I");

        bool profilerVisible = services.editorSession.profilerWindowVisible;
        if (ImGui::Checkbox("Profiler", &profilerVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.profilerWindowVisible, profilerVisible);
            if (!profilerVisible) {
                services.editorSession.profilerWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+P");

        bool navMeshVisible = services.editorSession.navMeshWindowVisible;
        if (ImGui::Checkbox("NavMesh", &navMeshVisible)) {
            setPersistedEditorSessionFlag(services.editorSession.navMeshWindowVisible, navMeshVisible);
            if (!navMeshVisible) {
                services.editorSession.navMeshWindowFocusRequested = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+N");

        ImGui::Separator();
        if (ImGui::Button("Show All")) {
            const bool wasSceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
            const bool wasInspectorVisible = services.editorSession.inspectorWindowVisible;
            const bool wasProfilerVisible = services.editorSession.profilerWindowVisible;
            const bool wasNavMeshVisible = services.editorSession.navMeshWindowVisible;
            services.editorSession.setToolWindowsVisible(true);
            if (wasSceneHierarchyVisible != services.editorSession.sceneHierarchyVisible ||
                wasInspectorVisible != services.editorSession.inspectorWindowVisible ||
                wasProfilerVisible != services.editorSession.profilerWindowVisible ||
                wasNavMeshVisible != services.editorSession.navMeshWindowVisible) {
                markEditorSessionImGuiSettingsDirty();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All")) {
            const bool wasSceneHierarchyVisible = services.editorSession.sceneHierarchyVisible;
            const bool wasInspectorVisible = services.editorSession.inspectorWindowVisible;
            const bool wasProfilerVisible = services.editorSession.profilerWindowVisible;
            const bool wasNavMeshVisible = services.editorSession.navMeshWindowVisible;
            services.editorSession.setToolWindowsVisible(false);
            if (wasSceneHierarchyVisible != services.editorSession.sceneHierarchyVisible ||
                wasInspectorVisible != services.editorSession.inspectorWindowVisible ||
                wasProfilerVisible != services.editorSession.profilerWindowVisible ||
                wasNavMeshVisible != services.editorSession.navMeshWindowVisible) {
                markEditorSessionImGuiSettingsDirty();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Press E to toggle all editor features.");
    }
    ImGui::End();

    services.editorSession.mainWindowVisible = open;
    services.editorSession.mainWindowFocusRequested = false;
}

std::string formatProfilerBytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3) {
        value /= 1024.0;
        ++unitIndex;
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unitIndex]);
    return buffer;
}

const char* profilerStatusLabel(const ProfilerStats& stats) {
    if (stats.startPending) {
        return "Starting next frame";
    }
    if (stats.stopPending) {
        return "Stopping after current frame";
    }
    if (stats.capturing) {
        return "Capturing";
    }
    return "Idle";
}

void drawProfilerScopeRows(const std::vector<ProfilerScopeNode>& nodes) {
    for (const ProfilerScopeNode& node : nodes) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const ImGuiTreeNodeFlags flags = node.children.empty()
            ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
            : ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::TreeNodeEx(static_cast<const void*>(&node), flags, "%s", node.name.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f", node.totalMs);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.3f", node.selfMs);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", node.callCount);
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(node.thread.c_str());

        if (open && !node.children.empty()) {
            drawProfilerScopeRows(node.children);
            ImGui::TreePop();
        }
    }
}

void drawProfilerMemoryGroup(const char* label, const std::vector<const ResourceMemoryEntry*>& entries) {
    ImGui::SeparatorText(label);
    if (entries.empty()) {
        ImGui::TextUnformatted("No resources recorded in this residency group.");
        return;
    }

    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> totalsByCategory{};
    std::uint64_t totalCpuBytes = 0u;
    std::uint64_t totalGpuBytes = 0u;
    for (const ResourceMemoryEntry* entry : entries) {
        totalCpuBytes += entry->cpuBytes;
        totalGpuBytes += entry->gpuBytes;
        auto& totals = totalsByCategory[entry->category];
        totals.first += entry->cpuBytes;
        totals.second += entry->gpuBytes;
    }

    ImGui::Text(
        "Totals: %zu resources | RAM %s | GPU %s",
        entries.size(),
        formatProfilerBytes(totalCpuBytes).c_str(),
        formatProfilerBytes(totalGpuBytes).c_str()
    );

    const std::string categoryTableId = std::string("ProfilerCategoryTotals##") + label;
    if (ImGui::BeginTable(categoryTableId.c_str(), 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("RAM");
        ImGui::TableSetupColumn("GPU");
        ImGui::TableHeadersRow();
        for (const auto& [category, totals] : totalsByCategory) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(category.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(formatProfilerBytes(totals.first).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(formatProfilerBytes(totals.second).c_str());
        }
        ImGui::EndTable();
    }

    const std::string resourceTableId = std::string("ProfilerResources##") + label;
    if (ImGui::BeginTable(
            resourceTableId.c_str(),
            4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Resource");
        ImGui::TableSetupColumn("RAM");
        ImGui::TableSetupColumn("GPU");
        ImGui::TableHeadersRow();
        for (const ResourceMemoryEntry* entry : entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry->category.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry->name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry->cpuBytes > 0u ? formatProfilerBytes(entry->cpuBytes).c_str() : "--");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(entry->gpuBytes > 0u ? formatProfilerBytes(entry->gpuBytes).c_str() : "--");
        }
        ImGui::EndTable();
    }
}

double profilerCadenceMs(
    const std::vector<std::shared_ptr<const ProfilerFrameSnapshot>>& snapshots,
    int frameIndex
) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(snapshots.size())) {
        return 0.0;
    }

    const auto cadenceBetween = [](const std::shared_ptr<const ProfilerFrameSnapshot>& earlier,
                                   const std::shared_ptr<const ProfilerFrameSnapshot>& later) {
        if (!earlier || !later || earlier->sessionId != later->sessionId || later->startNs <= earlier->startNs) {
            return 0.0;
        }
        return static_cast<double>(later->startNs - earlier->startNs) / 1'000'000.0;
    };

    if (frameIndex > 0) {
        const double previousCadenceMs = cadenceBetween(snapshots[frameIndex - 1], snapshots[frameIndex]);
        if (previousCadenceMs > 0.0) {
            return previousCadenceMs;
        }
    }
    if (frameIndex + 1 < static_cast<int>(snapshots.size())) {
        return cadenceBetween(snapshots[frameIndex], snapshots[frameIndex + 1]);
    }
    return 0.0;
}

std::string buildProfilerWindowTitle(
    const std::vector<std::shared_ptr<const ProfilerFrameSnapshot>>& snapshots,
    int frameIndex
) {
    const double cadenceMs = profilerCadenceMs(snapshots, frameIndex);
    if (cadenceMs <= 0.0) {
        return "Profiler###ProfilerWindow";
    }

    const double fps = 1000.0 / cadenceMs;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "Profiler (%.1f FPS)###ProfilerWindow", fps);
    return buffer;
}

void drawProfilerWindow(EngineServices& services) {
    if (!services.editorSession.profilerWindowVisible) {
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    const auto snapshots = services.profiler.snapshots();
    int titleFrameIndex = -1;
    if (!snapshots.empty()) {
        titleFrameIndex = services.editorSession.profilerSelectedFrame;
        if (services.editorSession.profilerFollowLatest ||
            titleFrameIndex < 0 ||
            titleFrameIndex >= static_cast<int>(snapshots.size())) {
            titleFrameIndex = static_cast<int>(snapshots.size()) - 1;
        }
    }

    if (services.editorSession.profilerWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(860.0f, 720.0f), ImGuiCond_FirstUseEver);
    const bool wasOpen = services.editorSession.profilerWindowVisible;
    bool open = wasOpen;
    const std::string profilerWindowTitle = buildProfilerWindowTitle(snapshots, titleFrameIndex);
    if (!ImGui::Begin(profilerWindowTitle.c_str(), &open)) {
        ImGui::End();
        services.editorSession.profilerWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    const ProfilerStats stats = services.profiler.stats();

    ImGui::BeginDisabled(stats.capturing || stats.startPending);
    if (ImGui::Button("Start Profiling")) {
        services.profiler.startCapture();
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, true);
        services.editorSession.profilerExportStatus.clear();
        services.editorSession.profilerExportStatusIsError = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled((!stats.capturing && !stats.stopPending) || stats.startPending);
    if (ImGui::Button("Stop Profiling")) {
        services.profiler.stopCapture();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(stats.bufferedFrames == 0u);
    if (ImGui::Button("Export Perfetto Trace")) {
        const ProfilerTraceCapture capture = services.profiler.rawCapture();
        const std::filesystem::path exportPath = defaultProfilerExportPath(capture);
        std::string exportError{};
        if (services.profiler.exportPerfettoTrace(exportPath.string(), &exportError)) {
            services.editorSession.profilerExportStatus =
                "Exported Perfetto trace to " + std::filesystem::absolute(exportPath).string();
            services.editorSession.profilerExportStatusIsError = false;
        } else {
            services.editorSession.profilerExportStatus = exportError.empty()
                ? "Perfetto export failed."
                : exportError;
            services.editorSession.profilerExportStatusIsError = true;
        }
    }
    ImGui::EndDisabled();

    if (!services.editorSession.profilerExportStatus.empty()) {
        if (services.editorSession.profilerExportStatusIsError) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "%s", services.editorSession.profilerExportStatus.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 0.40f, 1.0f), "%s", services.editorSession.profilerExportStatus.c_str());
        }
    }

    ImGui::Text(
        "Status: %s | Buffered: %zu | Pending GPU Frames: %zu",
        profilerStatusLabel(stats),
        stats.bufferedFrames,
        stats.pendingGpuFrames
    );
    ImGui::Text(
        "Dropped CPU scopes: %llu | Dropped GPU scopes: %llu | Dropped frames: %llu",
        static_cast<unsigned long long>(stats.droppedCpuScopes),
        static_cast<unsigned long long>(stats.droppedGpuScopes),
        static_cast<unsigned long long>(stats.droppedFrames)
    );

    if (snapshots.empty()) {
        ImGui::SeparatorText("Frames");
        ImGui::TextUnformatted("No captured frames available yet.");
        ImGui::End();
        services.editorSession.profilerWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    if (services.editorSession.profilerFollowLatest ||
        services.editorSession.profilerSelectedFrame < 0 ||
        services.editorSession.profilerSelectedFrame >= static_cast<int>(snapshots.size())) {
        services.editorSession.profilerSelectedFrame = static_cast<int>(snapshots.size()) - 1;
    }

    ImGui::SeparatorText("Frame History");
    bool followLatest = services.editorSession.profilerFollowLatest;
    if (ImGui::Checkbox("Follow Latest", &followLatest)) {
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, followLatest);
        if (followLatest) {
            services.editorSession.profilerSelectedFrame = static_cast<int>(snapshots.size()) - 1;
        }
    }

    std::vector<float> cpuTotals{};
    std::vector<float> gpuTotals{};
    cpuTotals.reserve(snapshots.size());
    gpuTotals.reserve(snapshots.size());
    float maxGraphValue = 1.0f;
    for (const auto& snapshot : snapshots) {
        cpuTotals.push_back(static_cast<float>(snapshot->cpuFrameMs));
        gpuTotals.push_back(static_cast<float>(snapshot->gpuFrameMs));
        maxGraphValue = std::max(maxGraphValue, std::max(cpuTotals.back(), gpuTotals.back()));
    }

    ImGui::PlotLines("CPU Frame (ms)", cpuTotals.data(), static_cast<int>(cpuTotals.size()), 0, nullptr, 0.0f, maxGraphValue, ImVec2(-1.0f, 80.0f));
    ImGui::PlotLines(
        "GPU Timed Passes (ms)",
        gpuTotals.data(),
        static_cast<int>(gpuTotals.size()),
        0,
        nullptr,
        0.0f,
        maxGraphValue,
        ImVec2(-1.0f, 80.0f)
    );

    int selectedFrame = services.editorSession.profilerSelectedFrame;
    if (ImGui::SliderInt("Selected Frame", &selectedFrame, 0, static_cast<int>(snapshots.size()) - 1)) {
        services.editorSession.profilerSelectedFrame = selectedFrame;
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, false);
    }

    const std::shared_ptr<const ProfilerFrameSnapshot>& selected = snapshots[services.editorSession.profilerSelectedFrame];
    ImGui::Text(
        "Frame #%llu | CPU %.3f ms | Profiler UI %.3f ms",
        static_cast<unsigned long long>(selected->frameNumber),
        selected->cpuFrameMs,
        selected->profilerUiMs
    );
    if (selected->gpuComplete) {
        ImGui::SameLine();
        ImGui::Text("GPU Timed Total %.3f ms", selected->gpuFrameMs);
    } else {
        ImGui::SameLine();
        ImGui::TextUnformatted("GPU Pending/Unavailable");
    }

    std::vector<const render::FrameCounterRecord*> frustumCounters{};
    frustumCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> occlusionCounters{};
    occlusionCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> runtimePolicyCounters{};
    runtimePolicyCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> generalCounters{};
    generalCounters.reserve(selected->counters.size());

    const auto counterValue = [&](std::string_view group, std::string_view name) -> std::optional<std::int64_t> {
        const auto it = std::find_if(selected->counters.begin(), selected->counters.end(), [group, name](const render::FrameCounterRecord& counter) {
            return counter.group == group && counter.name == name;
        });
        if (it == selected->counters.end()) {
            return std::nullopt;
        }
        return it->value;
    };
    for (const render::FrameCounterRecord& counter : selected->counters) {
        if (counter.group == "Frustum Culling") {
            frustumCounters.push_back(&counter);
        } else if (counter.group == "Occlusion Culling") {
            occlusionCounters.push_back(&counter);
        } else if (counter.group == "Runtime Policy") {
            runtimePolicyCounters.push_back(&counter);
        } else {
            generalCounters.push_back(&counter);
        }
    }

    sortCounterPointers(frustumCounters);
    sortCounterPointers(occlusionCounters);
    sortCounterPointers(runtimePolicyCounters);
    sortCounterPointers(generalCounters);

    if (!frustumCounters.empty()) {
        ImGui::SeparatorText("Frustum Culling");
        drawProfilerCounterTable("ProfilerFrustumCounters", frustumCounters);

        const double boundsTested = static_cast<double>(counterValue("Frustum Culling", "Bounds Tested").value_or(0));
        const double culled = static_cast<double>(counterValue("Frustum Culling", "Culled").value_or(0));
        const double cullRatio = boundsTested > 0.0 ? culled / boundsTested : 0.0;
        ImGui::Text("Cull Ratio: %.2f%%", cullRatio * 100.0);
    }

    if (!occlusionCounters.empty()) {
        ImGui::SeparatorText("Occlusion Culling");
        drawProfilerCounterTable("ProfilerOcclusionCounters", occlusionCounters);

        const double candidates = static_cast<double>(counterValue("Occlusion Culling", "Candidates").value_or(0));
        const double occluded = static_cast<double>(counterValue("Occlusion Culling", "Occluded").value_or(0));
        const double occlusionRatio = candidates > 0.0 ? occluded / candidates : 0.0;
        ImGui::Text("Occlusion Ratio: %.2f%%", occlusionRatio * 100.0);
    }
    if (!runtimePolicyCounters.empty()) {
        ImGui::SeparatorText("Runtime Policy");
        drawProfilerCounterTable("ProfilerRuntimePolicyCounters", runtimePolicyCounters);
    }
    if (!generalCounters.empty()) {
        ImGui::SeparatorText("Metrics");
        drawProfilerCounterTable("ProfilerGeneralCounters", generalCounters);
    }

    ImGui::SeparatorText("CPU Call Tree");
    if (ImGui::BeginTable(
            "ProfilerCpuTree",
            5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("Total ms");
        ImGui::TableSetupColumn("Self ms");
        ImGui::TableSetupColumn("Calls");
        ImGui::TableSetupColumn("Thread");
        ImGui::TableHeadersRow();
        drawProfilerScopeRows(selected->cpuScopes);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("GPU Timed Passes");
    if (ImGui::BeginTable(
            "ProfilerGpuPasses",
            3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Duration");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (const GpuPassSample& pass : selected->gpuPasses) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (pass.available) {
                ImGui::Text("%.3f ms", pass.durationMs);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableSetColumnIndex(2);
            if (pass.pending) {
                ImGui::TextUnformatted("Pending");
            } else if (pass.available) {
                ImGui::TextUnformatted("Ready");
            } else {
                ImGui::TextUnformatted("Unavailable");
            }
        }
        ImGui::EndTable();
    }

    std::vector<const ResourceMemoryEntry*> ramEntries{};
    std::vector<const ResourceMemoryEntry*> gpuEntries{};
    std::vector<const ResourceMemoryEntry*> bothEntries{};
    for (const ResourceMemoryEntry& entry : selected->resources) {
        if (entry.cpuBytes > 0u && entry.gpuBytes > 0u) {
            bothEntries.push_back(&entry);
        } else if (entry.cpuBytes > 0u) {
            ramEntries.push_back(&entry);
        } else if (entry.gpuBytes > 0u) {
            gpuEntries.push_back(&entry);
        }
    }

    drawProfilerMemoryGroup("Both", bothEntries);
    drawProfilerMemoryGroup("RAM", ramEntries);
    drawProfilerMemoryGroup("GPU", gpuEntries);

    ImGui::End();
    services.editorSession.profilerWindowVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.profilerWindowFocusRequested = false;
}

void drawTextureBrowser(EngineServices& services, render::Material& material, MaterialHandle materialHandle) {
    render::TextureRef* targetRef = render::textureRefForSlot(material, services.editorSession.textureBrowserSlot);
    if (targetRef == nullptr) {
        ImGui::TextUnformatted("No texture slot selected.");
        return;
    }

    const render::TextureSemantic semantic = render::textureSemanticForSlot(services.editorSession.textureBrowserSlot);
    std::vector<std::shared_ptr<render::Texture>> filtered = services.renderer.textureCatalog(semantic);
    if (services.editorSession.textureBrowserSearch[0] != '\0') {
        std::string needle(services.editorSession.textureBrowserSearch.data());
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        filtered.erase(
            std::remove_if(
                filtered.begin(),
                filtered.end(),
                [&needle](const std::shared_ptr<render::Texture>& texture) {
                    std::string lowered = texture->name;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                    return lowered.find(needle) == std::string::npos;
                }
            ),
            filtered.end()
        );
    }

    ImGui::Text("Target Material: %s", material.name.c_str());
    ImGui::Text("Target Slot: %s", render::materialTextureSlotName(services.editorSession.textureBrowserSlot));
    if (ImGui::Button("Back To Selection")) {
        services.editorSession.activeInspectorTab = InspectorTab::Selection;
        services.editorSession.textureBrowserFocusRequested = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##TextureSearch",
        "Search textures...",
        services.editorSession.textureBrowserSearch.data(),
        static_cast<int>(services.editorSession.textureBrowserSearch.size())
    );
    ImGui::Separator();
    ImGui::Text("%d textures", static_cast<int>(filtered.size()));

    ImGui::BeginChild("TextureBrowserResults", ImVec2(0.0f, 420.0f), false);
    const float cellWidth = 180.0f;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, cellWidth);
    const int columnCount = std::max(1, static_cast<int>(availableWidth / cellWidth));
    if (ImGui::BeginTable("TextureBrowserGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& texture : filtered) {
            ImGui::TableNextColumn();
            ImGui::PushID(texture.get());

            if (void* previewId = services.renderer.texturePreviewId(texture)) {
                ImGui::Image(previewId, ImVec2(96.0f, 96.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            } else {
                ImGui::BeginChild("NoPreview", ImVec2(96.0f, 96.0f), true);
                ImGui::TextUnformatted("No");
                ImGui::TextUnformatted("Preview");
                ImGui::EndChild();
            }

            ImGui::TextWrapped("%s", texture->name.c_str());
            ImGui::Text("%s | %s", render::textureOriginName(texture->origin), render::textureSemanticName(texture->semantic));
            ImGui::Text("%dx%d | %s", texture->width, texture->height, render::formatName(texture->format));
            if (ImGui::Button("Use Texture", ImVec2(-1.0f, 0.0f))) {
                const render::Material before = material;
                targetRef->texture = texture;
                targetRef->sampler = services.renderer.defaultSampler();
                targetRef->bindingMode = render::TextureBindingMode::ProjectTexture;
                const render::Material after = material;
                services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
                    "Assign Texture",
                    std::string{},
                    before,
                    after,
                    [&services, materialHandle](const render::Material& snapshot) {
                        if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                            *materialPtr = snapshot;
                            notifyMaterialChanged(services, materialHandle);
                        }
                    },
                    false
                ));
                services.editorSession.activeInspectorTab = InspectorTab::Selection;
                services.editorSession.textureBrowserFocusRequested = true;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void drawTextureSlotEditor(
    EngineServices& services,
    const std::string& materialName,
    MaterialHandle materialHandle,
    render::MaterialTextureSlot slot,
    render::Material& material
) {
    render::TextureRef* ref = render::textureRefForSlot(material, slot);
    if (ref == nullptr) {
        return;
    }

    ImGui::PushID(static_cast<int>(slot));
    if (!ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_DefaultOpen, "%s", render::materialTextureSlotName(slot))) {
        ImGui::PopID();
        return;
    }

    if (ref->texture) {
        if (void* previewId = services.renderer.texturePreviewId(ref->texture)) {
            ImGui::Image(previewId, ImVec2(64.0f, 64.0f), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Mode: %s", render::textureBindingModeName(ref->bindingMode));
    ImGui::Text("Texture: %s", ref->texture ? ref->texture->name.c_str() : "Default");
    ImGui::EndGroup();

    const render::Material beforeMaterial = material;
    int bindingMode = static_cast<int>(ref->bindingMode);
    if (ImGui::Combo("Binding Mode", &bindingMode, "Default\0Project Texture\0Inline Value\0")) {
        ref->bindingMode = static_cast<render::TextureBindingMode>(bindingMode);
        if (ref->bindingMode == render::TextureBindingMode::ProjectTexture) {
            auto catalog = services.renderer.textureCatalog(render::textureSemanticForSlot(slot));
            ref->texture = catalog.empty() ? nullptr : catalog.front();
            ref->sampler = services.renderer.defaultSampler();
        } else if (ref->bindingMode == render::TextureBindingMode::InlineValue) {
            ref->inlineValue = render::defaultInlineValueForSlot(slot);
            ref->texture = render::makeSolidTexture(
                materialName + " / " + render::materialTextureSlotName(slot) + " Inline",
                ref->inlineValue,
                false,
                render::textureSemanticForSlot(slot),
                render::TextureOrigin::InlinePrivate
            );
            ref->sampler = services.renderer.defaultSampler();
            services.renderer.registerTexture(ref->texture);
        }

        services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
            "Texture Binding",
            std::string{},
            beforeMaterial,
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            false
        ));
    }

    if (ref->bindingMode == render::TextureBindingMode::ProjectTexture) {
        const bool isTextureBrowserTarget = services.editorSession.textureBrowserSlot == slot;
        const char* textureTargetLabel =
            isTextureBrowserTarget ? "Pick texture in Texture Browser" : "Set texture as target";
        if (ImGui::Button(textureTargetLabel)) {
            services.editorSession.textureBrowserSlot = slot;
            services.editorSession.activeInspectorTab = InspectorTab::TextureBrowser;
            services.editorSession.textureBrowserFocusRequested = true;
        }
    } else if (ref->bindingMode == render::TextureBindingMode::InlineValue) {
        editSnapshotCommand<render::Material>(
            "InlineValue",
            "Edit Inline Material Value",
            "material-inline-" + std::to_string(materialHandle.value) + "-" + std::to_string(static_cast<int>(slot)),
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    render::TextureRef* target = render::textureRefForSlot(*materialPtr, services.editorSession.textureBrowserSlot);
                    (void)target;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            [&material, ref, slot](render::Material& editedMaterial) {
                render::TextureRef* editedRef = render::textureRefForSlot(editedMaterial, slot);
                bool changed = false;
                switch (slot) {
                    case render::MaterialTextureSlot::BaseColor:
                        changed = ImGui::ColorEdit4("Inline Value", glm::value_ptr(editedRef->inlineValue));
                        break;
                    case render::MaterialTextureSlot::MetallicRoughness: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::DragFloat3("Inline AO/Rough/Metal", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Normal:
                    case render::MaterialTextureSlot::DetailNormal: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::DragFloat3("Inline Normal", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Ao:
                    case render::MaterialTextureSlot::Alpha:
                    case render::MaterialTextureSlot::Height: {
                        float scalar = editedRef->inlineValue.x;
                        changed = ImGui::DragFloat("Inline Value", &scalar, 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(scalar, scalar, scalar, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Emissive: {
                        glm::vec3 value(editedRef->inlineValue);
                        changed = ImGui::ColorEdit3("Inline Value", glm::value_ptr(value));
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 1.0f);
                        }
                        break;
                    }
                    case render::MaterialTextureSlot::Clearcoat: {
                        glm::vec2 value(editedRef->inlineValue.x, editedRef->inlineValue.y);
                        changed = ImGui::DragFloat2("Inline Factor/Rough", glm::value_ptr(value), 0.01f, 0.0f, 1.0f, "%.2f");
                        if (changed) {
                            editedRef->inlineValue = glm::vec4(value, 0.0f, 1.0f);
                        }
                        break;
                    }
                }

                if (changed) {
                    editedRef->texture = render::makeSolidTexture(
                        material.name + " / " + render::materialTextureSlotName(slot) + " Inline",
                        editedRef->inlineValue,
                        false,
                        render::textureSemanticForSlot(slot),
                        render::TextureOrigin::InlinePrivate
                    );
                }
                return changed;
            },
            services.commands,
            true
        );
    }

    int uvSet = std::clamp(ref->uvSet, 0, 1);
    if (ImGui::SliderInt("UV Set", &uvSet, 0, 1)) {
        const render::Material before = material;
        ref->uvSet = uvSet;
        services.commands.execute(std::make_unique<SnapshotCommand<render::Material>>(
            "Change UV Set",
            std::string{},
            before,
            material,
            [&services, materialHandle](const render::Material& snapshot) {
                if (std::shared_ptr<render::Material> materialPtr = services.materials.get(materialHandle)) {
                    *materialPtr = snapshot;
                    notifyMaterialChanged(services, materialHandle);
                }
            },
            false
        ));
    }

    ImGui::TreePop();
    ImGui::PopID();
}

void drawMaterialTextureSlotGrid(
    EngineServices& services,
    const std::string& materialName,
    MaterialHandle materialHandle,
    render::Material& material
) {
    static constexpr render::MaterialTextureSlot kTextureSlots[] = {
        render::MaterialTextureSlot::BaseColor,
        render::MaterialTextureSlot::MetallicRoughness,
        render::MaterialTextureSlot::Normal,
        render::MaterialTextureSlot::Ao,
        render::MaterialTextureSlot::Emissive,
        render::MaterialTextureSlot::Alpha,
        render::MaterialTextureSlot::Clearcoat,
        render::MaterialTextureSlot::DetailNormal,
        render::MaterialTextureSlot::Height,
    };

    ImGui::SeparatorText("Texture Slots");

    constexpr float kSlotColumnMinWidth = 280.0f;
    const float availableWidth = std::max(ImGui::GetContentRegionAvail().x, kSlotColumnMinWidth);
    const int columnCount = std::clamp(static_cast<int>(availableWidth / kSlotColumnMinWidth), 1, 2);
    if (!ImGui::BeginTable(
            "MaterialTextureSlots",
            columnCount,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
        return;
    }

    for (render::MaterialTextureSlot slot : kTextureSlots) {
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(slot));

        constexpr float kSlotCardPadding = 8.0f;
        constexpr float kSlotCardRounding = 6.0f;

        const ImVec2 cellStartScreen = ImGui::GetCursorScreenPos();
        const ImVec2 cellStart = ImGui::GetCursorPos();
        const float cellWidth = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorPos(ImVec2(cellStart.x + kSlotCardPadding, cellStart.y + kSlotCardPadding));
        ImGui::BeginGroup();
        drawTextureSlotEditor(services, materialName, materialHandle, slot, material);
        ImGui::EndGroup();

        const ImVec2 groupMax = ImGui::GetItemRectMax();
        ImGui::Dummy(ImVec2(0.0f, kSlotCardPadding));

        const ImVec2 cardMin = cellStartScreen;
        const ImVec2 cardMax(cellStartScreen.x + cellWidth, groupMax.y + kSlotCardPadding);
        ImGui::GetWindowDrawList()->AddRect(
            cardMin,
            cardMax,
            ImGui::GetColorU32(ImVec4(0.48f, 0.48f, 0.48f, 0.9f)),
            kSlotCardRounding,
            0,
            1.0f
        );

        ImGui::PopID();
    }

    ImGui::EndTable();
}

void drawInspectorWindow(EngineServices& services) {
    if (!services.editorSession.inspectorWindowVisible) {
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    if (services.editorSession.inspectorWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::SetNextWindowSize(ImVec2(640.0f, 720.0f), ImGuiCond_FirstUseEver);

    std::string inspectorTitle = "Inspector";
    if (services.selection.current().has_value()) {
        inspectorTitle += " - " + selectionLabel(services);
    }
    inspectorTitle += "###Inspector";

    const bool wasOpen = services.editorSession.inspectorWindowVisible;
    bool open = wasOpen;
    if (!ImGui::Begin(inspectorTitle.c_str(), &open)) {
        ImGui::End();
        services.editorSession.inspectorWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    if (!services.selection.current().has_value()) {
        ImGui::TextUnformatted("Click an object or light to inspect it.");
        ImGui::End();
        services.editorSession.inspectorWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.inspectorWindowFocusRequested = false;
        services.editorSession.textureBrowserFocusRequested = false;
        return;
    }

    const EntityId selected = *services.selection.current();
    const ImGuiTabItemFlags selectionTabFlags =
        (services.editorSession.textureBrowserFocusRequested &&
         services.editorSession.activeInspectorTab == InspectorTab::Selection)
            ? ImGuiTabItemFlags_SetSelected
            : 0;
    const ImGuiTabItemFlags browserTabFlags =
        (services.editorSession.textureBrowserFocusRequested &&
         services.editorSession.activeInspectorTab == InspectorTab::TextureBrowser)
            ? ImGuiTabItemFlags_SetSelected
            : 0;

    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Selection", nullptr, selectionTabFlags)) {
            services.editorSession.activeInspectorTab = InspectorTab::Selection;
            ImGui::BeginChild("SelectionInspectorContent", ImVec2(0.0f, 0.0f), false);

            if (services.world.renderables.contains(selected)) {
                const EntityId transformEntity = services.world.editableTransformEntity(selected);
                drawTransformControls(
                    services,
                    transformEntity,
                    "Move Entity",
                    "Rotate Entity",
                    "Scale Entity",
                    "transform"
                );
                drawVisibilityControl(services, selected);

                if (selected.index < services.world.transformCache_.size() &&
                    services.world.transformCache_[selected.index].hasWorldBounds) {
                    const render::Bounds3& bounds = services.world.transformCache_[selected.index].worldBounds;
                    ImGui::Separator();
                    ImGui::Text("World Bounds Min: %.2f %.2f %.2f", bounds.min.x, bounds.min.y, bounds.min.z);
                    ImGui::Text("World Bounds Max: %.2f %.2f %.2f", bounds.max.x, bounds.max.y, bounds.max.z);
                }

                const RenderableComponent& renderable = services.world.renderables.get(selected);
                if (std::shared_ptr<render::Material> material = services.materials.get(renderable.material)) {
                    ImGui::Separator();
                    ImGui::Text("Material Asset: %s", material->name.c_str());

                    editSnapshotCommand<render::Material>(
                        "BaseColorFactor",
                        "Edit Base Color",
                        "material-base-color-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::ColorEdit3("Base Color Factor", glm::value_ptr(edited.baseColorFactor));
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "MetallicFactor",
                        "Edit Metallic",
                        "material-metallic-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Metallic Factor", &edited.metallicFactor, 0.01f, 0.0f, 1.0f);
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "RoughnessFactor",
                        "Edit Roughness",
                        "material-roughness-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Roughness Factor", &edited.roughnessFactor, 0.01f, 0.0f, 1.0f);
                        },
                        services.commands
                    );
                    editSnapshotCommand<render::Material>(
                        "NormalScale",
                        "Edit Normal Scale",
                        "material-normal-scale-" + std::to_string(renderable.material.value),
                        *material,
                        [&services, handle = renderable.material](const render::Material& snapshot) {
                            if (std::shared_ptr<render::Material> target = services.materials.get(handle)) {
                                *target = snapshot;
                                notifyMaterialChanged(services, handle);
                            }
                        },
                        [](render::Material& edited) {
                            return ImGui::DragFloat("Normal Scale", &edited.normalScale, 0.01f, 0.0f, 8.0f);
                        },
                        services.commands
                    );

                    drawMaterialTextureSlotGrid(services, material->name, renderable.material, *material);
                }
            } else if (PointLightComponent* pointLight = services.world.pointLights.tryGet(selected)) {
                if (TransformComponent* transform = services.world.transforms.tryGet(selected)) {
                    editSnapshotCommand<TransformComponent>(
                        "Position",
                        "Move Point Light",
                        "point-light-position-" + std::to_string(selected.index),
                        *transform,
                        [&services, selected](const TransformComponent& snapshot) {
                            if (TransformComponent* target = services.world.transforms.tryGet(selected)) {
                                *target = snapshot;
                                notifyTransformChanged(services, selected);
                                notifyLightChanged(services, selected);
                            }
                        },
                        [](TransformComponent& edited) {
                            return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
                        },
                        services.commands
                    );
                }

                editSnapshotCommand<PointLightComponent>(
                    "PointLightProps",
                    "Edit Point Light",
                    "point-light-props-" + std::to_string(selected.index),
                    *pointLight,
                    [&services, selected](const PointLightComponent& snapshot) {
                        if (PointLightComponent* target = services.world.pointLights.tryGet(selected)) {
                            *target = snapshot;
                            notifyLightChanged(services, selected);
                        }
                    },
                    [](PointLightComponent& edited) {
                        bool changed = false;
                        changed |= ImGui::DragFloat("Radius", &edited.radius, 0.1f, 0.1f, 128.0f);
                        changed |= ImGui::ColorEdit3("Color", glm::value_ptr(edited.color));
                        changed |= ImGui::DragFloat("Intensity", &edited.intensity, 0.1f, 0.0f, 128.0f);
                        changed |= ImGui::Checkbox("Movable", &edited.isMovable);
                        changed |= ImGui::Checkbox("Casts Shadow", &edited.castsShadow);
                        return changed;
                    },
                    services.commands
                );
            } else if (SpotLightComponent* spotLight = services.world.spotLights.tryGet(selected)) {
                if (TransformComponent* transform = services.world.transforms.tryGet(selected)) {
                    editSnapshotCommand<TransformComponent>(
                        "Position",
                        "Move Spot Light",
                        "spot-light-position-" + std::to_string(selected.index),
                        *transform,
                        [&services, selected](const TransformComponent& snapshot) {
                            if (TransformComponent* target = services.world.transforms.tryGet(selected)) {
                                *target = snapshot;
                                notifyTransformChanged(services, selected);
                                notifyLightChanged(services, selected);
                            }
                        },
                        [](TransformComponent& edited) {
                            return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
                        },
                        services.commands
                    );
                }

                editSnapshotCommand<SpotLightComponent>(
                    "SpotLightProps",
                    "Edit Spot Light",
                    "spot-light-props-" + std::to_string(selected.index),
                    *spotLight,
                    [&services, selected](const SpotLightComponent& snapshot) {
                        if (SpotLightComponent* target = services.world.spotLights.tryGet(selected)) {
                            *target = snapshot;
                            notifyLightChanged(services, selected);
                        }
                    },
                    [](SpotLightComponent& edited) {
                        bool changed = false;
                        changed |= ImGui::DragFloat("Radius", &edited.radius, 0.1f, 0.1f, 128.0f);
                        changed |= ImGui::ColorEdit3("Color", glm::value_ptr(edited.color));
                        changed |= ImGui::DragFloat("Intensity", &edited.intensity, 0.1f, 0.0f, 128.0f);
                        changed |= ImGui::DragFloat3("Target", glm::value_ptr(edited.target), 0.05f);
                        changed |= ImGui::DragFloat("Inner Angle", &edited.innerAngle, 0.25f, 1.0f, edited.outerAngle);
                        changed |= ImGui::DragFloat("Outer Angle", &edited.outerAngle, 0.25f, edited.innerAngle, 80.0f);
                        changed |= ImGui::Checkbox("Movable", &edited.isMovable);
                        changed |= ImGui::Checkbox("Casts Shadow", &edited.castsShadow);
                        return changed;
                    },
                    services.commands
                );
            } else {
                const EntityId transformEntity = services.world.editableTransformEntity(selected);
                if (transformEntity.valid()) {
                    drawTransformControls(
                        services,
                        transformEntity,
                        "Move Entity",
                        "Rotate Entity",
                        "Scale Entity",
                        "scene-node"
                    );
                }

                drawVisibilityControl(services, selected);

                const SceneHierarchyData hierarchy = buildSceneHierarchy(services);
                const std::size_t childCount = sceneHierarchyChildCount(hierarchy, selected);
                if (selected.index < services.world.transformCache_.size() &&
                    services.world.transformCache_[selected.index].hasWorldBounds) {
                    const render::Bounds3& bounds = services.world.transformCache_[selected.index].worldBounds;
                    ImGui::Separator();
                    ImGui::Text("World Bounds Min: %.2f %.2f %.2f", bounds.min.x, bounds.min.y, bounds.min.z);
                    ImGui::Text("World Bounds Max: %.2f %.2f %.2f", bounds.max.x, bounds.max.y, bounds.max.z);
                }
                if (childCount > 0u) {
                    ImGui::Separator();
                    ImGui::Text("Child Nodes: %d", static_cast<int>(childCount));
                } else if (!transformEntity.valid()) {
                    ImGui::TextUnformatted("Selected scene node has no editable properties.");
                }
            }

            drawAnimationInspector(services, selected);

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (services.world.renderables.contains(selected) &&
            ImGui::BeginTabItem("Texture Browser", nullptr, browserTabFlags)) {
            if (std::shared_ptr<render::Material> material = services.materials.get(
                    services.world.renderables.get(selected).material)) {
                drawTextureBrowser(services, *material, services.world.renderables.get(selected).material);
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    services.editorSession.inspectorWindowVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.inspectorWindowFocusRequested = false;
    services.editorSession.textureBrowserFocusRequested = false;
}

}  // namespace

void BootstrapState::onEnter(EngineServices& services) {
    if (!services.renderer.init()) {
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    registerEditorSessionImGuiSettings(services.editorSession);
    services.currentScene = services.sceneRegistry.defaultScene();
    services.sceneLoaded = services.sceneFactory.buildScene(
        services.currentScene,
        services.world,
        services.materials,
        services.renderer
    );
    if (!services.sceneLoaded) {
        spdlog::error("Application: failed to build default scene");
        services.requestedMode = AppMode::Shutdown;
        return;
    }
    if (!services.navigationSystem.initializeScene(services.currentScene, services.world, services.navigation)) {
        spdlog::error("Application: navigation init failed: {}", services.navigation.statusMessage);
    }
    services.requestedMode = AppMode::Gameplay;
}

void BootstrapState::onExit(EngineServices&) {}
void BootstrapState::update(EngineServices&) {}
void BootstrapState::renderUi(EngineServices&) {}

void GameplayState::onEnter(EngineServices&) {}

void GameplayState::onExit(EngineServices&) {}

void GameplayState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void GameplayState::renderUi(EngineServices&) {}

void EditorState::onEnter(EngineServices& services) {
    if (!services.editorSession.mainWindowVisible) {
        services.editorSession.openMainWindow();
    }
}

void EditorState::onExit(EngineServices& services) {
    services.editorSession.suspendEditorUi();
}

void EditorState::update(EngineServices& services) {
    updateOrbitCamera(services.camera, services.time);
}

void EditorState::renderUi(EngineServices& services) {
    drawEditorMainWindow(services);

    const auto profilerWindowStart = std::chrono::steady_clock::now();
    drawProfilerWindow(services);
    const auto profilerWindowEnd = std::chrono::steady_clock::now();
    services.profiler.recordProfilerUiTime(
        std::chrono::duration<double, std::milli>(profilerWindowEnd - profilerWindowStart).count()
    );

    ALKANZAR_PROFILE_SCOPE(services.profiler, "State UI Render");
    drawSceneHierarchyWindow(services);
    drawNavMeshWindow(services);
    drawInspectorWindow(services);
}

void ShutdownState::onEnter(EngineServices& services) {
    services.running = false;
}

void ShutdownState::onExit(EngineServices&) {}
void ShutdownState::update(EngineServices&) {}
void ShutdownState::renderUi(EngineServices&) {}

}  // namespace core
