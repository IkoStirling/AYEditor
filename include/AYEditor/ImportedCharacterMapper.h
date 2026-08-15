#pragma once
// AYEditor/ImportedCharacterMapper.h - Phase 1 G1.
//
// Pure data-shape transform: scan a ConversionResult (returned by
// IConverter::convert via Importer::importFile) for the asset types
// we need to spawn an `EditorPlayRuntime` skinned character, and
// produce a populated `ImportedCharacter` whose path fields are
// absolute on-disk paths under the editor cache.
//
// No I/O. No logging. Caller (EditorApp::run) is responsible for the
// user-facing log line. This separation keeps the mapper unit-testable
// without touching the filesystem or stderr.
//
// Type-string spellings pinned by sub-converter emissions:
//   "Mesh"        - AYResource/src/Converter/MeshConverter.cpp:229
//   "Material"    - AYResource/src/Converter/MaterialConverter.cpp:164
//   "Skeleton"    - AYResource/src/Converter/SkeletonConverter.cpp:113
//   "Animation"   - AYResource/src/Converter/AnimationConverter.cpp:165
//
// Visibility policy (2026-07-27): Mesh + Skeleton are REQUIRED.
// Material and Animation are optional (listed in `missing` for
// diagnostics but do not fail `success`). Empty animation → bind-pose
// preview. All Mesh entries after the first go into
// `additionalMeshPaths` (MMD multi-part FBX).

#include <AYResource/IConverter.h>
#include <string>
#include <vector>

#include "AYEditor/EditorPlayRuntime.h"  // ImportedCharacter

namespace ayt::editor
{

// Diagnostic summary of a failed / partial mapping. Pure-data, no
// exceptions. `missing` lists the type names ("Mesh" / "Material" /
// "Skeleton" / "Animation") whose ConvertedResource entries were
// absent or whose virtual path was empty. Order in `missing` matches
// the order types are checked below (Mesh, Material, Skeleton,
// Animation) so log lines stay stable across runs.
struct ImportedCharacterMapDiagnostics {
    std::vector<std::string> missing;
    bool success = false;
};

// Walk `result.resources` and pick assets. Each picked virtual path is
// resolved against `cacheRoot` to produce an absolute on-disk path of
// the form `<cacheRoot>assets/<virtualPath>`.
//
// Returns:
//   * success when Mesh + Skeleton are present (isValid() true).
//   * All Mesh resources after the first populate additionalMeshPaths.
ImportedCharacter mapConversionToImportedCharacter(
    const ayt::resource::ConversionResult& result,
    const std::string& cacheRoot,
    ImportedCharacterMapDiagnostics& outDiag);

} // namespace ayt::editor
