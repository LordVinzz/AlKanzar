#include "CombatSimulation.hpp"

namespace core {

void setCombatState(CombatantComponent& combatant, CombatState state) {
    if (combatant.state == state) {
        return;
    }
    if (combatStateIsTransient(state) &&
        combatStateCanResume(combatant.state)) {
        combatant.resumeState = combatant.state;
    } else if (combatStateCanResume(state)) {
        combatant.resumeState = state;
    }
    combatant.state = state;
    combatant.stateElapsedSeconds = 0.0f;
}

const CombatAnimationProfile& combatAnimationProfile(
    const CombatantComponent& combatant
) {
    switch (combatant.weaponMode) {
        case CombatWeaponMode::Melee:
            return combatant.animations.melee;
        case CombatWeaponMode::Ranged:
            return combatant.animations.ranged;
        case CombatWeaponMode::Unarmed:
        default:
            return combatant.animations.unarmed;
    }
}

std::string_view combatAnimationClip(const CombatantComponent& combatant) {
    const CombatAnimationProfile& profile = combatAnimationProfile(combatant);
    switch (combatant.state) {
        case CombatState::Combat:
            return profile.readyClip;
        case CombatState::Attacking:
            return profile.attackClip.empty()
                ? std::string_view(profile.readyClip)
                : std::string_view(profile.attackClip);
        case CombatState::HitReaction:
            return combatant.animations.hitReactionClip;
        case CombatState::Fleeing:
            return combatant.animations.fleeClip;
        case CombatState::Scripted:
            return combatant.animations.scriptedClip;
        case CombatState::Downed:
            return combatant.animations.downedClip;
        case CombatState::Dead:
            return combatant.animations.deadClip;
        case CombatState::Idle:
        default:
            return {};
    }
}

}  // namespace core
