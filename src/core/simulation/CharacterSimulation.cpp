#include "CharacterSimulation.hpp"

#include <algorithm>

#include "core/rules/CharacterRules.hpp"

namespace core {

void normalizeCharacterComponents(
    CharacterComponent& character,
    AbilityScoresComponent& abilities,
    SkillRanksComponent& skills,
    CharacterVitalsComponent& vitals
) {
    CharacterRuleData ruleData = characterRuleData(character);
    normalizeCharacterRuleData(ruleData, abilities, skills, vitals);
    character.race = ruleData.race;
    character.kit = ruleData.kit;
    character.experience = ruleData.experience;
    character.groundIndicatorRadius = std::clamp(
        character.groundIndicatorRadius,
        0.1f,
        10.0f
    );
}

}  // namespace core
