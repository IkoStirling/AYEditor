#include "AYEditor/EditorWorkspace.h"

namespace ayt::editor {

EditorWorkspace::EditorWorkspace()
    : _documents(_registry)
{
    _documentListener = _documents.addListener(
        [this](const EditorDocumentEvent& event) {
            onDocumentEvent(event);
        });
}

EditorWorkspace::~EditorWorkspace()
{
    _documents.removeListener(_documentListener);
}

void EditorWorkspace::onDocumentEvent(const EditorDocumentEvent& event)
{
    switch (event.type) {
    case EditorDocumentEventType::Opened:
        (void)_selections.contextFor(event.documentId);
        break;
    case EditorDocumentEventType::Activated:
        (void)_selections.activate(event.documentId);
        break;
    case EditorDocumentEventType::Closed:
        (void)_selections.remove(event.documentId);
        break;
    }
}

} // namespace ayt::editor
