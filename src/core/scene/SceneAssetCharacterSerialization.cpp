#include "SceneAssetSerializationDetail.hpp"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace core::scene_asset_serialization_detail {

std::string quoteLuaString(std::string_view value) {
    std::ostringstream output{};
    output << '"';
    for (unsigned char byte : value) {
        switch (byte) {
            case '\\': output << "\\\\"; break;
            case '"': output << "\\\""; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (byte < 0x20u || byte == 0x7fu) {
                    output << "\\x" << std::hex << std::uppercase
                           << std::setw(2) << std::setfill('0')
                           << static_cast<int>(byte)
                           << std::dec << std::nouppercase << std::setfill(' ');
                } else {
                    output << static_cast<char>(byte);
                }
                break;
        }
    }
    output << '"';
    return output.str();
}

const char* affiliationToken(CharacterAffiliation affiliation) {
    switch (affiliation) {
        case CharacterAffiliation::Player: return "Player";
        case CharacterAffiliation::FriendlyNpc: return "FriendlyNpc";
        case CharacterAffiliation::HostileNpc: return "HostileNpc";
    }
    return "FriendlyNpc";
}

const char* raceToken(CharacterRace race) {
    switch (race) {
        case CharacterRace::Human: return "Human";
        case CharacterRace::Elf: return "Elf";
        case CharacterRace::HalfElf: return "HalfElf";
        case CharacterRace::Orc: return "Orc";
        case CharacterRace::HalfOrc: return "HalfOrc";
        case CharacterRace::Demon: return "Demon";
        case CharacterRace::Dwarf: return "Dwarf";
    }
    return "Human";
}

const char* kitToken(CharacterKit kit) {
    switch (kit) {
        case CharacterKit::Fighter: return "Fighter";
        case CharacterKit::Tracker: return "Tracker";
        case CharacterKit::Paladin: return "Paladin";
        case CharacterKit::Barbarian: return "Barbarian";
        case CharacterKit::Cleric: return "Cleric";
        case CharacterKit::Druid: return "Druid";
        case CharacterKit::Mage: return "Mage";
        case CharacterKit::Specialist: return "Specialist";
        case CharacterKit::Sorcerer: return "Sorcerer";
        case CharacterKit::Rogue: return "Rogue";
        case CharacterKit::Bard: return "Bard";
    }
    return "Fighter";
}

const char* skillToken(CharacterSkill skill) {
    switch (skill) {
        case CharacterSkill::Running: return "Running";
        case CharacterSkill::Acrobatics: return "Acrobatics";
        case CharacterSkill::Stealth: return "Stealth";
        case CharacterSkill::Perception: return "Perception";
        case CharacterSkill::Survival: return "Survival";
        case CharacterSkill::Tracking: return "Tracking";
        case CharacterSkill::FirstAid: return "FirstAid";
        case CharacterSkill::Climbing: return "Climbing";
        case CharacterSkill::Crafting: return "Crafting";
        case CharacterSkill::Knowledge: return "Knowledge";
        case CharacterSkill::Investigation: return "Investigation";
        case CharacterSkill::Persuasion: return "Persuasion";
        case CharacterSkill::Intimidation: return "Intimidation";
        case CharacterSkill::Deception: return "Deception";
        case CharacterSkill::Performance: return "Performance";
        case CharacterSkill::Command: return "Command";
        case CharacterSkill::Count: break;
    }
    return "Running";
}

const char* skillRankToken(SkillRank rank) {
    switch (rank) {
        case SkillRank::Untrained: return "Untrained";
        case SkillRank::Initiate: return "Initiate";
        case SkillRank::Expert: return "Expert";
        case SkillRank::Master: return "Master";
        case SkillRank::Legendary: return "Legendary";
    }
    return "Untrained";
}

void appendCharacterLua(
    std::ostream& output,
    std::string_view variable,
    const CharacterBlueprint& character
) {
    output << variable << ".character({\n"
           << "    affiliation = " << quoteLuaString(affiliationToken(character.character.affiliation)) << ",\n"
           << "    controller = "
           << quoteLuaString(isPlayerControlled(character.controller) ? "Player" : "Uncontrolled") << ",\n";
    if (character.partyMember.has_value()) {
        output << "    party_slot = " << static_cast<int>(character.partyMember->slot) << ",\n"
               << "    party_active = " << (character.partyMember->active ? "true" : "false") << ",\n";
    }
    output << "    race = " << quoteLuaString(raceToken(character.character.race)) << ",\n"
           << "    kit = " << quoteLuaString(kitToken(character.character.kit)) << ",\n"
           << "    experience = " << character.character.experience << ",\n"
           << "    indicator_radius = " << character.character.groundIndicatorRadius << ",\n"
           << "    abilities = {\n"
           << "        strength = " << character.abilities.strength << ",\n"
           << "        agility = " << character.abilities.agility << ",\n"
           << "        physique = " << character.abilities.physique << ",\n"
           << "        intelligence = " << character.abilities.intelligence << ",\n"
           << "        faith = " << character.abilities.faith << ",\n"
           << "        charisma = " << character.abilities.charisma << ",\n"
           << "    },\n"
           << "    skills = {\n";
    for (std::size_t index = 0u; index < kCharacterSkillCount; ++index) {
        const SkillRank rank = character.skills.ranks[index];
        if (rank == SkillRank::Untrained) {
            continue;
        }
        output << "        " << skillToken(static_cast<CharacterSkill>(index))
               << " = " << quoteLuaString(skillRankToken(rank)) << ",\n";
    }
    output << "    },\n"
           << "    vitals = {\n"
           << "        current_hp = " << character.vitals.currentHitPoints << ",\n"
           << "        maximum_hp = " << character.vitals.maximumHitPoints << ",\n"
           << "        mana = " << character.vitals.currentMana << ",\n"
           << "    },\n"
           << "})\n";
}

}  // namespace core::scene_asset_serialization_detail
