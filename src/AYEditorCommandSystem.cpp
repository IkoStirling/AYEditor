#include "AYEditor/EditorCommandSystem.h"

#include <algorithm>

namespace ayt::editor {
namespace {

class CompositeEditorCommand final : public IEditorCommand {
public:
    CompositeEditorCommand(std::string label,
        std::vector<std::unique_ptr<IEditorCommand>> commands)
        : _label(std::move(label)), _commands(std::move(commands)) {}

    const std::string& label() const noexcept override { return _label; }

    bool execute() override {
        size_t applied = 0;
        for (; applied < _commands.size(); ++applied) {
            if (!_commands[applied]->execute()) break;
        }
        if (applied == _commands.size()) return true;
        while (applied > 0) _commands[--applied]->undo();
        return false;
    }

    void undo() override {
        for (auto it = _commands.rbegin(); it != _commands.rend(); ++it) {
            (*it)->undo();
        }
    }

private:
    std::string _label;
    std::vector<std::unique_ptr<IEditorCommand>> _commands;
};

} // namespace

bool EditorCommandHistory::execute(std::unique_ptr<IEditorCommand> command)
{
    if (command == nullptr || !command->execute()) return false;

    if (_transaction.active) {
        if (!tryMerge(_transaction.commands, *command)) {
            _transaction.commands.push_back(std::move(command));
        }
        return true;
    }
    return appendApplied(std::move(command));
}

bool EditorCommandHistory::undo()
{
    if (!canUndo()) return false;
    _history[_cursor - 1]->undo();
    --_cursor;
    notifyChanged();
    return true;
}

bool EditorCommandHistory::redo()
{
    if (!canRedo() || !_history[_cursor]->execute()) return false;
    ++_cursor;
    notifyChanged();
    return true;
}

bool EditorCommandHistory::beginTransaction(std::string label)
{
    if (_transaction.active) return false;
    _transaction.active = true;
    _transaction.label = std::move(label);
    _transaction.commands.clear();
    return true;
}

bool EditorCommandHistory::commitTransaction()
{
    if (!_transaction.active) return false;
    _transaction.active = false;
    if (_transaction.commands.empty()) {
        _transaction.label.clear();
        return false;
    }

    std::string label = _transaction.label;
    if (label.empty()) {
        label = _transaction.commands.size() == 1
            ? _transaction.commands.front()->label() : "Transaction";
    }
    // A transaction is an explicit user-action boundary even when it contains
    // one command. Keeping the composite prevents a merge with the preceding
    // gesture and preserves the transaction label in Undo/Redo UI.
    std::unique_ptr<IEditorCommand> committed =
        std::make_unique<CompositeEditorCommand>(
            std::move(label), std::move(_transaction.commands));
    _transaction.commands.clear();
    _transaction.label.clear();
    return appendApplied(std::move(committed));
}

bool EditorCommandHistory::cancelTransaction()
{
    if (!_transaction.active) return false;
    for (auto it = _transaction.commands.rbegin();
         it != _transaction.commands.rend(); ++it) {
        (*it)->undo();
    }
    const bool changedState = !_transaction.commands.empty();
    _transaction = {};
    if (changedState) notifyChanged();
    return true;
}

void EditorCommandHistory::clear(bool markClean)
{
    if (_transaction.active) {
        for (auto it = _transaction.commands.rbegin();
             it != _transaction.commands.rend(); ++it) {
            (*it)->undo();
        }
        _transaction = {};
    }
    _history.clear();
    _cursor = 0;
    _savedCursor = markClean
        ? std::optional<size_t>(size_t{0}) : std::nullopt;
    notifyChanged();
}

void EditorCommandHistory::markSaved() noexcept
{
    _savedCursor = _cursor;
}

bool EditorCommandHistory::isDirty() const noexcept
{
    if (_transaction.active && !_transaction.commands.empty()) return true;
    return !_savedCursor.has_value() || _cursor != *_savedCursor;
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
    if (command == nullptr) return false;
    truncateRedo();
    const bool mayMerge = _cursor > 0 && _cursor == _history.size()
        && (!_savedCursor.has_value() || *_savedCursor != _cursor);
    if (mayMerge && tryMerge(_history, *command)) {
        notifyChanged();
        return true;
    }
    _history.push_back(std::move(command));
    _cursor = _history.size();
    notifyChanged();
    return true;
}

bool EditorCommandHistory::tryMerge(
    std::vector<std::unique_ptr<IEditorCommand>>& target,
    const IEditorCommand& newer)
{
    if (target.empty()) return false;
    const std::string key = newer.mergeKey();
    return !key.empty() && target.back()->mergeKey() == key
        && target.back()->mergeFrom(newer);
}

void EditorCommandHistory::notifyChanged()
{
    if (_changed != nullptr) _changed();
}

bool EditorCommandRouter::registerGlobal(EditorCommandBinding binding,
                                         std::string* error)
{
    if (binding.id.empty() || binding.execute == nullptr) {
        if (error != nullptr) *error = "Command id and execute callback are required.";
        return false;
    }
    if (_globals.find(binding.id) != _globals.end()) {
        if (error != nullptr) *error = "Command id is already registered.";
        return false;
    }
    _globals.emplace(binding.id, std::move(binding));
    if (error != nullptr) error->clear();
    return true;
}

bool EditorCommandRouter::unregisterGlobal(const std::string& commandId)
{
    return _globals.erase(commandId) != 0;
}

const EditorCommandBinding* EditorCommandRouter::findGlobal(
    const std::string& commandId) const noexcept
{
    const auto it = _globals.find(commandId);
    return it == _globals.end() ? nullptr : &it->second;
}

bool EditorCommandRouter::handles(const std::string& commandId) const
{
    return (_activeTarget != nullptr
            && _activeTarget->handlesCommand(commandId))
        || findGlobal(commandId) != nullptr;
}

bool EditorCommandRouter::canExecute(const std::string& commandId) const
{
    if (_activeTarget != nullptr
        && _activeTarget->handlesCommand(commandId)) {
        return _activeTarget->canExecuteCommand(commandId);
    }
    const EditorCommandBinding* binding = findGlobal(commandId);
    return binding != nullptr
        && (binding->isEnabled == nullptr || binding->isEnabled());
}

bool EditorCommandRouter::execute(const std::string& commandId)
{
    if (_activeTarget != nullptr
        && _activeTarget->handlesCommand(commandId)) {
        return _activeTarget->canExecuteCommand(commandId)
            && _activeTarget->executeCommand(commandId);
    }
    const EditorCommandBinding* binding = findGlobal(commandId);
    if (binding == nullptr
        || (binding->isEnabled != nullptr && !binding->isEnabled())) {
        return false;
    }
    return binding->execute();
}

} // namespace ayt::editor
