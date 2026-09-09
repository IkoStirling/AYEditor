#pragma once

#include "AYEditor/EditorAssetDatabase.h"

#include <string>
#include <vector>

namespace ayt::editor {

struct EditorAssetOperationResult {
    std::size_t affectedAssets = 0;
    std::size_t updatedReferences = 0;
    std::vector<std::string> resultingLogicalPaths;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

// Project-scoped file operations used by every AYEditor host. Move and rename
// update portable/logical references in authored text assets as one
// transaction; copy deliberately preserves references to the original data.
class EditorAssetOperations final {
public:
    explicit EditorAssetOperations(std::string projectRoot = {});

    void setProjectRoot(std::string projectRoot);
    EditorAssetOperationResult rename(
        const EditorAssetDatabase& database,
        const EditorAssetRecord& record,
        const std::string& newFileName) const;
    EditorAssetOperationResult move(
        const EditorAssetDatabase& database,
        const std::vector<EditorAssetRecord>& records,
        const std::string& destinationLogicalFolder) const;
    EditorAssetOperationResult copy(
        const EditorAssetDatabase& database,
        const std::vector<EditorAssetRecord>& records,
        const std::string& destinationLogicalFolder) const;

private:
    std::string _projectRoot;
};

} // namespace ayt::editor
