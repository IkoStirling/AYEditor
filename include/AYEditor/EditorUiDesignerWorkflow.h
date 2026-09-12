#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

struct EditorUiLayoutHandler {
    std::string widgetId;
    std::string eventName;
    std::string handler;
};

struct EditorUiScreenLayoutLink {
    std::string flowPath;
    std::string screenId;
    std::string layoutAsset;
    std::string resolvedLayoutPath;
};

struct EditorUiHandlerCompletion {
    std::string flowPath;
    std::string screenId;
    std::string layoutPath;
    std::string handler;
    std::string suggestedSignal;
    bool createsSignal = false;
};

enum class EditorUiRenameKind {
    LayoutAsset,
    FlowSignal,
    WidgetHandler,
    WidgetId,
};

struct EditorUiRenameRequest {
    EditorUiRenameKind kind = EditorUiRenameKind::WidgetId;
    std::string oldValue;
    std::string newValue;
    // Required for WidgetId/WidgetHandler. Optional Flow path restriction for
    // FlowSignal. LayoutAsset intentionally searches the full project index.
    std::string scopePath;
};

struct EditorUiFileEdit {
    std::string path;
    std::string beforeText;
    std::string afterText;
    std::size_t replacementCount = 0u;
};

struct EditorUiRenamePlan {
    EditorUiRenameRequest request;
    std::vector<EditorUiFileEdit> edits;
    std::vector<std::string> diagnostics;
    bool safe = false;
};

// Project-wide read/plan/apply service shared by UI Layout and UI Flow
// editors. It never owns either editor's in-memory document; callers must save
// dirty documents before applying a file transaction.
class EditorUiDesignerWorkflow {
public:
    explicit EditorUiDesignerWorkflow(std::string assetRoot = {});
    ~EditorUiDesignerWorkflow();

    EditorUiDesignerWorkflow(const EditorUiDesignerWorkflow&) = delete;
    EditorUiDesignerWorkflow& operator=(const EditorUiDesignerWorkflow&) = delete;

    void setAssetRoot(std::string assetRoot);
    const std::string& assetRoot() const noexcept { return _assetRoot; }
    bool refresh(std::string* error = nullptr);
    // Performs a metadata-only scan and reparses the project only when a UI
    // authoring file was added, removed or changed. changedPaths contains
    // normalized absolute paths and is suitable for open-document matching.
    bool refreshIfChanged(bool* changed,
                          std::vector<std::string>* changedPaths = nullptr,
                          std::string* error = nullptr);
    std::uint64_t revision() const noexcept { return _revision; }

    const std::vector<EditorUiScreenLayoutLink>& screenLinks() const {
        return _screenLinks;
    }
    const std::vector<std::string>& diagnostics() const {
        return _diagnostics;
    }
    std::vector<EditorUiScreenLayoutLink> screensForLayout(
        const std::string& layoutPath) const;
    std::vector<EditorUiLayoutHandler> handlersForLayout(
        const std::string& layoutPath) const;
    std::vector<EditorUiHandlerCompletion> handlerCompletions(
        const std::string& flowPath, const std::string& screenId) const;
    bool applyHandlerCompletions(
        const std::string& flowPath, const std::string& screenId,
        const std::vector<std::string>& handlers = {},
        std::size_t* applied = nullptr, std::string* error = nullptr);

    EditorUiRenamePlan planRename(const EditorUiRenameRequest& request) const;
    bool applyRename(const EditorUiRenamePlan& plan,
                     std::string* error = nullptr);

private:
    struct LayoutRecord;
    struct FlowRecord;
    struct FileStamp {
        std::uintmax_t size = 0u;
        std::int64_t writeTick = 0;
        bool operator==(const FileStamp&) const = default;
    };

    const LayoutRecord* findLayout(const std::string& path) const;
    const FlowRecord* findFlow(const std::string& path) const;
    bool captureFileStamps(
        std::unordered_map<std::string, FileStamp>& stamps,
        std::string* error) const;

    std::string _assetRoot;
    std::vector<LayoutRecord> _layouts;
    std::vector<FlowRecord> _flows;
    std::vector<EditorUiScreenLayoutLink> _screenLinks;
    std::vector<std::string> _diagnostics;
    std::unordered_map<std::string, FileStamp> _fileStamps;
    std::uint64_t _revision = 0u;
};

} // namespace ayt::editor
