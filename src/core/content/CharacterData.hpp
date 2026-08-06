#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace core {

enum class CharacterRace {
    Human = 0,
    Elf,
    HalfElf,
    Orc,
    HalfOrc,
    Demon,
    Dwarf,
};

enum class CharacterKit {
    Fighter = 0,
    Tracker,
    Paladin,
    Barbarian,
    Cleric,
    Druid,
    Mage,
    Specialist,
    Sorcerer,
    Rogue,
    Bard,
};

enum class CharacterSkill {
    Running = 0,
    Acrobatics,
    Stealth,
    Perception,
    Survival,
    Tracking,
    FirstAid,
    Climbing,
    Crafting,
    Knowledge,
    Investigation,
    Persuasion,
    Intimidation,
    Deception,
    Performance,
    Command,
    Count,
};

enum class SkillRank {
    Untrained = 0,
    Initiate,
    Expert,
    Master,
    Legendary,
};

enum class CasterProgression {
    None = 0,
    Half,
    Full,
};

inline constexpr std::size_t kCharacterSkillCount =
    static_cast<std::size_t>(CharacterSkill::Count);

struct CharacterRuleData {
    CharacterRace race{CharacterRace::Human};
    CharacterKit kit{CharacterKit::Fighter};
    std::int64_t experience{0};

    friend bool operator==(const CharacterRuleData&, const CharacterRuleData&) = default;
};

struct AbilityScores {
    int strength{10};
    int agility{10};
    int physique{10};
    int intelligence{10};
    int faith{10};
    int charisma{10};

    friend bool operator==(const AbilityScores&, const AbilityScores&) = default;
};

struct SkillRanks {
    std::array<SkillRank, kCharacterSkillCount> ranks{};

    friend bool operator==(const SkillRanks&, const SkillRanks&) = default;
};

struct CharacterVitals {
    int currentHitPoints{1};
    int maximumHitPoints{1};
    int currentMana{0};

    friend bool operator==(const CharacterVitals&, const CharacterVitals&) = default;
};

}  // namespace core
