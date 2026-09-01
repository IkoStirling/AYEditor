#pragma once

#include "AYEditor/EditorAssetDatabase.h"
#include "AYMath/MathTypes.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ayt::editor {

// Visual grouping is intentionally coarser than EditorAssetType. The exact
// type is written into the tile strip, while the strip color lets related
// resources share a stable visual family.
enum class EditorAssetTileCategory : std::uint8_t {
    Folder = 0,
    Geometry,
    Surface,
    Texture,
    Scene,
    Motion,
    Code,
    Audio,
    UserInterface,
    Other,
};

// AYEditor-owned, renderer-agnostic description of one asset tile. It carries
// presentation data only: no widget state, input callbacks, preview loading or
// filesystem mutation belongs here.
struct EditorAssetTilePresentation {
    EditorAssetId assetId = 0;
    EditorAssetType assetType = EditorAssetType::Unknown;
    EditorAssetTileCategory category = EditorAssetTileCategory::Other;
    bool folder = false;

    // The complete display name, including its extension. Inline rename may
    // split this later, but the presenter never removes or edits the suffix.
    std::wstring fullFileName;
    std::wstring typeAbbreviation;
    ayt::math::FVector4 categoryColor{0.42f, 0.45f, 0.50f, 1.0f};

    // True only for engine-native file formats. Imported source PNG/FBX files
    // do not gain the marker merely because they live under Imported.
    bool showEngineResourceMarker = false;
};

class EditorAssetTilePresenter final {
public:
    EditorAssetTilePresentation present(
        const EditorAssetRecord& record) const;
    EditorAssetTilePresentation present(
        const EditorAssetEntry& entry) const;

    static const wchar_t* typeAbbreviation(
        EditorAssetType type) noexcept;
    static EditorAssetTileCategory categoryFor(
        EditorAssetType type) noexcept;
    static ayt::math::FVector4 categoryColor(
        EditorAssetTileCategory category) noexcept;

    // Public so the future tile/view adapter and tests share the same marker
    // policy instead of duplicating an extension table in UI code.
    static bool isEngineNativeFileName(
        std::string_view fileName) noexcept;
};

} // namespace ayt::editor
