#include "ComponentRegistry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>

#include "ComponentInspector.hpp"
#include "core/app/EngineServices.hpp"
#include "core/ecs/World.hpp"
#include "core/simulation/CombatSimulation.hpp"

namespace core {
namespace {

std::string combatantMergeKey(EntityId entity, const char* field) {
    return "combatant-" + std::to_string(entity.index) + "-" +
        std::to_string(entity.generation) + "-" + field;
}

const char* combatStateLabel(CombatState state) {
    switch (state) {
        case CombatState::Combat: return "Combat";
        case CombatState::Attacking: return "Attacking";
        case CombatState::HitReaction: return "Hit Reaction";
        case CombatState::Fleeing: return "Fleeing";
        case CombatState::Scripted: return "Scripted";
        case CombatState::Downed: return "Downed";
        case CombatState::Dead: return "Dead";
        case CombatState::Idle:
        default: return "Idle";
    }
}

void applyCombatantSnapshot(
    EngineServices& services,
    EntityId entity,
    const CombatantComponent& snapshot
) {
    CombatantComponent* target = services.world.combatants.tryGet(entity);
    if (target == nullptr) {
        return;
    }

    CombatantComponent applied = snapshot;
    if (target->state == applied.state) {
        applied.resumeState = target->resumeState;
        applied.observedState = target->observedState;
        applied.stateElapsedSeconds = target->stateElapsedSeconds;
        applied.animationLoopOverrideActive =
            target->animationLoopOverrideActive;
        applied.animationLoopBeforeOverride =
            target->animationLoopBeforeOverride;
    } else {
        applied.observedState = target->state;
        applied.stateElapsedSeconds = 0.0f;
    }
    *target = std::move(applied);

    if (const std::optional<SceneObjectId> object =
            services.sceneDocument.objectForEntity(entity)) {
        (void)services.sceneDocument.captureRuntimeObject(
            *object,
            services.world
        );
    }
}

template <typename DrawFn>
bool editCombatantValue(
    EngineServices& services,
    EntityId entity,
    const char* itemId,
    const char* label,
    const char* field,
    DrawFn&& drawFn,
    bool mergeable = true
) {
    const CombatantComponent* combatant = services.world.combatants.tryGet(entity);
    if (combatant == nullptr) {
        return false;
    }
    bool changed = false;
    editComponentSnapshot<CombatantComponent>(
        itemId,
        label,
        combatantMergeKey(entity, field),
        *combatant,
        [&services, entity](const CombatantComponent& snapshot) {
            applyCombatantSnapshot(services, entity, snapshot);
        },
        [&changed, draw = std::forward<DrawFn>(drawFn)](
            CombatantComponent& edited
        ) mutable {
            changed = draw(edited);
            return changed;
        },
        services.commands,
        mergeable
    );
    return changed;
}

bool drawClipName(const char* label, std::string& value) {
    std::array<char, 256> buffer{};
    const std::size_t count = std::min(value.size(), buffer.size() - 1u);
    std::copy_n(value.data(), count, buffer.data());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) {
        return false;
    }
    value.assign(buffer.data());
    return true;
}

template <typename Accessor>
bool editClip(
    EngineServices& services,
    EntityId entity,
    const char* itemId,
    const char* label,
    const char* field,
    Accessor&& accessor
) {
    return editCombatantValue(
        services,
        entity,
        itemId,
        "Edit Combat Animation",
        field,
        [label, access = std::forward<Accessor>(accessor)](
            CombatantComponent& edited
        ) mutable {
            return drawClipName(label, access(edited));
        },
        false
    );
}

}  // namespace

void ComponentRegistry::registerCombatantDescriptor() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Combatant,
        "Combatant",
        "Gameplay",
        [](const World& world, EntityId entity) {
            return world.combatants.contains(entity);
        },
        [](World& world, EntityId entity) {
            if (world.characters.contains(entity)) {
                world.combatants.emplace(entity, CombatantComponent{});
            }
        },
        [](World& world, EntityId entity) {
            const CombatantComponent* combatant =
                world.combatants.tryGet(entity);
            AnimatedModelComponent* animation =
                world.animatedModels.tryGet(entity);
            if (combatant != nullptr && animation != nullptr &&
                combatant->animationLoopOverrideActive) {
                animation->loop = combatant->animationLoopBeforeOverride;
            }
            world.combatants.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            const CombatantComponent* combatant =
                services.world.combatants.tryGet(entity);
            if (combatant == nullptr) {
                return false;
            }

            bool changed = editCombatantValue(
                services,
                entity,
                "WeaponMode",
                "Change Combat Weapon Mode",
                "weapon-mode",
                [](CombatantComponent& edited) {
                    int value = static_cast<int>(edited.weaponMode);
                    if (!ImGui::Combo(
                            "Weapon Mode",
                            &value,
                            "Unarmed\0Melee\0Ranged\0")) {
                        return false;
                    }
                    edited.weaponMode = static_cast<CombatWeaponMode>(value);
                    return true;
                },
                false
            );
            changed |= editCombatantValue(
                services,
                entity,
                "CombatState",
                "Change Combat State",
                "state",
                [](CombatantComponent& edited) {
                    int value = static_cast<int>(edited.state);
                    if (!ImGui::Combo(
                            "State",
                            &value,
                            "Idle\0Combat\0Attacking\0Hit Reaction\0Fleeing\0Scripted\0Downed\0Dead\0")) {
                        return false;
                    }
                    setCombatState(
                        edited,
                        static_cast<CombatState>(value)
                    );
                    return true;
                },
                false
            );
            changed |= editCombatantValue(
                services,
                entity,
                "ScriptPhase",
                "Change Combat Script Phase",
                "script-phase",
                [](CombatantComponent& edited) {
                    int phase = static_cast<int>(edited.scriptPhase);
                    if (!ImGui::InputInt("Script / Boss Phase", &phase)) {
                        return false;
                    }
                    edited.scriptPhase = static_cast<std::uint16_t>(
                        std::clamp(phase, 0, 65535)
                    );
                    return true;
                }
            );

            ImGui::Text(
                "State Time: %.2f s",
                combatant->stateElapsedSeconds
            );
            if (combatStateIsTransient(combatant->state)) {
                ImGui::Text(
                    "Graph Exit: Downed at 0 HP, otherwise %s",
                    combatStateLabel(combatant->resumeState)
                );
            }
            ImGui::TextDisabled(
                "State and phase are explicit script inputs; no autonomous AI runs yet."
            );
            ImGui::SeparatorText("Weapon Animations");
            changed |= editClip(
                services, entity, "UnarmedReady", "Unarmed Ready", "unarmed-ready",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.unarmed.readyClip;
                }
            );
            changed |= editClip(
                services, entity, "UnarmedAttack", "Unarmed Attack", "unarmed-attack",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.unarmed.attackClip;
                }
            );
            changed |= editClip(
                services, entity, "MeleeReady", "Melee Ready", "melee-ready",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.melee.readyClip;
                }
            );
            changed |= editClip(
                services, entity, "MeleeAttack", "Melee Attack", "melee-attack",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.melee.attackClip;
                }
            );
            changed |= editClip(
                services, entity, "RangedReady", "Ranged Ready", "ranged-ready",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.ranged.readyClip;
                }
            );
            changed |= editClip(
                services, entity, "RangedAttack", "Ranged Attack", "ranged-attack",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.ranged.attackClip;
                }
            );

            ImGui::SeparatorText("State Animations");
            changed |= editClip(
                services, entity, "HitReaction", "Hit Reaction", "hit-reaction",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.hitReactionClip;
                }
            );
            changed |= editClip(
                services, entity, "Flee", "Flee", "flee",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.fleeClip;
                }
            );
            changed |= editClip(
                services, entity, "Scripted", "Scripted", "scripted",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.scriptedClip;
                }
            );
            changed |= editClip(
                services, entity, "Downed", "Downed", "downed",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.downedClip;
                }
            );
            changed |= editClip(
                services, entity, "Dead", "Dead", "dead",
                [](CombatantComponent& value) -> std::string& {
                    return value.animations.deadClip;
                }
            );
            return changed;
        },
        [](const World& world, EntityId entity) {
            return world.characters.contains(entity);
        },
    });
}

}  // namespace core
