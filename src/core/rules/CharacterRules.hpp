#pragma once

#include <array>
#include <cstdint>

#include "core/content/CharacterData.hpp"

namespace core {

struct CharacterDerivedStats {
    AbilityScores effectiveAbilities{};
    AbilityScores abilityModifiers{};
    int level{1};
    int levelBonus{1};
    int robustness{0};
    CasterProgression casterProgression{CasterProgression::None};
    int maximumMana{0};
    int initiativeBonus{0};
    int dodge{10};
    int armor{0};
    int fortitude{10};
    int reflexes{10};
    int willpower{10};
    int magicResistance{10};
    int poisonResistance{10};
    int magicAttackBonus{0};
    int offensiveSpellDifficulty{10};
    int concentrationBonus{0};
    int comfortableCarry{0};
    int maximumCarry{0};
    int movementMeters{6};
    std::array<int, kCharacterSkillCount> skillBonuses{};
};

[[nodiscard]] int abilityModifier(int value);
[[nodiscard]] AbilityScores racialAbilityModifiers(CharacterRace race);
[[nodiscard]] AbilityScores effectiveAbilityScores(
    const AbilityScores& base,
    CharacterRace race
);
[[nodiscard]] int experienceForLevel(int level);
[[nodiscard]] int levelForExperience(std::int64_t experience);
[[nodiscard]] int levelBonus(int level);
[[nodiscard]] int kitRobustness(CharacterKit kit);
[[nodiscard]] CasterProgression kitCasterProgression(CharacterKit kit);
[[nodiscard]] int startingHitPoints(const AbilityScores& effective, CharacterKit kit);
[[nodiscard]] int hitPointsPerLevel(const AbilityScores& effective, CharacterKit kit);
[[nodiscard]] int maximumMana(
    const AbilityScores& effective,
    CharacterKit kit,
    int level
);
[[nodiscard]] int skillRankBonus(SkillRank rank);
[[nodiscard]] SkillRank maximumSkillRankForLevel(int level);
[[nodiscard]] CharacterDerivedStats deriveCharacterStats(
    const CharacterRuleData& character,
    const AbilityScores& abilities,
    const SkillRanks& skills
);
void normalizeCharacterRuleData(
    CharacterRuleData& character,
    AbilityScores& abilities,
    SkillRanks& skills,
    CharacterVitals& vitals
);

}  // namespace core
