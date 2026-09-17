#pragma once

#include <cstdint>

#include "core/content/CombatData.hpp"

namespace core {

enum class CombatState {
    Idle = 0,
    Combat,
    Attacking,
    HitReaction,
    Fleeing,
    Scripted,
    Downed,
    Dead,
};

struct CombatantComponent {
    CombatWeaponMode weaponMode{CombatWeaponMode::Unarmed};
    CombatState state{CombatState::Idle};
    std::uint16_t scriptPhase{0u};
    CombatAnimationSet animations{};

    // Runtime-only bookkeeping. SCN persists the authored state and phase,
    // never transition history or elapsed simulation time.
    CombatState resumeState{CombatState::Idle};
    CombatState observedState{CombatState::Idle};
    float stateElapsedSeconds{0.0f};
    bool animationLoopOverrideActive{false};
    bool animationLoopBeforeOverride{true};

    friend bool operator==(
        const CombatantComponent&,
        const CombatantComponent&
    ) = default;
};

[[nodiscard]] inline bool combatStateIsTransient(CombatState state) {
    return state == CombatState::Attacking ||
        state == CombatState::HitReaction;
}

[[nodiscard]] inline bool combatStateCanResume(CombatState state) {
    return state == CombatState::Idle ||
        state == CombatState::Combat ||
        state == CombatState::Fleeing ||
        state == CombatState::Scripted;
}

[[nodiscard]] inline bool combatStateAllowsActions(CombatState state) {
    return state != CombatState::Downed && state != CombatState::Dead;
}

[[nodiscard]] inline bool combatStateLocksMovement(CombatState state) {
    return state == CombatState::Attacking ||
        state == CombatState::HitReaction ||
        state == CombatState::Downed ||
        state == CombatState::Dead;
}

}  // namespace core
