// AYImportedCharacterMapper.cpp - Phase 1 G1 implementation.
//
// Pure helper. Walks a ConversionResult, picks first-of-type (plus all
// Mesh entries), joins virtual paths against cacheRoot to produce
// absolute paths.

#include "AYImportedCharacterMapper.h"

#include <algorithm>
#include <cctype>

namespace ayt::editor
{

namespace {

std::string stripTrailingSeparator(const std::string& s)
{
    if (s.empty()) return s;
    const char last = s.back();
    if (last == '/' || last == '\\') {
        return s.substr(0, s.size() - 1);
    }
    return s;
}

std::string lowerCopy(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

const std::string& pickFirst(const std::vector<ayt::resource::ConversionResult::ConvertedResource>& v,
                             const char* typeWanted)
{
    static const std::string kMiss;
    const std::string wanted = lowerCopy(typeWanted);
    for (const auto& res : v) {
        if (lowerCopy(res.type) == wanted && !res.path.empty()) {
            return res.path;
        }
    }
    return kMiss;
}

std::string joinAssetPath(const std::string& cacheRoot, const std::string& virtualPath)
{
    const std::string root = stripTrailingSeparator(cacheRoot);
    std::string rel = virtualPath;
    // ConversionResult paths are usually "meshes/Foo.aymesh". Some
    // writers prefix "assets/" — strip so we don't double-join.
    if (rel.size() >= 7) {
        const std::string head = lowerCopy(rel.substr(0, 7));
        if (head == "assets/" || head == "assets\\") {
            rel = rel.substr(7);
        }
    }
    if (root.empty()) {
        return std::string("assets/") + rel;
    }
    return root + "/assets/" + rel;
}

} // namespace

ImportedCharacter mapConversionToImportedCharacter(
    const ayt::resource::ConversionResult& result,
    const std::string& cacheRoot,
    ImportedCharacterMapDiagnostics& outDiag)
{
    outDiag = ImportedCharacterMapDiagnostics{};

    const std::string& meshRef     = pickFirst(result.resources, "Mesh");
    const std::string& materialRef = pickFirst(result.resources, "Material");
    const std::string& skelRef     = pickFirst(result.resources, "Skeleton");
    const std::string& animRef     = pickFirst(result.resources, "Animation");

    ImportedCharacter out;
    if (!meshRef.empty())
        out.meshPath      = joinAssetPath(cacheRoot, meshRef);
    if (!materialRef.empty())
        out.materialPath  = joinAssetPath(cacheRoot, materialRef);
    if (!skelRef.empty())
        out.skeletonPath  = joinAssetPath(cacheRoot, skelRef);
    if (!animRef.empty())
        out.animationPath = joinAssetPath(cacheRoot, animRef);

    // Collect every Mesh after the first (MMD body/hair/parts).
    bool sawFirstMesh = false;
    for (const auto& res : result.resources) {
        if (lowerCopy(res.type) != "mesh" || res.path.empty()) {
            continue;
        }
        if (!sawFirstMesh) {
            sawFirstMesh = true;
            continue;
        }
        out.additionalMeshPaths.push_back(joinAssetPath(cacheRoot, res.path));
    }

    if (meshRef.empty())     outDiag.missing.emplace_back("Mesh");
    if (materialRef.empty()) outDiag.missing.emplace_back("Material");
    if (skelRef.empty())     outDiag.missing.emplace_back("Skeleton");
    if (animRef.empty())     outDiag.missing.emplace_back("Animation");

    // Mesh + Skeleton are enough to show a bind-pose skinned character.
    outDiag.success = out.isValid();
    return out;
}

} // namespace ayt::editor
