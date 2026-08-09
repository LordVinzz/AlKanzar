#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>

#include "core/ecs/World.hpp"
#include "core/navigation/Navigation.hpp"
#include "core/physics/PhysicsSystem.hpp"
#include "core/systems/PartyOrderSystem.hpp"
#include "core/systems/TaskScheduler.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace {

std::int64_t navigationCounter(
    const core::NavigationSystem& navigation,
    std::string_view name
) {
    for (const render::FrameCounterRecord& counter : navigation.profilingCounters()) {
        if (counter.name == name) {
            return counter.value;
        }
    }
    assert(false);
    return -1;
}

void waitForPathRequests(
    core::NavigationSystem& navigation,
    core::World& world,
    const core::NavigationRuntime& runtime
) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        navigation.applyCompletedPathRequests(world, runtime);
        if (navigationCounter(navigation, "Pending Path Requests") == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false);
}

core::EntityId addPartyAgent(
    core::World& world,
    std::uint8_t slot,
    const glm::vec3& position
) {
    const core::EntityId entity = world.createEntity();
    core::CharacterComponent character{};
    character.affiliation = slot == 0u
        ? core::CharacterAffiliation::Player
        : core::CharacterAffiliation::FriendlyNpc;
    world.characters.emplace(entity, character);
    world.characterControllers.emplace(
        entity,
        core::CharacterControllerComponent{core::CharacterControllerKind::Player}
    );
    world.partyMembers.emplace(entity, core::PartyMemberComponent{slot, true});
    world.transforms.emplace(
        entity,
        core::TransformComponent{position, glm::vec3(0.0f), glm::vec3(1.0f)}
    );
    world.navAgents.emplace(entity, core::NavAgentComponent{});
    return entity;
}

core::NavigationRuntime makeOpenNavigationRuntime(core::NavigationSystem& navigation) {
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{
            1,
            0.0f,
            {
                glm::vec2(-10.0f, -10.0f),
                glm::vec2(10.0f, -10.0f),
                glm::vec2(10.0f, 10.0f),
                glm::vec2(-10.0f, 10.0f),
            }
        }
    };
    assert(navigation.rebuildRuntime(runtime));
    return runtime;
}

core::EntityId addPhysicsAgent(
    core::World& world,
    const glm::vec3& position
) {
    const core::EntityId entity = world.createEntity();
    world.transforms.emplace(
        entity,
        core::TransformComponent{position, glm::vec3(0.0f), glm::vec3(1.0f)}
    );
    core::NavAgentComponent agent{};
    agent.clearanceSource = core::NavAgentClearanceSource::BoxCollider;
    world.navAgents.emplace(entity, agent);
    world.boxColliders.emplace(entity, core::BoxColliderComponent{
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.22f, 1.0f, 0.22f),
        false,
        false
    });
    world.rigidbodies.emplace(
        entity,
        core::RigidbodyComponent{1.0f, 0.0f, 0.0f, false, false}
    );
    return entity;
}

void testControllableAgentGetsDefaultPhysicsAndColliderClearance() {
    core::World world{};
    const auto model = std::make_shared<render::GltfModelData>();
    const core::EntityId entity = world.createEntity();
    world.characters.emplace(entity, core::CharacterComponent{});
    world.characterControllers.emplace(
        entity,
        core::CharacterControllerComponent{core::CharacterControllerKind::Player}
    );
    world.partyMembers.emplace(entity, core::PartyMemberComponent{0u, true});
    world.animatedModels.emplace(
        entity,
        core::AnimatedModelComponent{model}
    );

    core::NavigationSystem navigation{};
    navigation.syncCharacterAgentControl(world, entity);

    assert(world.navAgents.contains(entity));
    assert(world.boxColliders.contains(entity));
    assert(world.rigidbodies.contains(entity));
    const core::NavAgentComponent& agent = world.navAgents.get(entity);
    const core::BoxColliderComponent& collider = world.boxColliders.get(entity);
    const core::RigidbodyComponent& rigidbody = world.rigidbodies.get(entity);
    assert(agent.clearanceSource == core::NavAgentClearanceSource::BoxCollider);
    assert(glm::distance(collider.halfExtents * 2.0f, glm::vec3(0.44f, 2.0f, 0.44f)) < 1.0e-6f);
    assert(glm::distance(collider.center, glm::vec3(0.0f, 1.0f, 0.0f)) < 1.0e-6f);
    assert(!rigidbody.isKinematic);
    assert(!rigidbody.useGravity);

    world.boxColliders.get(entity).halfExtents = glm::vec3(0.5f);
    world.rigidbodies.get(entity).useGravity = true;
    world.navAgents.get(entity).clearanceSource = core::NavAgentClearanceSource::None;
    navigation.syncCharacterAgentControl(world, entity);
    assert(glm::distance(
        world.boxColliders.get(entity).halfExtents,
        glm::vec3(0.5f)
    ) < 1.0e-6f);
    assert(world.rigidbodies.get(entity).useGravity);
    assert(world.navAgents.get(entity).clearanceSource == core::NavAgentClearanceSource::None);

    const core::EntityId uncontrolledNpc = world.createEntity();
    world.characters.emplace(uncontrolledNpc, core::CharacterComponent{});
    world.characterControllers.emplace(
        uncontrolledNpc,
        core::CharacterControllerComponent{}
    );
    world.animatedModels.emplace(
        uncontrolledNpc,
        core::AnimatedModelComponent{model}
    );
    navigation.syncCharacterAgentControl(world, uncontrolledNpc);
    assert(!world.navAgents.contains(uncontrolledNpc));
    assert(!world.boxColliders.contains(uncontrolledNpc));
    assert(!world.rigidbodies.contains(uncontrolledNpc));
}

void testPhysicsDrivenAgentUsesVelocityIntentAndReachesDestination() {
    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime = makeOpenNavigationRuntime(navigation);
    core::PhysicsSystem physics{};
    core::World world{};
    const core::EntityId entity = addPhysicsAgent(world, glm::vec3(0.0f));
    assert(navigation.setAgentDestination(
        world,
        runtime,
        entity,
        glm::vec3(2.0f, 0.0f, 0.0f)
    ));

    const core::TimeContext time{0.0f, 0.1f};
    navigation.updateAgents(world, runtime, time);
    assert(glm::distance(
        world.transforms.get(entity).position,
        glm::vec3(0.0f)
    ) < 1.0e-6f);
    assert(world.navAgents.get(entity).desiredVelocity.x > 0.0f);

    physics.update(world, time, scheduler, false);
    navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    assert(world.transforms.get(entity).position.x > 0.0f);

    for (int step = 0; step < 20; ++step) {
        navigation.updateAgents(world, runtime, time);
        physics.update(world, time, scheduler, false);
        navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    }
    const core::NavAgentComponent& agent = world.navAgents.get(entity);
    assert(!agent.moving);
    assert(!agent.destination.has_value());
    assert(glm::distance(
        world.transforms.get(entity).position,
        glm::vec3(2.0f, 0.0f, 0.0f)
    ) <= agent.arrivalRadius);
    assert(glm::length(world.rigidbodies.get(entity).velocity) < 1.0e-6f);
}

void testLocalAvoidanceSteersHeadOnAgentsToOppositeSides() {
    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime = makeOpenNavigationRuntime(navigation);
    core::PhysicsSystem physics{};
    core::World world{};
    const core::EntityId left = addPhysicsAgent(
        world,
        glm::vec3(-1.0f, 0.0f, 0.0f)
    );
    const core::EntityId right = addPhysicsAgent(
        world,
        glm::vec3(1.0f, 0.0f, 0.0f)
    );
    assert(navigation.setAgentDestination(
        world,
        runtime,
        left,
        glm::vec3(4.0f, 0.0f, 0.0f)
    ));
    assert(navigation.setAgentDestination(
        world,
        runtime,
        right,
        glm::vec3(-4.0f, 0.0f, 0.0f)
    ));

    const core::TimeContext time{0.0f, 0.1f};
    navigation.updateAgents(world, runtime, time);
    const float leftLateral = world.navAgents.get(left).desiredVelocity.z;
    const float rightLateral = world.navAgents.get(right).desiredVelocity.z;
    assert(std::abs(leftLateral) > 0.01f);
    assert(std::abs(rightLateral) > 0.01f);
    assert(leftLateral * rightLateral < 0.0f);
    assert(navigationCounter(
        navigation,
        "Local Avoidance Adjustments"
    ) >= 2);

    physics.update(world, time, scheduler, false);
    navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    assert(world.transforms.get(left).position.z *
        world.transforms.get(right).position.z < 0.0f);

    for (int step = 0; step < 50; ++step) {
        navigation.updateAgents(world, runtime, time);
        physics.update(world, time, scheduler, false);
        navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    }
    assert(!world.navAgents.get(left).moving);
    assert(!world.navAgents.get(right).moving);
    assert(world.transforms.get(left).position.x > 3.8f);
    assert(world.transforms.get(right).position.x < -3.8f);
}

void testPhysicsRecoveryKeepsAgentOnNavmeshAndReplansInvalidPath() {
    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime = makeOpenNavigationRuntime(navigation);
    core::World world{};
    const core::EntityId entity = addPhysicsAgent(
        world,
        glm::vec3(9.6f, 0.0f, 0.0f)
    );
    core::NavAgentComponent& agent = world.navAgents.get(entity);
    agent.physicsStepStart = world.transforms.get(entity).position;
    agent.desiredVelocity = glm::vec3(2.0f, 0.0f, 0.0f);
    world.rigidbodies.get(entity).velocity = agent.desiredVelocity;
    world.transforms.get(entity).position.x = 9.9f;

    navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    assert(std::abs(world.transforms.get(entity).position.x - 9.6f) < 1.0e-6f);
    assert(glm::length(world.rigidbodies.get(entity).velocity) < 1.0e-6f);
    assert(navigationCounter(navigation, "Boundary Recoveries") == 1);

    world.transforms.get(entity).position = glm::vec3(0.0f);
    agent.physicsStepStart = glm::vec3(0.0f);
    agent.pathCorners = {glm::vec3(20.0f, 0.0f, 0.0f)};
    agent.destination = glm::vec3(5.0f, 0.0f, 0.0f);
    agent.moving = true;
    navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    assert(!agent.moving);
    assert(agent.pathCorners.empty());
    assert(navigationCounter(navigation, "Collision Replans") == 1);
    assert(navigationCounter(navigation, "Pending Path Requests") == 1);

    waitForPathRequests(navigation, world, runtime);
    assert(world.navAgents.get(entity).moving);
    assert(world.navAgents.get(entity).destination.has_value());
}

void testPhysicsDrivenAgentPreservesExplicitLinkTraversal() {
    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime{};
    runtime.asset.polygons = {
        core::NavPolygon{10, 0.0f, {
            glm::vec2(0.0f, 0.0f),
            glm::vec2(1.0f, 0.0f),
            glm::vec2(1.0f, 1.0f),
            glm::vec2(0.0f, 1.0f),
        }},
        core::NavPolygon{11, 3.0f, {
            glm::vec2(0.0f, 0.0f),
            glm::vec2(1.0f, 0.0f),
            glm::vec2(1.0f, 1.0f),
            glm::vec2(0.0f, 1.0f),
        }},
    };
    runtime.asset.links = {
        core::NavLink{
            3,
            10,
            11,
            glm::vec3(0.5f, 0.0f, 0.5f),
            glm::vec3(0.5f, 3.0f, 0.5f),
            true,
        },
    };
    assert(navigation.rebuildRuntime(runtime));

    core::PhysicsSystem physics{};
    core::World world{};
    const core::EntityId entity = addPhysicsAgent(
        world,
        glm::vec3(0.5f, 0.0f, 0.5f)
    );
    const glm::vec3 destination(0.5f, 3.0f, 0.5f);
    assert(navigation.setAgentDestination(
        world,
        runtime,
        entity,
        destination
    ));

    bool traversedLink = false;
    const core::TimeContext time{0.0f, 0.1f};
    for (int step = 0; step < 24; ++step) {
        navigation.updateAgents(world, runtime, time);
        traversedLink = traversedLink ||
            world.navAgents.get(entity).traversingLink;
        physics.update(world, time, scheduler, false);
        navigation.reconcileAgentsAfterPhysics(world, runtime, scheduler);
    }

    const core::NavAgentComponent& agent = world.navAgents.get(entity);
    assert(traversedLink);
    assert(!agent.moving);
    assert(!agent.destination.has_value());
    assert(glm::distance(
        world.transforms.get(entity).position,
        destination
    ) <= agent.arrivalRadius);
    assert(navigationCounter(navigation, "Boundary Recoveries") == 0);
}

void testFormationSlotsStayCenteredDistinctAndBounded() {
    const glm::vec3 center(4.0f, 2.0f, -3.0f);
    const std::vector<glm::vec3> destinations =
        core::makePartyFormationDestinations(
            center,
            glm::vec2(1.0f, 0.0f),
            3u,
            1.4f
        );
    assert(destinations.size() == 3u);

    glm::vec3 average(0.0f);
    for (const glm::vec3& destination : destinations) {
        average += destination;
        assert(destination.y == center.y);
    }
    average /= static_cast<float>(destinations.size());
    assert(glm::distance(average, center) < 1.0e-4f);
    assert(glm::distance(destinations[0], destinations[1]) > 0.5f);
    assert(glm::distance(destinations[1], destinations[2]) > 0.5f);
    assert(destinations[0].x > center.x);

    assert(core::makePartyFormationDestinations(
        center,
        glm::vec2(0.0f),
        core::kMaximumPartySize + 4u,
        1.0f
    ).size() == core::kMaximumPartySize);
}

void testAgentDestinationProjectionStaysOnTheNavmesh() {
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime = makeOpenNavigationRuntime(navigation);
    core::World world{};
    const core::EntityId agent = addPartyAgent(
        world,
        0u,
        glm::vec3(0.0f)
    );

    const std::optional<glm::vec3> projected = navigation.projectAgentDestination(
        world,
        runtime,
        agent,
        glm::vec3(15.0f, 0.0f, 2.0f)
    );
    assert(projected.has_value());
    assert(projected->x <= 10.0f);
    assert(projected->x >= 9.99f);
    assert(projected->z == 2.0f);
}

void testGroupMoveRequestsWithoutSimulationStepAndReplacesStaleOrders() {
    core::TaskScheduler scheduler(core::TaskSchedulerConfig{2u});
    core::NavigationSystem navigation{};
    core::NavigationRuntime runtime = makeOpenNavigationRuntime(navigation);
    core::World world{};
    const core::EntityId leader = addPartyAgent(
        world,
        0u,
        glm::vec3(-1.0f, 0.0f, 0.0f)
    );
    const core::EntityId scout = addPartyAgent(
        world,
        1u,
        glm::vec3(0.0f, 0.0f, 0.0f)
    );
    const core::EntityId guardian = addPartyAgent(
        world,
        2u,
        glm::vec3(1.0f, 0.0f, 0.0f)
    );
    const core::EntityId uncontrolled = world.createEntity();
    world.characters.emplace(uncontrolled, core::CharacterComponent{});
    world.characterControllers.emplace(
        uncontrolled,
        core::CharacterControllerComponent{}
    );
    world.transforms.emplace(uncontrolled, core::TransformComponent{});
    world.navAgents.emplace(uncontrolled, core::NavAgentComponent{});

    const std::vector<core::EntityId> selection{
        leader,
        scout,
        guardian,
        uncontrolled
    };
    core::PartyOrderSystem orders{};
    const core::PartyMoveOrderResult first = orders.issueMoveOrder(
        world,
        runtime,
        navigation,
        scheduler,
        selection,
        glm::vec3(8.0f, 0.0f, 0.0f)
    );
    assert(first.assignments.size() == 3u);
    assert(first.requestedCount == 3u);
    assert(navigationCounter(navigation, "Pending Path Requests") == 3);

    const core::PartyMoveOrderResult second = orders.issueMoveOrder(
        world,
        runtime,
        navigation,
        scheduler,
        selection,
        glm::vec3(-8.0f, 0.0f, 0.0f)
    );
    assert(second.assignments.size() == 3u);
    assert(second.requestedCount == 3u);
    assert(navigationCounter(navigation, "Pending Path Requests") == 3);
    assert(navigationCounter(navigation, "Stale Path Results") == 3);

    waitForPathRequests(navigation, world, runtime);
    for (const core::PartyMoveAssignment& assignment : second.assignments) {
        assert(assignment.navigationDestination.has_value());
        const core::NavAgentComponent& agent = world.navAgents.get(assignment.entity);
        assert(agent.destination.has_value());
        assert(glm::distance(
            *agent.destination,
            *assignment.navigationDestination
        ) < 1.0e-4f);
    }
}

}  // namespace

int main() {
    testControllableAgentGetsDefaultPhysicsAndColliderClearance();
    testPhysicsDrivenAgentUsesVelocityIntentAndReachesDestination();
    testLocalAvoidanceSteersHeadOnAgentsToOppositeSides();
    testPhysicsRecoveryKeepsAgentOnNavmeshAndReplansInvalidPath();
    testPhysicsDrivenAgentPreservesExplicitLinkTraversal();
    testFormationSlotsStayCenteredDistinctAndBounded();
    testAgentDestinationProjectionStaysOnTheNavmesh();
    testGroupMoveRequestsWithoutSimulationStepAndReplacesStaleOrders();
    return EXIT_SUCCESS;
}
