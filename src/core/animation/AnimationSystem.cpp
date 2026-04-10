#include "AnimationSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/quaternion_common.hpp>
#include <glm/gtc/quaternion.hpp>

#include "render/resources/StaticGltfModel.hpp"

namespace core {

namespace {

constexpr float kEpsilon = 1.0e-5f;

std::size_t taskGrain(std::size_t count, std::size_t workerCount) {
    const std::size_t lanes = std::max<std::size_t>(1u, workerCount + 1u) * 2u;
    return std::max<std::size_t>(8u, (count + lanes - 1u) / lanes);
}

float advanceTime(float time, float delta, float duration, bool loop) {
    if (duration <= kEpsilon) {
        return 0.0f;
    }

    float updated = time + delta;
    if (!loop) {
        return glm::clamp(updated, 0.0f, duration);
    }

    updated = std::fmod(updated, duration);
    if (updated < 0.0f) {
        updated += duration;
    }
    return updated;
}

std::size_t sampleKeyframeIndex(const std::vector<float>& times, float time, bool loop, float duration) {
    if (times.size() <= 1u) {
        return 0u;
    }

    const float sampledTime = advanceTime(time, 0.0f, duration > kEpsilon ? duration : times.back(), loop);
    auto upper = std::upper_bound(times.begin(), times.end(), sampledTime);
    if (upper == times.begin()) {
        return 0u;
    }
    const std::size_t index = static_cast<std::size_t>(std::distance(times.begin(), upper) - 1);
    return std::min(index, times.size() - 1u);
}

float sampleAlpha(const std::vector<float>& times, std::size_t index, float time, bool loop, float duration) {
    if (times.empty() || index + 1u >= times.size()) {
        return 0.0f;
    }

    const float sampledTime = advanceTime(time, 0.0f, duration > kEpsilon ? duration : times.back(), loop);
    const float start = times[index];
    const float end = times[index + 1u];
    if (end <= start + kEpsilon) {
        return 0.0f;
    }
    return glm::clamp((sampledTime - start) / (end - start), 0.0f, 1.0f);
}

glm::vec3 sampleVec3Track(
    const render::Vec3AnimationTrack& track,
    float time,
    const glm::vec3& defaultValue,
    bool loop,
    float duration
) {
    if (track.values.empty() || track.times.empty()) {
        return defaultValue;
    }
    if (track.values.size() == 1u || track.times.size() == 1u) {
        return track.values.front();
    }

    const std::size_t index = sampleKeyframeIndex(track.times, time, loop, duration);
    if (index + 1u >= track.values.size() || track.interpolation == render::AnimationInterpolation::Step) {
        return track.values[index];
    }

    const float alpha = sampleAlpha(track.times, index, time, loop, duration);
    return glm::mix(track.values[index], track.values[index + 1u], alpha);
}

glm::quat sampleQuatTrack(
    const render::QuatAnimationTrack& track,
    float time,
    const glm::quat& defaultValue,
    bool loop,
    float duration
) {
    if (track.values.empty() || track.times.empty()) {
        return defaultValue;
    }
    if (track.values.size() == 1u || track.times.size() == 1u) {
        return glm::normalize(track.values.front());
    }

    const std::size_t index = sampleKeyframeIndex(track.times, time, loop, duration);
    if (index + 1u >= track.values.size() || track.interpolation == render::AnimationInterpolation::Step) {
        return glm::normalize(track.values[index]);
    }

    const float alpha = sampleAlpha(track.times, index, time, loop, duration);
    return glm::normalize(glm::slerp(track.values[index], track.values[index + 1u], alpha));
}

void ensureAnimationStorage(AnimatedModelComponent& component) {
    if (!component.model) {
        component.localPose.clear();
        component.globalNodeMatrices.clear();
        component.skinJointMatrices.clear();
        component.currentClip = -1;
        component.nextClip = -1;
        component.requestedClip = -1;
        component.currentTime = 0.0f;
        component.nextTime = 0.0f;
        component.blendElapsed = 0.0f;
        return;
    }

    const render::GltfModelData& model = *component.model;
    if (component.localPose.size() != model.nodes.size()) {
        component.localPose.resize(model.nodes.size());
    }
    if (component.globalNodeMatrices.size() != model.nodes.size()) {
        component.globalNodeMatrices.resize(model.nodes.size(), glm::mat4(1.0f));
    }
    if (component.skinJointMatrices.size() != model.skins.size()) {
        component.skinJointMatrices.resize(model.skins.size());
    }
    for (std::size_t skinIndex = 0; skinIndex < model.skins.size(); ++skinIndex) {
        if (component.skinJointMatrices[skinIndex].size() != model.skins[skinIndex].jointNodeIndices.size()) {
            component.skinJointMatrices[skinIndex].assign(
                model.skins[skinIndex].jointNodeIndices.size(),
                glm::mat4(1.0f)
            );
        }
    }
}

void writeRestPose(const render::GltfModelData& model, std::vector<AnimationLocalPose>& outPose) {
    outPose.resize(model.nodes.size());
    for (std::size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
        outPose[nodeIndex].translation = model.nodes[nodeIndex].localTransform.translation;
        outPose[nodeIndex].rotation = model.nodes[nodeIndex].localTransform.rotation;
        outPose[nodeIndex].scale = model.nodes[nodeIndex].localTransform.scale;
    }
}

void sampleClipPose(
    const render::GltfModelData& model,
    int clipIndex,
    float time,
    bool loop,
    std::vector<AnimationLocalPose>& outPose
) {
    writeRestPose(model, outPose);
    if (clipIndex < 0 || clipIndex >= static_cast<int>(model.animations.size())) {
        return;
    }

    const render::AnimationClip& clip = model.animations[static_cast<std::size_t>(clipIndex)];
    const float duration = clip.duration > kEpsilon ? clip.duration : 0.0f;
    for (std::size_t nodeIndex = 0; nodeIndex < clip.nodeChannels.size() && nodeIndex < outPose.size(); ++nodeIndex) {
        const render::NodeAnimationChannels& channels = clip.nodeChannels[nodeIndex];
        AnimationLocalPose& pose = outPose[nodeIndex];
        if (channels.translation.has_value()) {
            pose.translation = sampleVec3Track(
                *channels.translation,
                time,
                pose.translation,
                loop,
                duration
            );
        }
        if (channels.rotation.has_value()) {
            pose.rotation = sampleQuatTrack(
                *channels.rotation,
                time,
                pose.rotation,
                loop,
                duration
            );
        }
        if (channels.scale.has_value()) {
            pose.scale = sampleVec3Track(
                *channels.scale,
                time,
                pose.scale,
                loop,
                duration
            );
        }
    }
}

AnimationLocalPose blendPose(const AnimationLocalPose& from, const AnimationLocalPose& to, float alpha) {
    AnimationLocalPose pose{};
    pose.translation = glm::mix(from.translation, to.translation, alpha);
    pose.rotation = glm::normalize(glm::slerp(from.rotation, to.rotation, alpha));
    pose.scale = glm::mix(from.scale, to.scale, alpha);
    return pose;
}

void buildGlobalMatrices(
    const render::GltfModelData& model,
    const std::vector<AnimationLocalPose>& localPose,
    std::vector<glm::mat4>& outGlobalMatrices
) {
    std::vector<std::uint8_t> resolved(model.nodes.size(), 0u);
    const auto resolveNode = [&](const auto& self, int nodeIndex) -> glm::mat4 {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
            return glm::mat4(1.0f);
        }
        if (resolved[static_cast<std::size_t>(nodeIndex)] != 0u) {
            return outGlobalMatrices[static_cast<std::size_t>(nodeIndex)];
        }

        const AnimationLocalPose& pose = localPose[static_cast<std::size_t>(nodeIndex)];
        glm::mat4 matrix = render::composeNodeTransform(render::NodeTransform{
            pose.translation,
            pose.rotation,
            pose.scale
        });
        const int parentIndex = model.nodes[static_cast<std::size_t>(nodeIndex)].parentIndex;
        if (parentIndex >= 0) {
            matrix = self(self, parentIndex) * matrix;
        }

        outGlobalMatrices[static_cast<std::size_t>(nodeIndex)] = matrix;
        resolved[static_cast<std::size_t>(nodeIndex)] = 1u;
        return matrix;
    };

    for (int nodeIndex = 0; nodeIndex < static_cast<int>(model.nodes.size()); ++nodeIndex) {
        resolveNode(resolveNode, nodeIndex);
    }
}

void buildSkinPalettes(
    const render::GltfModelData& model,
    const std::vector<glm::mat4>& globalNodeMatrices,
    std::vector<std::vector<glm::mat4>>& outSkinMatrices
) {
    outSkinMatrices.resize(model.skins.size());
    for (std::size_t skinIndex = 0; skinIndex < model.skins.size(); ++skinIndex) {
        const render::SkinData& skin = model.skins[skinIndex];
        std::vector<glm::mat4>& skinMatrices = outSkinMatrices[skinIndex];
        skinMatrices.resize(skin.jointNodeIndices.size(), glm::mat4(1.0f));
        for (std::size_t jointIndex = 0; jointIndex < skin.jointNodeIndices.size(); ++jointIndex) {
            const int nodeIndex = skin.jointNodeIndices[jointIndex];
            if (nodeIndex < 0 || nodeIndex >= static_cast<int>(globalNodeMatrices.size())) {
                skinMatrices[jointIndex] = glm::mat4(1.0f);
                continue;
            }
            skinMatrices[jointIndex] =
                globalNodeMatrices[static_cast<std::size_t>(nodeIndex)] * skin.inverseBindMatrices[jointIndex];
        }
    }
}

void requestClipIfNeeded(AnimatedModelComponent& component) {
    if (!component.model) {
        return;
    }

    const int clipCount = static_cast<int>(component.model->animations.size());
    if (component.currentClip < 0 && clipCount > 0) {
        component.currentClip = render::findDefaultAnimationClipIndex(*component.model);
        component.currentTime = 0.0f;
    }

    if (component.requestedClip < 0 || component.requestedClip >= clipCount) {
        component.requestedClip = -1;
        return;
    }

    const int requestedClip = component.requestedClip;
    component.requestedClip = -1;

    if (requestedClip == component.currentClip && component.nextClip < 0) {
        return;
    }

    if (component.currentClip < 0 || component.blendDuration <= kEpsilon) {
        component.currentClip = requestedClip;
        component.currentTime = 0.0f;
        component.nextClip = -1;
        component.nextTime = 0.0f;
        component.blendElapsed = 0.0f;
        return;
    }

    if (component.nextClip >= 0) {
        component.currentClip = component.nextClip;
        component.currentTime = component.nextTime;
        component.nextClip = -1;
        component.nextTime = 0.0f;
    }

    if (requestedClip == component.currentClip) {
        component.blendElapsed = 0.0f;
        return;
    }

    component.nextClip = requestedClip;
    component.nextTime = 0.0f;
    component.blendElapsed = 0.0f;
}

void updateAnimatedModel(AnimatedModelComponent& component, const TimeContext& time) {
    ensureAnimationStorage(component);
    if (!component.model) {
        return;
    }

    const render::GltfModelData& model = *component.model;
    requestClipIfNeeded(component);

    if (component.playing) {
        const float delta = time.deltaSeconds * component.speed;
        if (component.currentClip >= 0 && component.currentClip < static_cast<int>(model.animations.size())) {
            const float duration = model.animations[static_cast<std::size_t>(component.currentClip)].duration;
            component.currentTime = advanceTime(component.currentTime, delta, duration, component.loop);
        }
        if (component.nextClip >= 0 && component.nextClip < static_cast<int>(model.animations.size())) {
            const float duration = model.animations[static_cast<std::size_t>(component.nextClip)].duration;
            component.nextTime = advanceTime(component.nextTime, delta, duration, component.loop);
            component.blendElapsed += time.deltaSeconds;
        }
    }

    std::vector<AnimationLocalPose> currentPose{};
    std::vector<AnimationLocalPose> nextPose{};
    sampleClipPose(model, component.currentClip, component.currentTime, component.loop, currentPose);

    if (component.nextClip >= 0) {
        sampleClipPose(model, component.nextClip, component.nextTime, component.loop, nextPose);
        const float alpha = component.blendDuration <= kEpsilon
            ? 1.0f
            : glm::clamp(component.blendElapsed / component.blendDuration, 0.0f, 1.0f);
        component.localPose.resize(currentPose.size());
        for (std::size_t nodeIndex = 0; nodeIndex < currentPose.size(); ++nodeIndex) {
            component.localPose[nodeIndex] = blendPose(currentPose[nodeIndex], nextPose[nodeIndex], alpha);
        }

        if (alpha >= 1.0f) {
            component.currentClip = component.nextClip;
            component.currentTime = component.nextTime;
            component.nextClip = -1;
            component.nextTime = 0.0f;
            component.blendElapsed = 0.0f;
        }
    } else {
        component.localPose = std::move(currentPose);
    }

    buildGlobalMatrices(model, component.localPose, component.globalNodeMatrices);
    buildSkinPalettes(model, component.globalNodeMatrices, component.skinJointMatrices);
}

}  // namespace

void AnimationSystem::update(World& world, const TimeContext& time, TaskScheduler& scheduler, bool useParallel) const {
    if (world.animatedModels.size() == 0u) {
        return;
    }

    if (!useParallel || world.animatedModels.size() < 2u) {
        for (AnimatedModelComponent& component : world.animatedModels.values()) {
            updateAnimatedModel(component, time);
        }
        return;
    }

    TaskGroup group;
    std::vector<AnimatedModelComponent>& components = world.animatedModels.values();
    scheduler.parallelFor(
        group,
        components.size(),
        taskGrain(components.size(), scheduler.workerCount()),
        "Animation Update",
        [&](std::size_t begin, std::size_t end) {
            for (std::size_t index = begin; index < end; ++index) {
                updateAnimatedModel(components[index], time);
            }
        }
    );
    scheduler.wait(group);
}

}  // namespace core
