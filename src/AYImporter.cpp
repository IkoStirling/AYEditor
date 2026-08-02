// AYImporter.cpp — thin Editor façade over AYResource::importAsset (P5).
//
// Cache layout: see AYEditor/docs/cache-path-convention.md.
// Force rebuild: env AY_EDITOR_FORCE_IMPORT=1.

#include "AYImporter.h"

#include "AYImportJob.h"

#include <ayio/Env.h>
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
                                      const std::string& destinationDir)
{
    ayt::resource::ImportOptions opts;
    opts.sourcePath = sourcePath;
    opts.outputDir = destinationDir;
    opts.force = forceImportRequested();
    opts.requireCharacterAssets = true;
    opts.loadOption = ayt::resource::IConverter::LoadOption::Full;

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
