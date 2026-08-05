#include <cassert>
#include <cstdlib>
#include <memory>

#include "core/ecs/ComponentRegistry.hpp"
#include "core/ecs/World.hpp"
#include "core/editor/SelectionModel.hpp"
#include "core/gameplay/CharacterRules.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/scene/SceneRegistry.hpp"
#include "core/systems/RenderExtractionSystem.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "core/transform/TransformSystem.hpp"
#include "render/engine/RenderSceneView.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace {

void testAbilityAndRaceRules() {
    assert(core::abilityModifier(1) == -5);
    assert(core::abilityModifier(6) == -2);
    assert(core::abilityModifier(10) == 0);
    assert(core::abilityModifier(14) == 2);
    assert(core::abilityModifier(31) == 10);

    const core::AbilityScoresComponent orc = core::effectiveAbilityScores(
        core::AbilityScoresComponent{12, 14, 10, 10, 10, 8},
        core::CharacterRace::Orc
    );
    assert(orc.strength == 14);
    assert(orc.agility == 14);
    assert(orc.physique == 10);
    assert(orc.intelligence == 8);
    assert(orc.faith == 10);
    assert(orc.charisma == 8);

    const core::AbilityScoresComponent dwarf = core::effectiveAbilityScores(
        core::AbilityScoresComponent{10, 2, 10, 10, 10, 10},
        core::CharacterRace::Dwarf
    );
    assert(dwarf.agility == 1);
    assert(dwarf.physique == 12);
}

void testProgressionRules() {
    assert(core::experienceForLevel(1) == 0);
    assert(core::experienceForLevel(2) == 1000);
    assert(core::experienceForLevel(5) == 10000);
    assert(core::experienceForLevel(40) == 780000);
    assert(core::levelForExperience(-1) == 1);
    assert(core::levelForExperience(999) == 1);
    assert(core::levelForExperience(1000) == 2);
    assert(core::levelForExperience(779999) == 39);
    assert(core::levelForExperience(780000) == 40);
    assert(core::levelBonus(1) == 1);
    assert(core::levelBonus(8) == 1);
    assert(core::levelBonus(9) == 2);
    assert(core::levelBonus(40) == 5);

    assert(core::maximumSkillRankForLevel(1) == core::SkillRank::Initiate);
    assert(core::maximumSkillRankForLevel(10) == core::SkillRank::Expert);
    assert(core::maximumSkillRankForLevel(20) == core::SkillRank::Master);
    assert(core::maximumSkillRankForLevel(30) == core::SkillRank::Legendary);
}

void testHitPointAndManaRules() {
    const core::AbilityScoresComponent fighterAbilities{14, 12, 14, 10, 10, 10};
    assert(core::kitRobustness(core::CharacterKit::Fighter) == 2);
    assert(core::startingHitPoints(fighterAbilities, core::CharacterKit::Fighter) == 53);
    assert(core::hitPointsPerLevel(fighterAbilities, core::CharacterKit::Fighter) == 7);

    const core::AbilityScoresComponent clericAbilities{10, 10, 12, 12, 14, 12};
    assert(core::kitCasterProgression(core::CharacterKit::Cleric) == core::CasterProgression::Full);
    assert(core::maximumMana(clericAbilities, core::CharacterKit::Cleric, 1) == 33);
    assert(core::kitCasterProgression(core::CharacterKit::Paladin) == core::CasterProgression::Half);
    assert(core::maximumMana(clericAbilities, core::CharacterKit::Paladin, 1) == 17);
    assert(core::maximumMana(clericAbilities, core::CharacterKit::Rogue, 40) == 0);
}

void testDerivedStatsAndSkillBonuses() {
    core::CharacterComponent character{};
    character.affiliation = core::CharacterAffiliation::Player;
    character.race = core::CharacterRace::Human;
    character.kit = core::CharacterKit::Fighter;

    const core::AbilityScoresComponent abilities{14, 12, 14, 10, 10, 10};
    core::SkillRanksComponent skills{};
    skills.ranks[static_cast<std::size_t>(core::CharacterSkill::Running)] = core::SkillRank::Initiate;
    skills.ranks[static_cast<std::size_t>(core::CharacterSkill::Acrobatics)] = core::SkillRank::Initiate;

    const core::CharacterDerivedStats derived = core::deriveCharacterStats(character, abilities, skills);
    assert(derived.level == 1);
    assert(derived.levelBonus == 1);
    assert(derived.initiativeBonus == 1);
    assert(derived.dodge == 11);
    assert(derived.armor == 2);
    assert(derived.fortitude == 12);
    assert(derived.reflexes == 11);
    assert(derived.willpower == 10);
    assert(derived.magicResistance == 10);
    assert(derived.poisonResistance == 12);
    assert(derived.magicAttackBonus == 1);
    assert(derived.offensiveSpellDifficulty == 11);
    assert(derived.concentrationBonus == 1);
    assert(derived.comfortableCarry == 70);
    assert(derived.maximumCarry == 140);
    assert(derived.movementMeters == 6);
    assert(derived.skillBonuses[static_cast<std::size_t>(core::CharacterSkill::Running)] == 4);
    assert(derived.skillBonuses[static_cast<std::size_t>(core::CharacterSkill::Acrobatics)] == 3);

    character.race = core::CharacterRace::Orc;
    const core::CharacterDerivedStats orcDerived =
        core::deriveCharacterStats(character, abilities, skills);
    assert(orcDerived.effectiveAbilities.strength == 16);
    assert(orcDerived.comfortableCarry == 100);
    assert(orcDerived.maximumCarry == 200);
}

void testNormalizationKeepsHistoricalMaximumHitPoints() {
    core::CharacterComponent character{};
    character.experience = -100;
    character.groundIndicatorRadius = -1.0f;
    core::AbilityScoresComponent abilities{0, -2, 0, 0, 0, 0};
    core::SkillRanksComponent skills{};
    skills.ranks[0] = core::SkillRank::Legendary;
    core::CharacterVitalsComponent vitals{80, 47, 999};

    core::normalizeCharacterData(character, abilities, skills, vitals);

    assert(character.experience == 0);
    assert(character.groundIndicatorRadius == 0.1f);
    assert(abilities.strength == 1);
    assert(abilities.agility == 1);
    assert(skills.ranks[0] == core::SkillRank::Initiate);
    assert(vitals.maximumHitPoints == 47);
    assert(vitals.currentHitPoints == 47);
    assert(vitals.currentMana == 0);

    abilities.physique = 30;
    character.kit = core::CharacterKit::Barbarian;
    core::normalizeCharacterData(character, abilities, skills, vitals);
    assert(vitals.maximumHitPoints == 47);
}

void testCharacterWorldLifecycleAndOwnerResolution() {
    core::World world{};
    const core::EntityId root = world.createEntity();
    const core::EntityId child = world.createEntity();
    const core::EntityId skinnedChild = world.createEntity();
    world.characters.emplace(root, core::CharacterComponent{});
    world.abilityScores.emplace(root, core::AbilityScoresComponent{});
    world.skillRanks.emplace(root, core::SkillRanksComponent{});
    world.characterVitals.emplace(root, core::CharacterVitalsComponent{});
    world.parents.emplace(child, core::ParentComponent{root});
    world.skinnedRenderables.emplace(
        skinnedChild,
        core::SkinnedRenderableComponent{root, 0, 0, 0}
    );

    assert(world.characterOwnerEntity(root) == root);
    assert(world.characterOwnerEntity(child) == root);
    assert(world.characterOwnerEntity(skinnedChild) == root);

    world.destroyEntity(root);
    assert(!world.characters.contains(root));
    assert(!world.abilityScores.contains(root));
    assert(!world.skillRanks.contains(root));
    assert(!world.characterVitals.contains(root));
    assert(!world.characterOwnerEntity(child).valid());
    assert(!world.characterOwnerEntity(skinnedChild).valid());

    world.clear();
    assert(world.characters.size() == 0u);
    assert(world.abilityScores.size() == 0u);
    assert(world.skillRanks.size() == 0u);
    assert(world.characterVitals.size() == 0u);
}

void testCharacterDescriptorOwnsTheCompleteComponentBundle() {
    core::World world{};
    const core::EntityId entity = world.createEntity();
    const core::ComponentRegistry registry{};
    const core::ComponentDescriptor* descriptor = registry.find(core::ComponentKind::Character);
    assert(descriptor != nullptr);
    assert(!descriptor->hasComponent(world, entity));

    descriptor->addComponent(world, entity);
    assert(descriptor->hasComponent(world, entity));
    assert(world.characters.contains(entity));
    assert(world.abilityScores.contains(entity));
    assert(world.skillRanks.contains(entity));
    assert(world.characterVitals.contains(entity));
    assert(world.characterVitals.get(entity).currentHitPoints == 45);
    assert(world.characterVitals.get(entity).maximumHitPoints == 45);

    descriptor->removeComponent(world, entity);
    assert(!world.characters.contains(entity));
    assert(!world.abilityScores.contains(entity));
    assert(!world.skillRanks.contains(entity));
    assert(!world.characterVitals.contains(entity));
}

void testDefaultSceneDefinesThreeCharacterProfiles() {
    const core::SceneBlueprint scene = core::SceneRegistry{}.defaultScene();
    assert(scene.models.size() >= 3u);
    assert(scene.models[0].character.has_value());
    assert(scene.models[1].character.has_value());
    assert(scene.models[2].character.has_value());

    const core::CharacterBlueprint& player = *scene.models[0].character;
    const core::CharacterBlueprint& friendly = *scene.models[1].character;
    const core::CharacterBlueprint& hostile = *scene.models[2].character;
    assert(player.character.affiliation == core::CharacterAffiliation::Player);
    assert(player.character.race == core::CharacterRace::Human);
    assert(player.character.kit == core::CharacterKit::Fighter);
    assert(player.vitals.maximumHitPoints == 53);
    assert(friendly.character.affiliation == core::CharacterAffiliation::FriendlyNpc);
    assert(friendly.character.kit == core::CharacterKit::Cleric);
    assert(friendly.vitals.currentMana == 33);
    assert(hostile.character.affiliation == core::CharacterAffiliation::HostileNpc);
    assert(hostile.character.race == core::CharacterRace::Orc);
    assert(hostile.character.kit == core::CharacterKit::Rogue);

    const auto isInitiate = [](const core::CharacterBlueprint& blueprint, core::CharacterSkill skill) {
        return blueprint.skills.ranks[static_cast<std::size_t>(skill)] == core::SkillRank::Initiate;
    };
    assert(isInitiate(player, core::CharacterSkill::Running));
    assert(isInitiate(player, core::CharacterSkill::Perception));
    assert(isInitiate(player, core::CharacterSkill::Acrobatics));
    assert(isInitiate(friendly, core::CharacterSkill::FirstAid));
    assert(isInitiate(friendly, core::CharacterSkill::Persuasion));
    assert(isInitiate(friendly, core::CharacterSkill::Survival));
    assert(isInitiate(hostile, core::CharacterSkill::Stealth));
    assert(isInitiate(hostile, core::CharacterSkill::Deception));
    assert(isInitiate(hostile, core::CharacterSkill::Intimidation));
}

void testNavigationControlIsGrantedOnlyToThePlayer() {
    core::World world{};
    const auto model = std::make_shared<render::GltfModelData>();
    const auto addAnimatedCharacter = [&world, &model](core::CharacterAffiliation affiliation) {
        const core::EntityId entity = world.createEntity();
        world.animatedModels.emplace(entity, core::AnimatedModelComponent{model});
        core::CharacterComponent character{};
        character.affiliation = affiliation;
        world.characters.emplace(entity, character);
        return entity;
    };
    const core::EntityId player = addAnimatedCharacter(core::CharacterAffiliation::Player);
    const core::EntityId friendly = addAnimatedCharacter(core::CharacterAffiliation::FriendlyNpc);
    const core::EntityId hostile = addAnimatedCharacter(core::CharacterAffiliation::HostileNpc);

    core::NavigationRuntime runtime{};
    const core::SceneBlueprint emptyScene{};
    core::NavigationSystem{}.initializeScene(emptyScene, world, runtime);

    assert(world.navAgents.contains(player));
    assert(world.locomotion.contains(player));
    assert(!world.navAgents.contains(friendly));
    assert(!world.locomotion.contains(friendly));
    assert(!world.navAgents.contains(hostile));
    assert(!world.locomotion.contains(hostile));
}

void testGroundIndicatorsExtractAffiliationColorsAndRootTransforms() {
    core::World world{};
    const auto addCharacter = [&world](
        core::CharacterAffiliation affiliation,
        const glm::vec3& position,
        bool visible
    ) {
        const core::EntityId entity = world.createEntity();
        world.transforms.emplace(
            entity,
            core::TransformComponent{position, glm::vec3(0.0f), glm::vec3(1.0f)}
        );
        world.visibilities.emplace(entity, core::VisibilityComponent{visible});
        core::CharacterComponent character{};
        character.affiliation = affiliation;
        character.groundIndicatorRadius = 0.75f;
        world.characters.emplace(entity, character);
        world.markTransformsDirty(entity);
        return entity;
    };

    const core::EntityId player = addCharacter(
        core::CharacterAffiliation::Player,
        glm::vec3(1.0f, 0.0f, 2.0f),
        true
    );
    const core::EntityId friendly = addCharacter(
        core::CharacterAffiliation::FriendlyNpc,
        glm::vec3(3.0f, 0.0f, 4.0f),
        true
    );
    const core::EntityId hostile = addCharacter(
        core::CharacterAffiliation::HostileNpc,
        glm::vec3(5.0f, 0.0f, 6.0f),
        true
    );
    addCharacter(core::CharacterAffiliation::HostileNpc, glm::vec3(9.0f), false);

    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::TransformSystem{}.update(world, scheduler, true);
    core::FrameSceneData frame{};
    core::RenderExtractionSystem{}.extract(
        world,
        core::SelectionModel{},
        frame,
        scheduler,
        true
    );

    assert(frame.groundIndicators.size() == 3u);
    assert(frame.groundIndicators[0].owner == player);
    assert(frame.groundIndicators[1].owner == friendly);
    assert(frame.groundIndicators[2].owner == hostile);
    assert(frame.groundIndicators[0].center.x == 1.0f);
    assert(frame.groundIndicators[0].center.y == 0.02f);
    assert(frame.groundIndicators[0].center.z == 2.0f);
    assert(frame.groundIndicators[0].radius == 0.75f);
    assert(frame.groundIndicators[0].color.g > frame.groundIndicators[0].color.r);
    assert(frame.groundIndicators[1].color.b > frame.groundIndicators[1].color.r);
    assert(frame.groundIndicators[2].color.r > frame.groundIndicators[2].color.b);

    const render::RenderSceneView renderScene = render::buildRenderSceneView(
        frame,
        {},
        scheduler,
        true
    );
    assert(renderScene.groundIndicators.size() == frame.groundIndicators.size());
    assert(renderScene.groundIndicators[2].owner == hostile);
    assert(renderScene.groundIndicators[2].radius == 0.75f);
}

}  // namespace

int main() {
    testAbilityAndRaceRules();
    testProgressionRules();
    testHitPointAndManaRules();
    testDerivedStatsAndSkillBonuses();
    testNormalizationKeepsHistoricalMaximumHitPoints();
    testCharacterWorldLifecycleAndOwnerResolution();
    testCharacterDescriptorOwnsTheCompleteComponentBundle();
    testDefaultSceneDefinesThreeCharacterProfiles();
    testNavigationControlIsGrantedOnlyToThePlayer();
    testGroundIndicatorsExtractAffiliationColorsAndRootTransforms();
    return EXIT_SUCCESS;
}
