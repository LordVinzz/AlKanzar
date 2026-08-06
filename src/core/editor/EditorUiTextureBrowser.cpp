#include "core/editor/EditorUi.hpp"
#include "core/editor/EditorUiCommands.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/ComponentRegistry.hpp"
#include "render/resources/Material.hpp"

namespace core {

void drawTextureBrowser(EngineServices& services, EntityId entity, render::Material& material) {
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
                    [&services, entity](const render::Material& snapshot) {
                        if (MaterialComponent* materialComponent = services.world.materials.tryGet(entity);
                            materialComponent != nullptr && materialComponent->material) {
                            *materialComponent->material = snapshot;
                            notifyEditorMaterialChanged(services, entity);
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

}  // namespace core
