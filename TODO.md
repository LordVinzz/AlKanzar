Recommended Next Step: Skeletal Animation System

(* https://github.com/thedmd/imgui-node-editor Node Editor for animations shaderpass ect *)

This is the single highest-leverage thing to build next. Here's why:

Everything downstream depends on it. Characters need to walk, attack, idle, cast, die. Without animation, you
can't prototype combat, movement, or any gameplay. Your glTF loader (tinygltf) already parses skeletal animation
data from .glb files — you're loading Adventurer.glb which likely has a skeleton — but the engine currently
discards it.

What this involves:

1. Skeleton data structures — Joint hierarchy (parent indices, inverse bind matrices, names). A SkeletonComponent
  for the ECS.
2. Animation clip storage — Keyframe channels (translation, rotation, scale per joint) with interpolation (step,
linear, cubic spline — all defined in glTF).
3. Animation sampler/evaluator — Given a clip + time, produce a pose (array of joint local transforms). Handle
looping, clamping.
4. Skinning pipeline — Compute joint world matrices → multiply by inverse bind matrices → upload as a mat4[]
uniform or SSBO. Modify your vertex shader to do skinned_pos = (jointMatrix[joint0] * weight0 + ...) * position.
5. glTF loader extension — Extract skins, animations, accessors for joint indices/weights from tinygltf. You
already have the loader; this adds ~200-300 lines.
6. Animation state machine — Simple state graph (Idle → Walk → Attack → Idle) with blend transitions. Doesn't
need to be fancy yet — even a basic crossfade between two clips is enough to start.
7. AnimationSystem — ECS system that ticks active animation states each frame, integrates with your existing
TaskScheduler for parallel pose evaluation.

Rough implementation order:

1. Skeleton + AnimationClip data structures
2. Extend glTF loader to extract skin/animation data
3. SkinnedMeshBuffer (vertex attributes: joint indices + weights)
4. Skinning vertex shader variant
5. AnimationEvaluator (sample clip → pose)
6. AnimationSystem (tick each frame, upload joint matrices)
7. Simple state machine (idle/walk/attack transitions)

After animation, the natural BG1 progression would be:

- Pathfinding + Click-to-Move (nav mesh + A* — the core BG1 interaction: click ground, character walks there with
  walk animation)
- Character Stats & Party (D&D-style attributes, HP, AC, THAC0, party of up to 6)
- Real-Time-with-Pause Combat (action rounds, attack rolls, damage, spells)
- Dialogue System (branching trees with stat/reputation checks)
- Area/Map System (distinct areas with walkable regions, area transitions, world map travel)
- Inventory & Equipment (items affecting stats, paper doll)
- Audio (ambient, music, SFX, voice — SDL2 is already there)
- Save/Load (world state serialization)