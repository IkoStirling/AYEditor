#pragma once
#include <AYEditor/EditorWorkspace.h>
#include <AYUI/Authoring/TimelineModel.h>

namespace ayt::editor {
/** @brief Project opaque authoring selections into the existing Editor selection service.
 * Managed documents use their actual workspace ID; standalone views use a local context.
 * Resolve contexts each time so closed workspace contexts are never retained.
 * This is a one-way projection; domain owners still handle selection and ID semantics.
 */
class EditorAuthoringSelectionBridge {
public:
    EditorAuthoringSelectionBridge(EditorWorkspace& workspace, std::shared_ptr<IEditorDocument> document)
        : _workspace(workspace), _document(std::move(document)),
          _standalone(_document->typeId() + ":" + _document->path()) {}
    EditorSelectionContext* context() noexcept {
        for (const auto& record : _workspace.documents().records()) {
            if (record.document == _document) {
                if (auto* context = _workspace.selections().find(record.documentId)) return context;
            }
        }
        return &_standalone;
    }
    bool publish(const std::string& domain, const std::vector<std::string>& ids, const std::string& primary) {
        auto* selection = context();
        std::vector<EditorObjectRef> items;
        for (const auto& id : ids) items.push_back({selection->documentId(), domain, id});
        const EditorObjectRef main{selection->documentId(), domain, primary};
        return selection->setSelection(std::move(items), primary.empty() ? nullptr : &main);
    }
    bool publish(const ayt::ui::authoring::TimelineSelection& selection) {
        if (!selection.keyIds.empty()) return publish("timeline.key", selection.keyIds, selection.primaryKeyId);
        return publish("timeline.track", selection.trackId.empty() ? std::vector<std::string>{}
            : std::vector<std::string>{selection.trackId}, selection.trackId);
    }
private:
    EditorWorkspace& _workspace;
    std::shared_ptr<IEditorDocument> _document;
    EditorSelectionContext _standalone;
};
} // namespace ayt::editor
