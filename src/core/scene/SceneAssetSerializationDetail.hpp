#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include "SceneBlueprint.hpp"

namespace core::scene_asset_serialization_detail {

[[nodiscard]] std::string quoteLuaString(std::string_view value);
[[nodiscard]] const char* affiliationToken(CharacterAffiliation affiliation);
[[nodiscard]] const char* raceToken(CharacterRace race);
[[nodiscard]] const char* kitToken(CharacterKit kit);
[[nodiscard]] const char* skillToken(CharacterSkill skill);
[[nodiscard]] const char* skillRankToken(SkillRank rank);

void appendCharacterLua(
    std::ostream& output,
    std::string_view variable,
    const CharacterBlueprint& character
);

}  // namespace core::scene_asset_serialization_detail
