#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::editor {

class IEditorDocument;

struct EditorRecoveryResult {
    std::size_t documents = 0;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

struct EditorRecoveryDocument {
    std::string originalPath;
    std::string recoveryPath;
    std::string title;
};

// Crash marker plus autosave copies. A stale session is rotated into a dated
// recovery transaction before a new lock is created, so a later clean exit
// cannot erase the files that the user may still choose to restore.
class EditorRecoveryStore final {
public:
    explicit EditorRecoveryStore(std::string projectRoot = {});
    ~EditorRecoveryStore();

    EditorRecoveryStore(const EditorRecoveryStore&) = delete;
    EditorRecoveryStore& operator=(const EditorRecoveryStore&) = delete;

    void setProjectRoot(std::string projectRoot);
    bool beginSession(std::string* error = nullptr);
    EditorRecoveryResult autosave(
        const std::vector<const IEditorDocument*>& documents);
    bool hasRecoverableSession() const noexcept { return !_previousRoot.empty(); }
    std::vector<EditorRecoveryDocument> recoverableDocuments() const;
    EditorRecoveryResult restorePrevious();
    EditorRecoveryResult restorePrevious(
        const std::vector<std::size_t>& documentIndices);
    bool discardPrevious(std::string* error = nullptr);
    void markCleanShutdown();

private:
    struct Entry {
        std::string originalPath;
        std::string recoveryName;
        std::string title;
    };

    bool loadPreviousManifest();
    bool writeCurrentManifest(std::string* error);
    bool writePreviousManifest(std::string* error);

    std::string _projectRoot;
    std::string _currentRoot;
    std::string _previousRoot;
    std::vector<Entry> _currentEntries;
    std::vector<Entry> _previousEntries;
    bool _begun = false;
    bool _clean = false;
};

} // namespace ayt::editor
