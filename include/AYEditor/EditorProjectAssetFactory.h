#pragma once

#include "AYEditor/EditorAssetDatabase.h"

#include <string>

namespace ayt::editor {

struct EditorProjectAssetCreateResult {
    bool success = false;
    EditorAssetType type = EditorAssetType::Unknown;
    std::string absolutePath;
    std::string logicalPath;
    std::string error;

    explicit operator bool() const noexcept { return success; }
};

// Creates valid authoring documents in the conventional project folders.
// Names are made unique, so a menu action never overwrites existing content.
EditorProjectAssetCreateResult createEditorProjectAsset(
    const std::string& projectRoot, EditorAssetType type);

} // namespace ayt::editor
