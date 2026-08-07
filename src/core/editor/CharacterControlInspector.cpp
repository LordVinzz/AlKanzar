#include "CharacterControlInspector.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/ComponentInspector.hpp"
#include "core/ecs/World.hpp"
#include "core/simulation/CharacterComponents.hpp"

namespace core {
namespace {

struct CharacterControlSnapshot {
    CharacterControllerComponent controller{};
    std::optional<PartyMemberComponent> partyMember{};
};

std::optional<CharacterControlSnapshot> captureControlSnapshot(
    const World& world,
    EntityId entity
) {
    const CharacterControllerComponent* controller =
        world.characterControllers.tryGet(entity);
    if (controller == nullptr) {
        return std::nullopt;
    }
    const PartyMemberComponent* partyMember = world.partyMembers.tryGet(entity);
    return CharacterControlSnapshot{
        *controller,
        partyMember != nullptr
            ? std::optional<PartyMemberComponent>{*partyMember}
            : std::nullopt
    };
}

void applyControlSnapshot(
    EngineServices& services,
    EntityId entity,
    CharacterControlSnapshot snapshot
) {
    World& world = services.world;
    CharacterControllerComponent* controller =
        world.characterControllers.tryGet(entity);
    if (controller == nullptr) {
        return;
    }
    if (!isPlayerControlled(snapshot.controller)) {
        snapshot.partyMember.reset();
    } else if (!snapshot.partyMember.has_value()) {
        snapshot.partyMember = PartyMemberComponent{};
    }
    if (snapshot.partyMember.has_value()) {
        snapshot.partyMember->slot = static_cast<std::uint8_t>(
            std::min<std::size_t>(
                snapshot.partyMember->slot,
                kMaximumPartySize - 1u
            )
        );
    }

    *controller = snapshot.controller;
    if (snapshot.partyMember.has_value()) {
        world.partyMembers.emplace(entity, *snapshot.partyMember);
    } else {
        world.partyMembers.remove(entity);
    }
    services.navigationSystem.syncCharacterAgentControl(world, entity);
}

template <typename DrawFn>
bool editControlValue(
    EngineServices& services,
    EntityId entity,
    const char* itemId,
    const std::string& label,
    const std::string& mergeKey,
    DrawFn&& drawFn,
    bool mergeable = true
) {
    const std::optional<CharacterControlSnapshot> current =
        captureControlSnapshot(services.world, entity);
    if (!current.has_value()) {
        return false;
    }

    bool changed = false;
    editComponentSnapshot<CharacterControlSnapshot>(
        itemId,
        label,
        mergeKey,
        *current,
        [&services, entity](const CharacterControlSnapshot& snapshot) {
            applyControlSnapshot(services, entity, snapshot);
        },
        [&changed, draw = std::forward<DrawFn>(drawFn)](
            CharacterControlSnapshot& edited
        ) mutable {
            changed = draw(edited);
            return changed;
        },
        services.commands,
        mergeable
    );
    return changed;
}

}  // namespace

bool drawCharacterControlInspector(EngineServices& services, EntityId entity) {
    const std::optional<CharacterControlSnapshot> current =
        captureControlSnapshot(services.world, entity);
    if (!current.has_value()) {
        ImGui::TextUnformatted("Character control data is incomplete.");
        return false;
    }

    ImGui::SeparatorText("Control");
    bool changed = editControlValue(
        services,
        entity,
        "Controller",
        "Change Character Controller",
        "character-controller-" + std::to_string(entity.index),
        [](CharacterControlSnapshot& edited) {
            int value = static_cast<int>(edited.controller.kind);
            if (!ImGui::Combo("Controller", &value, "Uncontrolled\0Player\0")) {
                return false;
            }
            edited.controller.kind = static_cast<CharacterControllerKind>(value);
            return true;
        },
        false
    );

    const std::optional<CharacterControlSnapshot> afterController =
        captureControlSnapshot(services.world, entity);
    if (!afterController.has_value() || !isPlayerControlled(afterController->controller)) {
        return changed;
    }
    changed |= editControlValue(
        services,
        entity,
        "PartyActive",
        "Change Party Membership",
        "character-party-active-" + std::to_string(entity.index),
        [](CharacterControlSnapshot& edited) {
            if (!edited.partyMember.has_value()) {
                edited.partyMember = PartyMemberComponent{};
            }
            return ImGui::Checkbox("Active Party Member", &edited.partyMember->active);
        },
        false
    );
    changed |= editControlValue(
        services,
        entity,
        "PartySlot",
        "Change Party Slot",
        "character-party-slot-" + std::to_string(entity.index),
        [](CharacterControlSnapshot& edited) {
            int slot = edited.partyMember.has_value()
                ? static_cast<int>(edited.partyMember->slot)
                : 0;
            if (!ImGui::InputInt("Party Slot", &slot)) {
                return false;
            }
            if (!edited.partyMember.has_value()) {
                edited.partyMember = PartyMemberComponent{};
            }
            edited.partyMember->slot = static_cast<std::uint8_t>(
                std::clamp(slot, 0, static_cast<int>(kMaximumPartySize - 1u))
            );
            return true;
        }
    );
    return changed;
}

}  // namespace core
