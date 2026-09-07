#pragma once

#include "AYEditor/EditorCommandSystem.h"
#include "AYEditor/EditorDocumentManager.h"
#include "AYEditor/EditorSelectionContext.h"

namespace ayt::editor {

// Service root for editor extensions. This initial implementation is model
// only: it deliberately does not take ownership of DockArea, UIManager,
// renderer, native windows, or module-specific editor sessions.
class EditorWorkspace {
public:
    EditorWorkspace();
    ~EditorWorkspace();

    EditorWorkspace(const EditorWorkspace&) = delete;
    EditorWorkspace& operator=(const EditorWorkspace&) = delete;

    EditorExtensionRegistry& registry() noexcept { return _registry; }
    const EditorExtensionRegistry& registry() const noexcept { return _registry; }
    EditorDocumentManager& documents() noexcept { return _documents; }
    const EditorDocumentManager& documents() const noexcept { return _documents; }
    EditorCommandRouter& commands() noexcept { return _commands; }
    const EditorCommandRouter& commands() const noexcept { return _commands; }
    EditorSelectionService& selections() noexcept { return _selections; }
    const EditorSelectionService& selections() const noexcept {
        return _selections;
    }

private:
    void onDocumentEvent(const EditorDocumentEvent& event);

    EditorExtensionRegistry _registry;
    EditorDocumentManager _documents;
    EditorCommandRouter _commands;
    EditorSelectionService _selections;
    EditorDocumentManager::ListenerId _documentListener = 0;
};

} // namespace ayt::editor
