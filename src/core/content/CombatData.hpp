#pragma once

#include <string>

namespace core {

enum class CombatWeaponMode {
    Unarmed = 0,
    Melee,
    Ranged,
};

struct CombatAnimationProfile {
    std::string readyClip{};
    std::string attackClip{};

    friend bool operator==(
        const CombatAnimationProfile&,
        const CombatAnimationProfile&
    ) = default;
};

struct CombatAnimationSet {
    CombatAnimationProfile unarmed{};
    CombatAnimationProfile melee{};
    CombatAnimationProfile ranged{};
    std::string hitReactionClip{};
    std::string fleeClip{};
    std::string scriptedClip{};
    std::string downedClip{};
    std::string deadClip{};

    friend bool operator==(
        const CombatAnimationSet&,
        const CombatAnimationSet&
    ) = default;
};

}  // namespace core
