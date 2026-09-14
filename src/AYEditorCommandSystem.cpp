#include "AYEditor/EditorCommandSystem.h"

namespace ayt::editor {
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
