// AYImporter.cpp — Phase 1 ED-01 implementation.
//
// Stage 1 of editor wiring. Wraps AYResource::IConverter with a
// top-level `Importer::importFile(...)` entry so the AYEditor +
// future tools don't need to know about the extension switch inside
// the factory.
//
// Cache layout: see AYEditor/docs/cache-path-convention.md.
// Callers pass `<cacheRoot>/assets/` as `destinationDir`; FBXConverter
// fans out into `<cacheRoot>/assets/{meshes,materials,skeletons,
// animations,textures}/` plus a `<basename>.aydep.json` sidecar.
//
// Cache reuse (2026-07-27): when the sidecar exists and is not older
// than the source FBX, we load ConversionResult::fromJson and skip
// convert(). Large MMD-origin FBX (e.g. Sour) can take ~2 minutes;
// second launch must not pay that cost. Force with AY_EDITOR_FORCE_IMPORT=1.

#include "AYImporter.h"

#include <ayio/File.h>
#include <ayio/Path.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace ayt::editor
{

namespace {

bool fileExists(const std::string& p) {
    if (p.empty()) return false;
    return ayt::io::File::exists(p);
}

std::string stemOf(const std::string& path) {
    std::string base = path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    return base;
}

std::string joinDirFile(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    if (last == '/' || last == '\\') {
        return dir + file;
    }
    return dir + "/" + file;
}

bool forceImportRequested() {
    const char* v = std::getenv("AY_EDITOR_FORCE_IMPORT");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// True when sidecar is usable: exists, parses, has at least one Mesh,
// and is not older than the source file.
bool tryLoadCachedConversion(const std::string& sourcePath,
                             const std::string& destinationDir,
                             ayt::resource::ConversionResult& out)
{
    const std::string baseName = stemOf(sourcePath);
    if (baseName.empty()) {
        return false;
    }
    const std::string depPath = joinDirFile(destinationDir, baseName + ".aydep.json");
    if (!fileExists(depPath)) {
        return false;
    }

    const uint64_t srcMtime = ayt::io::File::lastModifiedTime(sourcePath);
    const uint64_t depMtime = ayt::io::File::lastModifiedTime(depPath);
    if (srcMtime != 0 && depMtime != 0 && depMtime < srcMtime) {
        std::fprintf(stderr,
                     "[Importer] cache stale (fbx newer than %s); reconverting\n",
                     depPath.c_str());
        return false;
    }

    const std::string json = ayt::io::File::readAllText(depPath);
    if (json.empty()) {
        return false;
    }

    out = ayt::resource::ConversionResult::fromJson(json);
    bool hasMesh = false;
    bool hasSkel = false;
    for (const auto& res : out.resources) {
        std::string t = res.type;
        for (char& c : t) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (t == "mesh" && !res.path.empty()) hasMesh = true;
        if (t == "skeleton" && !res.path.empty()) hasSkel = true;
    }
    if (!hasMesh || !hasSkel) {
        std::fprintf(stderr,
                     "[Importer] cache %s missing Mesh/Skeleton; reconverting\n",
                     depPath.c_str());
        return false;
    }

    // Spot-check that the primary mesh file still exists on disk.
    for (const auto& res : out.resources) {
        std::string t = res.type;
        for (char& c : t) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (t == "mesh" && !res.path.empty()) {
            const std::string abs = joinDirFile(destinationDir, res.path);
            if (!fileExists(abs)) {
                std::fprintf(stderr,
                             "[Importer] cached mesh missing on disk (%s); reconverting\n",
                             abs.c_str());
                return false;
            }
            break;
        }
    }

    return true;
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

    if (!forceImportRequested() &&
        tryLoadCachedConversion(sourcePath, destinationDir, r.conversion)) {
        r.success = true;
        r.usedCache = true;
        std::fprintf(stderr,
                     "[Importer] reusing cache for '%s' (skip convert; "
                     "set AY_EDITOR_FORCE_IMPORT=1 to rebuild)\n",
                     sourcePath.c_str());
        return r;
    }

    std::fprintf(stderr,
                 "[Importer] converting '%s' → '%s' "
                 "(large FBX may take ~1–2 minutes; please wait)...\n",
                 sourcePath.c_str(), destinationDir.c_str());
    std::fflush(stderr);

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
    r.usedCache = false;
    std::fprintf(stderr,
                 "[Importer] convert finished: resources=%zu\n",
                 r.conversion.resources.size());
    std::fflush(stderr);
    return r;
}

} // namespace ayt::editor
