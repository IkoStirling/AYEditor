#pragma once
// AYEditor/Importer.h — Editor façade over AYResource::importAsset (P5).
//
// Keeps the ED-01 Result shape for EditorShellDemo / ImportDialog.
// Extension routing, cache reuse (.aydep.json), and convert live in
// AYResource/ImportJob.h (library core; CLI: AYTool/import_tool).

#include <AYResource/IConverter.h>
#include <string>

namespace ayt::editor
{

class Importer
{
public:
    struct MaterialPolicy {
        std::string tag;
        std::string opaqueIndices;
        std::string maskIndices;
        std::string blendIndices;
        std::string doubleSidedIndices;
        std::string opaqueNames;
        std::string maskNames;
        std::string blendNames;
        std::string doubleSidedNames;
        float normalMapYSign = 1.0f;
    };
    // Run one import. Writes the converted files under `destinationDir`
    // (typically the opened project's `.ayeditor_cache/assets/` root; see
    // EditorPlayRuntime::resolvePersistentCacheRoot). `sourcePath` must
    // already exist on disk — the IConverter implementation will open
    // it. Returns the ConversionResult from `IConverter::convert()` on
    // success; returns a result with `success=false` and `errorMessage`
    // filled in on failure (extension unknown, file missing, converter
    // threw, etc.). Never throws.
    struct Result {
        ayt::resource::ConversionResult conversion;
        bool        success = false;
        bool        usedCache = false;  // true when .aydep.json reused (no convert)
        std::string errorMessage;
    };

    // Import or reuse cache. If `<destinationDir>/<basename>.aydep.json`
    // exists and is not older than `sourcePath`, loads ConversionResult
    // from the sidecar and skips FBX convert (important for multi-minute
    // MMD/character FBX). Set env `AY_EDITOR_FORCE_IMPORT=1` to force
    // a full re-convert.
    static Result importFile(const std::string& sourcePath,
                             const std::string& destinationDir,
                             const MaterialPolicy& materialPolicy = {},
                             const ayt::resource::SourceCoordinatePolicy& sourceCoordinates = {});

    // Import an animation source without cooking any scene meshes, materials,
    // textures or helper geometry. The resulting clip is intended to target a
    // separately imported character skeleton with identical bone names.
    static Result importAnimationFile(
        const std::string& sourcePath,
        const std::string& destinationDir,
        const ayt::resource::SourceCoordinatePolicy& sourceCoordinates = {});

    // Generic Content Browser import. Unlike importFile(), this does not
    // require a character skeleton/mesh set and therefore accepts ordinary
    // static models and every other extension supported by AYResource.
    static Result importAssetFile(
        const std::string& sourcePath,
        const std::string& destinationDir,
        const ayt::resource::SourceCoordinatePolicy& sourceCoordinates = {});

    // Lowercase the extension of `path` (.fbx / .FBX → "fbx"). Returns
    // empty string if no extension found.
    static std::string extensionOf(const std::string& path);

    // True when extensionOf(sourcePath) is in the supported table. Use
    // this in the UI to enable/disable the "Import" button before the
    // user picks a file.
    static bool isSupportedExtension(const std::string& sourcePath);
};

} // namespace ayt::editor
