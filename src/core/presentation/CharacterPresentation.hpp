#pragma once

#include <glm/vec4.hpp>

#include "core/simulation/CharacterComponents.hpp"

namespace core {

[[nodiscard]] inline glm::vec4 characterGroundIndicatorColor(
    CharacterAffiliation affiliation,
    bool selected = true
) {
    switch (affiliation) {
        case CharacterAffiliation::Player:
            return selected
                ? glm::vec4(0.10f, 0.95f, 0.18f, 0.90f)
                : glm::vec4(0.04f, 0.32f, 0.08f, 0.78f);
        case CharacterAffiliation::FriendlyNpc:
            return glm::vec4(0.12f, 0.42f, 1.00f, 0.90f);
        case CharacterAffiliation::HostileNpc:
            return glm::vec4(0.95f, 0.12f, 0.10f, 0.90f);
    }
    return glm::vec4(1.0f);
}

}  // namespace core
