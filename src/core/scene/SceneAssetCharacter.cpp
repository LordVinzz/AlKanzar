#include "SceneAssetDetail.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace core::scene_asset_detail {
namespace {

using namespace std::string_view_literals;

template <typename Enum, std::size_t Size>
bool parseEnum(
    std::string_view token,
    const std::array<std::pair<std::string_view, Enum>, Size>& values,
    Enum& outValue
) {
    for (const auto& [name, value] : values) {
        if (token == name) {
            outValue = value;
            return true;
        }
    }
    return false;
}

constexpr std::array kAffiliations{
    std::pair{"Player"sv, CharacterAffiliation::Player},
    std::pair{"FriendlyNpc"sv, CharacterAffiliation::FriendlyNpc},
    std::pair{"HostileNpc"sv, CharacterAffiliation::HostileNpc},
};

constexpr std::array kControllers{
    std::pair{"Uncontrolled"sv, CharacterControllerKind::Uncontrolled},
    std::pair{"Player"sv, CharacterControllerKind::Player},
};

constexpr std::array kRaces{
    std::pair{"Human"sv, CharacterRace::Human},
    std::pair{"Elf"sv, CharacterRace::Elf},
    std::pair{"HalfElf"sv, CharacterRace::HalfElf},
    std::pair{"Orc"sv, CharacterRace::Orc},
    std::pair{"HalfOrc"sv, CharacterRace::HalfOrc},
    std::pair{"Demon"sv, CharacterRace::Demon},
    std::pair{"Dwarf"sv, CharacterRace::Dwarf},
};

constexpr std::array kKits{
    std::pair{"Fighter"sv, CharacterKit::Fighter},
    std::pair{"Tracker"sv, CharacterKit::Tracker},
    std::pair{"Paladin"sv, CharacterKit::Paladin},
    std::pair{"Barbarian"sv, CharacterKit::Barbarian},
    std::pair{"Cleric"sv, CharacterKit::Cleric},
    std::pair{"Druid"sv, CharacterKit::Druid},
    std::pair{"Mage"sv, CharacterKit::Mage},
    std::pair{"Specialist"sv, CharacterKit::Specialist},
    std::pair{"Sorcerer"sv, CharacterKit::Sorcerer},
    std::pair{"Rogue"sv, CharacterKit::Rogue},
    std::pair{"Bard"sv, CharacterKit::Bard},
};

constexpr std::array kSkills{
    std::pair{"Running"sv, CharacterSkill::Running},
    std::pair{"Acrobatics"sv, CharacterSkill::Acrobatics},
    std::pair{"Stealth"sv, CharacterSkill::Stealth},
    std::pair{"Perception"sv, CharacterSkill::Perception},
    std::pair{"Survival"sv, CharacterSkill::Survival},
    std::pair{"Tracking"sv, CharacterSkill::Tracking},
    std::pair{"FirstAid"sv, CharacterSkill::FirstAid},
    std::pair{"Climbing"sv, CharacterSkill::Climbing},
    std::pair{"Crafting"sv, CharacterSkill::Crafting},
    std::pair{"Knowledge"sv, CharacterSkill::Knowledge},
    std::pair{"Investigation"sv, CharacterSkill::Investigation},
    std::pair{"Persuasion"sv, CharacterSkill::Persuasion},
    std::pair{"Intimidation"sv, CharacterSkill::Intimidation},
    std::pair{"Deception"sv, CharacterSkill::Deception},
    std::pair{"Performance"sv, CharacterSkill::Performance},
    std::pair{"Command"sv, CharacterSkill::Command},
};

bool readInt(
    lua_State* state,
    int tableIndex,
    const char* field,
    int& outValue,
    bool required,
    std::string* error,
    std::string_view path
) {
    std::int64_t value = outValue;
    if (!readIntegerField(state, tableIndex, field, value, required, error, path)) {
        return false;
    }
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return fail(error, std::string(path) + "." + field, "is outside the supported integer range");
    }
    outValue = static_cast<int>(value);
    return true;
}

bool parseAbilities(
    lua_State* state,
    int tableIndex,
    AbilityScoresComponent& abilities,
    std::string* error,
    std::string_view path
) {
    return validateStringFields(
        state,
        tableIndex,
        {"strength", "agility", "physique", "intelligence", "faith", "charisma"},
        error,
        path
    ) &&
        readInt(state, tableIndex, "strength", abilities.strength, true, error, path) &&
        readInt(state, tableIndex, "agility", abilities.agility, true, error, path) &&
        readInt(state, tableIndex, "physique", abilities.physique, true, error, path) &&
        readInt(state, tableIndex, "intelligence", abilities.intelligence, true, error, path) &&
        readInt(state, tableIndex, "faith", abilities.faith, true, error, path) &&
        readInt(state, tableIndex, "charisma", abilities.charisma, true, error, path);
}

bool parseSkills(
    lua_State* state,
    int tableIndex,
    SkillRanksComponent& skills,
    std::string* error,
    std::string_view path
) {
    if (!validateArray(state, tableIndex, error, path)) {
        return false;
    }
    const std::size_t count = lua_rawlen(state, tableIndex);
    for (std::size_t index = 1u; index <= count; ++index) {
        lua_rawgeti(state, tableIndex, static_cast<lua_Integer>(index));
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            return fail(error, std::string(path) + "[" + std::to_string(index) + "]", "must be a skill name");
        }
        std::size_t tokenSize = 0u;
        const char* tokenData = lua_tolstring(state, -1, &tokenSize);
        CharacterSkill skill = CharacterSkill::Count;
        const bool known = parseEnum(
            std::string_view(tokenData, tokenSize),
            kSkills,
            skill
        );
        lua_pop(state, 1);
        if (!known) {
            return fail(error, std::string(path) + "[" + std::to_string(index) + "]", "contains an unknown skill");
        }
        SkillRank& rank = skills.ranks[static_cast<std::size_t>(skill)];
        if (rank != SkillRank::Untrained) {
            return fail(error, std::string(path) + "[" + std::to_string(index) + "]", "duplicates a skill");
        }
        rank = SkillRank::Initiate;
    }
    return true;
}

bool parseVitals(
    lua_State* state,
    int tableIndex,
    CharacterVitalsComponent& vitals,
    std::string* error,
    std::string_view path
) {
    return validateStringFields(
        state,
        tableIndex,
        {"current_hp", "maximum_hp", "mana"},
        error,
        path
    ) &&
        readInt(state, tableIndex, "current_hp", vitals.currentHitPoints, true, error, path) &&
        readInt(state, tableIndex, "maximum_hp", vitals.maximumHitPoints, true, error, path) &&
        readInt(state, tableIndex, "mana", vitals.currentMana, true, error, path);
}

}  // namespace

bool parseCharacterTable(
    lua_State* state,
    int tableIndex,
    CharacterBlueprint& outCharacter,
    std::string* error,
    std::string_view path
) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    if (!validateStringFields(
            state,
            absoluteIndex,
            {"affiliation", "controller", "party_slot", "race", "kit", "experience", "indicator_radius", "abilities", "skills", "vitals"},
            error,
            path)) {
        return false;
    }

    CharacterBlueprint character{};
    lua_getfield(state, absoluteIndex, "controller");
    const bool controllerIsExplicit = !lua_isnil(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, absoluteIndex, "party_slot");
    const bool partySlotIsExplicit = !lua_isnil(state, -1);
    lua_pop(state, 1);

    std::string affiliationToken{};
    std::string controllerToken{};
    std::string raceToken{};
    std::string kitToken{};
    std::int64_t partySlot = -1;
    if (!readStringField(state, absoluteIndex, "affiliation", affiliationToken, true, error, path) ||
        !readStringField(state, absoluteIndex, "controller", controllerToken, false, error, path) ||
        !readIntegerField(state, absoluteIndex, "party_slot", partySlot, false, error, path) ||
        !readStringField(state, absoluteIndex, "race", raceToken, true, error, path) ||
        !readStringField(state, absoluteIndex, "kit", kitToken, true, error, path) ||
        !readIntegerField(state, absoluteIndex, "experience", character.character.experience, false, error, path) ||
        !readFloatField(state, absoluteIndex, "indicator_radius", character.character.groundIndicatorRadius, false, error, path)) {
        return false;
    }
    if (!parseEnum(std::string_view(affiliationToken), kAffiliations, character.character.affiliation)) {
        return fail(error, std::string(path) + ".affiliation", "contains an unknown affiliation");
    }
    if (controllerIsExplicit) {
        if (!parseEnum(std::string_view(controllerToken), kControllers, character.controller.kind)) {
            return fail(error, std::string(path) + ".controller", "contains an unknown controller");
        }
    } else if (character.character.affiliation == CharacterAffiliation::Player) {
        // Preserve the control semantics of SCN V1 assets authored before the
        // controller and party fields became explicit.
        character.controller.kind = CharacterControllerKind::Player;
        partySlot = 0;
    }
    if (isPlayerControlled(character.controller)) {
        if (!partySlotIsExplicit && controllerIsExplicit) {
            return fail(error, std::string(path) + ".party_slot", "is required for a Player controller");
        }
        if (partySlot < 0 || partySlot >= static_cast<std::int64_t>(kMaximumPartySize)) {
            return fail(error, std::string(path) + ".party_slot", "is outside the supported party range");
        }
        character.partyMember = PartyMemberComponent{
            static_cast<std::uint8_t>(partySlot),
            true
        };
    } else if (partySlotIsExplicit) {
        return fail(error, std::string(path) + ".party_slot", "requires a Player controller");
    }
    if (!parseEnum(std::string_view(raceToken), kRaces, character.character.race)) {
        return fail(error, std::string(path) + ".race", "contains an unknown race");
    }
    if (!parseEnum(std::string_view(kitToken), kKits, character.character.kit)) {
        return fail(error, std::string(path) + ".kit", "contains an unknown kit");
    }
    if (character.character.experience < 0 || character.character.groundIndicatorRadius <= 0.0f) {
        return fail(error, path, "experience must be non-negative and indicator_radius must be positive");
    }

    FieldStatus status = pushTableField(state, absoluteIndex, "abilities", true, error, path);
    if (status != FieldStatus::Present) {
        return false;
    }
    const std::string abilitiesPath = std::string(path) + ".abilities";
    const bool abilitiesValid = parseAbilities(state, -1, character.abilities, error, abilitiesPath);
    lua_pop(state, 1);
    if (!abilitiesValid) {
        return false;
    }
    if (character.abilities.strength <= 0 || character.abilities.agility <= 0 ||
        character.abilities.physique <= 0 || character.abilities.intelligence <= 0 ||
        character.abilities.faith <= 0 || character.abilities.charisma <= 0) {
        return fail(error, abilitiesPath, "all ability scores must be positive");
    }

    status = pushTableField(state, absoluteIndex, "skills", true, error, path);
    if (status != FieldStatus::Present) {
        return false;
    }
    const std::string skillsPath = std::string(path) + ".skills";
    const bool skillsValid = parseSkills(state, -1, character.skills, error, skillsPath);
    lua_pop(state, 1);
    if (!skillsValid) {
        return false;
    }

    status = pushTableField(state, absoluteIndex, "vitals", true, error, path);
    if (status != FieldStatus::Present) {
        return false;
    }
    const std::string vitalsPath = std::string(path) + ".vitals";
    const bool vitalsValid = parseVitals(state, -1, character.vitals, error, vitalsPath);
    lua_pop(state, 1);
    if (!vitalsValid) {
        return false;
    }
    if (character.vitals.maximumHitPoints <= 0 || character.vitals.currentHitPoints < 0 ||
        character.vitals.currentHitPoints > character.vitals.maximumHitPoints ||
        character.vitals.currentMana < 0) {
        return fail(error, vitalsPath, "hit points or mana are outside their valid range");
    }

    outCharacter = std::move(character);
    return true;
}

}  // namespace core::scene_asset_detail
