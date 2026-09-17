#include "SceneAssetSerializationDetail.hpp"

#include <ostream>

namespace core::scene_asset_serialization_detail {
namespace {

const char* weaponModeToken(CombatWeaponMode mode) {
    switch (mode) {
        case CombatWeaponMode::Melee: return "Melee";
        case CombatWeaponMode::Ranged: return "Ranged";
        case CombatWeaponMode::Unarmed:
        default: return "Unarmed";
    }
}

const char* combatStateToken(CombatState state) {
    switch (state) {
        case CombatState::Combat: return "Combat";
        case CombatState::Attacking: return "Attacking";
        case CombatState::HitReaction: return "HitReaction";
        case CombatState::Fleeing: return "Fleeing";
        case CombatState::Scripted: return "Scripted";
        case CombatState::Downed: return "Downed";
        case CombatState::Dead: return "Dead";
        case CombatState::Idle:
        default: return "Idle";
    }
}

void appendAnimation(
    std::ostream& output,
    const char* field,
    const std::string& clip
) {
    output << "        " << field << " = " << quoteLuaString(clip) << ",\n";
}

}  // namespace

void appendCombatantLua(
    std::ostream& output,
    std::string_view variable,
    const CombatantComponent& combatant
) {
    output << variable << ".combatant({\n"
           << "    weapon_mode = " << quoteLuaString(weaponModeToken(combatant.weaponMode)) << ",\n"
           << "    state = " << quoteLuaString(combatStateToken(combatant.state)) << ",\n"
           << "    script_phase = " << combatant.scriptPhase << ",\n"
           << "    animations = {\n";
    appendAnimation(output, "unarmed_ready", combatant.animations.unarmed.readyClip);
    appendAnimation(output, "unarmed_attack", combatant.animations.unarmed.attackClip);
    appendAnimation(output, "melee_ready", combatant.animations.melee.readyClip);
    appendAnimation(output, "melee_attack", combatant.animations.melee.attackClip);
    appendAnimation(output, "ranged_ready", combatant.animations.ranged.readyClip);
    appendAnimation(output, "ranged_attack", combatant.animations.ranged.attackClip);
    appendAnimation(output, "hit_reaction", combatant.animations.hitReactionClip);
    appendAnimation(output, "flee", combatant.animations.fleeClip);
    appendAnimation(output, "scripted", combatant.animations.scriptedClip);
    appendAnimation(output, "downed", combatant.animations.downedClip);
    appendAnimation(output, "dead", combatant.animations.deadClip);
    output << "    },\n})\n";
}

}  // namespace core::scene_asset_serialization_detail
