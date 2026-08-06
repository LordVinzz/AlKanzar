#include <cassert>
#include <cstdlib>

#include "core/rules/CharacterRules.hpp"

int main() {
    core::CharacterRuleData character{};
    character.race = core::CharacterRace::Human;
    character.kit = core::CharacterKit::Fighter;
    character.experience = core::experienceForLevel(5);

    const core::AbilityScores abilities{14, 12, 14, 10, 10, 10};
    const core::SkillRanks skills{};
    const core::CharacterDerivedStats derived =
        core::deriveCharacterStats(character, abilities, skills);

    assert(derived.level == 5);
    assert(derived.robustness == 2);
    assert(derived.maximumMana == 0);
    assert(core::startingHitPoints(abilities, character.kit) == 53);
    return EXIT_SUCCESS;
}
