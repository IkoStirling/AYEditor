#pragma once
// AYImportedCharacterMapper.h - Phase 1 G1.
//
// Pure data-shape transform: scan a ConversionResult (returned by
// IConverter::convert via Importer::importFile) for the four asset
// types we need to spawn an `EditorPlayRuntime` skinned character,
// and produce a populated `ImportedCharacter` whose path fields are
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
// Phase 1 policy: Animation is REQUIRED. Mesh-only or Mesh+Skeleton-only
// conversions are reported as `success = false` with "Animation" in
// `missing`. This avoids an `AnimationComponent` deref crash on the
// first tick when `clipPath` is empty. Phase 2 may relax when
// static-pose previews land.

#include <IAYConverter.h>
#include <string>
#include <vector>

#include "AYEditorPlayRuntime.h"  // ImportedCharacter

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

// Walk `result.resources` and pick the first entry of each required
// type. Each picked virtual path is resolved against `cacheRoot` to
// produce an absolute on-disk path of the form
// `<cacheRoot>assets/<virtualPath>` (forward slashes inside the
// virtual path are preserved; the cacheRoot/join layer normalizes
// trailing separators).
//
// `cacheRoot` may be passed with or without a trailing separator
// ('/' or '\\'). The mapper appends "assets/" before the virtual
// path; an empty `cacheRoot` yields paths that begin with
// "assets/..." - callers should pass the same value as
// `EditorPlayRuntime::resolvePersistentCacheRoot()`.
//
// Returns:
//   * When all four required types are present and non-empty,
//     `outCharacter.isValid()` is true; `outDiag.success` is true;
//     `outDiag.missing` is empty.
//   * When any required type is missing or its path is empty,
//     `outCharacter` is default-constructed (all empty strings,
//     isValid() == false); `outDiag.success` is false;
//     `outDiag.missing` lists the type names that were not found.
ImportedCharacter mapConversionToImportedCharacter(
    const ayt::resource::ConversionResult& result,
    const std::string& cacheRoot,
    ImportedCharacterMapDiagnostics& outDiag);

} // namespace ayt::editor