// Test_ImportedCharacterMapper.cpp - Phase 1 G1 unit tests.
//
// Pure data-shape transform; no I/O, no FBX, no ECS. Drives the
// mapper with synthetic ConversionResult vectors that mirror the
// type strings emitted by the AYResource sub-converters (see
// AYImportedCharacterMapper.h for the four canonical lines).
//
// Drives the "character wins, cube is fallback" policy: only
// conversions with all four required types produce a valid
// ImportedCharacter. Missing Animation is the most important
// regression to pin - AnimationComponent dereferences clipPath
// during the first tick, so a partially-mapped character would
// crash the editor.

#include "AYTest.h"
#include "AYImportedCharacterMapper.h"

#include <string>
#include <vector>

using ayt::editor::ImportedCharacter;
using ayt::editor::ImportedCharacterMapDiagnostics;
using ayt::editor::mapConversionToImportedCharacter;
using ayt::resource::ConversionResult;

namespace {

// Convenience: build a synthetic ConversionResult with all four
// required types wired up. Override individual entries by index
// after construction when crafting negative cases.
ConversionResult makeFullResult()
{
    ConversionResult r;
    auto add = [&](const std::string& type, const std::string& path) {
        ConversionResult::ConvertedResource res;
        res.type = type;
        res.path = path;
        r.resources.push_back(res);
    };
    add("Mesh",       "meshes/suzanne_RootNode_Suzanne.aymesh");
    add("Material",   "materials/suzanne_material_0.aymat");
    add("Skeleton",   "skeletons/suzanne_Skeleton.ayskel");
    add("Animation",  "animations/suzanne_idle.ayanm");
    return r;
}

constexpr const char* kCacheRoot = "D:/tmp/ayeditor_cache";

} // namespace

TEST_SUITE(AYEditor_ImportedCharacterMapper)

// Happy path: a full FBX yields a valid ImportedCharacter whose four
// path fields are absolute paths under <cacheRoot>/assets/. This is
// the canonical happy path AYEditorShell_Demo --import D:/Projects/
// suzanne.fbx drives end-to-end.
TEST_CASE(map_conversion_with_all_four_types_returns_valid_character)
{
    ConversionResult r = makeFullResult();
    ImportedCharacterMapDiagnostics diag;
    ImportedCharacter c =
        mapConversionToImportedCharacter(r, kCacheRoot, diag);

    CHECK_TRUE(diag.success);
    CHECK_TRUE(diag.missing.empty());
    CHECK_TRUE(c.isValid());

    CHECK_TRUE(c.meshPath ==
        "D:/tmp/ayeditor_cache/assets/meshes/suzanne_RootNode_Suzanne.aymesh");
    CHECK_TRUE(c.materialPath ==
        "D:/tmp/ayeditor_cache/assets/materials/suzanne_material_0.aymat");
    CHECK_TRUE(c.skeletonPath ==
        "D:/tmp/ayeditor_cache/assets/skeletons/suzanne_Skeleton.ayskel");
    CHECK_TRUE(c.animationPath ==
        "D:/tmp/ayeditor_cache/assets/animations/suzanne_idle.ayanm");
}

// Phase 1 policy: Animation is required. Mesh-only or Mesh+Skeleton
// only conversions are rejected. This pins the policy against
// accidental relaxation in a future commit.
TEST_CASE(map_conversion_missing_animation_returns_invalid_with_diagnostic)
{
    ConversionResult r = makeFullResult();
    // Drop the Animation entry.
    r.resources.pop_back();

    ImportedCharacterMapDiagnostics diag;
    ImportedCharacter c =
        mapConversionToImportedCharacter(r, kCacheRoot, diag);

    CHECK_FALSE(diag.success);
    CHECK_TRUE(diag.missing.size() == 1);
    CHECK_TRUE(diag.missing[0] == "Animation");
    CHECK_FALSE(c.isValid());
    CHECK_TRUE(c.animationPath.empty());
    // Mesh / material / skeleton paths are still populated so a
    // future Inspector (ED-03) can preview the partial asset.
    CHECK_FALSE(c.meshPath.empty());
    CHECK_FALSE(c.materialPath.empty());
    CHECK_FALSE(c.skeletonPath.empty());
}

// Mesh-only FBX: a static mesh with no skin. All three remaining
// types must show up in `missing`. Regression-prevention for the
// case where a designer imports a non-character FBX.
TEST_CASE(map_conversion_mesh_only_returns_invalid_with_diagnostic)
{
    ConversionResult r;
    ConversionResult::ConvertedResource mesh;
    mesh.type = "Mesh";
    mesh.path = "meshes/static_box.aymesh";
    r.resources.push_back(mesh);

    ImportedCharacterMapDiagnostics diag;
    ImportedCharacter c =
        mapConversionToImportedCharacter(r, kCacheRoot, diag);

    CHECK_FALSE(diag.success);
    CHECK_TRUE(diag.missing.size() == 3);
    // Order: Mesh, Material, Skeleton, Animation; mesh present, so
    // Material / Skeleton / Animation appear (in that stable order).
    CHECK_TRUE(diag.missing[0] == "Material");
    CHECK_TRUE(diag.missing[1] == "Skeleton");
    CHECK_TRUE(diag.missing[2] == "Animation");
    CHECK_FALSE(c.isValid());
}

// When two meshes are present (e.g. an FBX with a low-poly collider
// proxy), we deliberately pick the FIRST one. Today no caller hits
// this path - all four types come from the same FBX - but pinning
// the policy now prevents future surprise.
TEST_CASE(map_conversion_picks_first_resource_of_each_type_when_duplicates)
{
    ConversionResult r = makeFullResult();
    ConversionResult::ConvertedResource second;
    second.type = "Mesh";
    second.path = "meshes/suzanne_collider.aymesh";
    // Insert the second mesh AFTER the original mesh so pickFirst
    // returns the first hit.
    r.resources.insert(r.resources.begin() + 1, second);

    ImportedCharacterMapDiagnostics diag;
    ImportedCharacter c =
        mapConversionToImportedCharacter(r, kCacheRoot, diag);

    CHECK_TRUE(diag.success);
    CHECK_TRUE(c.meshPath ==
        "D:/tmp/ayeditor_cache/assets/meshes/suzanne_RootNode_Suzanne.aymesh");
}

// Defensive: EditorPlayRuntime::resolvePersistentCacheRoot() and any
// future caller may pass the cache root with or without a trailing
// separator. All three forms must produce the same absolute paths.
TEST_CASE(map_conversion_resolves_cache_root_with_or_without_trailing_slash)
{
    ConversionResult r = makeFullResult();

    {
        ImportedCharacterMapDiagnostics d;
        ImportedCharacter c =
            mapConversionToImportedCharacter(r, "D:/tmp/ayeditor_cache", d);
        CHECK_TRUE(d.success);
        CHECK_TRUE(c.meshPath ==
            "D:/tmp/ayeditor_cache/assets/meshes/suzanne_RootNode_Suzanne.aymesh");
    }
    {
        ImportedCharacterMapDiagnostics d;
        ImportedCharacter c =
            mapConversionToImportedCharacter(r, "D:/tmp/ayeditor_cache/", d);
        CHECK_TRUE(d.success);
        CHECK_TRUE(c.meshPath ==
            "D:/tmp/ayeditor_cache/assets/meshes/suzanne_RootNode_Suzanne.aymesh");
    }
    {
        ImportedCharacterMapDiagnostics d;
        ImportedCharacter c =
            mapConversionToImportedCharacter(r, "D:/tmp/ayeditor_cache\\", d);
        CHECK_TRUE(d.success);
        CHECK_TRUE(c.meshPath ==
            "D:/tmp/ayeditor_cache/assets/meshes/suzanne_RootNode_Suzanne.aymesh");
    }
}

// Defensive: an empty ConversionResult (e.g. importer hit a parse
// error mid-pipeline and returned no resources) must not crash and
// must produce an invalid result with all four types missing.
TEST_CASE(map_conversion_with_empty_resources_vector_returns_invalid)
{
    ConversionResult r;  // no entries
    ImportedCharacterMapDiagnostics diag;
    ImportedCharacter c =
        mapConversionToImportedCharacter(r, kCacheRoot, diag);

    CHECK_FALSE(diag.success);
    CHECK_TRUE(diag.missing.size() == 4);
    CHECK_TRUE(c.isValid() == false);
    CHECK_TRUE(c.meshPath.empty());
    CHECK_TRUE(c.materialPath.empty());
    CHECK_TRUE(c.skeletonPath.empty());
    CHECK_TRUE(c.animationPath.empty());
}

TEST_SUITE_END