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

/// Creates valid authoring documents in the project's Assets tree and returns
/// their browser paths. AYStats creation also writes a starter definition,
/// recipe book, source manifest, and cooked preview resource as one asset set.
/// Names are made unique so a menu action never overwrites existing content.
EditorProjectAssetCreateResult createEditorProjectAsset(
    const std::string& projectRoot, EditorAssetType type,
    const std::string& parentActorClassPath = {});

} // namespace ayt::editor
