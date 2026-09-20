#include "AYEditor/EditorGameFlowDocument.h"

#include <AYApplication/GameFlowMigration.h>
#include <AYApplication/GameFlowProgram.h>
#include <AYIO/File.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace ayt::editor
{
namespace
{

using namespace ayt::app;

template<class T>
T* findById(std::vector<T>& values, std::string_view id)
{
    const auto found = std::find_if(values.begin(), values.end(),
        [id](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}

template<class T>
const T* findById(const std::vector<T>& values, std::string_view id)
{
    const auto found = std::find_if(values.begin(), values.end(),
        [id](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}

template<class T>
bool eraseById(std::vector<T>& values, std::string_view id)
{
    const auto found = std::find_if(values.begin(), values.end(),
        [id](const T& value) { return value.id == id; });
    if (found == values.end()) return false;
    values.erase(found);
    return true;
}

std::string trim(std::string value)
{
    const auto visible = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), visible));
    value.erase(std::find_if(value.rbegin(), value.rend(), visible).base(),
                value.end());
    return value;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return value;
}

GameFlowValueType parseValueType(std::string value, bool& valid)
{
    value = lowerAscii(trim(std::move(value)));
    valid = true;
    if (value == "boolean" || value == "bool") {
        return GameFlowValueType::Boolean;
    }
    if (value == "integer" || value == "int") {
        return GameFlowValueType::Integer;
    }
    if (value == "number" || value == "float" || value == "double") {
        return GameFlowValueType::Number;
    }
    if (value == "string") return GameFlowValueType::String;
    valid = false;
    return GameFlowValueType::String;
}

bool valueMatches(const GameFlowValue& value, GameFlowValueType type)
{
    if (std::holds_alternative<std::monostate>(value.data)) return true;
    switch (type) {
    case GameFlowValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case GameFlowValueType::Integer:
        return std::holds_alternative<std::int64_t>(value.data);
    case GameFlowValueType::Number:
        return std::holds_alternative<std::int64_t>(value.data)
            || std::holds_alternative<double>(value.data);
    case GameFlowValueType::String:
        return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

GameFlowValueType inferredType(const GameFlowValue& value)
{
    if (std::holds_alternative<bool>(value.data)) {
        return GameFlowValueType::Boolean;
    }
    if (std::holds_alternative<std::int64_t>(value.data)) {
        return GameFlowValueType::Integer;
    }
    if (std::holds_alternative<double>(value.data)) {
        return GameFlowValueType::Number;
    }
    return GameFlowValueType::String;
}

std::string firstDiagnostic(
    const std::vector<GameFlowDiagnostic>& diagnostics,
    std::string fallback)
{
    if (diagnostics.empty()) return fallback;
    return diagnostics.front().path.empty()
        ? diagnostics.front().message
        : diagnostics.front().path + ": " + diagnostics.front().message;
}

bool hasError(const std::vector<GameFlowDiagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const GameFlowDiagnostic& diagnostic) {
            return diagnostic.severity == GameFlowDiagnosticSeverity::Error;
        });
}

GameFlowTransitionDefinition* findTransition(
    GameFlowDocument& document, std::string_view id)
{
    return findById(document.transitions, id);
}

const GameFlowTransitionDefinition* findTransition(
    const GameFlowDocument& document, std::string_view id)
{
    return findById(document.transitions, id);
}

const GameFlowFieldDefinition* findField(
    const std::vector<GameFlowFieldDefinition>& fields, std::string_view id)
{
    return findById(fields, id);
}

std::vector<EditorGameFlowArgumentView> argumentViews(
    const GameFlowPayload& authored,
    const std::vector<GameFlowFieldDefinition>* definition)
{
    std::vector<EditorGameFlowArgumentView> result;
    std::set<std::string, std::less<>> known;
    if (definition != nullptr) {
        result.reserve(definition->size() + authored.size());
        for (const auto& field : *definition) {
            EditorGameFlowArgumentView item;
            item.id = field.id;
            item.type = field.type;
            item.required = field.required;
            item.known = true;
            const auto found = authored.find(field.id);
            if (found != authored.end()) {
                item.value = found->second;
                item.authored = true;
            } else {
                item.value = field.defaultValue;
            }
            known.insert(field.id);
            result.push_back(std::move(item));
        }
    }
    for (const auto& [id, value] : authored) {
        if (known.contains(id)) continue;
        result.push_back({id, inferredType(value), value, false, true, false});
    }
    return result;
}

bool propertiesEqual(const EditorGameFlowProperties& left,
                     const EditorGameFlowProperties& right)
{
    return left.id == right.id
        && left.first == right.first
        && left.second == right.second
        && left.third == right.third
        && left.fourth == right.fourth
        && left.fifth == right.fifth
        && left.sixth == right.sixth
        && left.number == right.number
        && left.integer == right.integer
        && left.flag == right.flag
        && left.value == right.value;
}

} // namespace

class EditorGameFlowDocument::SnapshotCommand final : public IEditorCommand {
public:
    SnapshotCommand(EditorGameFlowDocument& document, Snapshot before,
                    Snapshot after, std::string label)
        : _document(&document), _before(std::move(before)),
          _after(std::move(after)), _label(std::move(label)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (_document == nullptr) return false;
        _document->restore(_after);
        return true;
    }
    bool undo() override {
        if (_document == nullptr) return false;
        _document->restore(_before);
        return true;
    }

private:
    EditorGameFlowDocument* _document = nullptr;
    Snapshot _before;
    Snapshot _after;
    std::string _label;
};

EditorGameFlowDocument::EditorGameFlowDocument()
{
    _history.setChangedCallback([this]() { onHistoryChanged(); });
}

EditorGameFlowDocument::~EditorGameFlowDocument() = default;

const char* EditorGameFlowDocument::kindName(
    EditorGameFlowObjectKind kind) noexcept
{
    switch (kind) {
    case EditorGameFlowObjectKind::Document: return "Document";
    case EditorGameFlowObjectKind::Intent: return "Intent";
    case EditorGameFlowObjectKind::IntentField: return "Intent Field";
    case EditorGameFlowObjectKind::State: return "State";
    case EditorGameFlowObjectKind::Transition: return "Transition";
    case EditorGameFlowObjectKind::Guard: return "Guard";
    case EditorGameFlowObjectKind::Action: return "Action";
    case EditorGameFlowObjectKind::ActionArgument: return "Argument";
    }
    return "Object";
}

bool EditorGameFlowDocument::initialize(
    const std::string& path,
    const std::string& displayPath,
    std::string* error)
{
    if (path.empty()) {
        createNew();
        updateTitle(displayPath);
        if (error != nullptr) error->clear();
        return true;
    }
    return loadFromPath(path, displayPath, error);
}

void EditorGameFlowDocument::createNew()
{
    _flow = {};
    _flow.id = "new-game-flow";
    _flow.initialState = "Boot";
    _flow.intents = {GameFlowIntentDefinition{"app.start", {}}};
    _flow.states = {GameFlowStateDefinition{"Boot", {}, {}}};
    _path.clear();
    _title = "Untitled Game Flow";
    _selection = {};
    _history.discardHistory(EditorHistoryDiscardState::MarkClean);
}

bool EditorGameFlowDocument::loadFromPath(
    const std::string& path,
    const std::string& displayPath,
    std::string* error)
{
    if (!ayt::io::File::exists(path)) {
        if (error != nullptr) *error = "GameFlow file does not exist.";
        return false;
    }
    const std::string source = ayt::io::File::readAllText(path);
    if (source.empty()) {
        if (error != nullptr) *error = "GameFlow file is empty or unreadable.";
        return false;
    }
    GameFlowDocument loaded;
    std::vector<GameFlowDiagnostic> diagnostics;
    GameFlowMigrationReport migration;
    if (!GameFlowSerializer::deserialize(
            source, loaded, &diagnostics, &migration)) {
        if (error != nullptr) {
            *error = firstDiagnostic(diagnostics, "GameFlow loading failed.");
        }
        return false;
    }
    _flow = std::move(loaded);
    _path = path;
    _selection = {};
    // Keep successful in-memory migration visible to the author. The normal
    // save path serializes the current schema and clears this dirty state.
    updateTitle(displayPath);
    _history.discardHistory(migration.changed
        ? EditorHistoryDiscardState::KeepDirty
        : EditorHistoryDiscardState::MarkClean);
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::save(std::string* error)
{
    if (_path.empty()) {
        if (error != nullptr) *error = "GameFlow has no destination path.";
        return false;
    }
    if (hasError(_diagnostics)) {
        if (error != nullptr) {
            *error = firstDiagnostic(
                _diagnostics, "GameFlow has validation errors.");
        }
        return false;
    }
    if (!writeToPath(_path, error)) return false;
    (void)_history.markSaved();
    return true;
}

bool EditorGameFlowDocument::saveAs(
    const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) *error = "Save As path is empty.";
        return false;
    }
    if (hasError(_diagnostics)) {
        if (error != nullptr) {
            *error = firstDiagnostic(
                _diagnostics, "GameFlow has validation errors.");
        }
        return false;
    }
    if (!writeToPath(path, error)) return false;
    const bool historyWasDirty = _history.isDirty();
    _path = path;
    updateTitle();
    (void)_history.markSaved();
    if (!historyWasDirty) onHistoryChanged();
    return true;
}

bool EditorGameFlowDocument::reload(std::string* error)
{
    if (_path.empty()) {
        if (error != nullptr) *error = "GameFlow has no source path.";
        return false;
    }
    return loadFromPath(_path, {}, error);
}

bool EditorGameFlowDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    return writeToPath(path, error);
}

bool EditorGameFlowDocument::writeToPath(
    const std::string& path, std::string* error) const
{
    std::string encoded;
    std::vector<GameFlowDiagnostic> diagnostics;
    if (!GameFlowSerializer::serialize(_flow, encoded, &diagnostics, true)) {
        if (error != nullptr) {
            *error = firstDiagnostic(diagnostics, "GameFlow serialization failed.");
        }
        return false;
    }
    if (!ayt::io::File::atomicWrite(path, encoded.data(), encoded.size())) {
        if (error != nullptr) *error = "Atomic GameFlow save failed.";
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

void EditorGameFlowDocument::updateTitle(const std::string& displayPath)
{
    const std::string& source = displayPath.empty() ? _path : displayPath;
    if (source.empty()) {
        _title = "Untitled Game Flow";
        return;
    }
    const std::string fileName =
        std::filesystem::path(source).filename().string();
    _title = fileName.empty() ? source : fileName;
}

void EditorGameFlowDocument::refreshDiagnostics()
{
    _diagnostics.clear();
    _valid = validateGameFlow(_flow, nullptr, &_diagnostics);
    if (!_valid) return;

    GameFlowDocument registeredOnly = _flow;
    for (std::size_t transitionIndex = 0;
         transitionIndex < registeredOnly.transitions.size(); ++transitionIndex) {
        auto& transition = registeredOnly.transitions[transitionIndex];
        if (!transition.guard.guard.empty()
            && _registry.findGuard(transition.guard.guard) == nullptr) {
            _diagnostics.push_back({GameFlowDiagnosticSeverity::Warning,
                "$.transitions[" + std::to_string(transitionIndex)
                    + "].guard.id",
                "Guard '" + transition.guard.guard
                    + "' is not registered in this editor assembly; authored data is preserved."});
            transition.guard = {};
        }
        for (std::size_t actionIndex = 0;
             actionIndex < transition.actions.size();) {
            const auto& action = transition.actions[actionIndex];
            if (_registry.findAction(action.action) != nullptr) {
                ++actionIndex;
                continue;
            }
            _diagnostics.push_back({GameFlowDiagnosticSeverity::Warning,
                "$.transitions[" + std::to_string(transitionIndex)
                    + "].actions[" + std::to_string(actionIndex) + "].id",
                "Action '" + action.action
                    + "' is not registered in this editor assembly; authored data is preserved."});
            transition.actions.erase(transition.actions.begin()
                + static_cast<std::ptrdiff_t>(actionIndex));
        }
    }

    std::vector<GameFlowDiagnostic> typedDiagnostics;
    if (!validateGameFlow(registeredOnly, &_registry, &typedDiagnostics)) {
        _valid = false;
    }
    _diagnostics.insert(_diagnostics.end(),
        std::make_move_iterator(typedDiagnostics.begin()),
        std::make_move_iterator(typedDiagnostics.end()));
}

void EditorGameFlowDocument::actionRegistryChanged()
{
    refreshDiagnostics();
    ++_revision;
    notifyChanged();
}

bool EditorGameFlowDocument::buildPlan(
    GameFlowPlan& plan,
    std::vector<GameFlowDiagnostic>* diagnostics) const
{
    return buildGameFlowPlan(_flow, _registry, plan, diagnostics);
}

EditorGameFlowDocument::Snapshot EditorGameFlowDocument::snapshot() const
{
    return {_flow, _selection};
}

void EditorGameFlowDocument::commitMutation(Snapshot before, std::string label)
{
    (void)_history.recordApplied(std::make_unique<SnapshotCommand>(
        *this, std::move(before), snapshot(), std::move(label)));
}

void EditorGameFlowDocument::restore(Snapshot value)
{
    _flow = std::move(value.flow);
    _selection = std::move(value.selection);
    if (!selectionExists(_selection)) _selection = {};
}

bool EditorGameFlowDocument::undo()
{
    return _history.undo();
}

bool EditorGameFlowDocument::redo()
{
    return _history.redo();
}

bool EditorGameFlowDocument::handlesCommand(
    const std::string& commandId) const
{
    return commandId == "file.save" || commandId == "edit.undo"
        || commandId == "edit.redo";
}

bool EditorGameFlowDocument::canExecuteCommand(
    const std::string& commandId) const
{
    if (commandId == "file.save") {
        return isDirty() && !_path.empty();
    }
    if (commandId == "edit.undo") return canUndo();
    if (commandId == "edit.redo") return canRedo();
    return false;
}

bool EditorGameFlowDocument::executeCommand(const std::string& commandId)
{
    if (commandId == "file.save") {
        std::string error;
        return save(&error);
    }
    if (commandId == "edit.undo") return undo();
    if (commandId == "edit.redo") return redo();
    return false;
}

void EditorGameFlowDocument::onHistoryChanged()
{
    ++_revision;
    refreshDiagnostics();
    notifyChanged();
}

bool EditorGameFlowDocument::selectionExists(
    const EditorGameFlowSelection& selection) const
{
    switch (selection.kind) {
    case EditorGameFlowObjectKind::Document:
        return selection.id.empty() || selection.id == _flow.id;
    case EditorGameFlowObjectKind::Intent:
        return findById(_flow.intents, selection.id) != nullptr;
    case EditorGameFlowObjectKind::IntentField: {
        const auto* intent = findById(_flow.intents, selection.ownerId);
        return intent != nullptr
            && findField(intent->payload, selection.id) != nullptr;
    }
    case EditorGameFlowObjectKind::State:
        return findById(_flow.states, selection.id) != nullptr;
    case EditorGameFlowObjectKind::Transition:
        return findTransition(_flow, selection.id) != nullptr;
    case EditorGameFlowObjectKind::Guard: {
        const auto* transition = findTransition(_flow, selection.ownerId);
        return transition != nullptr && !transition->guard.guard.empty()
            && (selection.id.empty()
                || selection.id == transition->guard.guard);
    }
    case EditorGameFlowObjectKind::Action: {
        const auto* transition = findTransition(_flow, selection.ownerId);
        return transition != nullptr
            && selection.index < transition->actions.size()
            && (selection.id.empty()
                || selection.id == transition->actions[selection.index].action);
    }
    case EditorGameFlowObjectKind::ActionArgument: {
        const auto* transition = findTransition(_flow, selection.ownerId);
        if (transition == nullptr) return false;
        if (selection.index == kEditorGameFlowNoIndex) {
            const auto* definition =
                _registry.findGuard(transition->guard.guard);
            return transition->guard.arguments.contains(selection.id)
                || (definition != nullptr
                    && findField(definition->arguments, selection.id) != nullptr);
        }
        if (selection.index >= transition->actions.size()) return false;
        const auto& action = transition->actions[selection.index];
        const auto* definition = _registry.findAction(action.action);
        return action.arguments.contains(selection.id)
            || (definition != nullptr
                && findField(definition->arguments, selection.id) != nullptr);
    }
    }
    return false;
}

bool EditorGameFlowDocument::select(EditorGameFlowSelection selection)
{
    if (!selectionExists(selection)) return false;
    if (selection.kind == EditorGameFlowObjectKind::Document) {
        selection.id.clear();
        selection.ownerId.clear();
        selection.index = kEditorGameFlowNoIndex;
    }
    if (_selection == selection) return true;
    _selection = std::move(selection);
    ++_revision;
    notifyChanged();
    return true;
}

std::vector<EditorGameFlowOutlineItem> EditorGameFlowDocument::outline() const
{
    std::vector<EditorGameFlowOutlineItem> result;
    result.push_back({{}, _flow.id.empty() ? "Game Flow" : _flow.id, 0});
    for (const auto& intent : _flow.intents) {
        result.push_back({{EditorGameFlowObjectKind::Intent, intent.id},
                          "Intent: " + intent.id, 1});
        for (const auto& field : intent.payload) {
            result.push_back({{EditorGameFlowObjectKind::IntentField,
                               field.id, intent.id},
                              field.id + " : "
                                  + gameFlowValueTypeName(field.type), 2});
        }
    }
    for (const auto& state : _flow.states) {
        result.push_back({{EditorGameFlowObjectKind::State, state.id},
                          "State: " + state.id, 1});
    }
    for (const auto& transition : _flow.transitions) {
        result.push_back({{EditorGameFlowObjectKind::Transition,
                           transition.id},
                          "Transition: " + transition.id, 1});
        if (!transition.guard.guard.empty()) {
            result.push_back({{EditorGameFlowObjectKind::Guard,
                               transition.guard.guard, transition.id},
                              "Guard: " + transition.guard.guard, 2});
            const auto* definition =
                _registry.findGuard(transition.guard.guard);
            for (const auto& argument : argumentViews(
                     transition.guard.arguments,
                     definition == nullptr ? nullptr : &definition->arguments)) {
                result.push_back({{EditorGameFlowObjectKind::ActionArgument,
                                   argument.id, transition.id,
                                   kEditorGameFlowNoIndex},
                                  "Argument: " + argument.id, 3});
            }
        }
        for (std::size_t index = 0; index < transition.actions.size(); ++index) {
            const auto& action = transition.actions[index];
            result.push_back({{EditorGameFlowObjectKind::Action,
                               action.action, transition.id, index},
                              "Action " + std::to_string(index + 1u)
                                  + ": " + action.action, 2});
            const auto* definition = _registry.findAction(action.action);
            for (const auto& argument : argumentViews(
                     action.arguments,
                     definition == nullptr ? nullptr : &definition->arguments)) {
                result.push_back({{EditorGameFlowObjectKind::ActionArgument,
                                   argument.id, transition.id, index},
                                  "Argument: " + argument.id, 3});
            }
        }
    }
    return result;
}

EditorGameFlowProperties EditorGameFlowDocument::selectedProperties() const
{
    EditorGameFlowProperties result;
    switch (_selection.kind) {
    case EditorGameFlowObjectKind::Document:
        result.id = _flow.id;
        result.first = _flow.initialState;
        break;
    case EditorGameFlowObjectKind::Intent:
        if (const auto* value = findById(_flow.intents, _selection.id)) {
            result.id = value->id;
        }
        break;
    case EditorGameFlowObjectKind::IntentField:
        if (const auto* intent = findById(_flow.intents, _selection.ownerId)) {
            if (const auto* value = findField(intent->payload, _selection.id)) {
                result.id = value->id;
                result.first = gameFlowValueTypeName(value->type);
                result.flag = value->required;
                result.value = value->defaultValue;
            }
        }
        break;
    case EditorGameFlowObjectKind::State:
        if (const auto* value = findById(_flow.states, _selection.id)) {
            result.id = value->id;
            result.first = value->parent;
            result.second = value->initialChild;
        }
        break;
    case EditorGameFlowObjectKind::Transition:
        if (const auto* value = findTransition(_flow, _selection.id)) {
            result.id = value->id;
            result.first = value->fromState;
            result.second = value->triggerIntent;
            result.third = value->toState;
            result.fourth = value->guard.guard;
            result.fifth = value->onFailureState;
            result.sixth = value->onCancelState;
            result.number = value->timeoutSeconds;
            result.integer = value->priority;
        }
        break;
    case EditorGameFlowObjectKind::Guard:
        if (const auto* transition = findTransition(_flow, _selection.ownerId)) {
            result.id = _selection.id;
            result.first = transition->guard.guard;
        }
        break;
    case EditorGameFlowObjectKind::Action:
        if (const auto* transition = findTransition(_flow, _selection.ownerId);
            transition != nullptr && _selection.index < transition->actions.size()) {
            result.id = _selection.id;
            result.first = transition->actions[_selection.index].action;
        }
        break;
    case EditorGameFlowObjectKind::ActionArgument:
        result.id = _selection.id;
        for (const auto& argument : selectedArguments()) {
            if (argument.id == _selection.id) {
                result.value = argument.value;
                result.first = gameFlowValueTypeName(argument.type);
                result.flag = argument.authored;
                break;
            }
        }
        break;
    }
    return result;
}

EditorGameFlowPropertyLabels
EditorGameFlowDocument::selectedPropertyLabels() const
{
    switch (_selection.kind) {
    case EditorGameFlowObjectKind::Document:
        return {"Initial State"};
    case EditorGameFlowObjectKind::Intent:
        return {};
    case EditorGameFlowObjectKind::IntentField:
        return {"Value Type", {}, {}, {}, {}, {}, {}, {}, "Required",
                "Default Value"};
    case EditorGameFlowObjectKind::State:
        return {"Parent", "Initial Child"};
    case EditorGameFlowObjectKind::Transition:
        return {"From State", "Trigger Intent", "To State", "Guard",
                "Failure State", "Cancel State", "Timeout Seconds",
                "Priority"};
    case EditorGameFlowObjectKind::Guard:
    case EditorGameFlowObjectKind::Action:
        return {"Registered Type"};
    case EditorGameFlowObjectKind::ActionArgument:
        return {"Value Type", {}, {}, {}, {}, {}, {}, {}, "Authored",
                "Effective Value"};
    }
    return {};
}

bool EditorGameFlowDocument::renameSelection(
    std::string newId, std::string* error)
{
    newId = trim(std::move(newId));
    if (newId.empty()) {
        if (error != nullptr) *error = "ID cannot be empty.";
        return false;
    }
    const std::string oldId = _selection.kind
            == EditorGameFlowObjectKind::Document
        ? _flow.id : _selection.id;
    if (newId == oldId) return true;

    switch (_selection.kind) {
    case EditorGameFlowObjectKind::Document:
        _flow.id = std::move(newId);
        return true;
    case EditorGameFlowObjectKind::Intent: {
        if (findById(_flow.intents, newId) != nullptr) break;
        auto* value = findById(_flow.intents, oldId);
        if (value == nullptr) return false;
        value->id = newId;
        for (auto& transition : _flow.transitions) {
            if (transition.triggerIntent == oldId) {
                transition.triggerIntent = newId;
            }
        }
        _selection.id = std::move(newId);
        return true;
    }
    case EditorGameFlowObjectKind::IntentField: {
        auto* intent = findById(_flow.intents, _selection.ownerId);
        if (intent == nullptr) return false;
        if (findById(intent->payload, newId) != nullptr) break;
        auto* field = findById(intent->payload, oldId);
        if (field == nullptr) return false;
        field->id = newId;
        _selection.id = std::move(newId);
        return true;
    }
    case EditorGameFlowObjectKind::State: {
        if (findById(_flow.states, newId) != nullptr) break;
        auto* value = findById(_flow.states, oldId);
        if (value == nullptr) return false;
        value->id = newId;
        if (_flow.initialState == oldId) _flow.initialState = newId;
        for (auto& state : _flow.states) {
            if (state.parent == oldId) state.parent = newId;
            if (state.initialChild == oldId) state.initialChild = newId;
        }
        for (auto& transition : _flow.transitions) {
            if (transition.fromState == oldId) transition.fromState = newId;
            if (transition.toState == oldId) transition.toState = newId;
            if (transition.onFailureState == oldId) {
                transition.onFailureState = newId;
            }
            if (transition.onCancelState == oldId) {
                transition.onCancelState = newId;
            }
        }
        _selection.id = std::move(newId);
        return true;
    }
    case EditorGameFlowObjectKind::Transition:
        if (findTransition(_flow, newId) != nullptr) break;
        if (auto* value = findTransition(_flow, oldId)) {
            value->id = newId;
            _selection.id = std::move(newId);
            return true;
        }
        return false;
    case EditorGameFlowObjectKind::Guard:
    case EditorGameFlowObjectKind::Action:
    case EditorGameFlowObjectKind::ActionArgument:
        if (error != nullptr) {
            *error = "Registered type and argument IDs are defined by their schema.";
        }
        return false;
    }
    if (error != nullptr) *error = "ID already exists in this collection.";
    return false;
}

bool EditorGameFlowDocument::applySelectedProperties(
    const EditorGameFlowProperties& properties, std::string* error)
{
    if (!selectionExists(_selection)) {
        if (error != nullptr) *error = "Selected GameFlow object no longer exists.";
        return false;
    }
    const EditorGameFlowProperties current = selectedProperties();
    if (propertiesEqual(current, properties)) {
        if (error != nullptr) error->clear();
        return true;
    }
    if (_selection.kind == EditorGameFlowObjectKind::Transition
        && (!std::isfinite(properties.number) || properties.number < 0.0)) {
        if (error != nullptr) {
            *error = "Transition timeout must be finite and non-negative.";
        }
        return false;
    }
    if (_selection.kind == EditorGameFlowObjectKind::State) {
        const std::string parent = trim(properties.first);
        const std::string initialChild = trim(properties.second);
        if (!parent.empty()) {
            if (parent == _selection.id || _flow.findState(parent) == nullptr) {
                if (error != nullptr) *error = "State parent is invalid.";
                return false;
            }
            const GameFlowStateDefinition* cursor = _flow.findState(parent);
            std::set<std::string, std::less<>> visited;
            while (cursor != nullptr && visited.insert(cursor->id).second) {
                if (cursor->parent == _selection.id) {
                    if (error != nullptr) {
                        *error = "State parent would create a hierarchy cycle.";
                    }
                    return false;
                }
                cursor = cursor->parent.empty()
                    ? nullptr : _flow.findState(cursor->parent);
            }
        }
        if (!initialChild.empty()) {
            const auto* child = _flow.findState(initialChild);
            if (child == nullptr || child->parent != _selection.id) {
                if (error != nullptr) {
                    *error = "Initial Child must be a direct child of this State.";
                }
                return false;
            }
        }
    }
    bool valueTypeValid = true;
    GameFlowValueType valueType = GameFlowValueType::String;
    if (_selection.kind == EditorGameFlowObjectKind::IntentField) {
        valueType = parseValueType(properties.first, valueTypeValid);
        if (!valueTypeValid) {
            if (error != nullptr) *error = "Unknown GameFlow value type.";
            return false;
        }
        if (!valueMatches(properties.value, valueType)) {
            if (error != nullptr) {
                *error = "Default value does not match the selected type.";
            }
            return false;
        }
    }

    if (_selection.kind == EditorGameFlowObjectKind::ActionArgument) {
        if (properties.id != current.id) {
            if (error != nullptr) {
                *error = "Action argument IDs are defined by the action schema.";
            }
            return false;
        }
        return setSelectedArgument(_selection.id, properties.value, error);
    }
    if ((_selection.kind == EditorGameFlowObjectKind::Guard
         || _selection.kind == EditorGameFlowObjectKind::Action)
        && (properties.id != current.id
            || properties.first != current.first)) {
        if (error != nullptr) {
            *error = "Registered Action and Guard types are defined by their schema.";
        }
        return false;
    }

    Snapshot before = snapshot();
    if (_selection.kind != EditorGameFlowObjectKind::Guard
        && _selection.kind != EditorGameFlowObjectKind::Action
        && !renameSelection(properties.id, error)) return false;
    switch (_selection.kind) {
    case EditorGameFlowObjectKind::Document:
        _flow.initialState = trim(properties.first);
        break;
    case EditorGameFlowObjectKind::Intent:
        break;
    case EditorGameFlowObjectKind::IntentField:
        if (auto* intent = findById(_flow.intents, _selection.ownerId)) {
            if (auto* field = findById(intent->payload, _selection.id)) {
                field->type = valueType;
                field->required = properties.flag;
                field->defaultValue = properties.value;
            }
        }
        break;
    case EditorGameFlowObjectKind::State:
        if (auto* value = findById(_flow.states, _selection.id)) {
            value->parent = trim(properties.first);
            value->initialChild = trim(properties.second);
        }
        break;
    case EditorGameFlowObjectKind::Transition:
        if (auto* value = findTransition(_flow, _selection.id)) {
            value->fromState = trim(properties.first);
            value->triggerIntent = trim(properties.second);
            value->toState = trim(properties.third);
            value->guard.guard = trim(properties.fourth);
            if (value->guard.guard.empty()) value->guard.arguments.clear();
            value->onFailureState = trim(properties.fifth);
            value->onCancelState = trim(properties.sixth);
            value->timeoutSeconds = properties.number;
            value->priority = properties.integer;
        }
        break;
    case EditorGameFlowObjectKind::Guard:
    case EditorGameFlowObjectKind::Action:
        break;
    case EditorGameFlowObjectKind::ActionArgument:
        break;
    }
    commitMutation(std::move(before), "Edit Game Flow Properties");
    if (error != nullptr) error->clear();
    return true;
}

std::string EditorGameFlowDocument::uniqueId(
    EditorGameFlowObjectKind kind, std::string_view ownerId) const
{
    std::string stem;
    switch (kind) {
    case EditorGameFlowObjectKind::Intent: stem = "intent"; break;
    case EditorGameFlowObjectKind::IntentField: stem = "field"; break;
    case EditorGameFlowObjectKind::State: stem = "state"; break;
    case EditorGameFlowObjectKind::Transition: stem = "transition"; break;
    case EditorGameFlowObjectKind::Guard: stem = "guard"; break;
    case EditorGameFlowObjectKind::Action: stem = "action"; break;
    case EditorGameFlowObjectKind::ActionArgument: stem = "argument"; break;
    case EditorGameFlowObjectKind::Document: stem = "game-flow"; break;
    }
    for (unsigned serial = 1u;; ++serial) {
        const std::string candidate = stem + "_" + std::to_string(serial);
        bool exists = false;
        if (kind == EditorGameFlowObjectKind::Intent) {
            exists = findById(_flow.intents, candidate) != nullptr;
        } else if (kind == EditorGameFlowObjectKind::IntentField) {
            if (const auto* intent = findById(_flow.intents, ownerId)) {
                exists = findById(intent->payload, candidate) != nullptr;
            }
        } else if (kind == EditorGameFlowObjectKind::State) {
            exists = findById(_flow.states, candidate) != nullptr;
        } else if (kind == EditorGameFlowObjectKind::Transition) {
            exists = findTransition(_flow, candidate) != nullptr;
        }
        if (!exists) return candidate;
    }
}

bool EditorGameFlowDocument::addObject(
    EditorGameFlowObjectKind kind,
    std::string ownerId,
    std::string* error)
{
    if (kind == EditorGameFlowObjectKind::Document) {
        if (error != nullptr) *error = "A GameFlow already has one document root.";
        return false;
    }
    if (kind == EditorGameFlowObjectKind::IntentField) {
        const std::string fieldId = uniqueId(kind, ownerId);
        return addIntentField(std::move(ownerId),
            {fieldId, GameFlowValueType::String, false, {}}, error);
    }
    if (kind == EditorGameFlowObjectKind::Action) {
        const auto actionTypes = _registry.actionTypes();
        const std::string type = actionTypes.empty()
            ? uniqueId(kind) : actionTypes.front().id;
        return addAction(std::move(ownerId), type, error);
    }
    if (kind == EditorGameFlowObjectKind::Guard) {
        const auto guardTypes = _registry.guardTypes();
        const std::string type = guardTypes.empty()
            ? uniqueId(kind) : guardTypes.front().id;
        return setTransitionGuard(std::move(ownerId), type, error);
    }
    if (kind == EditorGameFlowObjectKind::ActionArgument) {
        if (error != nullptr) {
            *error = "Arguments are created from their registered schema or by setting a value.";
        }
        return false;
    }

    Snapshot before = snapshot();
    if (kind == EditorGameFlowObjectKind::Intent) {
        const std::string id = uniqueId(kind);
        _flow.intents.push_back({id, {}});
        _selection = {kind, id};
    } else if (kind == EditorGameFlowObjectKind::State) {
        if (!ownerId.empty() && findById(_flow.states, ownerId) == nullptr) {
            if (error != nullptr) *error = "Parent state does not exist.";
            return false;
        }
        const std::string id = uniqueId(kind);
        _flow.states.push_back({id, std::move(ownerId), {}});
        _selection = {kind, id};
    } else if (kind == EditorGameFlowObjectKind::Transition) {
        if (error != nullptr) {
            *error = "Create a Transition by connecting two States and choosing an Intent.";
        }
        return false;
    } else {
        if (error != nullptr) *error = "Unsupported GameFlow object kind.";
        return false;
    }
    commitMutation(std::move(before), "Add Game Flow Object");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::addIntentField(
    std::string intentId,
    GameFlowFieldDefinition field,
    std::string* error)
{
    auto* intent = findById(_flow.intents, intentId);
    if (intent == nullptr) {
        if (error != nullptr) *error = "Intent does not exist.";
        return false;
    }
    field.id = trim(std::move(field.id));
    if (field.id.empty()) {
        if (error != nullptr) *error = "Intent field ID cannot be empty.";
        return false;
    }
    if (findById(intent->payload, field.id) != nullptr) {
        if (error != nullptr) *error = "Intent field ID already exists.";
        return false;
    }
    if (!valueMatches(field.defaultValue, field.type)) {
        if (error != nullptr) *error = "Intent field default has the wrong type.";
        return false;
    }
    Snapshot before = snapshot();
    const std::string fieldId = field.id;
    intent->payload.push_back(std::move(field));
    _selection = {EditorGameFlowObjectKind::IntentField,
                  fieldId, std::move(intentId)};
    commitMutation(std::move(before), "Add Intent Field");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::addAction(
    std::string transitionId,
    std::string actionType,
    std::string* error)
{
    auto* transition = findTransition(_flow, transitionId);
    if (transition == nullptr) {
        if (error != nullptr) *error = "Transition does not exist.";
        return false;
    }
    actionType = trim(std::move(actionType));
    if (actionType.empty()) {
        if (error != nullptr) *error = "Action type cannot be empty.";
        return false;
    }
    Snapshot before = snapshot();
    transition->actions.push_back({actionType, {}});
    _selection = {EditorGameFlowObjectKind::Action, actionType,
                  std::move(transitionId), transition->actions.size() - 1u};
    commitMutation(std::move(before), "Add Game Flow Action");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::addTransition(
    std::string fromState,
    std::string triggerIntent,
    std::string toState,
    std::string* error)
{
    fromState = trim(std::move(fromState));
    triggerIntent = trim(std::move(triggerIntent));
    toState = trim(std::move(toState));
    if (findById(_flow.states, fromState) == nullptr
        || findById(_flow.states, toState) == nullptr) {
        if (error != nullptr) *error = "Transition endpoints must be existing States.";
        return false;
    }
    if (findById(_flow.intents, triggerIntent) == nullptr) {
        if (error != nullptr) *error = "Transition trigger must be an existing Intent.";
        return false;
    }

    Snapshot before = snapshot();
    const std::string id = uniqueId(EditorGameFlowObjectKind::Transition);
    _flow.transitions.push_back(
        {id, std::move(fromState), std::move(triggerIntent), std::move(toState)});
    _selection = {EditorGameFlowObjectKind::Transition, id};
    commitMutation(std::move(before), "Connect Game Flow States");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::addSubflowCall(
    std::string transitionId,
    std::string subflowId,
    std::string* error)
{
    auto* transition = findTransition(_flow, transitionId);
    if (transition == nullptr) {
        if (error != nullptr) *error = "Transition does not exist.";
        return false;
    }
    subflowId = trim(std::move(subflowId));
    if (subflowId.empty()) {
        if (error != nullptr) *error = "Subflow id cannot be empty.";
        return false;
    }

    Snapshot before = snapshot();
    GameFlowActionCall action;
    action.action = std::string(kGameFlowActionEnter);
    action.arguments.emplace(std::string(kGameFlowSubflowIdArgument),
                             GameFlowValue(subflowId));
    transition->actions.push_back(std::move(action));
    _selection = {EditorGameFlowObjectKind::Action,
                  std::string(kGameFlowActionEnter), transitionId,
                  transition->actions.size() - 1u};
    commitMutation(std::move(before), "Add Subflow Node");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::setTransitionGuard(
    std::string transitionId,
    std::string guardType,
    std::string* error)
{
    auto* transition = findTransition(_flow, transitionId);
    if (transition == nullptr) {
        if (error != nullptr) *error = "Transition does not exist.";
        return false;
    }
    guardType = trim(std::move(guardType));
    if (transition->guard.guard == guardType) {
        if (error != nullptr) error->clear();
        return true;
    }
    Snapshot before = snapshot();
    transition->guard = {guardType, {}};
    _selection = guardType.empty()
        ? EditorGameFlowSelection{EditorGameFlowObjectKind::Transition,
                                  transitionId}
        : EditorGameFlowSelection{EditorGameFlowObjectKind::Guard,
                                  guardType, transitionId};
    commitMutation(std::move(before), "Set Transition Guard");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::moveSelectedAction(
    std::ptrdiff_t offset, std::string* error)
{
    if (_selection.kind != EditorGameFlowObjectKind::Action
        && _selection.kind != EditorGameFlowObjectKind::ActionArgument) {
        if (error != nullptr) *error = "Select an Action before reordering.";
        return false;
    }
    auto* transition = findTransition(_flow, _selection.ownerId);
    if (transition == nullptr || _selection.index >= transition->actions.size()) {
        if (error != nullptr) *error = "Selected Action no longer exists.";
        return false;
    }
    const std::ptrdiff_t current =
        static_cast<std::ptrdiff_t>(_selection.index);
    const std::ptrdiff_t target = current + offset;
    if (target < 0
        || target >= static_cast<std::ptrdiff_t>(transition->actions.size())) {
        if (error != nullptr) *error = "Action cannot move beyond the action list.";
        return false;
    }
    if (target == current) {
        if (error != nullptr) error->clear();
        return true;
    }
    Snapshot before = snapshot();
    auto action = std::move(transition->actions[_selection.index]);
    transition->actions.erase(transition->actions.begin() + current);
    transition->actions.insert(transition->actions.begin() + target,
                               std::move(action));
    _selection.index = static_cast<std::size_t>(target);
    commitMutation(std::move(before), "Move Game Flow Action");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::selectionIsReferenced(
    std::string& reference) const
{
    if (_selection.kind == EditorGameFlowObjectKind::Intent) {
        for (const auto& transition : _flow.transitions) {
            if (transition.triggerIntent == _selection.id) {
                reference = "Transition " + transition.id;
                return true;
            }
        }
    } else if (_selection.kind == EditorGameFlowObjectKind::State) {
        if (_flow.initialState == _selection.id) {
            reference = "initialState";
            return true;
        }
        for (const auto& state : _flow.states) {
            if (state.parent == _selection.id
                || state.initialChild == _selection.id) {
                reference = "State " + state.id;
                return true;
            }
        }
        for (const auto& transition : _flow.transitions) {
            if (transition.fromState == _selection.id
                || transition.toState == _selection.id
                || transition.onFailureState == _selection.id
                || transition.onCancelState == _selection.id) {
                reference = "Transition " + transition.id;
                return true;
            }
        }
    }
    return false;
}

bool EditorGameFlowDocument::deleteSelection(std::string* error)
{
    if (_selection.kind == EditorGameFlowObjectKind::Document) {
        if (error != nullptr) *error = "The GameFlow document root cannot be deleted.";
        return false;
    }
    std::string reference;
    if (selectionIsReferenced(reference)) {
        if (error != nullptr) {
            *error = "Object is still referenced by " + reference + ".";
        }
        return false;
    }
    Snapshot before = snapshot();
    bool erased = false;
    switch (_selection.kind) {
    case EditorGameFlowObjectKind::Intent:
        erased = eraseById(_flow.intents, _selection.id);
        break;
    case EditorGameFlowObjectKind::IntentField:
        if (auto* intent = findById(_flow.intents, _selection.ownerId)) {
            erased = eraseById(intent->payload, _selection.id);
        }
        break;
    case EditorGameFlowObjectKind::State:
        erased = eraseById(_flow.states, _selection.id);
        break;
    case EditorGameFlowObjectKind::Transition:
        erased = eraseById(_flow.transitions, _selection.id);
        break;
    case EditorGameFlowObjectKind::Guard:
        if (auto* transition = findTransition(_flow, _selection.ownerId)) {
            transition->guard = {};
            erased = true;
        }
        break;
    case EditorGameFlowObjectKind::Action:
        if (auto* transition = findTransition(_flow, _selection.ownerId);
            transition != nullptr && _selection.index < transition->actions.size()) {
            transition->actions.erase(transition->actions.begin()
                + static_cast<std::ptrdiff_t>(_selection.index));
            erased = true;
        }
        break;
    case EditorGameFlowObjectKind::ActionArgument: {
        auto* transition = findTransition(_flow, _selection.ownerId);
        if (transition != nullptr
            && _selection.index == kEditorGameFlowNoIndex) {
            erased = transition->guard.arguments.erase(_selection.id) != 0u;
        } else if (transition != nullptr
                   && _selection.index < transition->actions.size()) {
            erased = transition->actions[_selection.index].arguments.erase(
                _selection.id) != 0u;
        }
        break;
    }
    case EditorGameFlowObjectKind::Document:
        break;
    }
    if (!erased) {
        if (error != nullptr) *error = "Selected object cannot be deleted.";
        return false;
    }
    _selection = {};
    commitMutation(std::move(before), "Delete Game Flow Object");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::deleteObjects(
    const std::vector<EditorGameFlowSelection>& selections,
    std::string* error)
{
    std::set<std::string, std::less<>> states;
    std::set<std::string, std::less<>> intents;
    std::set<std::string, std::less<>> transitions;
    for (const auto& selection : selections) {
        if (selection.kind == EditorGameFlowObjectKind::State) {
            states.insert(selection.id);
        } else if (selection.kind == EditorGameFlowObjectKind::Intent) {
            intents.insert(selection.id);
        } else if (selection.kind == EditorGameFlowObjectKind::Transition) {
            transitions.insert(selection.id);
        }
    }
    if (states.empty() && intents.empty() && transitions.empty()) {
        if (error != nullptr) *error = "Select States, Intents, or Transitions to delete.";
        return false;
    }

    if (states.contains(_flow.initialState)) {
        if (error != nullptr) {
            *error = "Choose a new initial State before deleting the current one.";
        }
        return false;
    }
    for (const auto& transition : _flow.transitions) {
        const bool referenced = states.contains(transition.fromState)
            || states.contains(transition.toState)
            || states.contains(transition.onFailureState)
            || states.contains(transition.onCancelState)
            || intents.contains(transition.triggerIntent);
        if (referenced && !transitions.contains(transition.id)) {
            if (error != nullptr) {
                *error = "Selection is referenced by Transition '"
                    + transition.id
                    + "'. Select that Transition explicitly before deleting.";
            }
            return false;
        }
    }
    for (const auto& state : _flow.states) {
        if (states.contains(state.id)) continue;
        if (states.contains(state.parent)
            || states.contains(state.initialChild)) {
            if (error != nullptr) {
                *error = "State '" + state.id
                    + "' has a hierarchy reference to the selection. Update it before deleting.";
            }
            return false;
        }
    }

    Snapshot before = snapshot();
    _flow.transitions.erase(std::remove_if(_flow.transitions.begin(),
        _flow.transitions.end(), [&](const auto& transition) {
            return transitions.contains(transition.id);
        }), _flow.transitions.end());
    _flow.states.erase(std::remove_if(_flow.states.begin(), _flow.states.end(),
        [&](const auto& state) { return states.contains(state.id); }),
        _flow.states.end());
    _flow.intents.erase(std::remove_if(_flow.intents.begin(), _flow.intents.end(),
        [&](const auto& intent) { return intents.contains(intent.id); }),
        _flow.intents.end());
    _selection = {};
    commitMutation(std::move(before), "Delete Game Flow Selection");
    if (error != nullptr) error->clear();
    return true;
}

EditorGameFlowClipboard EditorGameFlowDocument::copyObjects(
    const std::vector<EditorGameFlowSelection>& selections) const
{
    std::set<std::string, std::less<>> stateIds;
    std::set<std::string, std::less<>> intentIds;
    std::set<std::string, std::less<>> transitionIds;
    for (const auto& selection : selections) {
        if (selection.kind == EditorGameFlowObjectKind::State) {
            stateIds.insert(selection.id);
        } else if (selection.kind == EditorGameFlowObjectKind::Intent) {
            intentIds.insert(selection.id);
        } else if (selection.kind == EditorGameFlowObjectKind::Transition) {
            transitionIds.insert(selection.id);
        } else if (selection.kind == EditorGameFlowObjectKind::Action
                   || selection.kind == EditorGameFlowObjectKind::Guard
                   || selection.kind == EditorGameFlowObjectKind::ActionArgument) {
            transitionIds.insert(selection.ownerId);
        }
    }
    for (const auto& transition : _flow.transitions) {
        if (transitionIds.contains(transition.id)
            || (stateIds.contains(transition.fromState)
                && stateIds.contains(transition.toState))) {
            transitionIds.insert(transition.id);
            stateIds.insert(transition.fromState);
            stateIds.insert(transition.toState);
            if (!transition.onFailureState.empty()) {
                stateIds.insert(transition.onFailureState);
            }
            if (!transition.onCancelState.empty()) {
                stateIds.insert(transition.onCancelState);
            }
            intentIds.insert(transition.triggerIntent);
        }
    }

    EditorGameFlowClipboard result;
    for (const auto& intent : _flow.intents) {
        if (intentIds.contains(intent.id)) result.intents.push_back(intent);
    }
    for (const auto& state : _flow.states) {
        if (stateIds.contains(state.id)) result.states.push_back(state);
    }
    for (const auto& transition : _flow.transitions) {
        if (transitionIds.contains(transition.id)) {
            result.transitions.push_back(transition);
        }
    }
    return result;
}

bool EditorGameFlowDocument::pasteObjects(
    const EditorGameFlowClipboard& clipboard,
    std::string* error)
{
    if (clipboard.empty()) {
        if (error != nullptr) *error = "The GameFlow clipboard is empty.";
        return false;
    }
    Snapshot before = snapshot();
    std::map<std::string, std::string, std::less<>> intentMap;
    std::map<std::string, std::string, std::less<>> stateMap;
    const auto uniqueCopyId = [](std::string base, const auto& exists) {
        if (!exists(base)) return base;
        for (unsigned serial = 1u;; ++serial) {
            const std::string candidate = base + "_copy"
                + (serial == 1u ? std::string{} : "_" + std::to_string(serial));
            if (!exists(candidate)) return candidate;
        }
    };

    for (auto intent : clipboard.intents) {
        const std::string oldId = intent.id;
        intent.id = uniqueCopyId(intent.id, [&](std::string_view candidate) {
            return findById(_flow.intents, candidate) != nullptr
                || std::any_of(intentMap.begin(), intentMap.end(),
                    [&](const auto& item) { return item.second == candidate; });
        });
        intentMap.emplace(oldId, intent.id);
        _flow.intents.push_back(std::move(intent));
    }
    for (auto state : clipboard.states) {
        const std::string oldId = state.id;
        state.id = uniqueCopyId(state.id, [&](std::string_view candidate) {
            return findById(_flow.states, candidate) != nullptr
                || std::any_of(stateMap.begin(), stateMap.end(),
                    [&](const auto& item) { return item.second == candidate; });
        });
        stateMap.emplace(oldId, state.id);
        _flow.states.push_back(std::move(state));
    }
    const auto mapped = [](const auto& values, const std::string& id) {
        const auto found = values.find(id);
        return found == values.end() ? std::string{} : found->second;
    };
    for (std::size_t index = _flow.states.size() - clipboard.states.size();
         index < _flow.states.size(); ++index) {
        auto& state = _flow.states[index];
        state.parent = mapped(stateMap, state.parent);
        state.initialChild = mapped(stateMap, state.initialChild);
    }
    std::string lastTransition;
    for (auto transition : clipboard.transitions) {
        transition.id = uniqueCopyId(transition.id,
            [&](std::string_view candidate) {
                return findTransition(_flow, candidate) != nullptr;
            });
        transition.fromState = mapped(stateMap, transition.fromState);
        transition.toState = mapped(stateMap, transition.toState);
        transition.triggerIntent = mapped(intentMap, transition.triggerIntent);
        transition.onFailureState = mapped(stateMap, transition.onFailureState);
        transition.onCancelState = mapped(stateMap, transition.onCancelState);
        lastTransition = transition.id;
        _flow.transitions.push_back(std::move(transition));
    }
    if (!clipboard.states.empty()) {
        _selection = {EditorGameFlowObjectKind::State,
                      stateMap.at(clipboard.states.front().id)};
    } else if (!lastTransition.empty()) {
        _selection = {EditorGameFlowObjectKind::Transition, lastTransition};
    } else if (!clipboard.intents.empty()) {
        _selection = {EditorGameFlowObjectKind::Intent,
                      intentMap.at(clipboard.intents.front().id)};
    }
    commitMutation(std::move(before), "Paste Game Flow Selection");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::applyTemplate(
    EditorGameFlowTemplate value,
    std::string* error)
{
    if (value != EditorGameFlowTemplate::MainMenuToResult) {
        if (error != nullptr) *error = "Unknown GameFlow template.";
        return false;
    }
    Snapshot before = snapshot();
    const std::string documentId = _flow.id.empty() ? "game-flow" : _flow.id;
    _flow = {};
    _flow.id = documentId;
    _flow.initialState = "main-menu";
    _flow.intents = {
        {"game.start", {}}, {"world.loaded", {}}, {"game.pause", {}},
        {"game.resume", {}}, {"game.finish", {}}, {"game.restart", {}},
    };
    _flow.states = {
        {"main-menu", {}, {}}, {"loading", {}, {}},
        {"gameplay", {}, {}}, {"pause", {}, {}}, {"result", {}, {}},
    };
    _flow.transitions = {
        {"start-game", "main-menu", "game.start", "loading"},
        {"finish-loading", "loading", "world.loaded", "gameplay"},
        {"pause-game", "gameplay", "game.pause", "pause"},
        {"resume-game", "pause", "game.resume", "gameplay"},
        {"finish-game", "gameplay", "game.finish", "result"},
        {"restart-game", "result", "game.restart", "loading"},
    };
    _selection = {EditorGameFlowObjectKind::Document, _flow.id};
    commitMutation(std::move(before), "Apply Game Flow Template");
    if (error != nullptr) error->clear();
    return true;
}

std::vector<EditorGameFlowArgumentView>
EditorGameFlowDocument::selectedArguments() const
{
    const GameFlowTransitionDefinition* transition = nullptr;
    if (_selection.kind == EditorGameFlowObjectKind::Transition) {
        transition = findTransition(_flow, _selection.id);
    } else {
        transition = findTransition(_flow, _selection.ownerId);
    }
    if (transition == nullptr) return {};

    if (_selection.kind == EditorGameFlowObjectKind::Guard
        || (_selection.kind == EditorGameFlowObjectKind::ActionArgument
            && _selection.index == kEditorGameFlowNoIndex)) {
        const auto* definition =
            _registry.findGuard(transition->guard.guard);
        return argumentViews(transition->guard.arguments,
            definition == nullptr ? nullptr : &definition->arguments);
    }
    if ((_selection.kind == EditorGameFlowObjectKind::Action
         || _selection.kind == EditorGameFlowObjectKind::ActionArgument)
        && _selection.index < transition->actions.size()) {
        const auto& action = transition->actions[_selection.index];
        const auto* definition = _registry.findAction(action.action);
        return argumentViews(action.arguments,
            definition == nullptr ? nullptr : &definition->arguments);
    }
    return {};
}

bool EditorGameFlowDocument::setSelectedArgument(
    std::string argumentId,
    GameFlowValue value,
    std::string* error)
{
    argumentId = trim(std::move(argumentId));
    if (argumentId.empty()) {
        if (error != nullptr) *error = "Argument ID cannot be empty.";
        return false;
    }
    auto* transition = findTransition(_flow, _selection.ownerId);
    GameFlowPayload* arguments = nullptr;
    const std::vector<GameFlowFieldDefinition>* fields = nullptr;
    if (_selection.kind == EditorGameFlowObjectKind::Guard
        || (_selection.kind == EditorGameFlowObjectKind::ActionArgument
            && _selection.index == kEditorGameFlowNoIndex)) {
        if (transition != nullptr) {
            arguments = &transition->guard.arguments;
            if (const auto* definition =
                    _registry.findGuard(transition->guard.guard)) {
                fields = &definition->arguments;
            }
        }
    } else if ((_selection.kind == EditorGameFlowObjectKind::Action
                || _selection.kind == EditorGameFlowObjectKind::ActionArgument)
               && transition != nullptr
               && _selection.index < transition->actions.size()) {
        auto& action = transition->actions[_selection.index];
        arguments = &action.arguments;
        if (const auto* definition = _registry.findAction(action.action)) {
            fields = &definition->arguments;
        }
    }
    if (arguments == nullptr) {
        if (error != nullptr) *error = "Select an Action or Guard argument owner.";
        return false;
    }
    if (fields != nullptr) {
        const auto* field = findField(*fields, argumentId);
        if (field == nullptr) {
            if (error != nullptr) *error = "Argument is not declared by this type.";
            return false;
        }
        if (!valueMatches(value, field->type)
            || std::holds_alternative<std::monostate>(value.data)) {
            if (error != nullptr) *error = "Argument value has the wrong type.";
            return false;
        }
    }
    const auto found = arguments->find(argumentId);
    if (found != arguments->end() && found->second == value) {
        if (error != nullptr) error->clear();
        return true;
    }
    Snapshot before = snapshot();
    arguments->insert_or_assign(argumentId, std::move(value));
    _selection = {EditorGameFlowObjectKind::ActionArgument, argumentId,
                  _selection.ownerId, _selection.index};
    commitMutation(std::move(before), "Set Game Flow Argument");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorGameFlowDocument::clearSelectedArgument(
    std::string_view argumentId, std::string* error)
{
    auto* transition = findTransition(_flow, _selection.ownerId);
    GameFlowPayload* arguments = nullptr;
    if (_selection.kind == EditorGameFlowObjectKind::Guard
        || (_selection.kind == EditorGameFlowObjectKind::ActionArgument
            && _selection.index == kEditorGameFlowNoIndex)) {
        if (transition != nullptr) arguments = &transition->guard.arguments;
    } else if ((_selection.kind == EditorGameFlowObjectKind::Action
                || _selection.kind == EditorGameFlowObjectKind::ActionArgument)
               && transition != nullptr
               && _selection.index < transition->actions.size()) {
        arguments = &transition->actions[_selection.index].arguments;
    }
    if (arguments == nullptr || !arguments->contains(argumentId)) {
        if (error != nullptr) *error = "Authored argument does not exist.";
        return false;
    }
    Snapshot before = snapshot();
    arguments->erase(argumentId);
    if (_selection.kind == EditorGameFlowObjectKind::ActionArgument) {
        if (_selection.index == kEditorGameFlowNoIndex) {
            _selection.kind = EditorGameFlowObjectKind::Guard;
            _selection.id = transition->guard.guard;
        } else {
            _selection.kind = EditorGameFlowObjectKind::Action;
            _selection.id = transition->actions[_selection.index].action;
        }
    }
    commitMutation(std::move(before), "Clear Game Flow Argument");
    if (error != nullptr) error->clear();
    return true;
}

void EditorGameFlowDocument::notifyChanged()
{
    if (_changed != nullptr) _changed();
}

} // namespace ayt::editor
