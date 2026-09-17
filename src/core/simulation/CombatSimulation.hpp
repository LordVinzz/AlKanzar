#pragma once

#include <string_view>

#include "CombatComponents.hpp"

namespace core {

void setCombatState(CombatantComponent& combatant, CombatState state);

[[nodiscard]] const CombatAnimationProfile& combatAnimationProfile(
    const CombatantComponent& combatant
);

[[nodiscard]] std::string_view combatAnimationClip(
    const CombatantComponent& combatant
);

}  // namespace core
