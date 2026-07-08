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
#include "AYEditorPlayRuntime.h"
#include "AYEntity.h"
#include "AYEntityModule.h"
#include "AYCharacterEntity.h"

#include <components/AYAnimationComponent.h>
#include <components/AYMeshComponent.h>
#include <components/AYSkeletonComponent.h>

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

TEST_SUITE_END
