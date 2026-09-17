#include <cassert>
#include <cstdlib>
#include <memory>
#include <utility>

#include "core/editor/ComponentRegistry.hpp"
#include "core/ecs/World.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/simulation/CombatSimulation.hpp"
#include "core/systems/CombatSystem.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace {

core::CombatantComponent makeConfiguredCombatant() {
    core::CombatantComponent combatant{};
    combatant.animations.unarmed = {"Unarmed Ready", "Unarmed Attack"};
    combatant.animations.melee = {"Sword Ready", "Sword Attack"};
    combatant.animations.ranged = {"Bow Ready", "Bow Shoot"};
    combatant.animations.hitReactionClip = "Hit";
    combatant.animations.fleeClip = "Run";
    combatant.animations.scriptedClip = "Boss Phase";
    combatant.animations.downedClip = "Downed";
    combatant.animations.deadClip = "Death";
    return combatant;
}

void testCombatAnimationSelectionUsesWeaponModeAndState() {
    core::CombatantComponent combatant = makeConfiguredCombatant();
    combatant.weaponMode = core::CombatWeaponMode::Melee;

    combatant.state = core::CombatState::Combat;
    assert(core::combatAnimationClip(combatant) == "Sword Ready");
    combatant.state = core::CombatState::Attacking;
    assert(core::combatAnimationClip(combatant) == "Sword Attack");

    combatant.weaponMode = core::CombatWeaponMode::Ranged;
    assert(core::combatAnimationClip(combatant) == "Bow Shoot");
    combatant.state = core::CombatState::HitReaction;
    assert(core::combatAnimationClip(combatant) == "Hit");
    combatant.state = core::CombatState::Fleeing;
    assert(core::combatAnimationClip(combatant) == "Run");
    combatant.state = core::CombatState::Scripted;
    assert(core::combatAnimationClip(combatant) == "Boss Phase");
    combatant.state = core::CombatState::Downed;
    assert(core::combatAnimationClip(combatant) == "Downed");
    combatant.state = core::CombatState::Dead;
    assert(core::combatAnimationClip(combatant) == "Death");

    combatant.weaponMode = core::CombatWeaponMode::Unarmed;
    combatant.animations.unarmed.attackClip.clear();
    combatant.state = core::CombatState::Attacking;
    assert(core::combatAnimationClip(combatant) == "Unarmed Ready");
    assert(core::combatStateAllowsActions(combatant.state));
    assert(core::combatStateLocksMovement(combatant.state));
    assert(!core::combatStateAllowsActions(core::CombatState::Downed));
}

void testTransientStateRemembersItsGraphExit() {
    core::CombatantComponent combatant{};
    combatant.state = core::CombatState::Combat;
    combatant.resumeState = core::CombatState::Combat;
    combatant.observedState = core::CombatState::Combat;

    core::setCombatState(combatant, core::CombatState::HitReaction);
    assert(combatant.state == core::CombatState::HitReaction);
    assert(combatant.resumeState == core::CombatState::Combat);
    assert(combatant.observedState == core::CombatState::Combat);

    core::setCombatState(combatant, core::CombatState::Attacking);
    assert(combatant.resumeState == core::CombatState::Combat);
    core::setCombatState(combatant, core::CombatState::Idle);
    assert(combatant.resumeState == core::CombatState::Idle);
}

void testCombatSystemDownsOnlyCombatantsAndStopsMovement() {
    core::World world{};
    const core::EntityId combatantEntity = world.createEntity();
    world.combatants.emplace(combatantEntity, makeConfiguredCombatant());
    world.characterVitals.emplace(
        combatantEntity,
        core::CharacterVitalsComponent{0, 20, 0}
    );

    core::NavAgentComponent agent{};
    agent.moving = true;
    agent.destination = glm::vec3(4.0f, 0.0f, 6.0f);
    agent.pathCorners = {
        glm::vec3(2.0f, 0.0f, 3.0f),
        glm::vec3(4.0f, 0.0f, 6.0f),
    };
    agent.desiredVelocity = glm::vec3(1.0f, 0.0f, 1.0f);
    world.navAgents.emplace(combatantEntity, agent);

    core::RigidbodyComponent body{};
    body.velocity = glm::vec3(2.0f, 3.0f, 4.0f);
    world.rigidbodies.emplace(combatantEntity, body);

    const core::EntityId nonCombatant = world.createEntity();
    world.characterVitals.emplace(
        nonCombatant,
        core::CharacterVitalsComponent{0, 20, 0}
    );
    core::NavAgentComponent nonCombatantAgent{};
    nonCombatantAgent.moving = true;
    nonCombatantAgent.destination = glm::vec3(8.0f, 0.0f, 9.0f);
    nonCombatantAgent.desiredVelocity = glm::vec3(0.5f, 0.0f, 0.25f);
    world.navAgents.emplace(nonCombatant, nonCombatantAgent);

    core::TimeContext time{};
    time.deltaSeconds = 1.0f / 60.0f;
    core::CombatSystem{}.update(world, time);

    const core::CombatantComponent& combatant = world.combatants.get(combatantEntity);
    assert(combatant.state == core::CombatState::Downed);
    assert(combatant.stateElapsedSeconds == 0.0f);

    const core::NavAgentComponent& stoppedAgent = world.navAgents.get(combatantEntity);
    assert(!stoppedAgent.moving);
    assert(!stoppedAgent.destination.has_value());
    assert(stoppedAgent.pathCorners.empty());
    assert(stoppedAgent.desiredVelocity == glm::vec3(0.0f));
    assert(world.rigidbodies.get(combatantEntity).velocity ==
        glm::vec3(0.0f, 3.0f, 0.0f));

    const core::NavAgentComponent& unaffectedAgent = world.navAgents.get(nonCombatant);
    assert(unaffectedAgent.moving);
    assert(unaffectedAgent.destination.has_value());
    assert(unaffectedAgent.desiredVelocity == glm::vec3(0.5f, 0.0f, 0.25f));
}

void testCombatSystemRequestsConfiguredAnimation() {
    auto model = std::make_shared<render::GltfModelData>();
    model->animations = {
        render::AnimationClip{"Idle", 1.0f, {}},
        render::AnimationClip{"Sword Ready", 1.0f, {}},
        render::AnimationClip{"Sword Attack", 1.0f, {}},
        render::AnimationClip{"Bow Shoot", 1.0f, {}},
        render::AnimationClip{"Death", 1.0f, {}},
    };

    core::World world{};
    const core::EntityId entity = world.createEntity();
    core::CombatantComponent combatant = makeConfiguredCombatant();
    combatant.weaponMode = core::CombatWeaponMode::Melee;
    combatant.state = core::CombatState::Attacking;
    world.combatants.emplace(entity, combatant);

    core::AnimatedModelComponent animation{};
    animation.model = model;
    animation.currentClip = 0;
    world.animatedModels.emplace(entity, animation);

    core::TimeContext time{};
    time.deltaSeconds = 0.1f;
    const core::CombatSystem system{};
    system.update(world, time);
    assert(world.animatedModels.get(entity).requestedClip == 2);
    assert(world.combatants.get(entity).stateElapsedSeconds == 0.0f);

    system.update(world, time);
    assert(world.combatants.get(entity).stateElapsedSeconds == 0.1f);

    core::CombatantComponent& edited = world.combatants.get(entity);
    edited.weaponMode = core::CombatWeaponMode::Ranged;
    core::setCombatState(edited, core::CombatState::Attacking);
    world.animatedModels.get(entity).requestedClip = -1;
    system.update(world, time);
    assert(world.animatedModels.get(entity).requestedClip == 3);
}

void testCombatAnimationGraphCancelsJitterAndReturnsToCombat() {
    auto model = std::make_shared<render::GltfModelData>();
    model->animations = {
        render::AnimationClip{"Idle", 1.0f, {}},
        render::AnimationClip{"Sword Ready", 1.0f, {}},
        render::AnimationClip{"Hit", 0.5f, {}},
    };

    core::World world{};
    const core::EntityId entity = world.createEntity();
    core::CombatantComponent combatant = makeConfiguredCombatant();
    combatant.weaponMode = core::CombatWeaponMode::Melee;
    combatant.state = core::CombatState::HitReaction;
    combatant.resumeState = core::CombatState::Combat;
    combatant.observedState = core::CombatState::HitReaction;
    world.combatants.emplace(entity, combatant);
    world.characterVitals.emplace(
        entity,
        core::CharacterVitalsComponent{10, 20, 0}
    );
    world.locomotion.emplace(entity, core::LocomotionComponent{0, 0});

    core::AnimatedModelComponent animation{};
    animation.model = model;
    animation.currentClip = 0;
    animation.nextClip = 2;
    animation.nextTime = 0.2f;
    animation.requestedClip = 0;
    world.animatedModels.emplace(entity, animation);

    core::TimeContext time{};
    time.deltaSeconds = 0.0f;
    const core::CombatSystem system{};
    system.update(world, time);
    assert(world.animatedModels.get(entity).requestedClip == -1);
    assert(!world.animatedModels.get(entity).loop);

    core::AnimatedModelComponent& playing = world.animatedModels.get(entity);
    playing.currentClip = 2;
    playing.currentTime = 0.45f;
    playing.nextClip = -1;
    time.deltaSeconds = 0.1f;
    system.update(world, time);
    assert(world.combatants.get(entity).state == core::CombatState::Combat);
    assert(playing.requestedClip == 1);
    assert(!playing.loop);

    playing.currentClip = 1;
    playing.currentTime = 0.1f;
    playing.nextClip = -1;
    playing.requestedClip = -1;
    system.update(world, time);
    assert(playing.loop);
}

void testStableStatePreservesManualAnimationLoopSetting() {
    auto model = std::make_shared<render::GltfModelData>();
    model->animations = {
        render::AnimationClip{"Sword Ready", 1.0f, {}},
    };

    core::World world{};
    const core::EntityId entity = world.createEntity();
    core::CombatantComponent combatant = makeConfiguredCombatant();
    combatant.weaponMode = core::CombatWeaponMode::Melee;
    combatant.state = core::CombatState::Combat;
    combatant.resumeState = core::CombatState::Combat;
    combatant.observedState = core::CombatState::Combat;
    world.combatants.emplace(entity, combatant);

    core::AnimatedModelComponent animation{};
    animation.model = std::move(model);
    animation.currentClip = 0;
    animation.loop = false;
    world.animatedModels.emplace(entity, std::move(animation));

    core::CombatSystem{}.update(world, core::TimeContext{});
    assert(!world.animatedModels.get(entity).loop);
}

void testIdleCombatantPreservesNavigationAnimationRequest() {
    auto model = std::make_shared<render::GltfModelData>();
    model->animations = {
        render::AnimationClip{"Idle", 1.0f, {}},
        render::AnimationClip{"Walk", 1.0f, {}},
    };

    core::World world{};
    const core::EntityId entity = world.createEntity();
    world.combatants.emplace(entity, core::CombatantComponent{});
    world.locomotion.emplace(entity, core::LocomotionComponent{0, 1});
    core::AnimatedModelComponent animation{};
    animation.model = std::move(model);
    animation.currentClip = 0;
    animation.requestedClip = 1;
    world.animatedModels.emplace(entity, std::move(animation));

    core::CombatSystem{}.update(world, core::TimeContext{});
    assert(world.animatedModels.get(entity).requestedClip == 1);
}

void testLockedCombatStateStopsDirectNavigationMotion() {
    core::World world{};
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(entity, core::TransformComponent{});
    core::CombatantComponent combatant{};
    combatant.state = core::CombatState::Attacking;
    world.combatants.emplace(entity, combatant);

    core::NavAgentComponent agent{};
    agent.moving = true;
    agent.destination = glm::vec3(2.0f, 0.0f, 0.0f);
    agent.pathCorners.push_back(*agent.destination);
    world.navAgents.emplace(entity, agent);

    core::TimeContext time{};
    time.deltaSeconds = 0.5f;
    core::NavigationSystem{}.updateAgents(
        world,
        core::NavigationRuntime{},
        time
    );

    assert(world.transforms.get(entity).position == glm::vec3(0.0f));
    assert(world.navAgents.get(entity).moving);
    assert(world.navAgents.get(entity).pathCorners.size() == 1u);
    assert(world.navAgents.get(entity).desiredVelocity == glm::vec3(0.0f));
}

void testCombatantWorldLifecycleAndDescriptorConstraints() {
    core::World world{};
    const core::EntityId entity = world.createEntity();
    const core::ComponentRegistry registry{};
    const core::ComponentDescriptor* characterDescriptor =
        registry.find(core::ComponentKind::Character);
    const core::ComponentDescriptor* combatantDescriptor =
        registry.find(core::ComponentKind::Combatant);
    assert(characterDescriptor != nullptr);
    assert(combatantDescriptor != nullptr);
    assert(combatantDescriptor->canAddComponent);
    assert(!combatantDescriptor->canAddComponent(world, entity));

    characterDescriptor->addComponent(world, entity);
    assert(combatantDescriptor->canAddComponent(world, entity));
    combatantDescriptor->addComponent(world, entity);
    assert(world.combatants.contains(entity));

    core::AnimatedModelComponent animation{};
    animation.loop = false;
    world.animatedModels.emplace(entity, animation);
    world.combatants.get(entity).animationLoopOverrideActive = true;
    world.combatants.get(entity).animationLoopBeforeOverride = true;
    combatantDescriptor->removeComponent(world, entity);
    assert(world.animatedModels.get(entity).loop);
    assert(!world.combatants.contains(entity));

    combatantDescriptor->addComponent(world, entity);
    world.animatedModels.get(entity).loop = false;
    world.combatants.get(entity).animationLoopOverrideActive = true;
    world.combatants.get(entity).animationLoopBeforeOverride = true;

    characterDescriptor->removeComponent(world, entity);
    assert(!world.combatants.contains(entity));
    assert(world.animatedModels.get(entity).loop);

    characterDescriptor->addComponent(world, entity);
    combatantDescriptor->addComponent(world, entity);
    world.destroyEntity(entity);
    assert(!world.combatants.contains(entity));

    const core::EntityId second = world.createEntity();
    world.combatants.emplace(second, core::CombatantComponent{});
    world.clear();
    assert(world.combatants.size() == 0u);
}

}  // namespace

int main() {
    testCombatAnimationSelectionUsesWeaponModeAndState();
    testTransientStateRemembersItsGraphExit();
    testCombatSystemDownsOnlyCombatantsAndStopsMovement();
    testCombatSystemRequestsConfiguredAnimation();
    testCombatAnimationGraphCancelsJitterAndReturnsToCombat();
    testStableStatePreservesManualAnimationLoopSetting();
    testIdleCombatantPreservesNavigationAnimationRequest();
    testLockedCombatStateStopsDirectNavigationMotion();
    testCombatantWorldLifecycleAndDescriptorConstraints();
    return EXIT_SUCCESS;
}
