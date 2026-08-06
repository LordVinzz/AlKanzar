#pragma once

#include "core/content/CharacterData.hpp"

namespace core {

enum class CharacterAffiliation {
    Player = 0,
    FriendlyNpc,
    HostileNpc,
};

struct CharacterComponent {
    CharacterAffiliation affiliation{CharacterAffiliation::FriendlyNpc};
    CharacterRace race{CharacterRace::Human};
    CharacterKit kit{CharacterKit::Fighter};
    std::int64_t experience{0};
    float groundIndicatorRadius{0.65f};

    friend bool operator==(const CharacterComponent&, const CharacterComponent&) = default;
};

using AbilityScoresComponent = AbilityScores;
using SkillRanksComponent = SkillRanks;
using CharacterVitalsComponent = CharacterVitals;

[[nodiscard]] inline CharacterRuleData characterRuleData(
    const CharacterComponent& character
) {
    return CharacterRuleData{character.race, character.kit, character.experience};
}

}  // namespace core
