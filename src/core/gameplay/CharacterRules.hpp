#pragma once

#include <array>
#include <cstdint>

#include "CharacterComponents.hpp"

namespace core {

struct CharacterDerivedStats {
    AbilityScoresComponent effectiveAbilities{};
    AbilityScoresComponent abilityModifiers{};
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
[[nodiscard]] AbilityScoresComponent racialAbilityModifiers(CharacterRace race);
[[nodiscard]] AbilityScoresComponent effectiveAbilityScores(
    const AbilityScoresComponent& base,
    CharacterRace race
);
[[nodiscard]] int experienceForLevel(int level);
[[nodiscard]] int levelForExperience(std::int64_t experience);
[[nodiscard]] int levelBonus(int level);
[[nodiscard]] int kitRobustness(CharacterKit kit);
[[nodiscard]] CasterProgression kitCasterProgression(CharacterKit kit);
[[nodiscard]] int startingHitPoints(const AbilityScoresComponent& effective, CharacterKit kit);
[[nodiscard]] int hitPointsPerLevel(const AbilityScoresComponent& effective, CharacterKit kit);
[[nodiscard]] int maximumMana(
    const AbilityScoresComponent& effective,
    CharacterKit kit,
    int level
);
[[nodiscard]] int skillRankBonus(SkillRank rank);
[[nodiscard]] SkillRank maximumSkillRankForLevel(int level);
[[nodiscard]] CharacterDerivedStats deriveCharacterStats(
    const CharacterComponent& character,
    const AbilityScoresComponent& abilities,
    const SkillRanksComponent& skills
);
void normalizeCharacterData(
    CharacterComponent& character,
    AbilityScoresComponent& abilities,
    SkillRanksComponent& skills,
    CharacterVitalsComponent& vitals
);

[[nodiscard]] const char* characterAffiliationName(CharacterAffiliation affiliation);
[[nodiscard]] const char* characterRaceName(CharacterRace race);
[[nodiscard]] const char* characterKitName(CharacterKit kit);
[[nodiscard]] const char* characterSkillName(CharacterSkill skill);
[[nodiscard]] const char* skillRankName(SkillRank rank);
[[nodiscard]] const char* casterProgressionName(CasterProgression progression);

}  // namespace core
