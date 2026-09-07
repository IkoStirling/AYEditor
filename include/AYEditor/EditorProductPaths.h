#pragma once

#include <filesystem>

namespace ayt::editor {

// One immutable description of the editor product layout.  Installed builds
// resolve every fixed asset from EngineAssets and every writable product path
// from the product root; development builds may use the CMake-provided source
// asset hint without pretending the build tree is an installed product.
struct EditorProductPaths {
    std::filesystem::path productRoot;
    std::filesystem::path engineAssetsRoot;
    std::filesystem::path userWorkspaceRoot;
    std::filesystem::path runtimeRoot;
    std::filesystem::path logsRoot;
    bool portableLayout = false;

    static EditorProductPaths detect();

    std::filesystem::path engineAsset(
        const std::filesystem::path& relative) const;
    std::filesystem::path editorAsset(
        const std::filesystem::path& relative) const;
};

} // namespace ayt::editor
