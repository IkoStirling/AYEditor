#pragma once

#include "AYEditor/EditorAssetDatabase.h"

#include <string>
#include <vector>

namespace ayt::editor {

struct EditorAssetTrashResult {
    std::size_t moved = 0;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

struct EditorAssetTrashEntry {
    std::string originalPath;
    std::string trashPath;
};

struct EditorAssetTrashTransaction {
    std::string id;
    std::vector<EditorAssetTrashEntry> entries;
};

// Project-scoped recoverable deletion. Files are moved below
// .ayeditor/trash and can be restored as one transaction.
class EditorAssetTrash final {
public:
    explicit EditorAssetTrash(std::string projectRoot = {});

    void setProjectRoot(std::string projectRoot);
    EditorAssetTrashResult moveToTrash(
        const std::vector<EditorAssetRecord>& records);
    EditorAssetTrashResult restoreLast();
    EditorAssetTrashResult restore(const std::string& transactionId);
    EditorAssetTrashResult purge(const std::string& transactionId);
    EditorAssetTrashResult purgeAll();
    const std::vector<EditorAssetTrashTransaction>& transactions() const noexcept {
        return _transactions;
    }
    bool canRestoreLast() const noexcept { return !_transactions.empty(); }

private:
    void reload();

    std::string _projectRoot;
    std::vector<EditorAssetTrashTransaction> _transactions;
};

} // namespace ayt::editor
