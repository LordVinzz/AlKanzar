#include "SceneAssetDetail.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace core::scene_asset_detail {
namespace {

using namespace std::string_view_literals;

template <typename Enum, std::size_t Size>
bool parseEnum(
    std::string_view token,
    const std::array<std::pair<std::string_view, Enum>, Size>& values,
    Enum& outValue
) {
    for (const auto& [name, value] : values) {
        if (token == name) {
            outValue = value;
            return true;
        }
    }
    return false;
}

constexpr std::array kWeaponModes{
    std::pair{"Unarmed"sv, CombatWeaponMode::Unarmed},
    std::pair{"Melee"sv, CombatWeaponMode::Melee},
    std::pair{"Ranged"sv, CombatWeaponMode::Ranged},
};

constexpr std::array kStates{
    std::pair{"Idle"sv, CombatState::Idle},
    std::pair{"Combat"sv, CombatState::Combat},
    std::pair{"Attacking"sv, CombatState::Attacking},
    std::pair{"HitReaction"sv, CombatState::HitReaction},
    std::pair{"Fleeing"sv, CombatState::Fleeing},
    std::pair{"Scripted"sv, CombatState::Scripted},
    std::pair{"Downed"sv, CombatState::Downed},
    std::pair{"Dead"sv, CombatState::Dead},
};

bool parseAnimations(
    lua_State* state,
    int tableIndex,
    CombatAnimationSet& animations,
    std::string* error,
    std::string_view path
) {
    return validateStringFields(
        state,
        tableIndex,
        {
            "unarmed_ready", "unarmed_attack", "melee_ready",
            "melee_attack", "ranged_ready", "ranged_attack",
            "hit_reaction", "flee", "scripted", "downed", "dead"
        },
        error,
        path
    ) &&
        readStringField(state, tableIndex, "unarmed_ready", animations.unarmed.readyClip, false, error, path) &&
        readStringField(state, tableIndex, "unarmed_attack", animations.unarmed.attackClip, false, error, path) &&
        readStringField(state, tableIndex, "melee_ready", animations.melee.readyClip, false, error, path) &&
        readStringField(state, tableIndex, "melee_attack", animations.melee.attackClip, false, error, path) &&
        readStringField(state, tableIndex, "ranged_ready", animations.ranged.readyClip, false, error, path) &&
        readStringField(state, tableIndex, "ranged_attack", animations.ranged.attackClip, false, error, path) &&
        readStringField(state, tableIndex, "hit_reaction", animations.hitReactionClip, false, error, path) &&
        readStringField(state, tableIndex, "flee", animations.fleeClip, false, error, path) &&
        readStringField(state, tableIndex, "scripted", animations.scriptedClip, false, error, path) &&
        readStringField(state, tableIndex, "downed", animations.downedClip, false, error, path) &&
        readStringField(state, tableIndex, "dead", animations.deadClip, false, error, path);
}

}  // namespace

bool parseCombatantTable(
    lua_State* state,
    int tableIndex,
    CombatantComponent& outCombatant,
    std::string* error,
    std::string_view path
) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    if (!validateStringFields(
            state,
            absoluteIndex,
            {"weapon_mode", "state", "script_phase", "animations"},
            error,
            path)) {
        return false;
    }

    CombatantComponent combatant{};
    std::string weaponModeToken{};
    std::string stateToken{"Idle"};
    std::int64_t scriptPhase = 0;
    if (!readStringField(state, absoluteIndex, "weapon_mode", weaponModeToken, true, error, path) ||
        !readStringField(state, absoluteIndex, "state", stateToken, false, error, path) ||
        !readIntegerField(state, absoluteIndex, "script_phase", scriptPhase, false, error, path)) {
        return false;
    }
    if (!parseEnum(weaponModeToken, kWeaponModes, combatant.weaponMode)) {
        return fail(error, std::string(path) + ".weapon_mode", "contains an unknown weapon mode");
    }
    if (!parseEnum(stateToken, kStates, combatant.state)) {
        return fail(error, std::string(path) + ".state", "contains an unknown combat state");
    }
    if (scriptPhase < 0 ||
        scriptPhase > static_cast<std::int64_t>(std::numeric_limits<std::uint16_t>::max())) {
        return fail(error, std::string(path) + ".script_phase", "is outside the supported phase range");
    }
    combatant.scriptPhase = static_cast<std::uint16_t>(scriptPhase);

    const FieldStatus animationsStatus = pushTableField(
        state,
        absoluteIndex,
        "animations",
        false,
        error,
        path
    );
    if (animationsStatus == FieldStatus::Error) {
        return false;
    }
    if (animationsStatus == FieldStatus::Present) {
        const std::string animationsPath = std::string(path) + ".animations";
        const bool valid = parseAnimations(
            state,
            -1,
            combatant.animations,
            error,
            animationsPath
        );
        lua_pop(state, 1);
        if (!valid) {
            return false;
        }
    }

    combatant.resumeState = combatStateCanResume(combatant.state)
        ? combatant.state
        : CombatState::Idle;
    combatant.observedState = combatant.state;
    combatant.stateElapsedSeconds = 0.0f;
    outCombatant = std::move(combatant);
    return true;
}

}  // namespace core::scene_asset_detail
