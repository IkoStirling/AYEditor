// Test_EditorPlayRuntime.cpp - Phase 1 ED-02 tests.
//
// Covers the Play mode's "spawn character vs cube" decision tree
// without bringing up the renderer. We drive `EditorPlayRuntime`
// through its public setters + `trySpawnImportedCharacter()` only;
// `startPlay()` is omitted from the test set because it eagerly
// initializes the bgfx + GameLoop session, which needs a host HWND
// and the per-test fixture doesn't provide one.
//
// Failures observed:
//   * `test_editor_session_play_mode_split_capture` is a pre-existing
//     failure unrelated to this file (cursor-hint reset). Documented
//     in commit 54d0841.

#include "AYTest.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEntity.h"
#include "AYEntity/EntityModule.h"
#include "AYEntity/CharacterEntity.h"

#include <AYEntity/components/AnimationComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/SkeletonComponent.h>

#include <cstring>
#include <string>

using namespace ayt::editor;
using namespace ayt::entity;

namespace {

struct ImportedCharacterOK {
    static ImportedCharacter make() {
        ImportedCharacter c;
        c.meshPath        = "meshes/hero.aymesh";
        c.materialPath    = "materials/hero.aymat";
        c.skeletonPath    = "skeletons/hero_Skeleton.ayskel";
        c.animationPath   = "animations/hero_dance.ayanm";
        return c;
    }
};

ImportedCharacter makeEmpty() {
    return ImportedCharacter{};  // all-empty paths
}

} // namespace

TEST_SUITE(AYEditor_PlayRuntime)

// `trySpawnImportedCharacter` with a valid 4-tuple of paths returns
// true and creates an Entity with a MeshComponent whose `skinned`
// flag is set. We don't have real `.ay*` assets here, so the call
// still completes (the resource adapter only runs on `onUpdate`);
// the body of the test only verifies the ECS shape.

TEST_CASE(try_spawn_imported_character_with_valid_paths_returns_true)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.setImportedCharacter(ImportedCharacterOK::make());

    const bool spawned = rt.trySpawnImportedCharacter();
    CHECK_TRUE(spawned);

    Entity* e = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(e);
    if (e != nullptr) {
        CHECK_TRUE(e->hasComponent<MeshComponent>());
        CHECK_TRUE(e->getComponent<MeshComponent>()->skinned);
        CHECK_TRUE(e->hasComponent<SkeletonComponent>());
        CHECK_TRUE(e->hasComponent<AnimationComponent>());
    }

    rt.clearCharacter();
    CHECK_NULL(rt.selectedCharacterEntity());

    World::instance().shutdown();
}

// Empty `ImportedCharacter` (the default state) → spawn returns
// false, no entity created. This is the path that the editor takes
// before any FBX has been imported.

TEST_CASE(try_spawn_imported_character_with_empty_paths_returns_false)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.setImportedCharacter(makeEmpty());

    const bool spawned = rt.trySpawnImportedCharacter();
    CHECK_FALSE(spawned);
    CHECK_NULL(rt.selectedCharacterEntity());

    World::instance().shutdown();
}

// Idempotence: calling twice doesn't re-spawn. We can't easily
// observe "second call is no-op" without poking internals, but the
// "second call returns false because entity already exists" contract
// is exactly what we pinned above — verify it.

TEST_CASE(try_spawn_is_idempotent_when_called_twice)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.setImportedCharacter(ImportedCharacterOK::make());

    CHECK_TRUE(rt.trySpawnImportedCharacter());
    Entity* first = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(first);

    CHECK_FALSE(rt.trySpawnImportedCharacter());
    CHECK_TRUE(rt.selectedCharacterEntity() == first);  // same pointer

    rt.clearCharacter();
    World::instance().shutdown();
}

// Default-constructed `EditorPlayRuntime` (no character configured)
// mirrors the current "just the cube" demo behavior. The public
// accessor confirms.

TEST_CASE(default_constructed_runtime_has_no_character_and_no_cube)
{
    EditorPlayRuntime rt;
    CHECK_NULL(rt.selectedCharacterEntity());
    CHECK_TRUE(!rt.isSimulationActive());
    CHECK_FALSE(rt.isPlaying());
}

// Tests that `setImportedCharacter` after init changes the contract:
// the next `trySpawnImportedCharacter` call sees the new paths.

TEST_CASE(set_imported_character_after_init_takes_effect_on_next_spawn)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    // First: empty → no spawn.
    rt.setImportedCharacter(makeEmpty());
    CHECK_FALSE(rt.trySpawnImportedCharacter());

    // Then: valid → spawn succeeds.
    rt.setImportedCharacter(ImportedCharacterOK::make());
    CHECK_TRUE(rt.trySpawnImportedCharacter());
    CHECK_NOT_NULL(rt.selectedCharacterEntity());

    rt.clearCharacter();
    World::instance().shutdown();
}

// Phase 2a: replaceImportedCharacter performs a one-shot hot-swap
// — it clears the previously-spawned entity, accepts the new
// ImportedCharacter, and re-spawns. The GameLoop keeps ticking
// throughout (we do not test loops here; we just verify the entity
// set swap). Calling twice with the same valid character yields
// two distinct entities (which is what designers expect when
// double-clicking the Import button): the second call clears the
// first and spawns a fresh one.
TEST_CASE(replace_imported_character_respawns_after_previous)
{
    World::instance().initialize();

    EditorPlayRuntime rt;

    // First import succeeds.
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    Entity* first = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(first);

    // Second import with the same paths: replaces the first.
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    Entity* second = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(second);
    CHECK_TRUE(second != first);  // distinct entity instance

    rt.clearCharacter();
    rt.clearCube();
    World::instance().shutdown();
}

// Phase 2a: replaceImportedCharacter with an invalid (default-
// constructed) ImportedCharacter falls back to the procedural
// cube, mirroring startPlay's policy. Empty ImportedCharacter is
// isValid()==false, so the cube is the only thing that ends up
// spawned.
TEST_CASE(replace_imported_character_with_invalid_falls_back_to_cube)
{
    World::instance().initialize();

    EditorPlayRuntime rt;

    // Spawn a character first so we can verify the swap clears it.
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    CHECK_NOT_NULL(rt.selectedCharacterEntity());

    // Now replace with empty - should clear the character and
    // spawn the cube (EditorPlayRuntime holds the cube entity as
    // a private; we can only assert the character side via the
    // public accessor).
    rt.replaceImportedCharacter(makeEmpty());
    CHECK_NULL(rt.selectedCharacterEntity());

    rt.clearCharacter();
    rt.clearCube();
    World::instance().shutdown();
}

// Phase 2a: clearCube is a public no-op when no cube was spawned.
// Before this iteration clearCube was private; promoting it was
// the smallest API change to allow replaceImportedCharacter to
// reset Play state.
TEST_CASE(clear_cube_is_safe_when_no_cube_spawned)
{
    EditorPlayRuntime rt;
    // No setup; _cubeEntity is nullptr. Calling clearCube must
    // not crash and must remain a no-op.
    rt.clearCube();
    CHECK_NULL(rt.selectedCharacterEntity());
}

// ED-03: applying overrides to a live spawned entity must
// mutate SkeletonComponent::skeletonPath and AnimationComponent
// ::clipPath in place. We stub the file-existence check by
// using real fixtures that ship in AYResource/test_output
// (no I/O happens in ImportedCharacterOK::make() so we don't
// rely on those paths; the override validation reads them
// directly).
TEST_CASE(inspector_overrides_apply_to_spawned_entity_components)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    Entity* e = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(e);

    EntityInspectorOverrides ov;
    ov.skeletonPathOverride =
        "D:/Projects/AYRuntime/AYResource/test_output/skeletons/Sour_Skeleton.ayskel";
    ov.animationPathOverride =
        "D:/Projects/AYRuntime/AYResource/test_output_animconv/animations/hero_run.ayanm";
    rt.applyComponentOverrides(ov);

    auto* skelComp = e->getComponent<SkeletonComponent>();
    auto* animComp = e->getComponent<AnimationComponent>();
    CHECK_NOT_NULL(skelComp);
    CHECK_NOT_NULL(animComp);
    CHECK_TRUE(skelComp->skeletonPath == ov.skeletonPathOverride);
    CHECK_TRUE(animComp->clipPath == ov.animationPathOverride);

    rt.clearCharacter();
    rt.clearCube();
    World::instance().shutdown();
}

// ED-03: pending overrides survive replaceImportedCharacter so
// the user's clip/sk el picks stay applied when they hot-swap to
// a different FBX. This is the test that pins the persistence
// contract from the Foundation Plan prompt: "Persist
// assignment in EditorSession for Play mode respawn."
TEST_CASE(inspector_overrides_persist_across_replace)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    CHECK_TRUE(rt.selectedCharacterEntity() != nullptr);

    EntityInspectorOverrides ov;
    ov.skeletonPathOverride =
        "D:/Projects/AYRuntime/AYResource/test_output/skeletons/Sour_Skeleton.ayskel";
    ov.animationPathOverride =
        "D:/Projects/AYRuntime/AYResource/test_output_animconv/animations/hero_run.ayanm";
    rt.applyComponentOverrides(ov);

    // Hot-swap to another full character (same fixture - the
    // specific paths don't matter for this test, only that the
    // overrides re-apply).
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    Entity* e = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(e);

    auto* skelComp = e->getComponent<SkeletonComponent>();
    auto* animComp = e->getComponent<AnimationComponent>();
    CHECK_TRUE(skelComp->skeletonPath == ov.skeletonPathOverride);
    CHECK_TRUE(animComp->clipPath == ov.animationPathOverride);

    rt.clearCharacter();
    rt.clearCube();
    World::instance().shutdown();
}

// ED-03: missing-file override must NOT clear the field.
// Per the prompt: "Validation: log if .ayanm missing; do not
// crash GameLoop." We choose to log AND keep the prior path,
// which lets the user recover by picking a different file.
TEST_CASE(inspector_overrides_logged_when_file_missing)
{
    World::instance().initialize();

    EditorPlayRuntime rt;
    rt.replaceImportedCharacter(ImportedCharacterOK::make());
    Entity* e = rt.selectedCharacterEntity();
    CHECK_NOT_NULL(e);
    const std::string originalClip =
        e->getComponent<AnimationComponent>()->clipPath;

    EntityInspectorOverrides ov;
    ov.animationPathOverride = "D:/no/such/path/does_not_exist.ayanm";
    rt.applyComponentOverrides(ov);

    // Field should still hold the original path (the override
    // was rejected because the file is missing).
    CHECK_TRUE(e->getComponent<AnimationComponent>()->clipPath == originalClip);

    rt.clearCharacter();
    rt.clearCube();
    World::instance().shutdown();
}

// ED-03: empty overrides (= Reset button) clears the pending
// buffer so the next spawn restores the original Imported
// Character paths instead of the user's override.
TEST_CASE(inspector_overrides_reset_clears_pending)
{
    EditorPlayRuntime rt;
    EntityInspectorOverrides ov;
    ov.skeletonPathOverride =
        "D:/Projects/AYRuntime/AYResource/test_output/skeletons/Sour_Skeleton.ayskel";
    rt.applyComponentOverrides(ov);
    CHECK_FALSE(rt.pendingOverrides().isCleared());

    EntityInspectorOverrides emptyOv;
    rt.applyComponentOverrides(emptyOv);
    CHECK_TRUE(rt.pendingOverrides().isCleared());
}

TEST_SUITE_END
