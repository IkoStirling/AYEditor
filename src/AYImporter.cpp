// AYImporter.cpp — thin Editor façade over AYResource::importAsset (P5).
//
// Cache layout: see AYEditor/docs/cache-path-convention.md.
// Force rebuild: env AY_EDITOR_FORCE_IMPORT=1.

#include "AYEditor/Importer.h"

#include "AYResource/ImportJob.h"

#include <AYIO/Env.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace ayt::editor
{

namespace {

bool forceImportRequested()
{
    const std::string v = ayt::io::env::get("AY_EDITOR_FORCE_IMPORT").value_or("");
    return !v.empty() && v[0] != '0';
}

bool cookTexturesRequested()
{
    // Release-cook override: AY_IMPORT_COOK_TEXTURES=1 runs the full
    // BC7+mips → .aytex pipeline. Default (dev) references raw texture
    // files (png/jpg/...) and copies them into textures/ instead — see
    // ImportOptions::cookTextures.
    const std::string v = ayt::io::env::get("AY_IMPORT_COOK_TEXTURES").value_or("");
    return !v.empty() && v[0] != '0';
}

} // namespace

std::string Importer::extensionOf(const std::string& path)
{
    return ayt::resource::importExtensionOf(path);
}

bool Importer::isSupportedExtension(const std::string& sourcePath)
{
    return ayt::resource::isImportSupportedExtension(sourcePath);
}

Importer::Result Importer::importFile(const std::string& sourcePath,
                                      const std::string& destinationDir,
                                      const MaterialPolicy& materialPolicy,
                                      const ayt::resource::SourceCoordinatePolicy& sourceCoordinates)
{
    ayt::resource::ImportOptions opts;
    opts.sourcePath = sourcePath;
    opts.outputDir = destinationDir;
    opts.force = forceImportRequested();
    opts.requireCharacterAssets = true;
    opts.loadOption = ayt::resource::IConverter::LoadOption::Full;
    opts.cookTextures = cookTexturesRequested();
    opts.materialPolicy.tag = materialPolicy.tag
        + (materialPolicy.normalMapYSign < 0.0f
            ? "|normal-map-y=-1" : "|normal-map-y=+1");
    opts.materialPolicy.opaqueIndices = materialPolicy.opaqueIndices;
    opts.materialPolicy.maskIndices = materialPolicy.maskIndices;
    opts.materialPolicy.blendIndices = materialPolicy.blendIndices;
    opts.materialPolicy.doubleSidedIndices = materialPolicy.doubleSidedIndices;
    opts.materialPolicy.opaqueNames = materialPolicy.opaqueNames;
    opts.materialPolicy.maskNames = materialPolicy.maskNames;
    opts.materialPolicy.blendNames = materialPolicy.blendNames;
    opts.materialPolicy.doubleSidedNames = materialPolicy.doubleSidedNames;
    opts.materialPolicy.normalMapYSign = materialPolicy.normalMapYSign;
    opts.sourceCoordinates = sourceCoordinates;

    const ayt::resource::ImportResult core = ayt::resource::importAsset(opts);

    Result r;
    r.conversion = core.conversion;
    r.success = core.ok;
    r.usedCache = core.usedCache;
    r.errorMessage = core.error;
    if (core.cancelled && r.errorMessage.empty()) {
        r.errorMessage = "import cancelled";
    }
    return r;
}

Importer::Result Importer::importAnimationFile(
    const std::string& sourcePath,
    const std::string& destinationDir,
    const ayt::resource::SourceCoordinatePolicy& sourceCoordinates)
{
    ayt::resource::ImportOptions opts;
    opts.sourcePath = sourcePath;
    opts.outputDir = destinationDir;
    opts.force = forceImportRequested();
    opts.requireCharacterAssets = false;
    opts.requireAnimationAssets = true;
    opts.loadOption = ayt::resource::IConverter::LoadOption::AnimationOnly;
    opts.cookTextures = false;
    opts.sourceCoordinates = sourceCoordinates;

    const ayt::resource::ImportResult core = ayt::resource::importAsset(opts);

    Result r;
    r.conversion = core.conversion;
    r.success = core.ok;
    r.usedCache = core.usedCache;
    r.errorMessage = core.error;
    if (r.success) {
        bool hasAnimation = false;
        for (const auto& resource : r.conversion.resources) {
            std::string type = resource.type;
            std::transform(type.begin(), type.end(), type.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (type == "animation" && !resource.path.empty()) {
                hasAnimation = true;
                break;
            }
        }
        if (!hasAnimation) {
            r.success = false;
            r.errorMessage = "animation source produced no Animation resource";
        }
    }
    if (core.cancelled && r.errorMessage.empty()) {
        r.errorMessage = "import cancelled";
    }
    return r;
}

Importer::Result Importer::importAssetFile(
    const std::string& sourcePath,
    const std::string& destinationDir,
    const ayt::resource::SourceCoordinatePolicy& sourceCoordinates,
    bool force)
{
    ayt::resource::ImportOptions opts;
    opts.sourcePath = sourcePath;
    opts.outputDir = destinationDir;
    opts.force = force || forceImportRequested();
    opts.requireCharacterAssets = false;
    opts.requireAnimationAssets = false;
    opts.loadOption = ayt::resource::IConverter::LoadOption::Full;
    opts.cookTextures = cookTexturesRequested();
    opts.sourceCoordinates = sourceCoordinates;

    const ayt::resource::ImportResult core = ayt::resource::importAsset(opts);
    Result r;
    r.conversion = core.conversion;
    r.success = core.ok;
    r.usedCache = core.usedCache;
    r.errorMessage = core.error;
    if (core.cancelled && r.errorMessage.empty()) {
        r.errorMessage = "import cancelled";
    }
    return r;
}

} // namespace ayt::editor
