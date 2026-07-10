// AYImportedCharacterMapper.cpp - Phase 1 G1 implementation.
//
// Pure helper. Walks a ConversionResult, picks first-of-type, joins
// virtual paths against cacheRoot to produce absolute paths.
//
// The forward-slash join is intentional: the on-disk files were
// written by the sub-converters via `<outputDir>/<virtualPath>` with
// forward slashes (see MeshConverter.cpp:202, SkeletonConverter.cpp:
// 100, etc.). On Windows, `ayt::resource::resolveAssetPath` short-
// circuits on absolute paths via `ayt::io::path::normalize`, which
// accepts both slash directions. We emit forward slashes here for
// consistency with the virtual-path strings the sub-converters
// produced; downstream callers don't need to care.

#include "AYImportedCharacterMapper.h"

#include <algorithm>
#include <cctype>

namespace ayt::editor
{

namespace {

// Strip one trailing path separator if present ('/' or '\\').
// Empty input returns empty.
std::string stripTrailingSeparator(const std::string& s)
{
    if (s.empty()) return s;
    const char last = s.back();
    if (last == '/' || last == '\\') {
        return s.substr(0, s.size() - 1);
    }
    return s;
}

// Lowercase for case-insensitive type-string match. The sub-
// converters in AYResource emit canonical capitalization
// ("Mesh" / "Material" / "Skeleton" / "Animation"), but a future
// converter or test fixture could pass lowercase; we accept either.
std::string lowerCopy(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Linear scan for the first resource whose `type` field matches
// `typeWanted` (case-insensitive). Returns the virtual path on hit,
// empty string on miss. Empty `path` strings on a hit are treated as
// misses (defensive: a converter could in theory produce a
// ConvertedResource with `path == ""`).
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

// Build "<cacheRoot>/assets/<virtualPath>" with one slash between
// each component. Empty cacheRoot yields "/assets/<virtualPath>"
// (intentional - unit tests pass "" to check the join logic).
std::string joinAssetPath(const std::string& cacheRoot, const std::string& virtualPath)
{
    const std::string root = stripTrailingSeparator(cacheRoot);
    if (root.empty()) {
        return "assets/" + virtualPath;
    }
    return root + "/assets/" + virtualPath;
}

} // namespace

ImportedCharacter mapConversionToImportedCharacter(
    const ayt::resource::ConversionResult& result,
    const std::string& cacheRoot,
    ImportedCharacterMapDiagnostics& outDiag)
{
    outDiag = ImportedCharacterMapDiagnostics{};

    // Order matters: append-to-missing order is Mesh, Material,
    // Skeleton, Animation. Stable across runs so log lines are
    // grep-friendly.
    //
    // We resolve each type on its own and only call joinAssetPath
    // when pickFirst returned a non-empty virtual path. Joining an
    // empty virtual path would produce "<cacheRoot>/assets/"
    // (directory-only, no filename), which would falsely make
    // out.<field>.empty() == false and could trip ImportedCharacter::
    // isValid() into returning true on a partial fill. Leaving the
    // field empty on miss instead keeps the contract: a hit yields
    // a usable absolute path; a miss leaves the field empty so
    // isValid() correctly reports false on partial conversions.
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

    if (meshRef.empty())     outDiag.missing.emplace_back("Mesh");
    if (materialRef.empty()) outDiag.missing.emplace_back("Material");
    if (skelRef.empty())     outDiag.missing.emplace_back("Skeleton");
    if (animRef.empty())     outDiag.missing.emplace_back("Animation");

    // ImportedCharacter::isValid() requires all four paths non-empty,
    // so this is exactly equivalent to checking that missing is
    // empty. We set success explicitly for clarity in the diagnostic
    // struct; partial fills leave `out` partially populated so a
    // future Inspector (ED-03) can still preview the available bits,
    // but `isValid()` (and therefore the G2 caller + startPlay's
    // trySpawnImportedCharacter) correctly returns false.
    outDiag.success = out.isValid();
    return out;
}

} // namespace ayt::editor