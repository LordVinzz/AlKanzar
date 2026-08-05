#include "SceneRegistry.hpp"

#include <initializer_list>

namespace {

core::SkillRanksComponent initiateSkills(
    std::initializer_list<core::CharacterSkill> trainedSkills
) {
    core::SkillRanksComponent skills{};
    for (const core::CharacterSkill skill : trainedSkills) {
        skills.ranks[static_cast<std::size_t>(skill)] = core::SkillRank::Initiate;
    }
    return skills;
}

}  // namespace

namespace core {

SceneBlueprint SceneRegistry::defaultScene() const {
    SceneBlueprint blueprint{};
    blueprint.navMeshAssetPath = "navmeshes/DefaultScene.navmesh";
    blueprint.models = {
        ModelInstanceBlueprint{
            "Player - Human Fighter",
            "Adventurer.glb",
            render::RenderLayer::Actors,
            TransformComponent{glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            false,
            0.0f,
            CharacterBlueprint{
                CharacterComponent{
                    CharacterAffiliation::Player,
                    CharacterRace::Human,
                    CharacterKit::Fighter,
                    0,
                    0.65f
                },
                AbilityScoresComponent{14, 12, 14, 10, 10, 10},
                initiateSkills({
                    CharacterSkill::Running,
                    CharacterSkill::Intimidation,
                    CharacterSkill::Command,
                    CharacterSkill::Perception,
                    CharacterSkill::FirstAid,
                    CharacterSkill::Acrobatics,
                }),
                CharacterVitalsComponent{53, 53, 0}
            }
        },
        ModelInstanceBlueprint{
            "Friendly NPC - Human Cleric",
            "Adventurer.glb",
            render::RenderLayer::Actors,
            TransformComponent{glm::vec3(-2.5f, 0.0f, 1.5f), glm::vec3(0.0f), glm::vec3(1.0f)},
            false,
            0.0f,
            CharacterBlueprint{
                CharacterComponent{
                    CharacterAffiliation::FriendlyNpc,
                    CharacterRace::Human,
                    CharacterKit::Cleric,
                    0,
                    0.65f
                },
                AbilityScoresComponent{10, 10, 12, 12, 14, 12},
                initiateSkills({
                    CharacterSkill::FirstAid,
                    CharacterSkill::Knowledge,
                    CharacterSkill::Command,
                    CharacterSkill::Perception,
                    CharacterSkill::Persuasion,
                    CharacterSkill::Survival,
                }),
                CharacterVitalsComponent{44, 44, 33}
            }
        },
        ModelInstanceBlueprint{
            "Hostile NPC - Orc Rogue",
            "Adventurer.glb",
            render::RenderLayer::Actors,
            TransformComponent{glm::vec3(2.5f, 0.0f, 1.5f), glm::vec3(0.0f), glm::vec3(1.0f)},
            false,
            0.0f,
            CharacterBlueprint{
                CharacterComponent{
                    CharacterAffiliation::HostileNpc,
                    CharacterRace::Orc,
                    CharacterKit::Rogue,
                    0,
                    0.65f
                },
                AbilityScoresComponent{12, 14, 10, 10, 10, 8},
                initiateSkills({
                    CharacterSkill::Acrobatics,
                    CharacterSkill::Stealth,
                    CharacterSkill::Investigation,
                    CharacterSkill::Deception,
                    CharacterSkill::Perception,
                    CharacterSkill::Intimidation,
                }),
                CharacterVitalsComponent{40, 40, 0}
            }
        },
        ModelInstanceBlueprint{
            "House",
            "FantasyHouse.glb",
            render::RenderLayer::Geometry,
            TransformComponent{
                glm::vec3(-3.0f, 0.0f, -8.0f),
                glm::vec3(0.0f, -35.0f, 0.0f),
                glm::vec3(1.0f)
            },
            true,
            7.5f
        }
    };

    blueprint.lightVolumes = {
        LightVolumeBlueprint{
            "Global Light Volume",
            TransformComponent{},
            glm::vec3(100.0f)
        }
    };

    blueprint.pointLights = {
        PointLightBlueprint{
            "Point Light",
            TransformComponent{glm::vec3(1.5f, 1.2f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            40.0f,
            glm::vec3(0.9f, 0.7f, 1.0f),
            20.0f,
            0.0f,
            false,
            true,
            0.000015f,
            0.0045f
        }
    };

    blueprint.spotLights = {
        SpotLightBlueprint{
            "Spot Light",
            TransformComponent{glm::vec3(5.5f, 10.2f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f)},
            32.0f,
            glm::vec3(0.55f, 0.70f, 0.95f),
            1.4f,
            glm::vec3(-3.0f, 1.2f, -8.0f),
            15.0f,
            25.0f,
            0.0f,
            false,
            true,
            0.0012f,
            0.004f
        }
    };

    return blueprint;
}

}  // namespace core
