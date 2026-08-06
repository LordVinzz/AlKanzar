#include "ComponentRegistry.hpp"

#include "core/ecs/World.hpp"
#include "core/app/EngineServices.hpp"
#include "core/editor/CharacterInspector.hpp"
#include "core/rules/CharacterRules.hpp"
#include "core/simulation/CharacterSimulation.hpp"

namespace core {

void ComponentRegistry::registerCharacterDescriptor() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Character,
        "Character",
        "Gameplay",
        [](const World& world, EntityId entity) {
            return world.characters.contains(entity);
        },
        [](World& world, EntityId entity) {
            CharacterComponent character{};
            AbilityScoresComponent abilities{};
            SkillRanksComponent skills{};
            const AbilityScoresComponent effective = effectiveAbilityScores(
                abilities,
                character.race
            );
            const int maximumHitPoints = startingHitPoints(effective, character.kit);
            const int currentMana = maximumMana(effective, character.kit, 1);
            CharacterVitalsComponent vitals{
                maximumHitPoints,
                maximumHitPoints,
                currentMana
            };
            normalizeCharacterComponents(character, abilities, skills, vitals);
            world.characters.emplace(entity, character);
            world.abilityScores.emplace(entity, abilities);
            world.skillRanks.emplace(entity, skills);
            world.characterVitals.emplace(entity, vitals);
        },
        [](World& world, EntityId entity) {
            world.characterVitals.remove(entity);
            world.skillRanks.remove(entity);
            world.abilityScores.remove(entity);
            world.characters.remove(entity);
        },
        [](EngineServices& services, EntityId entity) {
            return drawCharacterInspector(services, entity);
        },
    });
}

}  // namespace core
