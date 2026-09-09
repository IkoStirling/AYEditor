#pragma once

#include "AYEditor/EditorAssetDatabase.h"

#include <string>
#include <vector>
#include <cstdint>

namespace ayt::editor {

struct EditorAssetOperationResult {
    std::size_t affectedAssets = 0;
    std::size_t updatedReferences = 0;
    std::vector<std::string> resultingLogicalPaths;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

struct EditorAssetOperationHistoryEntry {
    std::int64_t timestamp = 0;
    std::string operation;
    std::string source;
    std::string destination;
    std::string error;

    bool succeeded() const noexcept { return error.empty(); }
};

// Shared persistent log used by rename/move/copy, trash and recovery actions.
// One entry represents one affected resource so batch operations remain easy
// to inspect without embedding another database in the editor.
std::vector<EditorAssetOperationHistoryEntry> readEditorAssetOperationHistory(
    const std::string& projectRoot);
bool appendEditorAssetOperationHistory(
    const std::string& projectRoot,
    const EditorAssetOperationHistoryEntry& entry);
bool clearEditorAssetOperationHistory(const std::string& projectRoot);

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
