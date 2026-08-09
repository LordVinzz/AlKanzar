#include "ComponentRegistry.hpp"
#include "ComponentInspector.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include "core/ecs/World.hpp"
#include "core/app/EngineServices.hpp"

namespace core {

ComponentRegistry::ComponentRegistry() {
    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Transform,
        "Transform",
        "Core",
        [](const World& world, EntityId entity) {
            return world.transforms.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.transforms.emplace(entity, TransformComponent{});
        },
        [](World& world, EntityId entity) {
            world.transforms.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            TransformComponent* transform = services.world.transforms.tryGet(entity);
            if (transform == nullptr) {
                return false;
            }
            bool changed = false;
            editComponentSnapshot<TransformComponent>(
                "Position",
                "Move Entity",
                "transform-position-" + std::to_string(entity.index),
                *transform,
                [&services, entity](const TransformComponent& snapshot) {
                    if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                        *target = snapshot;
                        notifyTransformChanged(services, entity);
                    }
                },
                [](TransformComponent& edited) {
                    return ImGui::DragFloat3("Position", glm::value_ptr(edited.position), 0.05f);
                },
                services.commands
            );
            editComponentSnapshot<TransformComponent>(
                "Rotation",
                "Rotate Entity",
                "transform-rotation-" + std::to_string(entity.index),
                *transform,
                [&services, entity](const TransformComponent& snapshot) {
                    if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                        *target = snapshot;
                        notifyTransformChanged(services, entity);
                    }
                },
                [](TransformComponent& edited) {
                    return ImGui::DragFloat3("Rotation", glm::value_ptr(edited.rotationDeg), 0.5f);
                },
                services.commands
            );
            editComponentSnapshot<TransformComponent>(
                "Scale",
                "Scale Entity",
                "transform-scale-" + std::to_string(entity.index),
                *transform,
                [&services, entity](const TransformComponent& snapshot) {
                    if (TransformComponent* target = services.world.transforms.tryGet(entity)) {
                        *target = snapshot;
                        target->scale = glm::max(target->scale, glm::vec3(0.01f));
                        notifyTransformChanged(services, entity);
                    }
                },
                [](TransformComponent& edited) {
                    const bool localChanged = ImGui::DragFloat3("Scale", glm::value_ptr(edited.scale), 0.02f);
                    if (localChanged) {
                        edited.scale = glm::max(edited.scale, glm::vec3(0.01f));
                    }
                    return localChanged;
                },
                services.commands
            );
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Visibility,
        "Visibility",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.visibilities.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.visibilities.emplace(entity, VisibilityComponent{true});
        },
        [](World& world, EntityId entity) {
            world.visibilities.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            VisibilityComponent* vis = services.world.visibilities.tryGet(entity);
            if (vis == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::Checkbox("Visible", &vis->visible);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Renderable,
        "Renderable",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.renderables.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.renderables.emplace(entity, RenderableComponent{});
        },
        [](World& world, EntityId entity) {
            world.renderables.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            RenderableComponent* renderable = services.world.renderables.tryGet(entity);
            if (renderable == nullptr) {
                return false;
            }
            bool changed = false;
            ImGui::Text("Mesh Handle: %zu", renderable->mesh.value);
            int layer = static_cast<int>(renderable->layer);
            if (ImGui::Combo("Render Layer", &layer, "Ground\0Geometry\0Actors\0")) {
                renderable->layer = static_cast<render::RenderLayer>(layer);
                changed = true;
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Material,
        "Material",
        "Rendering",
        [](const World& world, EntityId entity) {
            return world.materials.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.materials.emplace(entity, MaterialComponent{std::make_shared<render::Material>()});
        },
        [](World& world, EntityId entity) {
            world.materials.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            drawMaterialInspector(services, entity);
            return true;
        },
    });

    registerDirectionalLightDescriptor();

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::LightVolume,
        "Light Volume",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.lightVolumes.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.lightVolumes.emplace(entity, LightVolumeComponent{});
        },
        [](World& world, EntityId entity) {
            world.lightVolumes.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            LightVolumeComponent* volume = services.world.lightVolumes.tryGet(entity);
            if (volume == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Half Extents", glm::value_ptr(volume->halfExtents), 0.05f, 0.01f, 1000.0f);
            if (changed) {
                volume->halfExtents = glm::max(volume->halfExtents, glm::vec3(0.01f));
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::PointLight,
        "Point Light",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.pointLights.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.pointLights.emplace(entity, PointLightComponent{});
        },
        [](World& world, EntityId entity) {
            world.pointLights.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            PointLightComponent* light = services.world.pointLights.tryGet(entity);
            if (light == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Radius", &light->radius, 0.1f, 0.1f, 128.0f);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            changed |= ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 128.0f);
            changed |= ImGui::Checkbox("Movable", &light->isMovable);
            changed |= ImGui::Checkbox("Casts Shadow", &light->castsShadow);
            if (changed) {
                notifyLightChanged(services, entity);
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::SpotLight,
        "Spot Light",
        "Lighting",
        [](const World& world, EntityId entity) {
            return world.spotLights.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.spotLights.emplace(entity, SpotLightComponent{});
        },
        [](World& world, EntityId entity) {
            world.spotLights.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            SpotLightComponent* light = services.world.spotLights.tryGet(entity);
            if (light == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Radius", &light->radius, 0.1f, 0.1f, 128.0f);
            changed |= ImGui::ColorEdit3("Color", glm::value_ptr(light->color));
            changed |= ImGui::DragFloat("Intensity", &light->intensity, 0.1f, 0.0f, 128.0f);
            changed |= ImGui::DragFloat3("Target", glm::value_ptr(light->target), 0.05f);
            changed |= ImGui::DragFloat("Inner Angle", &light->innerAngle, 0.25f, 1.0f, light->outerAngle);
            changed |= ImGui::DragFloat("Outer Angle", &light->outerAngle, 0.25f, light->innerAngle, 80.0f);
            changed |= ImGui::Checkbox("Movable", &light->isMovable);
            changed |= ImGui::Checkbox("Casts Shadow", &light->castsShadow);
            if (changed) {
                notifyLightChanged(services, entity);
            }
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::BoxCollider,
        "Box Collider",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.boxColliders.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.boxColliders.emplace(entity, BoxColliderComponent{});
        },
        [](World& world, EntityId entity) {
            world.boxColliders.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            BoxColliderComponent* collider = services.world.boxColliders.tryGet(entity);
            if (collider == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Center", glm::value_ptr(collider->center), 0.01f);
            changed |= ImGui::DragFloat3("Half Extents", glm::value_ptr(collider->halfExtents), 0.01f, 0.001f, 1000.0f);
            changed |= ImGui::Checkbox("Is Trigger", &collider->isTrigger);
            changed |= ImGui::Checkbox("Rotate With Entity", &collider->rotatesWithEntity);
            changed |= ImGui::Checkbox("Show Collider", &collider->showDebug);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::SphereCollider,
        "Sphere Collider",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.sphereColliders.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.sphereColliders.emplace(entity, SphereColliderComponent{});
        },
        [](World& world, EntityId entity) {
            world.sphereColliders.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            SphereColliderComponent* collider = services.world.sphereColliders.tryGet(entity);
            if (collider == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat3("Center", glm::value_ptr(collider->center), 0.01f);
            changed |= ImGui::DragFloat("Radius", &collider->radius, 0.01f, 0.001f, 1000.0f);
            changed |= ImGui::Checkbox("Is Trigger", &collider->isTrigger);
            changed |= ImGui::Checkbox("Show Collider", &collider->showDebug);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Rigidbody,
        "Rigidbody",
        "Physics",
        [](const World& world, EntityId entity) {
            return world.rigidbodies.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.rigidbodies.emplace(entity, RigidbodyComponent{});
        },
        [](World& world, EntityId entity) {
            world.rigidbodies.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            RigidbodyComponent* rb = services.world.rigidbodies.tryGet(entity);
            if (rb == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::DragFloat("Mass", &rb->mass, 0.1f, 0.001f, 10000.0f);
            changed |= ImGui::DragFloat("Linear Damping", &rb->linearDamping, 0.01f, 0.0f, 1.0f);
            changed |= ImGui::DragFloat("Angular Damping", &rb->angularDamping, 0.01f, 0.0f, 1.0f);
            changed |= ImGui::Checkbox("Is Kinematic", &rb->isKinematic);
            changed |= ImGui::Checkbox("Use Gravity", &rb->useGravity);
            ImGui::Separator();
            ImGui::Text("Velocity: %.2f %.2f %.2f", rb->velocity.x, rb->velocity.y, rb->velocity.z);
            ImGui::Text("Angular Vel: %.2f %.2f %.2f", rb->angularVelocity.x, rb->angularVelocity.y, rb->angularVelocity.z);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::NavAgent,
        "Nav Agent",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.navAgents.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.navAgents.emplace(entity, NavAgentComponent{});
        },
        [](World& world, EntityId entity) {
            if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(entity)) {
                rigidbody->velocity = glm::vec3(0.0f);
            }
            world.navAgents.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            NavAgentComponent* agent = services.world.navAgents.tryGet(entity);
            if (agent == nullptr) {
                return false;
            }
            bool changed = false;
            const bool ownsSphere = services.world.sphereColliders.contains(entity);
            const bool ownsBox = services.world.boxColliders.contains(entity);
            changed |= ImGui::DragFloat("Move Speed", &agent->moveSpeed, 0.05f, 0.0f, 50.0f);
            changed |= ImGui::DragFloat("Turn Speed (deg/s)", &agent->turnSpeedDeg, 1.0f, 0.0f, 1080.0f);
            changed |= ImGui::DragFloat("Arrival Radius", &agent->arrivalRadius, 0.01f, 0.01f, 10.0f);
            int clearanceSource = static_cast<int>(agent->clearanceSource);
            const char* clearanceSources[] = {"None", "Auto", "Sphere", "Box"};
            if (ImGui::Combo("Clearance Collider", &clearanceSource, clearanceSources, 4)) {
                agent->clearanceSource = static_cast<NavAgentClearanceSource>(clearanceSource);
                changed = true;
            }
            ImGui::Text("Owned Colliders: sphere %s, box %s", ownsSphere ? "yes" : "no", ownsBox ? "yes" : "no");
            ImGui::Separator();
            ImGui::Text("Moving: %s", agent->moving ? "yes" : "no");
            if (agent->destination.has_value()) {
                const glm::vec3& dest = *agent->destination;
                ImGui::Text("Destination: %.2f %.2f %.2f", dest.x, dest.y, dest.z);
            } else {
                ImGui::TextUnformatted("Destination: none");
            }
            ImGui::Text("Path Corners: %d", static_cast<int>(agent->pathCorners.size()));
            ImGui::Text(
                "Desired Velocity: %.2f %.2f %.2f",
                agent->desiredVelocity.x,
                agent->desiredVelocity.y,
                agent->desiredVelocity.z
            );
            ImGui::Text("Traversing Link: %s", agent->traversingLink ? "yes" : "no");
            ImGui::Text(
                "Recovery Replan: %s (attempt %u)",
                agent->recoveryReplanActive ? "yes" : "no",
                static_cast<unsigned int>(agent->recoveryReplanAttempts)
            );
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::Locomotion,
        "Locomotion",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.locomotion.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.locomotion.emplace(entity, LocomotionComponent{});
        },
        [](World& world, EntityId entity) {
            world.locomotion.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            LocomotionComponent* loco = services.world.locomotion.tryGet(entity);
            if (loco == nullptr) {
                return false;
            }
            bool changed = false;
            changed |= ImGui::InputInt("Idle Clip", &loco->idleClip);
            changed |= ImGui::InputInt("Walk Clip", &loco->walkClip);
            return changed;
        },
    });

    descriptors_.push_back(ComponentDescriptor{
        ComponentKind::NavSource,
        "Nav Source",
        "Navigation",
        [](const World& world, EntityId entity) {
            return world.navSources.contains(entity);
        },
        [](World& world, EntityId entity) {
            world.navSources.emplace(entity, NavSourceComponent{});
        },
        [](World& world, EntityId entity) {
            world.navSources.remove(entity);
        },
        [](EngineServices& services, EntityId entity) -> bool {
            NavSourceComponent* nav = services.world.navSources.tryGet(entity);
            if (nav == nullptr) {
                return false;
            }
            bool changed = false;
            const char* tagNames[] = {"Walkable", "Blocking", "Ignored"};
            int defaultTag = static_cast<int>(nav->defaultTag);
            if (ImGui::Combo("Default Tag", &defaultTag, tagNames, 3)) {
                nav->defaultTag = static_cast<NavSourceTag>(defaultTag);
                changed = true;
            }
            int effectiveTag = static_cast<int>(nav->effectiveTag);
            if (ImGui::Combo("Effective Tag", &effectiveTag, tagNames, 3)) {
                nav->effectiveTag = static_cast<NavSourceTag>(effectiveTag);
                changed = true;
            }
            return changed;
        },
    });

    registerCharacterDescriptor();
}

}  // namespace core
