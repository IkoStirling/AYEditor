#include "AYEditorCommand/EditorCommandHistory.h"

#include <algorithm>
#include <utility>

namespace ayt::editor {
namespace {

class CompositeEditorCommand final : public IEditorCommand {
public:
    CompositeEditorCommand(
        std::string label,
        std::vector<std::unique_ptr<IEditorCommand>> commands)
        : _label(std::move(label)), _commands(std::move(commands)) {}

    const std::string& label() const noexcept override { return _label; }

    bool execute() override {
        std::size_t applied = 0;
        for (; applied < _commands.size(); ++applied) {
            if (!_commands[applied]->isAlive()
                || !_commands[applied]->execute()) {
                break;
            }
        }
        if (applied == _commands.size()) return true;

        bool restored = true;
        while (applied > 0) {
            --applied;
            restored = _commands[applied]->isAlive()
                && _commands[applied]->undo() && restored;
        }
        _faulted = !restored;
        return false;
    }

    bool undo() override {
        std::size_t firstUndone = _commands.size();
        while (firstUndone > 0) {
            const std::size_t index = firstUndone - 1;
            if (!_commands[index]->isAlive() || !_commands[index]->undo()) {
                bool restored = true;
                for (std::size_t restore = firstUndone;
                     restore < _commands.size(); ++restore) {
                    restored = _commands[restore]->isAlive()
                        && _commands[restore]->execute() && restored;
                }
                _faulted = !restored;
                return false;
            }
            firstUndone = index;
        }
        return true;
    }

    bool isAlive() const noexcept override {
        return !_faulted
            && std::all_of(_commands.begin(), _commands.end(),
                [](const auto& command) { return command->isAlive(); });
    }

private:
    std::string _label;
    std::vector<std::unique_ptr<IEditorCommand>> _commands;
    bool _faulted = false;
};

} // namespace

EditorCommandHistory::EditorCommandHistory(std::size_t capacity) noexcept
    : _capacity(capacity == 0u ? 1u : capacity) {}

bool EditorCommandHistory::execute(std::unique_ptr<IEditorCommand> command)
{
    if (_faulted || command == nullptr || !command->isAlive()
        || !command->execute()) {
        return false;
    }

    if (_transaction.active) {
        if (!tryMerge(_transaction.commands, *command)) {
            _transaction.commands.push_back(std::move(command));
        }
        notifyChanged();
        return true;
    }
    return appendApplied(std::move(command));
}

bool EditorCommandHistory::recordApplied(
    std::unique_ptr<IEditorCommand> command)
{
    if (_faulted || command == nullptr || !command->isAlive()) return false;
    if (_transaction.active) {
        if (!tryMerge(_transaction.commands, *command)) {
            _transaction.commands.push_back(std::move(command));
        }
        notifyChanged();
        return true;
    }
    return appendApplied(std::move(command));
}

bool EditorCommandHistory::discardLastApplied()
{
    if (_faulted) return false;
    if (_transaction.active) {
        if (_transaction.commands.empty()) return false;
        _transaction.commands.pop_back();
        notifyChanged();
        return true;
    }
    if (_cursor == 0u || _cursor != _history.size()) return false;
    _history.pop_back();
    --_cursor;
    if (_savedCursor.has_value() && *_savedCursor > _cursor) {
        _savedCursor.reset();
    }
    notifyChanged();
    return true;
}

bool EditorCommandHistory::undo()
{
    if (!canUndo()) return false;
    IEditorCommand& command = *_history[_cursor - 1];
    if (!command.isAlive()) {
        markFaulted();
        return false;
    }
    if (!command.undo()) {
        if (!command.isAlive()) markFaulted();
        return false;
    }
    --_cursor;
    notifyChanged();
    return true;
}

bool EditorCommandHistory::redo()
{
    if (!canRedo()) return false;
    IEditorCommand& command = *_history[_cursor];
    if (!command.isAlive()) {
        markFaulted();
        return false;
    }
    if (!command.execute()) {
        if (!command.isAlive()) markFaulted();
        return false;
    }
    ++_cursor;
    notifyChanged();
    return true;
}

bool EditorCommandHistory::beginTransaction(std::string label)
{
    if (_faulted || _transaction.active) return false;
    _transaction.active = true;
    _transaction.label = std::move(label);
    _transaction.commands.clear();
    notifyChanged();
    return true;
}

bool EditorCommandHistory::commitTransaction()
{
    if (_faulted || !_transaction.active) return false;
    _transaction.active = false;
    if (_transaction.commands.empty()) {
        _transaction.label.clear();
        notifyChanged();
        return true;
    }

    std::string label = _transaction.label;
    if (label.empty()) {
        label = _transaction.commands.size() == 1u
            ? _transaction.commands.front()->label() : "Transaction";
    }
    // A transaction is an explicit user-action boundary even when it contains
    // one command. Keeping the composite preserves its label and prevents a
    // merge with the preceding gesture.
    auto committed = std::make_unique<CompositeEditorCommand>(
        std::move(label), std::move(_transaction.commands));
    _transaction.commands.clear();
    _transaction.label.clear();
    return appendApplied(std::move(committed));
}

bool EditorCommandHistory::cancelTransaction()
{
    if (_faulted || !_transaction.active) return false;

    std::size_t firstUndone = _transaction.commands.size();
    while (firstUndone > 0) {
        const std::size_t index = firstUndone - 1;
        IEditorCommand& command = *_transaction.commands[index];
        if (!command.isAlive() || !command.undo()) {
            bool restored = command.isAlive();
            for (std::size_t restore = firstUndone;
                 restore < _transaction.commands.size(); ++restore) {
                IEditorCommand& undone = *_transaction.commands[restore];
                restored = undone.isAlive() && undone.execute() && restored;
            }
            if (!restored) markFaulted();
            return false;
        }
        firstUndone = index;
    }

    _transaction = {};
    notifyChanged();
    return true;
}

void EditorCommandHistory::discardHistory(EditorHistoryDiscardState state)
{
    _transaction = {};
    _history.clear();
    _cursor = 0;
    _savedCursor = state == EditorHistoryDiscardState::MarkClean
        ? std::optional<std::size_t>(std::size_t{0}) : std::nullopt;
    _faulted = false;
    notifyChanged();
}

bool EditorCommandHistory::markSaved()
{
    if (_faulted || _transaction.active) return false;
    const bool changed = !_savedCursor.has_value() || *_savedCursor != _cursor;
    _savedCursor = _cursor;
    if (changed) notifyChanged();
    return true;
}

bool EditorCommandHistory::isDirty() const noexcept
{
    if (_transaction.active && !_transaction.commands.empty()) return true;
    return !_savedCursor.has_value() || _cursor != *_savedCursor;
}

bool EditorCommandHistory::setCapacity(std::size_t capacity)
{
    if (capacity == 0u) return false;
    if (_capacity == capacity) return true;
    _capacity = capacity;
    const std::size_t oldSize = _history.size();
    trimToCapacity();
    if (_history.size() != oldSize) notifyChanged();
    return true;
}

std::string EditorCommandHistory::undoLabel() const
{
    return canUndo() ? _history[_cursor - 1]->label() : std::string{};
}

std::string EditorCommandHistory::redoLabel() const
{
    return canRedo() ? _history[_cursor]->label() : std::string{};
}

void EditorCommandHistory::truncateRedo()
{
    if (_cursor >= _history.size()) return;
    _history.erase(_history.begin() + static_cast<std::ptrdiff_t>(_cursor),
                   _history.end());
    if (_savedCursor.has_value() && *_savedCursor > _cursor) {
        _savedCursor.reset();
    }
}

bool EditorCommandHistory::appendApplied(
    std::unique_ptr<IEditorCommand> command)
{
    if (_faulted || command == nullptr || !command->isAlive()) return false;
    truncateRedo();
    const bool mayMerge = _cursor > 0 && _cursor == _history.size()
        && (!_savedCursor.has_value() || *_savedCursor != _cursor);
    if (mayMerge && tryMerge(_history, *command)) {
        notifyChanged();
        return true;
    }
    _history.push_back(std::move(command));
    _cursor = _history.size();
    trimToCapacity();
    notifyChanged();
    return true;
}

bool EditorCommandHistory::tryMerge(
    std::vector<std::unique_ptr<IEditorCommand>>& target,
    const IEditorCommand& newer)
{
    if (target.empty() || !target.back()->isAlive()) return false;
    const std::string key = newer.mergeKey();
    return !key.empty() && target.back()->mergeKey() == key
        && target.back()->mergeFrom(newer);
}

void EditorCommandHistory::trimToCapacity()
{
    if (_history.size() <= _capacity) return;
    const std::size_t removed = _history.size() - _capacity;
    _history.erase(
        _history.begin(),
        _history.begin() + static_cast<std::ptrdiff_t>(removed));
    _cursor = _cursor > removed ? _cursor - removed : 0u;
    if (_savedCursor.has_value()) {
        if (*_savedCursor < removed) {
            _savedCursor.reset();
        } else {
            *_savedCursor -= removed;
        }
    }
}

void EditorCommandHistory::markFaulted()
{
    if (_faulted) return;
    _faulted = true;
    notifyChanged();
}

void EditorCommandHistory::notifyChanged()
{
    if (_changed != nullptr) _changed();
}

} // namespace ayt::editor
