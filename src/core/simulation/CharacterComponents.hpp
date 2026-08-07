#pragma once

#include <cstddef>
#include <cstdint>

#include "core/content/CharacterData.hpp"

namespace core {

enum class CharacterAffiliation {
    Player = 0,
    FriendlyNpc,
    HostileNpc,
};

inline constexpr std::size_t kMaximumPartySize = 6u;

enum class CharacterControllerKind {
    Uncontrolled = 0,
    Player,
};

struct CharacterControllerComponent {
    CharacterControllerKind kind{CharacterControllerKind::Uncontrolled};

    friend bool operator==(
        const CharacterControllerComponent&,
        const CharacterControllerComponent&
    ) = default;
};

struct PartyMemberComponent {
    std::uint8_t slot{0u};
    bool active{true};

    friend bool operator==(const PartyMemberComponent&, const PartyMemberComponent&) = default;
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

[[nodiscard]] inline bool isPlayerControlled(
    const CharacterControllerComponent& controller
) {
    return controller.kind == CharacterControllerKind::Player;
}

[[nodiscard]] inline bool isActivePlayerPartyMember(
    const CharacterControllerComponent* controller,
    const PartyMemberComponent* partyMember
) {
    return controller != nullptr && partyMember != nullptr &&
        isPlayerControlled(*controller) && partyMember->active &&
        partyMember->slot < kMaximumPartySize;
}

[[nodiscard]] inline CharacterRuleData characterRuleData(
    const CharacterComponent& character
) {
    return CharacterRuleData{character.race, character.kit, character.experience};
}

}  // namespace core
