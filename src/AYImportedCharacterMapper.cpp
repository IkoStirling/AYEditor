// AYImportedCharacterMapper.cpp - Phase 1 G1 implementation.
//
// Pure helper. Walks a ConversionResult, selects character-renderable
// skinned meshes when role metadata is available, and joins virtual paths
// against cacheRoot to produce absolute paths.

#include "AYEditor/ImportedCharacterMapper.h"

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

    const std::string& materialRef = pickFirst(result.resources, "Material");
    const std::string& skelRef     = pickFirst(result.resources, "Skeleton");
    const std::string& animRef     = pickFirst(result.resources, "Animation");

    // Modern sidecars classify every converted mesh. Character spawning must
    // not treat MMD rigid-body/collider helper meshes as renderable body parts.
    // Legacy sidecars have no role metadata, so preserve their original
    // first-plus-rest behavior until they are rebuilt.
    bool hasMeshRoleMetadata = false;
    std::vector<const ayt::resource::ConversionResult::ConvertedResource*>
        characterMeshes;
    for (const auto& res : result.resources) {
        if (lowerCopy(res.type) != "mesh" || res.path.empty()) {
            continue;
        }
        if (!res.role.empty()) {
            hasMeshRoleMetadata = true;
        }
    }
    for (const auto& res : result.resources) {
        if (lowerCopy(res.type) != "mesh" || res.path.empty()) {
            continue;
        }
        if (!hasMeshRoleMetadata || lowerCopy(res.role) == "skinnedmesh") {
            characterMeshes.push_back(&res);
        }
    }

    const std::string meshRef = characterMeshes.empty()
        ? std::string{}
        : characterMeshes.front()->path;

    ImportedCharacter out;
    if (!meshRef.empty())
        out.meshPath      = joinAssetPath(cacheRoot, meshRef);
    if (!materialRef.empty())
        out.materialPath  = joinAssetPath(cacheRoot, materialRef);
    if (!skelRef.empty())
        out.skeletonPath  = joinAssetPath(cacheRoot, skelRef);
    if (!animRef.empty())
        out.animationPath = joinAssetPath(cacheRoot, animRef);

    // Collect every additional skinned body/hair/part. Static helpers are
    // deliberately excluded when modern role metadata is present.
    for (size_t i = 1; i < characterMeshes.size(); ++i) {
        out.additionalMeshPaths.push_back(
            joinAssetPath(cacheRoot, characterMeshes[i]->path));
    }

    if (meshRef.empty())     outDiag.missing.emplace_back("Mesh");
    if (materialRef.empty()) outDiag.missing.emplace_back("Material");
    if (skelRef.empty())     outDiag.missing.emplace_back("Skeleton");
    if (animRef.empty())     outDiag.missing.emplace_back("Animation");

    // Mesh + Skeleton are enough to show a bind-pose skinned character.
    outDiag.success = out.isValid();
    return out;
}

std::string mapFirstAnimationPath(
    const ayt::resource::ConversionResult& result,
    const std::string& cacheRoot)
{
    const std::string& animationRef = pickFirst(result.resources, "Animation");
    return animationRef.empty()
        ? std::string{}
        : joinAssetPath(cacheRoot, animationRef);
}

} // namespace ayt::editor
