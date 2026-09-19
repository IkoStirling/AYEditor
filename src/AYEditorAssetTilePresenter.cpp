#include "AYEditor/EditorAssetTilePresenter.h"

#include "AYUI/UnicodeText.h"
#include <AYAnimationEditor/SkeletonEditorCore.h>

#include <array>

namespace ayt::editor {

namespace {

bool endsWithAsciiInsensitive(std::string_view text,
                              std::string_view suffix) noexcept
{
    if (suffix.size() > text.size()) return false;
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        unsigned char left = static_cast<unsigned char>(text[offset + i]);
        unsigned char right = static_cast<unsigned char>(suffix[i]);
        if (left >= 'A' && left <= 'Z') left = left - 'A' + 'a';
        if (right >= 'A' && right <= 'Z') right = right - 'A' + 'a';
        if (left != right) return false;
    }
    return true;
}

EditorAssetTilePresentation makePresentation(
    EditorAssetId assetId, EditorAssetType assetType, bool folder,
    const std::string& fullFileName)
{
    EditorAssetTilePresentation result;
    result.assetId = assetId;
    result.assetType = assetType;
    result.folder = folder;
    result.fullFileName = ayt::ui::decodeUtf8Text(fullFileName);
    result.typeAbbreviation = folder
        ? L"DIR" : EditorAssetTilePresenter::typeAbbreviation(assetType);
    result.category = folder
        ? EditorAssetTileCategory::Folder
        : EditorAssetTilePresenter::categoryFor(assetType);
    result.categoryColor =
        EditorAssetTilePresenter::categoryColor(result.category);
    result.showEngineResourceMarker = !folder
        && EditorAssetTilePresenter::isEngineNativeFileName(fullFileName);
    return result;
}

} // namespace

EditorAssetTilePresentation EditorAssetTilePresenter::present(
    const EditorAssetRecord& record) const
{
    EditorAssetTilePresentation result =
        makePresentation(record.id, record.type, false, record.name);
    if (record.type == EditorAssetType::Skeleton
        || record.type == EditorAssetType::SkeletonMapping) {
        const auto status = ayt::anim::editor::SkeletonEditorCore::inspectStatus(
            record.absolutePath);
        using A = ayt::anim::editor::SkeletonAdaptationState;
        using B = ayt::anim::editor::SkeletonBakeState;
        switch (status.adaptation) {
        case A::Unmapped: result.adaptationBadge = L"M:--"; break;
        case A::Incomplete: result.adaptationBadge = L"M:!"; break;
        case A::Validated: result.adaptationBadge = L"M:OK"; break;
        case A::Native: result.adaptationBadge = L"M:N"; break;
        }
        switch (status.bake) {
        case B::NotBaked: result.bakeBadge = L"B:--"; break;
        case B::Stale: result.bakeBadge = L"B:!"; break;
        case B::Ready: result.bakeBadge = L"B:OK"; break;
        case B::Failed: result.bakeBadge = L"B:X"; break;
        }
        // Mapping remains in the bottom information strip while bake is
        // rendered by the view as an independent top badge.
        result.typeAbbreviation += L" " + result.adaptationBadge;
    }
    return result;
}

EditorAssetTilePresentation EditorAssetTilePresenter::present(
    const EditorAssetEntry& entry) const
{
    return makePresentation(entry.assetId, entry.type, entry.folder,
                            entry.displayName);
}

const wchar_t* EditorAssetTilePresenter::typeAbbreviation(
    EditorAssetType type) noexcept
{
    switch (type) {
    case EditorAssetType::Mesh: return L"MESH";
    case EditorAssetType::Material: return L"MAT";
    case EditorAssetType::Texture: return L"TEX";
    case EditorAssetType::Scene: return L"SCN";
    case EditorAssetType::Animation: return L"ANIM";
    case EditorAssetType::Skeleton: return L"SKEL";
    case EditorAssetType::Script: return L"SCR";
    case EditorAssetType::Shader: return L"SHDR";
    case EditorAssetType::Audio: return L"AUD";
    case EditorAssetType::UiLayout: return L"UI";
    case EditorAssetType::SourceModel: return L"MODEL";
    case EditorAssetType::Tilemap: return L"MAP";
    case EditorAssetType::UiFlow: return L"UIFLOW";
    case EditorAssetType::GameFlow: return L"FLOW";
    case EditorAssetType::SkeletonMapping: return L"RIG";
    case EditorAssetType::Unknown: break;
    }
    return L"FILE";
}

EditorAssetTileCategory EditorAssetTilePresenter::categoryFor(
    EditorAssetType type) noexcept
{
    switch (type) {
    case EditorAssetType::Mesh:
    case EditorAssetType::SourceModel:
        return EditorAssetTileCategory::Geometry;
    case EditorAssetType::Material:
        return EditorAssetTileCategory::Surface;
    case EditorAssetType::Texture:
        return EditorAssetTileCategory::Texture;
    case EditorAssetType::Scene:
        return EditorAssetTileCategory::Scene;
    case EditorAssetType::Animation:
    case EditorAssetType::Skeleton:
    case EditorAssetType::SkeletonMapping:
        return EditorAssetTileCategory::Motion;
    case EditorAssetType::Script:
    case EditorAssetType::Shader:
        return EditorAssetTileCategory::Code;
    case EditorAssetType::Audio:
        return EditorAssetTileCategory::Audio;
    case EditorAssetType::UiLayout:
    case EditorAssetType::UiFlow:
        return EditorAssetTileCategory::UserInterface;
    case EditorAssetType::Tilemap:
        return EditorAssetTileCategory::Scene;
    case EditorAssetType::GameFlow:
        return EditorAssetTileCategory::Code;
    case EditorAssetType::Unknown:
        break;
    }
    return EditorAssetTileCategory::Other;
}

ayt::math::FVector4 EditorAssetTilePresenter::categoryColor(
    EditorAssetTileCategory category) noexcept
{
    switch (category) {
    case EditorAssetTileCategory::Folder:
        return {0.35f, 0.50f, 0.68f, 1.0f};
    case EditorAssetTileCategory::Geometry:
        return {0.18f, 0.48f, 0.78f, 1.0f};
    case EditorAssetTileCategory::Surface:
        return {0.56f, 0.36f, 0.76f, 1.0f};
    case EditorAssetTileCategory::Texture:
        return {0.18f, 0.62f, 0.46f, 1.0f};
    case EditorAssetTileCategory::Scene:
        return {0.86f, 0.48f, 0.18f, 1.0f};
    case EditorAssetTileCategory::Motion:
        return {0.76f, 0.34f, 0.58f, 1.0f};
    case EditorAssetTileCategory::Code:
        return {0.78f, 0.64f, 0.20f, 1.0f};
    case EditorAssetTileCategory::Audio:
        return {0.16f, 0.60f, 0.66f, 1.0f};
    case EditorAssetTileCategory::UserInterface:
        return {0.34f, 0.44f, 0.80f, 1.0f};
    case EditorAssetTileCategory::Other:
        break;
    }
    return {0.42f, 0.45f, 0.50f, 1.0f};
}

bool EditorAssetTilePresenter::isEngineNativeFileName(
    std::string_view fileName) noexcept
{
    // Keep compound suffixes before their shorter constituents if the table
    // grows. Metadata sidecars are not listed: the asset database deliberately
    // filters .aydep.json out of the browser.
    constexpr std::array<std::string_view, 16> nativeSuffixes = {
        ".gameflow.json", ".uiflow.json", ".aytilemap.json", ".ui.json", ".aytilemap",
        ".aymesh", ".aymat", ".aytex", ".ayscene",
        ".ayanm", ".ayanim", ".ayskel", ".ayrig", ".aysmap", ".logia", ".phoskia",
    };
    for (const std::string_view suffix : nativeSuffixes) {
        if (endsWithAsciiInsensitive(fileName, suffix)) return true;
    }
    return false;
}

} // namespace ayt::editor
