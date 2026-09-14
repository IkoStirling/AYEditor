#pragma once

#include "AYEditor/EditorExtension.h"
#include <AYEditorCommand/EditorCommandHistory.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace ayt::editor {

struct EditorCommandBinding {
    std::string id;
    std::wstring displayName;
    std::string defaultShortcut;
    std::function<bool()> execute;
    std::function<bool()> isEnabled;
};

class EditorCommandRouter {
public:
    bool registerGlobal(EditorCommandBinding binding,
                        std::string* error = nullptr);
    bool unregisterGlobal(const std::string& commandId);
    const EditorCommandBinding* findGlobal(
        const std::string& commandId) const noexcept;

    void setActiveTarget(IEditorCommandTarget* target) noexcept {
        _activeTarget = target;
    }
    IEditorCommandTarget* activeTarget() const noexcept {
        return _activeTarget;
    }

    bool handles(const std::string& commandId) const;
    bool canExecute(const std::string& commandId) const;
    bool execute(const std::string& commandId);

private:
    std::unordered_map<std::string, EditorCommandBinding> _globals;
    IEditorCommandTarget* _activeTarget = nullptr;
};

} // namespace ayt::editor
