#include "CharacterRules.hpp"

#include <algorithm>
#include <array>

namespace core {
namespace {

int clampedLevel(int level) {
    return std::clamp(level, 1, 40);
}

int modifierForSkill(CharacterSkill skill, const AbilityScoresComponent& modifiers) {
    switch (skill) {
        case CharacterSkill::Running:
            return modifiers.strength;
        case CharacterSkill::Acrobatics:
        case CharacterSkill::Stealth:
            return modifiers.agility;
        case CharacterSkill::Climbing:
            return modifiers.physique;
        case CharacterSkill::Crafting:
        case CharacterSkill::Knowledge:
        case CharacterSkill::Investigation:
            return modifiers.intelligence;
        case CharacterSkill::Perception:
        case CharacterSkill::Survival:
        case CharacterSkill::Tracking:
        case CharacterSkill::FirstAid:
            return modifiers.faith;
        case CharacterSkill::Persuasion:
        case CharacterSkill::Intimidation:
        case CharacterSkill::Deception:
        case CharacterSkill::Performance:
        case CharacterSkill::Command:
            return modifiers.charisma;
        case CharacterSkill::Count:
            break;
    }
    return 0;
}

int poisonResistanceRaceBonus(CharacterRace race) {
    return race == CharacterRace::Dwarf ? 2 : 0;
}

}  // namespace

int abilityModifier(int value) {
    return std::max(1, value) / 2 - 5;
}

AbilityScoresComponent racialAbilityModifiers(CharacterRace race) {
    AbilityScoresComponent modifiers{0, 0, 0, 0, 0, 0};
    switch (race) {
        case CharacterRace::Human:
            break;
        case CharacterRace::Elf:
            modifiers.agility = 1;
            modifiers.physique = -1;
            break;
        case CharacterRace::HalfElf:
            modifiers.agility = 1;
            modifiers.strength = -1;
            break;
        case CharacterRace::Orc:
            modifiers.strength = 2;
            modifiers.intelligence = -2;
            break;
        case CharacterRace::HalfOrc:
            modifiers.strength = 2;
            modifiers.charisma = -2;
            break;
        case CharacterRace::Demon:
            modifiers.intelligence = 1;
            modifiers.charisma = 1;
            modifiers.faith = -2;
            break;
        case CharacterRace::Dwarf:
            modifiers.physique = 2;
            modifiers.agility = -2;
            break;
    }
    return modifiers;
}

AbilityScoresComponent effectiveAbilityScores(
    const AbilityScoresComponent& base,
    CharacterRace race
) {
    const AbilityScoresComponent racial = racialAbilityModifiers(race);
    return AbilityScoresComponent{
        std::max(1, base.strength + racial.strength),
        std::max(1, base.agility + racial.agility),
        std::max(1, base.physique + racial.physique),
        std::max(1, base.intelligence + racial.intelligence),
        std::max(1, base.faith + racial.faith),
        std::max(1, base.charisma + racial.charisma),
    };
}

int experienceForLevel(int level) {
    const std::int64_t safeLevel = clampedLevel(level);
    return static_cast<int>(500 * (safeLevel - 1) * safeLevel);
}

int levelForExperience(std::int64_t experience) {
    const std::int64_t safeExperience = std::max<std::int64_t>(0, experience);
    int level = 1;
    while (level < 40 && safeExperience >= experienceForLevel(level + 1)) {
        ++level;
    }
    return level;
}

int levelBonus(int level) {
    return 1 + (clampedLevel(level) - 1) / 8;
}

int kitRobustness(CharacterKit kit) {
    switch (kit) {
        case CharacterKit::Fighter:
        case CharacterKit::Paladin:
            return 2;
        case CharacterKit::Barbarian:
            return 3;
        case CharacterKit::Tracker:
        case CharacterKit::Cleric:
        case CharacterKit::Druid:
        case CharacterKit::Rogue:
        case CharacterKit::Bard:
            return 1;
        case CharacterKit::Mage:
        case CharacterKit::Specialist:
        case CharacterKit::Sorcerer:
            return 0;
    }
    return 0;
}

CasterProgression kitCasterProgression(CharacterKit kit) {
    switch (kit) {
        case CharacterKit::Paladin:
        case CharacterKit::Bard:
            return CasterProgression::Half;
        case CharacterKit::Cleric:
        case CharacterKit::Druid:
        case CharacterKit::Mage:
        case CharacterKit::Specialist:
        case CharacterKit::Sorcerer:
            return CasterProgression::Full;
        case CharacterKit::Fighter:
        case CharacterKit::Tracker:
        case CharacterKit::Barbarian:
        case CharacterKit::Rogue:
            return CasterProgression::None;
    }
    return CasterProgression::None;
}

int startingHitPoints(const AbilityScoresComponent& effective, CharacterKit kit) {
    return 15 + std::max(1, effective.physique) * 2 + kitRobustness(kit) * 5;
}

int hitPointsPerLevel(const AbilityScoresComponent& effective, CharacterKit kit) {
    return std::max(1, 3 + abilityModifier(effective.physique) + kitRobustness(kit));
}

int maximumMana(
    const AbilityScoresComponent& effective,
    CharacterKit kit,
    int level
) {
    const CasterProgression progression = kitCasterProgression(kit);
    if (progression == CasterProgression::None) {
        return 0;
    }

    const int fullMana = std::max(
        10,
        23 + 2 * (
            abilityModifier(effective.intelligence) +
            abilityModifier(effective.faith) +
            abilityModifier(effective.charisma)
        ) + 2 * levelBonus(level)
    );
    return progression == CasterProgression::Half ? (fullMana + 1) / 2 : fullMana;
}

int skillRankBonus(SkillRank rank) {
    return static_cast<int>(rank) * 2;
}

SkillRank maximumSkillRankForLevel(int level) {
    const int safeLevel = clampedLevel(level);
    if (safeLevel >= 30) {
        return SkillRank::Legendary;
    }
    if (safeLevel >= 20) {
        return SkillRank::Master;
    }
    if (safeLevel >= 10) {
        return SkillRank::Expert;
    }
    return SkillRank::Initiate;
}

CharacterDerivedStats deriveCharacterStats(
    const CharacterComponent& character,
    const AbilityScoresComponent& abilities,
    const SkillRanksComponent& skills
) {
    CharacterDerivedStats out{};
    out.effectiveAbilities = effectiveAbilityScores(abilities, character.race);
    out.abilityModifiers = AbilityScoresComponent{
        abilityModifier(out.effectiveAbilities.strength),
        abilityModifier(out.effectiveAbilities.agility),
        abilityModifier(out.effectiveAbilities.physique),
        abilityModifier(out.effectiveAbilities.intelligence),
        abilityModifier(out.effectiveAbilities.faith),
        abilityModifier(out.effectiveAbilities.charisma),
    };
    out.level = levelForExperience(character.experience);
    out.levelBonus = levelBonus(out.level);
    out.robustness = kitRobustness(character.kit);
    out.casterProgression = kitCasterProgression(character.kit);
    out.maximumMana = maximumMana(out.effectiveAbilities, character.kit, out.level);
    out.initiativeBonus = out.abilityModifiers.agility;
    out.dodge = 10 + out.abilityModifiers.agility;
    out.armor = std::max(0, out.abilityModifiers.physique);
    out.fortitude = 10 + out.abilityModifiers.physique;
    out.reflexes = 10 + out.abilityModifiers.agility;
    out.willpower = 10 + out.abilityModifiers.faith;
    out.magicResistance = 10 + std::max(
        out.abilityModifiers.intelligence,
        out.abilityModifiers.faith
    );
    out.poisonResistance = out.fortitude + poisonResistanceRaceBonus(character.race);
    out.magicAttackBonus = out.abilityModifiers.charisma + out.levelBonus;
    out.offensiveSpellDifficulty = 10 + out.magicAttackBonus;
    out.concentrationBonus = std::max(
        out.abilityModifiers.intelligence,
        out.abilityModifiers.faith
    ) + out.levelBonus;
    out.comfortableCarry = out.effectiveAbilities.strength * 5;
    out.maximumCarry = out.effectiveAbilities.strength * 10;
    if (character.race == CharacterRace::Orc) {
        out.comfortableCarry = out.comfortableCarry * 5 / 4;
        out.maximumCarry = out.maximumCarry * 5 / 4;
    }

    for (std::size_t index = 0; index < kCharacterSkillCount; ++index) {
        const CharacterSkill skill = static_cast<CharacterSkill>(index);
        out.skillBonuses[index] = modifierForSkill(skill, out.abilityModifiers) +
            skillRankBonus(skills.ranks[index]);
    }
    return out;
}

void normalizeCharacterData(
    CharacterComponent& character,
    AbilityScoresComponent& abilities,
    SkillRanksComponent& skills,
    CharacterVitalsComponent& vitals
) {
    character.experience = std::max<std::int64_t>(0, character.experience);
    character.groundIndicatorRadius = std::clamp(character.groundIndicatorRadius, 0.1f, 10.0f);

    abilities.strength = std::max(1, abilities.strength);
    abilities.agility = std::max(1, abilities.agility);
    abilities.physique = std::max(1, abilities.physique);
    abilities.intelligence = std::max(1, abilities.intelligence);
    abilities.faith = std::max(1, abilities.faith);
    abilities.charisma = std::max(1, abilities.charisma);

    const SkillRank maxRank = maximumSkillRankForLevel(levelForExperience(character.experience));
    for (SkillRank& rank : skills.ranks) {
        rank = static_cast<SkillRank>(std::clamp(
            static_cast<int>(rank),
            static_cast<int>(SkillRank::Untrained),
            static_cast<int>(maxRank)
        ));
    }

    vitals.maximumHitPoints = std::max(1, vitals.maximumHitPoints);
    vitals.currentHitPoints = std::clamp(vitals.currentHitPoints, 0, vitals.maximumHitPoints);
    const CharacterDerivedStats derived = deriveCharacterStats(character, abilities, skills);
    vitals.currentMana = std::clamp(vitals.currentMana, 0, derived.maximumMana);
}

const char* characterAffiliationName(CharacterAffiliation affiliation) {
    static constexpr std::array names{"Player", "Friendly NPC", "Hostile NPC"};
    return names[static_cast<std::size_t>(affiliation)];
}

const char* characterRaceName(CharacterRace race) {
    static constexpr std::array names{"Human", "Elf", "Half-Elf", "Orc", "Half-Orc", "Demon", "Dwarf"};
    return names[static_cast<std::size_t>(race)];
}

const char* characterKitName(CharacterKit kit) {
    static constexpr std::array names{
        "Fighter", "Tracker", "Paladin", "Barbarian", "Cleric", "Druid",
        "Mage", "Specialist", "Sorcerer", "Rogue", "Bard"
    };
    return names[static_cast<std::size_t>(kit)];
}

const char* characterSkillName(CharacterSkill skill) {
    static constexpr std::array names{
        "Running", "Acrobatics", "Stealth", "Perception", "Survival", "Tracking",
        "First Aid", "Climbing", "Crafting", "Knowledge", "Investigation", "Persuasion",
        "Intimidation", "Deception", "Performance", "Command"
    };
    return names[static_cast<std::size_t>(skill)];
}

const char* skillRankName(SkillRank rank) {
    static constexpr std::array names{"Untrained", "Initiate", "Expert", "Master", "Legendary"};
    return names[static_cast<std::size_t>(rank)];
}

const char* casterProgressionName(CasterProgression progression) {
    static constexpr std::array names{"None", "Half caster", "Full caster"};
    return names[static_cast<std::size_t>(progression)];
}

}  // namespace core
