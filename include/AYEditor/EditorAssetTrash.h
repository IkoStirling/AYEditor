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

// Project-scoped recoverable deletion. Files are moved below
// .ayeditor/trash and can be restored as one transaction.
class EditorAssetTrash final {
public:
    explicit EditorAssetTrash(std::string projectRoot = {});

    void setProjectRoot(std::string projectRoot);
    EditorAssetTrashResult moveToTrash(
        const std::vector<EditorAssetRecord>& records);
    EditorAssetTrashResult restoreLast();
    bool canRestoreLast() const noexcept { return !_last.empty(); }

private:
    struct Entry {
        std::string originalPath;
        std::string trashPath;
    };

    std::string _projectRoot;
    std::string _lastTransactionPath;
    std::vector<Entry> _last;
};

} // namespace ayt::editor
