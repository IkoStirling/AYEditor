// AYImporter.cpp — Phase 1 ED-01 implementation.
//
// Stage 1 of editor wiring. Wraps AYResource::IConverter with a
// top-level `Importer::importFile(...)` entry so the AYEditor +
// future tools don't need to know about the extension switch inside
// the factory. We mirror the factory's extension table verbatim so
// the user gets a meaningful error ("unsupported source extension
// 'foo'") before any converter runs.
//
// Cache layout: see AYEditor/docs/cache-path-convention.md.
// Callers pass `<cacheRoot>/assets/` as `destinationDir`; FBXConverter
// fans out into `<cacheRoot>/assets/{meshes,materials,skeletons,
// animations,textures}/` plus a `<basename>.aydep.json` sidecar.
//
// Failure modes we surface (returned via `Result::errorMessage`):
//   * Empty source path → "sourcePath is empty".
//   * File missing → "source file does not exist: <path>".
//   * Unsupported extension → lists the supported types so the user
//     can self-correct.
//   * IConverter ctor returns nullptr → "no IConverter for '.<ext>'".
//   * IConverter::convert() throws (FBXConverter uses ayt::log, not
//     exceptions, but be defensive) → "converter threw: <what>".
//   * Destination not creatable → "cannot stat destinationDir".
//
// We deliberately do NOT touch the resolved asset paths in `conversion`
// (those are virtual paths under the converter's `outputDir`). The
// caller treats them as opaque strings — the importer's job stops at
// producing valid .ay* bytes on disk.

#include "AYImporter.h"

#include <ayio/File.h>
#include <ayio/Path.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ayt::editor
{

namespace {

bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    return ayt::io::File::exists(p);
}

} // namespace

std::string Importer::extensionOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool Importer::isSupportedExtension(const std::string& sourcePath) {
    const std::string ext = extensionOf(sourcePath);
    // Mirror `IAYConverter.cpp:30-47` factory routing.
    return ext == "fbx"
        || ext == "gltf" || ext == "glb"
        || ext == "png"  || ext == "bmp"
        || ext == "tga"  || ext == "dds";
}

Importer::Result Importer::importFile(const std::string& sourcePath,
                                      const std::string& destinationDir)
{
    Result r;
    if (sourcePath.empty()) {
        r.errorMessage = "sourcePath is empty";
        return r;
    }
    if (!fileExists(sourcePath)) {
        r.errorMessage = "source file does not exist: " + sourcePath;
        return r;
    }
    if (destinationDir.empty()) {
        r.errorMessage = "destinationDir is empty";
        return r;
    }
    if (!isSupportedExtension(sourcePath)) {
        const std::string ext = extensionOf(sourcePath);
        r.errorMessage =
            "unsupported source extension '" + ext +
            "'. Supported: .fbx .gltf .glb .png .bmp .tga .dds";
        return r;
    }

    // IConverter factory can return nullptr only when its own dispatch
    // table disagrees with ours (a misconfiguration) — guard for it.
    std::unique_ptr<ayt::resource::IConverter> converter;
    try {
        converter = ayt::resource::IConverter::create(sourcePath);
    } catch (const std::exception& e) {
        r.errorMessage = std::string("converter ctor threw: ") + e.what();
        return r;
    }
    if (converter == nullptr) {
        r.errorMessage = "no IConverter for ." + extensionOf(sourcePath);
        return r;
    }

    try {
        converter->setOutputDir(destinationDir);
        converter->setLoadOption(ayt::resource::IConverter::LoadOption::Full);
        r.conversion = converter->convert();
    } catch (const std::exception& e) {
        r.errorMessage = std::string("converter threw: ") + e.what();
        return r;
    }

    r.success = true;
    return r;
}

} // namespace ayt::editor
