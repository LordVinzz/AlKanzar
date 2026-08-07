#include "CharacterInspector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <imgui.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/CharacterControlInspector.hpp"
#include "core/editor/ComponentInspector.hpp"
#include "core/ecs/World.hpp"
#include "core/rules/CharacterRules.hpp"
#include "core/simulation/CharacterComponents.hpp"
#include "core/simulation/CharacterSimulation.hpp"

namespace core {
namespace {

const char* characterSkillLabel(CharacterSkill skill) {
    static constexpr std::array names{
        "Running", "Acrobatics", "Stealth", "Perception", "Survival", "Tracking",
        "First Aid", "Climbing", "Crafting", "Knowledge", "Investigation", "Persuasion",
        "Intimidation", "Deception", "Performance", "Command"
    };
    return names[static_cast<std::size_t>(skill)];
}

const char* casterProgressionLabel(CasterProgression progression) {
    static constexpr std::array names{"None", "Half caster", "Full caster"};
    return names[static_cast<std::size_t>(progression)];
}

struct CharacterEditorSnapshot {
    CharacterComponent character{};
    AbilityScoresComponent abilities{};
    SkillRanksComponent skills{};
    CharacterVitalsComponent vitals{};
};

std::optional<CharacterEditorSnapshot> captureCharacterSnapshot(
    const World& world,
    EntityId entity
) {
    const CharacterComponent* character = world.characters.tryGet(entity);
    const AbilityScoresComponent* abilities = world.abilityScores.tryGet(entity);
    const SkillRanksComponent* skills = world.skillRanks.tryGet(entity);
    const CharacterVitalsComponent* vitals = world.characterVitals.tryGet(entity);
    if (character == nullptr || abilities == nullptr || skills == nullptr || vitals == nullptr) {
        return std::nullopt;
    }
    return CharacterEditorSnapshot{*character, *abilities, *skills, *vitals};
}

void applyCharacterSnapshot(
    World& world,
    EntityId entity,
    const CharacterEditorSnapshot& requested
) {
    CharacterComponent* character = world.characters.tryGet(entity);
    AbilityScoresComponent* abilities = world.abilityScores.tryGet(entity);
    SkillRanksComponent* skills = world.skillRanks.tryGet(entity);
    CharacterVitalsComponent* vitals = world.characterVitals.tryGet(entity);
    if (character == nullptr || abilities == nullptr || skills == nullptr || vitals == nullptr) {
        return;
    }

    CharacterEditorSnapshot normalized = requested;
    normalizeCharacterComponents(
        normalized.character,
        normalized.abilities,
        normalized.skills,
        normalized.vitals
    );
    *character = normalized.character;
    *abilities = normalized.abilities;
    *skills = normalized.skills;
    *vitals = normalized.vitals;
}

template <typename DrawFn>
bool editCharacterValue(
    EngineServices& services,
    EntityId entity,
    const char* itemId,
    const std::string& commandLabel,
    const std::string& mergeKey,
    DrawFn&& drawFn,
    bool mergeable = true
) {
    const std::optional<CharacterEditorSnapshot> current =
        captureCharacterSnapshot(services.world, entity);
    if (!current.has_value()) {
        return false;
    }

    bool changed = false;
    editComponentSnapshot<CharacterEditorSnapshot>(
        itemId,
        commandLabel,
        mergeKey,
        *current,
        [&services, entity](const CharacterEditorSnapshot& snapshot) {
            applyCharacterSnapshot(services.world, entity, snapshot);
        },
        [&changed, draw = std::forward<DrawFn>(drawFn)](CharacterEditorSnapshot& edited) mutable {
            changed = draw(edited);
            return changed;
        },
        services.commands,
        mergeable
    );
    return changed;
}

void drawIdentityEditor(EngineServices& services, EntityId entity, bool& changed) {
    ImGui::SeparatorText("Identity");
    changed |= editCharacterValue(
        services,
        entity,
        "Affiliation",
        "Change Character Affiliation",
        "character-affiliation-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            int value = static_cast<int>(edited.character.affiliation);
            if (!ImGui::Combo("Affiliation", &value, "Player\0Friendly NPC\0Hostile NPC\0")) {
                return false;
            }
            edited.character.affiliation = static_cast<CharacterAffiliation>(value);
            return true;
        },
        false
    );
    changed |= editCharacterValue(
        services,
        entity,
        "Race",
        "Change Character Race",
        "character-race-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            int value = static_cast<int>(edited.character.race);
            if (!ImGui::Combo("Race", &value, "Human\0Elf\0Half-Elf\0Orc\0Half-Orc\0Demon\0Dwarf\0")) {
                return false;
            }
            edited.character.race = static_cast<CharacterRace>(value);
            return true;
        },
        false
    );
    changed |= editCharacterValue(
        services,
        entity,
        "Kit",
        "Change Character Kit",
        "character-kit-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            int value = static_cast<int>(edited.character.kit);
            if (!ImGui::Combo(
                    "Kit",
                    &value,
                    "Fighter\0Tracker\0Paladin\0Barbarian\0Cleric\0Druid\0Mage\0Specialist\0Sorcerer\0Rogue\0Bard\0")) {
                return false;
            }
            edited.character.kit = static_cast<CharacterKit>(value);
            return true;
        },
        false
    );
    changed |= editCharacterValue(
        services,
        entity,
        "Level",
        "Edit Character Level",
        "character-level-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            int level = levelForExperience(edited.character.experience);
            if (!ImGui::InputInt("Level", &level)) {
                return false;
            }
            edited.character.experience = experienceForLevel(std::clamp(level, 1, 40));
            return true;
        }
    );
    changed |= editCharacterValue(
        services,
        entity,
        "Experience",
        "Edit Character Experience",
        "character-experience-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            return ImGui::InputScalar(
                "Experience",
                ImGuiDataType_S64,
                &edited.character.experience
            );
        }
    );
    changed |= editCharacterValue(
        services,
        entity,
        "GroundIndicatorRadius",
        "Edit Character Indicator Radius",
        "character-indicator-radius-" + std::to_string(entity.index),
        [](CharacterEditorSnapshot& edited) {
            return ImGui::DragFloat(
                "Ground Indicator Radius",
                &edited.character.groundIndicatorRadius,
                0.02f,
                0.1f,
                10.0f
            );
        }
    );
}

struct AbilityRow {
    const char* name;
    int AbilityScoresComponent::*value;
};

constexpr std::array<AbilityRow, 6> kAbilityRows{{
    {"Strength", &AbilityScoresComponent::strength},
    {"Agility", &AbilityScoresComponent::agility},
    {"Physique", &AbilityScoresComponent::physique},
    {"Intelligence", &AbilityScoresComponent::intelligence},
    {"Faith", &AbilityScoresComponent::faith},
    {"Charisma", &AbilityScoresComponent::charisma},
}};

void drawAbilityEditor(EngineServices& services, EntityId entity, bool& changed) {
    const auto current = captureCharacterSnapshot(services.world, entity);
    if (!current.has_value()) {
        return;
    }
    const CharacterDerivedStats derived = deriveCharacterStats(
        characterRuleData(current->character),
        current->abilities,
        current->skills
    );

    ImGui::SeparatorText("Abilities");
    if (!ImGui::BeginTable(
            "CharacterAbilities",
            4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Ability");
    ImGui::TableSetupColumn("Base");
    ImGui::TableSetupColumn("Effective");
    ImGui::TableSetupColumn("Modifier");
    ImGui::TableHeadersRow();
    for (const AbilityRow& row : kAbilityRows) {
        ImGui::PushID(row.name);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(row.name);
        ImGui::TableSetColumnIndex(1);
        changed |= editCharacterValue(
            services,
            entity,
            "BaseAbility",
            std::string("Edit ") + row.name,
            "character-ability-" + std::to_string(entity.index) + "-" + row.name,
            [member = row.value](CharacterEditorSnapshot& edited) {
                return ImGui::InputInt("##Base", &(edited.abilities.*member));
            }
        );
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", derived.effectiveAbilities.*(row.value));
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%+d", derived.abilityModifiers.*(row.value));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawSkillEditor(EngineServices& services, EntityId entity, bool& changed) {
    const auto current = captureCharacterSnapshot(services.world, entity);
    if (!current.has_value()) {
        return;
    }
    const CharacterDerivedStats derived = deriveCharacterStats(
        characterRuleData(current->character),
        current->abilities,
        current->skills
    );
    const int maximumRank = static_cast<int>(maximumSkillRankForLevel(derived.level));
    static constexpr const char* rankNames[] = {
        "Untrained", "Initiate", "Expert", "Master", "Legendary"
    };

    ImGui::SeparatorText("Skills");
    ImGui::Text("Maximum rank at level %d: %s", derived.level, rankNames[maximumRank]);
    if (!ImGui::BeginTable(
            "CharacterSkills",
            3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Skill");
    ImGui::TableSetupColumn("Rank");
    ImGui::TableSetupColumn("Bonus");
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < kCharacterSkillCount; ++index) {
        const CharacterSkill skill = static_cast<CharacterSkill>(index);
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(characterSkillLabel(skill));
        ImGui::TableSetColumnIndex(1);
        changed |= editCharacterValue(
            services,
            entity,
            "SkillRank",
            std::string("Edit ") + characterSkillLabel(skill) + " Rank",
            "character-skill-" + std::to_string(entity.index) + "-" + std::to_string(index),
            [index, maximumRank](CharacterEditorSnapshot& edited) {
                int value = static_cast<int>(edited.skills.ranks[index]);
                if (!ImGui::Combo("##Rank", &value, rankNames, maximumRank + 1)) {
                    return false;
                }
                edited.skills.ranks[index] = static_cast<SkillRank>(value);
                return true;
            },
            false
        );
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%+d", derived.skillBonuses[index]);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void drawVitalsEditor(EngineServices& services, EntityId entity, bool& changed) {
    ImGui::SeparatorText("Vitals");
    const auto drawVital = [&](
        const char* label,
        int CharacterVitalsComponent::*member
    ) {
        changed |= editCharacterValue(
            services,
            entity,
            label,
            std::string("Edit ") + label,
            "character-vital-" + std::to_string(entity.index) + "-" + label,
            [label, member](CharacterEditorSnapshot& edited) {
                return ImGui::InputInt(label, &(edited.vitals.*member));
            }
        );
    };
    drawVital("Current Hit Points", &CharacterVitalsComponent::currentHitPoints);
    drawVital("Maximum Hit Points", &CharacterVitalsComponent::maximumHitPoints);
    drawVital("Current Mana", &CharacterVitalsComponent::currentMana);
}

void drawDerivedStats(const CharacterEditorSnapshot& snapshot) {
    const CharacterDerivedStats derived = deriveCharacterStats(
        characterRuleData(snapshot.character),
        snapshot.abilities,
        snapshot.skills
    );
    ImGui::SeparatorText("Derived Statistics");
    ImGui::Text("Level: %d   Level Bonus: %+d", derived.level, derived.levelBonus);
    ImGui::Text("Robustness: %+d   Caster: %s", derived.robustness, casterProgressionLabel(derived.casterProgression));
    ImGui::Text("Maximum Mana: %d   Initiative: %+d", derived.maximumMana, derived.initiativeBonus);
    ImGui::Text("Dodge: %d   Armor: %d", derived.dodge, derived.armor);
    ImGui::Text("Fortitude: %d   Reflexes: %d", derived.fortitude, derived.reflexes);
    ImGui::Text("Willpower: %d   Magic Resistance: %d", derived.willpower, derived.magicResistance);
    ImGui::Text("Poison Resistance: %d", derived.poisonResistance);
    ImGui::Text("Magic Attack: %+d   Spell Difficulty: %d", derived.magicAttackBonus, derived.offensiveSpellDifficulty);
    ImGui::Text("Concentration: %+d", derived.concentrationBonus);
    ImGui::Text("Carry: %d comfortable / %d maximum", derived.comfortableCarry, derived.maximumCarry);
    ImGui::Text("Movement: %d m", derived.movementMeters);
}

}  // namespace

bool drawCharacterInspector(EngineServices& services, EntityId entity) {
    if (!captureCharacterSnapshot(services.world, entity).has_value()) {
        ImGui::TextUnformatted("Character data is incomplete.");
        return false;
    }

    bool changed = false;
    drawIdentityEditor(services, entity, changed);
    changed |= drawCharacterControlInspector(services, entity);
    drawAbilityEditor(services, entity, changed);
    drawSkillEditor(services, entity, changed);
    drawVitalsEditor(services, entity, changed);
    if (const auto current = captureCharacterSnapshot(services.world, entity)) {
        drawDerivedStats(*current);
    }
    return changed;
}

}  // namespace core
