#pragma once

#include "AYEditorCommand/EditorCommandVersion.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayt::editor {

// Commands are responsible for keeping execute() and undo() atomic: returning
// false must leave the edited model in the state it had before the call.
// Commands that refer to a document-owned object should use a stable handle or
// weak lifetime token and report false from isAlive() after that owner expires.
class IEditorCommand {
public:
    virtual ~IEditorCommand() = default;

    virtual const std::string& label() const noexcept = 0;
    virtual bool execute() = 0;
    virtual bool undo() = 0;
    virtual bool isAlive() const noexcept { return true; }

    // mergeFrom() updates only the stored command payload. The newer command
    // has already modified the model before this function is called.
    virtual std::string mergeKey() const { return {}; }
    virtual bool mergeFrom(const IEditorCommand& /*newer*/) { return false; }
};

enum class EditorHistoryDiscardState {
    MarkClean,
    KeepDirty,
};

class EditorCommandHistory {
public:
    using ChangedCallback = std::function<void()>;

    static constexpr std::size_t kDefaultCapacity = 256u;

    explicit EditorCommandHistory(
        std::size_t capacity = kDefaultCapacity) noexcept;

    bool execute(std::unique_ptr<IEditorCommand> command);
    bool undo();
    bool redo();

    bool beginTransaction(std::string label);
    bool commitTransaction();
    bool cancelTransaction();
    bool transactionActive() const noexcept { return _transaction.active; }

    // Discarding destroys history and any pending transaction without applying
    // undo. Use cancelTransaction() when the current model must be restored.
    // This operation is the safe reset for document close/reload/World swap.
    void discardHistory(
        EditorHistoryDiscardState state = EditorHistoryDiscardState::MarkClean);

    bool markSaved();
    bool isDirty() const noexcept;
    bool canUndo() const noexcept {
        return !_faulted && !_transaction.active && _cursor > 0;
    }
    bool canRedo() const noexcept {
        return !_faulted && !_transaction.active && _cursor < _history.size();
    }

    std::size_t size() const noexcept { return _history.size(); }
    std::size_t cursor() const noexcept { return _cursor; }
    std::size_t capacity() const noexcept { return _capacity; }
    bool setCapacity(std::size_t capacity);

    // A fault means a retained command lost its owner or a composite rollback
    // could not restore a coherent state. Only discardHistory() clears it.
    bool faulted() const noexcept { return _faulted; }

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
    void trimToCapacity();
    void markFaulted();
    void notifyChanged();

    std::vector<std::unique_ptr<IEditorCommand>> _history;
    std::size_t _cursor = 0;
    std::optional<std::size_t> _savedCursor = std::size_t{0};
    std::size_t _capacity = kDefaultCapacity;
    Transaction _transaction;
    ChangedCallback _changed;
    bool _faulted = false;
};

} // namespace ayt::editor
