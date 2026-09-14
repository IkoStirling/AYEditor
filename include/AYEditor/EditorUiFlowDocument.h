#pragma once

#include "AYEditor/EditorCommandSystem.h"
#include "AYEditor/EditorExtension.h"
#include "AYEditorCommand/EditorCommandHistory.h"

#include <AYUI/UIFlow.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::editor {

enum class EditorUiFlowObjectKind : std::uint8_t {
    Document,
    Layer,
    Slot,
    Screen,
    Context,
    Entry,
    Signal,
    Action,
    Region,
    State,
    Transition,
    Graph,
};

struct EditorUiFlowSelection {
    EditorUiFlowObjectKind kind = EditorUiFlowObjectKind::Document;
    std::string id;
    // State IDs are local to a Region. Empty for every other kind.
    std::string ownerId;

    friend bool operator==(const EditorUiFlowSelection&,
                           const EditorUiFlowSelection&) = default;
};

struct EditorUiFlowOutlineItem {
    EditorUiFlowSelection selection;
    std::string label;
    int depth = 0;
};

// Fixed authoring projection used by the inspector. Field meaning is selected
// by kind and exposed through fieldLabels(); this keeps the editor UI generic
// without inventing a second serialized representation.
struct EditorUiFlowProperties {
    std::string id;
    std::string first;
    std::string second;
    std::string third;
    std::string fourth;
    std::string fifth;
    std::string sixth;
    std::string seventh;
    std::int32_t number = 0;
    bool flag = false;
};

struct EditorUiFlowPropertyLabels {
    std::string first;
    std::string second;
    std::string third;
    std::string fourth;
    std::string fifth;
    std::string sixth;
    std::string seventh;
    std::string number;
    std::string flag;
};

class EditorUiFlowDocument final : public IEditorDocument,
                                   public IEditorCommandTarget {
public:
    using ChangedHandler = std::function<void()>;

    EditorUiFlowDocument();
    ~EditorUiFlowDocument();
    EditorUiFlowDocument(const EditorUiFlowDocument&) = delete;
    EditorUiFlowDocument& operator=(const EditorUiFlowDocument&) = delete;

    bool initialize(const std::string& path,
                    const std::string& displayPath,
                    std::string* error = nullptr);
    void createNew();

    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _history.isDirty(); }
    std::uint64_t revision() const noexcept override { return _revision; }

    bool save(std::string* error = nullptr) override;
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path,
                std::string* error = nullptr) override;
    bool canReload() const noexcept override { return !_path.empty(); }
    bool reload(std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;

    const ayt::ui::UIFlowDocument& flow() const noexcept { return _flow; }
    const std::vector<ayt::ui::UIFlowDiagnostic>& diagnostics() const noexcept {
        return _diagnostics;
    }
    bool isValid() const noexcept { return _valid; }

    const EditorUiFlowSelection& selection() const noexcept {
        return _selection;
    }
    bool select(EditorUiFlowSelection selection);
    std::vector<EditorUiFlowOutlineItem> outline() const;

    EditorUiFlowProperties selectedProperties() const;
    EditorUiFlowPropertyLabels selectedPropertyLabels() const;
    bool applySelectedProperties(const EditorUiFlowProperties& properties,
                                 std::string* error = nullptr);

    bool addObject(EditorUiFlowObjectKind kind,
                   std::string ownerId = {},
                   std::string* error = nullptr);
    bool addGraphNode(std::string graphId,
                      std::string nodeType,
                      std::string* error = nullptr);
    bool connectGraphNodes(std::string graphId,
                           std::string fromNode,
                           std::string fromPin,
                           std::string toNode,
                           std::string toPin,
                           std::string* error = nullptr);
    bool deleteSelection(std::string* error = nullptr);

    bool canUndo() const noexcept { return _history.canUndo(); }
    bool canRedo() const noexcept { return _history.canRedo(); }
    bool undo();
    bool redo();
    EditorCommandHistory& commandHistory() noexcept { return _history; }
    const EditorCommandHistory& commandHistory() const noexcept {
        return _history;
    }

    bool handlesCommand(const std::string& commandId) const override;
    bool canExecuteCommand(const std::string& commandId) const override;
    bool executeCommand(const std::string& commandId) override;

    void setChangedHandler(ChangedHandler handler) {
        _changed = std::move(handler);
    }

    static const char* kindName(EditorUiFlowObjectKind kind) noexcept;

private:
    class SnapshotCommand;
    friend class SnapshotCommand;
    struct Snapshot {
        ayt::ui::UIFlowDocument flow;
        EditorUiFlowSelection selection;
    };

    bool loadFromPath(const std::string& path,
                      const std::string& displayPath,
                      std::string* error);
    bool writeToPath(const std::string& path,
                     bool updateIdentity,
                     std::string* error) const;
    void updateTitle(const std::string& displayPath = {});
    void refreshDiagnostics();
    void commitMutation(Snapshot before, std::string label = "Edit UI Flow");
    Snapshot snapshot() const;
    void restore(Snapshot value);
    void onHistoryChanged();
    bool selectionExists(const EditorUiFlowSelection& selection) const;
    std::string uniqueId(EditorUiFlowObjectKind kind,
                         std::string_view ownerId = {}) const;
    bool renameSelection(std::string newId, std::string* error);
    bool selectionIsReferenced(std::string& reference) const;
    void notifyChanged();

    std::string _typeId = "ayeditor.ui-flow.document";
    std::string _path;
    std::string _title = "Untitled UI Flow";
    ayt::ui::UIFlowDocument _flow;
    std::vector<ayt::ui::UIFlowDiagnostic> _diagnostics;
    EditorUiFlowSelection _selection;
    EditorCommandHistory _history{100u};
    ChangedHandler _changed;
    bool _valid = false;
    std::uint64_t _revision = 1;
};

} // namespace ayt::editor
