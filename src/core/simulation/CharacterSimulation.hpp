#pragma once

#include "CharacterComponents.hpp"

namespace core {

void normalizeCharacterComponents(
    CharacterComponent& character,
    AbilityScoresComponent& abilities,
    SkillRanksComponent& skills,
    CharacterVitalsComponent& vitals
);

}  // namespace core
