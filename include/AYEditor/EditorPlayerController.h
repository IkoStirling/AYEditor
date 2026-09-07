#pragma once
// AYEditor/EditorPlayerController.h — INT-01 (2026-07-15)
//
// Sample `script PlayerController`-host component. Used by Editor
// Play runtime as the canonical end-to-end integration smoke:
//   - Editor calls `EditorPlayRuntime::spawnPlayerControllerIfNeeded()`
//     which adds this component to the World singleton entity.
//   - `EditorPlayRuntime::bindPlayerScript()` then resolves
//     `<assetRoot>/Scripts/PlayerController.logia` and calls
//     `ScriptSubSystem::bindAndLoadFromFile(*this, path, errors)`.
//   - On Play tick, `self.position.x = self.position.x + self.speed * dt`
//     mutates AY_PROPERTY fields; the AYReflect-backed codegen
//     rewrite keeps these edits end-to-end observable.
//
// Field names (speed / jump_force / position) MUST match
// EngineAssets/AYScript/templates/player_controller.logia; the codegen rewrite bakes the
// host-type name "PlayerController" into Lua, so the
// `registerEditorPlayerControllerComponent()` registration must use the bare
// name when registering with AYReflect.

#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/components/ScriptComponent.h>

#include <AYEntity/IEntity.h>
#include <AYMath/MathTypes.h>

namespace ayt::editor {

#define AY_CURRENT_CLASS PlayerController
struct PlayerController : public ayt::entity::ScriptComponent {
    const char* getName() const override { return "PlayerController"; }

    AY_PROPERTY(float, speed, ayt::entity::kAttrSerialize)
    AY_PROPERTY(float, jump_force, ayt::entity::kAttrSerialize)
    AY_PROPERTY(ayt::math::FVector3, position, ayt::entity::kAttrSerialize)

    PlayerController() {
        speed = 10.0f;
        jump_force = 5.0f;
        position = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
        setScriptName("PlayerController");
    }
};
#undef AY_CURRENT_CLASS

// Called by EditorComponentModule during IModule::registerTypes().
[[nodiscard]] ayt::entity::ComponentRegistryResult
registerEditorPlayerControllerComponent(
    ayt::entity::ComponentRegistry& registry);

} // namespace ayt::editor
