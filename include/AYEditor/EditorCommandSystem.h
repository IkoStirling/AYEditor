#pragma once

#include "AYEditor/EditorExtension.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

class IEditorCommand {
public:
    virtual ~IEditorCommand() = default;
    virtual const std::string& label() const noexcept = 0;
    virtual bool execute() = 0;
    virtual void undo() = 0;
    virtual std::string mergeKey() const { return {}; }
    virtual bool mergeFrom(const IEditorCommand& /*newer*/) { return false; }
};

class EditorCommandHistory {
public:
    using ChangedCallback = std::function<void()>;

    bool execute(std::unique_ptr<IEditorCommand> command);
    bool undo();
    bool redo();

    bool beginTransaction(std::string label);
    bool commitTransaction();
    bool cancelTransaction();
    bool transactionActive() const noexcept { return _transaction.active; }

    void clear(bool markClean = true);
    void markSaved() noexcept;
    bool isDirty() const noexcept;
    bool canUndo() const noexcept { return !_transaction.active && _cursor > 0; }
    bool canRedo() const noexcept {
        return !_transaction.active && _cursor < _history.size();
    }
    size_t size() const noexcept { return _history.size(); }
    size_t cursor() const noexcept { return _cursor; }
    std::string undoLabel() const;
    std::string redoLabel() const;
    void setChangedCallback(ChangedCallback callback) {
        _changed = std::move(callback);
    }

private:
    struct Transaction {
        bool active = false;
        std::string label;
        std::vector<std::unique_ptr<IEditorCommand>> commands;
    };

    void truncateRedo();
    bool appendApplied(std::unique_ptr<IEditorCommand> command);
    static bool tryMerge(std::vector<std::unique_ptr<IEditorCommand>>& target,
                         const IEditorCommand& newer);
    void notifyChanged();

    std::vector<std::unique_ptr<IEditorCommand>> _history;
    size_t _cursor = 0;
    std::optional<size_t> _savedCursor = size_t{0};
    Transaction _transaction;
    ChangedCallback _changed;
};

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
