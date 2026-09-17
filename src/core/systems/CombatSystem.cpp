#include "CombatSystem.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "core/simulation/CombatSimulation.hpp"
#include "render/resources/StaticGltfModel.hpp"

namespace core {
namespace {

std::string lowercase(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

int findAnimationClip(
    const render::GltfModelData& model,
    std::string_view requestedName
) {
    if (requestedName.empty()) {
        return -1;
    }
    for (std::size_t index = 0u; index < model.animations.size(); ++index) {
        if (model.animations[index].name == requestedName) {
            return static_cast<int>(index);
        }
    }

    const std::string requestedLower = lowercase(requestedName);
    for (std::size_t index = 0u; index < model.animations.size(); ++index) {
        if (lowercase(model.animations[index].name).find(requestedLower) !=
            std::string::npos) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int resolveAnimationClip(
    const World& world,
    EntityId entity,
    const CombatantComponent& combatant,
    const AnimatedModelComponent& animation,
    bool stateEntered
) {
    if (!animation.model) {
        return -1;
    }

    int clip = findAnimationClip(
        *animation.model,
        combatAnimationClip(combatant)
    );
    const LocomotionComponent* locomotion = world.locomotion.tryGet(entity);
    if (clip < 0 && locomotion != nullptr) {
        if (combatant.state == CombatState::Fleeing) {
            clip = locomotion->walkClip;
        } else if (combatStateLocksMovement(combatant.state) ||
                   stateEntered) {
            clip = locomotion->idleClip;
        }
    }
    return clip;
}

void requestAnimation(
    AnimatedModelComponent& animation,
    int clip,
    bool restart
) {
    if (clip < 0) {
        return;
    }

    if (restart) {
        if (animation.nextClip == clip) {
            animation.nextTime = 0.0f;
        } else if (animation.currentClip == clip) {
            animation.currentTime = 0.0f;
        }
        animation.requestedClip = clip;
        return;
    }

    const int currentOrNext = animation.nextClip >= 0
        ? animation.nextClip
        : animation.currentClip;
    if (currentOrNext != clip) {
        animation.requestedClip = clip;
    } else if (animation.requestedClip != clip) {
        // Cancel a lower-priority locomotion request left earlier in this tick.
        animation.requestedClip = -1;
    }
}

bool observeStateTransition(
    CombatantComponent& combatant,
    float deltaSeconds
) {
    if (combatant.observedState == combatant.state) {
        combatant.stateElapsedSeconds += std::max(deltaSeconds, 0.0f);
        return false;
    }

    const CombatState previousState = combatant.observedState;
    if (combatStateIsTransient(combatant.state) &&
        combatStateCanResume(previousState)) {
        combatant.resumeState = previousState;
    } else if (combatStateCanResume(combatant.state)) {
        combatant.resumeState = combatant.state;
    }
    combatant.observedState = combatant.state;
    combatant.stateElapsedSeconds = 0.0f;
    return true;
}

bool transientAnimationWillComplete(
    const CombatantComponent& combatant,
    const AnimatedModelComponent& animation,
    int clip,
    float deltaSeconds
) {
    if (!combatStateIsTransient(combatant.state)) {
        return false;
    }
    if (!animation.model || clip < 0 ||
        clip >= static_cast<int>(animation.model->animations.size())) {
        return true;
    }

    float clipTime = -1.0f;
    if (animation.nextClip == clip) {
        clipTime = animation.nextTime;
    } else if (animation.currentClip == clip) {
        clipTime = animation.currentTime;
    }
    if (clipTime < 0.0f) {
        return false;
    }

    const float duration = animation.model->animations[
        static_cast<std::size_t>(clip)
    ].duration;
    const float nextTime = clipTime +
        std::max(deltaSeconds, 0.0f) * std::max(animation.speed, 0.0f);
    return duration <= 1.0e-5f || nextTime >= duration - 1.0e-5f;
}

CombatState stateAfterTransient(const CombatantComponent& combatant) {
    return combatStateCanResume(combatant.resumeState)
        ? combatant.resumeState
        : CombatState::Idle;
}

void stopMovement(
    World& world,
    EntityId entity,
    bool discardDestination
) {
    if (NavAgentComponent* agent = world.navAgents.tryGet(entity)) {
        agent->desiredVelocity = glm::vec3(0.0f);
        if (discardDestination) {
            agent->moving = false;
            agent->pathCorners.clear();
            agent->destination.reset();
            agent->traversingLink = false;
        }
    }
    if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity)) {
        rigidbody->velocity.x = 0.0f;
        rigidbody->velocity.z = 0.0f;
    }
}

}  // namespace

void CombatSystem::update(World& world, const TimeContext& time) const {
    for (EntityId entity : world.combatants.entities()) {
        CombatantComponent& combatant = world.combatants.get(entity);
        bool stateEntered = observeStateTransition(
            combatant,
            time.deltaSeconds
        );

        const CharacterVitalsComponent* vitals = world.characterVitals.tryGet(entity);
        if (vitals != nullptr && vitals->currentHitPoints <= 0 &&
            combatant.state != CombatState::Dead) {
            setCombatState(combatant, CombatState::Downed);
            stateEntered = observeStateTransition(combatant, 0.0f);
        }

        AnimatedModelComponent* animation = world.animatedModels.tryGet(entity);
        int animationClip = animation != nullptr
            ? resolveAnimationClip(
                world,
                entity,
                combatant,
                *animation,
                stateEntered
            )
            : -1;
        if (animation == nullptr && combatStateIsTransient(combatant.state)) {
            setCombatState(combatant, stateAfterTransient(combatant));
            stateEntered = observeStateTransition(combatant, 0.0f);
        } else if (animation != nullptr &&
                   combatStateIsTransient(combatant.state)) {
            if (!combatant.animationLoopOverrideActive) {
                combatant.animationLoopBeforeOverride = animation->loop;
                combatant.animationLoopOverrideActive = true;
            }
            animation->loop = false;
            if (transientAnimationWillComplete(
                    combatant,
                    *animation,
                    animationClip,
                    time.deltaSeconds)) {
                setCombatState(combatant, stateAfterTransient(combatant));
                stateEntered = observeStateTransition(combatant, 0.0f);
                animationClip = resolveAnimationClip(
                    world,
                    entity,
                    combatant,
                    *animation,
                    stateEntered
                );
            }
        }

        if (combatStateLocksMovement(combatant.state)) {
            stopMovement(
                world,
                entity,
                combatant.state == CombatState::Downed ||
                    combatant.state == CombatState::Dead
            );
        }

        if (animation != nullptr) {
            if (combatant.state == CombatState::Downed ||
                combatant.state == CombatState::Dead) {
                if (!combatant.animationLoopOverrideActive) {
                    combatant.animationLoopBeforeOverride = animation->loop;
                    combatant.animationLoopOverrideActive = true;
                }
                animation->loop = false;
            }
            requestAnimation(*animation, animationClip, stateEntered);
            if (!combatStateIsTransient(combatant.state) &&
                combatant.state != CombatState::Downed &&
                combatant.state != CombatState::Dead &&
                combatant.animationLoopOverrideActive &&
                animation->nextClip < 0 &&
                animation->requestedClip < 0) {
                animation->loop = combatant.animationLoopBeforeOverride;
                combatant.animationLoopOverrideActive = false;
            }
        }
    }
}

}  // namespace core
