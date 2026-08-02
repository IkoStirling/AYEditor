#pragma once
// AYImporter.h — Phase 1 ED-01.
//
// Thin orchestrator around AYResource::IConverter. Hides the
// `IConverter::create → setOutputDir → convert` dance so ED-01 only
// touches one type. Returns the same `ConversionResult` struct
// (defined in `AYResource/interface/IAYConverter.h:13-34`) so callers
// can show what files were produced and report errors via `ayt::log`.
//
// All extension routing lives here (we mirror the factory's table at
// `IAYConverter.cpp:30-47` so the user gets a clean error for
// unsupported types instead of an opaque nullptr from inside the
// factory). Adding a new source format means (a) implementing the
// converter, and (b) extending this switch.

#include <IAYConverter.h>
#include <string>

namespace ayt::editor
{

class Importer
{
public:
    // Run one import. Writes the converted files under `destinationDir`
    // (typically the editor's `ayeditor_cache/assets/` root; see
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
                             const std::string& destinationDir);

    // Lowercase the extension of `path` (.fbx / .FBX → "fbx"). Returns
    // empty string if no extension found.
    static std::string extensionOf(const std::string& path);

    // True when extensionOf(sourcePath) is in the supported table. Use
    // this in the UI to enable/disable the "Import" button before the
    // user picks a file.
    static bool isSupportedExtension(const std::string& sourcePath);
};

} // namespace ayt::editor
