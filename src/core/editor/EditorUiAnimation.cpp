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

}  // namespace core

