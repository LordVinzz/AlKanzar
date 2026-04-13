#include "PhysicsSystem.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>

#include "core/transform/TransformMath.hpp"

namespace core {

namespace {

constexpr float kGravity = 9.81f;
constexpr float kMinimumMass = 0.0001f;
constexpr float kContactSlop = 0.0005f;

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(8u, (count + lanes - 1u) / lanes);
}

render::Bounds3 centeredBounds(const glm::vec3& center, const glm::vec3& halfExtents) {
    return render::Bounds3{
        center - halfExtents,
        center + halfExtents,
    };
}

float maxAxisScale(const glm::mat4& modelMatrix) {
    const float xScale = glm::length(glm::vec3(modelMatrix[0]));
    const float yScale = glm::length(glm::vec3(modelMatrix[1]));
    const float zScale = glm::length(glm::vec3(modelMatrix[2]));
    return std::max(xScale, std::max(yScale, zScale));
}

enum class BodyShape {
    Box = 0,
    Sphere,
};

struct BodyState {
    EntityId entity{};
    TransformComponent transform{};
    RigidbodyComponent rigidbody{};
    BodyShape shape{BodyShape::Box};
    BoxColliderComponent box{};
    SphereColliderComponent sphere{};
    glm::mat4 worldMatrix{1.0f};
    render::Bounds3 worldBounds{};
    OrientedBox obb{};
    glm::vec3 sphereCenter{0.0f};
    float sphereRadius{0.0f};
    float inverseMass{0.0f};
    bool kinematic{false};
};

struct Contact {
    std::size_t a{0u};
    std::size_t b{0u};
    glm::vec3 normal{1.0f, 0.0f, 0.0f};
    float penetration{0.0f};
};

void updateDerivedState(BodyState& body) {
    body.worldMatrix = composeTransform(body.transform);
    if (body.shape == BodyShape::Box) {
        body.obb = makeOrientedBox(body.worldMatrix, body.box);
        body.worldBounds = body.obb.aabb;
        body.sphereCenter = glm::vec3(0.0f);
        body.sphereRadius = 0.0f;
    } else {
        body.sphereCenter = glm::vec3(body.worldMatrix * glm::vec4(body.sphere.center, 1.0f));
        body.sphereRadius = body.sphere.radius * std::max(maxAxisScale(body.worldMatrix), 0.001f);
        body.worldBounds = centeredBounds(body.sphereCenter, glm::vec3(body.sphereRadius));
        body.obb = OrientedBox{};
    }
}

bool boundsOverlap(const render::Bounds3& a, const render::Bounds3& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
        a.min.y <= b.max.y && a.max.y >= b.min.y &&
        a.min.z <= b.max.z && a.max.z >= b.min.z;
}

glm::vec3 orientedBoxClosestPoint(const OrientedBox& box, const glm::vec3& point) {
    glm::vec3 closest = box.center;
    const glm::vec3 delta = point - box.center;
    for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
        const float distance = glm::dot(delta, box.axes[axisIndex]);
        const float clamped = std::clamp(distance, -box.halfExtents[axisIndex], box.halfExtents[axisIndex]);
        closest += box.axes[axisIndex] * clamped;
    }
    return closest;
}

bool computeBoxBoxContact(const BodyState& a, const BodyState& b, Contact& outContact) {
    if (!boundsOverlap(a.worldBounds, b.worldBounds)) {
        return false;
    }

    constexpr float kSatEpsilon = 1.0e-6f;
    const OrientedBox& boxA = a.obb;
    const OrientedBox& boxB = b.obb;
    const glm::vec3 centerDelta = boxB.center - boxA.center;

    float rotation[3][3]{};
    float absRotation[3][3]{};
    glm::vec3 translation(
        glm::dot(centerDelta, boxA.axes[0]),
        glm::dot(centerDelta, boxA.axes[1]),
        glm::dot(centerDelta, boxA.axes[2])
    );

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = glm::dot(boxA.axes[i], boxB.axes[j]);
            absRotation[i][j] = std::abs(rotation[i][j]) + kSatEpsilon;
        }
    }

    float bestOverlap = std::numeric_limits<float>::max();
    glm::vec3 bestAxis(1.0f, 0.0f, 0.0f);
    const auto considerAxis = [&](const glm::vec3& axis, float overlap, float signedDistance) {
        if (overlap >= bestOverlap) {
            return;
        }
        glm::vec3 normal = axis;
        if (glm::dot(normal, normal) <= kSatEpsilon) {
            return;
        }
        normal = glm::normalize(normal);
        if (signedDistance < 0.0f) {
            normal = -normal;
        } else if (std::abs(signedDistance) <= kSatEpsilon && glm::dot(normal, centerDelta) < 0.0f) {
            normal = -normal;
        }
        bestOverlap = overlap;
        bestAxis = normal;
    };

    for (int i = 0; i < 3; ++i) {
        const float ra = boxA.halfExtents[i];
        const float rb =
            boxB.halfExtents.x * absRotation[i][0] +
            boxB.halfExtents.y * absRotation[i][1] +
            boxB.halfExtents.z * absRotation[i][2];
        const float overlap = ra + rb - std::abs(translation[i]);
        if (overlap <= 0.0f) {
            return false;
        }
        considerAxis(boxA.axes[i], overlap, translation[i]);
    }

    for (int j = 0; j < 3; ++j) {
        const float distance =
            translation.x * rotation[0][j] +
            translation.y * rotation[1][j] +
            translation.z * rotation[2][j];
        const float ra =
            boxA.halfExtents.x * absRotation[0][j] +
            boxA.halfExtents.y * absRotation[1][j] +
            boxA.halfExtents.z * absRotation[2][j];
        const float rb = boxB.halfExtents[j];
        const float overlap = ra + rb - std::abs(distance);
        if (overlap <= 0.0f) {
            return false;
        }
        considerAxis(boxB.axes[j], overlap, distance);
    }

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int nextI = (i + 1) % 3;
            const int nextNextI = (i + 2) % 3;
            const int nextJ = (j + 1) % 3;
            const int nextNextJ = (j + 2) % 3;
            const float ra =
                boxA.halfExtents[nextI] * absRotation[nextNextI][j] +
                boxA.halfExtents[nextNextI] * absRotation[nextI][j];
            const float rb =
                boxB.halfExtents[nextJ] * absRotation[i][nextNextJ] +
                boxB.halfExtents[nextNextJ] * absRotation[i][nextJ];
            const float distance = std::abs(
                translation[nextNextI] * rotation[nextI][j] -
                translation[nextI] * rotation[nextNextI][j]
            );
            const float overlap = ra + rb - distance;
            if (overlap <= 0.0f) {
                return false;
            }
            const glm::vec3 axis = glm::cross(boxA.axes[i], boxB.axes[j]);
            considerAxis(axis, overlap, glm::dot(axis, centerDelta));
        }
    }

    outContact.normal = bestAxis;
    outContact.penetration = bestOverlap;
    return outContact.penetration > 0.0f;
}

bool computeSphereSphereContact(const BodyState& a, const BodyState& b, Contact& outContact) {
    const glm::vec3 delta = b.sphereCenter - a.sphereCenter;
    const float distance = glm::length(delta);
    const float combinedRadius = a.sphereRadius + b.sphereRadius;
    if (distance >= combinedRadius) {
        return false;
    }

    outContact.normal = distance > 1.0e-5f ? delta / distance : glm::vec3(1.0f, 0.0f, 0.0f);
    outContact.penetration = combinedRadius - distance;
    return outContact.penetration > 0.0f;
}

bool computeBoxSphereContact(const BodyState& boxBody, const BodyState& sphereBody, Contact& outContact) {
    if (!boundsOverlap(boxBody.worldBounds, sphereBody.worldBounds)) {
        return false;
    }

    const glm::vec3 clamped = orientedBoxClosestPoint(boxBody.obb, sphereBody.sphereCenter);
    const glm::vec3 delta = sphereBody.sphereCenter - clamped;
    const float distance = glm::length(delta);
    if (distance > sphereBody.sphereRadius) {
        return false;
    }

    if (distance > 1.0e-5f) {
        outContact.normal = delta / distance;
        outContact.penetration = sphereBody.sphereRadius - distance;
        return outContact.penetration > 0.0f;
    }

    const glm::vec3 localDelta(
        glm::dot(sphereBody.sphereCenter - boxBody.obb.center, boxBody.obb.axes[0]),
        glm::dot(sphereBody.sphereCenter - boxBody.obb.center, boxBody.obb.axes[1]),
        glm::dot(sphereBody.sphereCenter - boxBody.obb.center, boxBody.obb.axes[2])
    );
    outContact.penetration = std::numeric_limits<float>::max();
    for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
        const float toNegative = sphereBody.sphereRadius + (localDelta[axisIndex] + boxBody.obb.halfExtents[axisIndex]);
        if (toNegative < outContact.penetration) {
            outContact.penetration = toNegative;
            outContact.normal = -boxBody.obb.axes[axisIndex];
        }

        const float toPositive = sphereBody.sphereRadius + (boxBody.obb.halfExtents[axisIndex] - localDelta[axisIndex]);
        if (toPositive < outContact.penetration) {
            outContact.penetration = toPositive;
            outContact.normal = boxBody.obb.axes[axisIndex];
        }
    }
    return outContact.penetration > 0.0f;
}

bool computeContact(const BodyState& a, const BodyState& b, Contact& outContact) {
    if (!boundsOverlap(a.worldBounds, b.worldBounds)) {
        return false;
    }
    if (a.shape == BodyShape::Box && b.shape == BodyShape::Box) {
        return computeBoxBoxContact(a, b, outContact);
    }
    if (a.shape == BodyShape::Sphere && b.shape == BodyShape::Sphere) {
        return computeSphereSphereContact(a, b, outContact);
    }
    if (a.shape == BodyShape::Box && b.shape == BodyShape::Sphere) {
        return computeBoxSphereContact(a, b, outContact);
    }
    if (a.shape == BodyShape::Sphere && b.shape == BodyShape::Box) {
        const bool hit = computeBoxSphereContact(b, a, outContact);
        outContact.normal = -outContact.normal;
        return hit;
    }
    return false;
}

}  // namespace

void PhysicsSystem::update(
    World& world,
    const TimeContext& timeContext,
    TaskScheduler& scheduler,
    bool useParallel
) const {
    std::vector<BodyState> bodies{};
    bodies.reserve(world.rigidbodies.size());
    for (EntityId entity : world.rigidbodies.entities()) {
        const TransformComponent* transform = world.transforms.tryGet(entity);
        if (transform == nullptr) {
            continue;
        }
        const BoxColliderComponent* box = world.boxColliders.tryGet(entity);
        const SphereColliderComponent* sphere = world.sphereColliders.tryGet(entity);
        if (box == nullptr && sphere == nullptr) {
            continue;
        }

        BodyState body{};
        body.entity = entity;
        body.transform = *transform;
        body.rigidbody = world.rigidbodies.get(entity);
        body.kinematic = body.rigidbody.isKinematic;
        body.inverseMass = body.kinematic ? 0.0f : 1.0f / std::max(body.rigidbody.mass, kMinimumMass);
        if (box != nullptr) {
            body.shape = BodyShape::Box;
            body.box = *box;
        } else {
            body.shape = BodyShape::Sphere;
            body.sphere = *sphere;
        }

        if (!body.kinematic) {
            if (body.rigidbody.useGravity) {
                body.rigidbody.velocity.y -= kGravity * timeContext.deltaSeconds;
            }
            const float linearDamping = std::clamp(1.0f - body.rigidbody.linearDamping * timeContext.deltaSeconds, 0.0f, 1.0f);
            const float angularDamping = std::clamp(1.0f - body.rigidbody.angularDamping * timeContext.deltaSeconds, 0.0f, 1.0f);
            body.rigidbody.velocity *= linearDamping;
            body.rigidbody.angularVelocity *= angularDamping;
            body.transform.position += body.rigidbody.velocity * timeContext.deltaSeconds;
        }

        updateDerivedState(body);
        bodies.push_back(std::move(body));
    }

    if (bodies.size() < 2u) {
        for (BodyState& body : bodies) {
            if (TransformComponent* transform = world.transforms.tryGet(body.entity)) {
                *transform = body.transform;
                world.markTransformsDirty(body.entity);
            }
            if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(body.entity)) {
                rigidbody->velocity = body.rigidbody.velocity;
                rigidbody->angularVelocity = body.rigidbody.angularVelocity;
            }
        }
        return;
    }

    std::vector<Contact> contacts{};
    if (!useParallel) {
        for (std::size_t a = 0u; a < bodies.size(); ++a) {
            for (std::size_t b = a + 1u; b < bodies.size(); ++b) {
                Contact contact{};
                contact.a = a;
                contact.b = b;
                if (computeContact(bodies[a], bodies[b], contact)) {
                    contacts.push_back(contact);
                }
            }
        }
    } else {
        const std::size_t grain = taskGrain(bodies.size(), scheduler.workerCount());
        const std::size_t chunkCount = (bodies.size() + grain - 1u) / grain;
        std::vector<std::vector<Contact>> chunkContacts(chunkCount);
        TaskGroup group;
        for (std::size_t chunkIndex = 0u; chunkIndex < chunkCount; ++chunkIndex) {
            const std::size_t begin = chunkIndex * grain;
            const std::size_t end = std::min(bodies.size(), begin + grain);
            scheduler.schedule(group, "Physics Contact Generation", [&, begin, end, chunkIndex]() {
                std::vector<Contact>& localContacts = chunkContacts[chunkIndex];
                for (std::size_t a = begin; a < end; ++a) {
                    for (std::size_t b = a + 1u; b < bodies.size(); ++b) {
                        Contact contact{};
                        contact.a = a;
                        contact.b = b;
                        if (computeContact(bodies[a], bodies[b], contact)) {
                            localContacts.push_back(contact);
                        }
                    }
                }
            });
        }
        scheduler.wait(group);
        for (std::vector<Contact>& localContacts : chunkContacts) {
            contacts.insert(contacts.end(), localContacts.begin(), localContacts.end());
        }
    }

    std::sort(contacts.begin(), contacts.end(), [](const Contact& lhs, const Contact& rhs) {
        if (lhs.a != rhs.a) {
            return lhs.a < rhs.a;
        }
        return lhs.b < rhs.b;
    });

    for (const Contact& contact : contacts) {
        BodyState& a = bodies[contact.a];
        BodyState& b = bodies[contact.b];
        const float totalInverseMass = a.inverseMass + b.inverseMass;
        if (totalInverseMass <= 0.0f) {
            continue;
        }

        const float correctionDistance = std::max(contact.penetration + kContactSlop, 0.0f);
        const glm::vec3 correction = contact.normal * (correctionDistance / totalInverseMass);
        if (!a.kinematic) {
            a.transform.position -= correction * a.inverseMass;
            updateDerivedState(a);
        }
        if (!b.kinematic) {
            b.transform.position += correction * b.inverseMass;
            updateDerivedState(b);
        }

        const float relativeVelocity = glm::dot(b.rigidbody.velocity - a.rigidbody.velocity, contact.normal);
        if (relativeVelocity >= 0.0f) {
            continue;
        }

        const float impulseMagnitude = -relativeVelocity / totalInverseMass;
        if (!a.kinematic) {
            a.rigidbody.velocity -= contact.normal * (impulseMagnitude * a.inverseMass);
        }
        if (!b.kinematic) {
            b.rigidbody.velocity += contact.normal * (impulseMagnitude * b.inverseMass);
        }
    }

    for (BodyState& body : bodies) {
        if (TransformComponent* transform = world.transforms.tryGet(body.entity)) {
            *transform = body.transform;
            world.markTransformsDirty(body.entity);
        }
        if (RigidbodyComponent* rigidbody = world.rigidbodies.tryGet(body.entity)) {
            rigidbody->velocity = body.rigidbody.velocity;
            rigidbody->angularVelocity = body.rigidbody.angularVelocity;
        }
    }
}

}  // namespace core
