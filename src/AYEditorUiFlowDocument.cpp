#include "AYEditor/EditorUiFlowDocument.h"

#include <AYIO/File.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace ayt::editor {
namespace {

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

std::vector<std::string> splitList(const std::string& source)
{
    std::vector<std::string> result;
    std::stringstream stream(source);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(std::move(item));
        if (!item.empty()) result.push_back(std::move(item));
    }
    return result;
}

std::string joinList(const std::vector<std::string>& values)
{
    std::string result;
    for (const std::string& value : values) {
        if (!result.empty()) result += ", ";
        result += value;
    }
    return result;
}

std::string joinAssignments(
    const std::vector<ayt::ui::UIFlowSlotAssignment>& values)
{
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += "; ";
        if (value.operation == ayt::ui::UIFlowSlotOperation::Hide) {
            result += "!" + value.slot;
        } else {
            result += value.slot + "=" + value.screen;
        }
    }
    return result;
}

bool parseAssignments(
    const std::string& source,
    std::vector<ayt::ui::UIFlowSlotAssignment>& values,
    std::string* error)
{
    values.clear();
    std::stringstream stream(source);
    std::string item;
    while (std::getline(stream, item, ';')) {
        item = trim(std::move(item));
        if (item.empty()) continue;
        if (item.front() == '!') {
            std::string slot = trim(item.substr(1));
            if (slot.empty()) {
                if (error != nullptr) *error = "Hide assignment needs a Slot ID.";
                return false;
            }
            values.push_back({std::move(slot),
                ayt::ui::UIFlowSlotOperation::Hide, {}});
            continue;
        }
        const std::size_t equals = item.find('=');
        if (equals == std::string::npos) {
            if (error != nullptr) {
                *error = "Context assignments use slot=screen or !slot, separated by ';'.";
            }
            return false;
        }
        std::string slot = trim(item.substr(0, equals));
        std::string screen = trim(item.substr(equals + 1));
        if (slot.empty() || screen.empty()) {
            if (error != nullptr) *error = "Present assignment needs Slot and Screen IDs.";
            return false;
        }
        values.push_back({std::move(slot),
            ayt::ui::UIFlowSlotOperation::Present, std::move(screen)});
    }
    return true;
}

std::string joinScreenEvents(
    const std::vector<ayt::ui::UIFlowScreenEventBinding>& values)
{
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += "; ";
        result += value.handler + "=" + value.signal;
    }
    return result;
}

bool parseScreenEvents(
    const std::string& source,
    std::vector<ayt::ui::UIFlowScreenEventBinding>& values,
    std::string* error)
{
    values.clear();
    std::unordered_set<std::string> handlers;
    std::stringstream stream(source);
    std::string item;
    while (std::getline(stream, item, ';')) {
        item = trim(std::move(item));
        if (item.empty()) continue;
        const std::size_t equals = item.find('=');
        if (equals == std::string::npos) {
            if (error != nullptr) {
                *error = "Screen events use handler=signal, separated by ';'.";
            }
            return false;
        }
        std::string handler = trim(item.substr(0, equals));
        std::string signal = trim(item.substr(equals + 1));
        if (handler.empty() || signal.empty()) {
            if (error != nullptr) {
                *error = "Screen event mappings require handler and Signal IDs.";
            }
            return false;
        }
        if (!handlers.insert(handler).second) {
            if (error != nullptr) {
                *error = "A Screen handler can map to only one Signal.";
            }
            return false;
        }
        values.push_back({std::move(handler), std::move(signal)});
    }
    return true;
}

bool parseUnsigned(const std::string& source, std::uint32_t& value)
{
    const std::string input = trim(source);
    if (input.empty()) {
        value = 0u;
        return true;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long parsed = std::stoul(input, &consumed);
        if (consumed != input.size()
            || parsed > static_cast<unsigned long>(UINT32_MAX)) return false;
        value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool contains(const std::vector<std::string>& values, std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

void replace(std::vector<std::string>& values,
             std::string_view oldId,
             const std::string& newId)
{
    for (std::string& value : values) {
        if (value == oldId) value = newId;
    }
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool parseScope(std::string value, ayt::ui::UIFlowScope& result)
{
    value = lowerAscii(trim(std::move(value)));
    if (value == "application") {
        result = ayt::ui::UIFlowScope::Application;
        return true;
    }
    if (value == "world") {
        result = ayt::ui::UIFlowScope::World;
        return true;
    }
    if (value == "owner") {
        result = ayt::ui::UIFlowScope::Owner;
        return true;
    }
    if (value == "transient") {
        result = ayt::ui::UIFlowScope::Transient;
        return true;
    }
    return false;
}

bool parseInputPolicy(std::string value,
                      ayt::ui::UIFlowInputPolicy& result)
{
    value = lowerAscii(trim(std::move(value)));
    if (value == "passthrough" || value == "pass-through") {
        result = ayt::ui::UIFlowInputPolicy::PassThrough;
        return true;
    }
    if (value == "consumehandled" || value == "consume-handled") {
        result = ayt::ui::UIFlowInputPolicy::ConsumeHandled;
        return true;
    }
    if (value == "blocklower" || value == "block-lower") {
        result = ayt::ui::UIFlowInputPolicy::BlockLower;
        return true;
    }
    return false;
}

bool parseInterruptPolicy(std::string value,
                          ayt::ui::UIFlowInterruptPolicy& result)
{
    value = lowerAscii(trim(std::move(value)));
    if (value == "queue") {
        result = ayt::ui::UIFlowInterruptPolicy::Queue;
        return true;
    }
    if (value == "cancelprevious" || value == "cancel-previous") {
        result = ayt::ui::UIFlowInterruptPolicy::CancelPrevious;
        return true;
    }
    if (value == "reverseprevious" || value == "reverse-previous") {
        result = ayt::ui::UIFlowInterruptPolicy::ReversePrevious;
        return true;
    }
    if (value == "ignoreifrunning" || value == "ignore-if-running") {
        result = ayt::ui::UIFlowInterruptPolicy::IgnoreIfRunning;
        return true;
    }
    if (value == "coalesce") {
        result = ayt::ui::UIFlowInterruptPolicy::Coalesce;
        return true;
    }
    return false;
}

std::string firstDiagnostic(
    const std::vector<ayt::ui::UIFlowDiagnostic>& diagnostics)
{
    if (diagnostics.empty()) return "UI Flow validation failed.";
    return diagnostics.front().path.empty()
        ? diagnostics.front().message
        : diagnostics.front().path + ": " + diagnostics.front().message;
}

} // namespace

class EditorUiFlowDocument::SnapshotCommand final : public IEditorCommand {
public:
    SnapshotCommand(EditorUiFlowDocument& document, Snapshot before,
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
    EditorUiFlowDocument* _document = nullptr;
    Snapshot _before;
    Snapshot _after;
    std::string _label;
};

EditorUiFlowDocument::EditorUiFlowDocument()
{
    _history.setChangedCallback([this]() { onHistoryChanged(); });
}

EditorUiFlowDocument::~EditorUiFlowDocument() = default;

const char* EditorUiFlowDocument::kindName(
    EditorUiFlowObjectKind kind) noexcept
{
    switch (kind) {
    case EditorUiFlowObjectKind::Document: return "Document";
    case EditorUiFlowObjectKind::Layer: return "Layer";
    case EditorUiFlowObjectKind::Slot: return "Slot";
    case EditorUiFlowObjectKind::Screen: return "Screen";
    case EditorUiFlowObjectKind::Context: return "Context";
    case EditorUiFlowObjectKind::Entry: return "Entry";
    case EditorUiFlowObjectKind::Signal: return "Signal";
    case EditorUiFlowObjectKind::Action: return "Action";
    case EditorUiFlowObjectKind::Region: return "Region";
    case EditorUiFlowObjectKind::State: return "State";
    case EditorUiFlowObjectKind::Transition: return "Transition";
    case EditorUiFlowObjectKind::Graph: return "Graph";
    }
    return "Object";
}

bool EditorUiFlowDocument::initialize(
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

void EditorUiFlowDocument::createNew()
{
    _flow = {};
    _flow.id = "new-ui-flow";
    _flow.defaultEntry = "Boot";
    _flow.layers = {
        ayt::ui::UIFlowLayerDefinition{"application", 0},
        ayt::ui::UIFlowLayerDefinition{"hud", 100},
    };
    _flow.slots = {
        ayt::ui::UIFlowSlotDefinition{"application.main", "application"},
        ayt::ui::UIFlowSlotDefinition{"hud.main", "hud"},
    };
    _flow.contexts = {ayt::ui::UIFlowContextDefinition{"Application"}};
    _flow.entries = {ayt::ui::UIFlowEntryDefinition{
        "Boot", {"Application"}, {}}};
    _path.clear();
    _title = "Untitled UI Flow";
    _selection = {};
    _history.discardHistory(EditorHistoryDiscardState::MarkClean);
}

bool EditorUiFlowDocument::loadFromPath(
    const std::string& path,
    const std::string& displayPath,
    std::string* error)
{
    if (!ayt::io::File::exists(path)) {
        if (error != nullptr) *error = "UI Flow file does not exist.";
        return false;
    }
    const std::string source = ayt::io::File::readAllText(path);
    if (source.empty()) {
        if (error != nullptr) *error = "UI Flow file is empty or unreadable.";
        return false;
    }
    ayt::ui::UIFlowDocument loaded;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::UIFlowSerializer::deserialize(
            source, loaded, &diagnostics)) {
        if (error != nullptr) *error = firstDiagnostic(diagnostics);
        return false;
    }
    _flow = std::move(loaded);
    _diagnostics = std::move(diagnostics);
    _valid = true;
    _path = path;
    _selection = {};
    updateTitle(displayPath);
    _history.discardHistory(EditorHistoryDiscardState::MarkClean);
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiFlowDocument::save(std::string* error)
{
    if (_path.empty()) {
        if (error != nullptr) *error = "UI Flow has no destination path.";
        return false;
    }
    if (!writeToPath(_path, true, error)) return false;
    (void)_history.markSaved();
    return true;
}

bool EditorUiFlowDocument::saveAs(
    const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) *error = "Save As path is empty.";
        return false;
    }
    if (!writeToPath(path, true, error)) return false;
    const bool historyWasDirty = _history.isDirty();
    _path = path;
    updateTitle();
    (void)_history.markSaved();
    if (!historyWasDirty) onHistoryChanged();
    return true;
}

bool EditorUiFlowDocument::reload(std::string* error)
{
    if (_path.empty()) {
        if (error != nullptr) *error = "UI Flow has no source path.";
        return false;
    }
    return loadFromPath(_path, {}, error);
}

bool EditorUiFlowDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    return writeToPath(path, false, error);
}

bool EditorUiFlowDocument::writeToPath(
    const std::string& path,
    bool,
    std::string* error) const
{
    std::string encoded;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::UIFlowSerializer::serialize(
            _flow, encoded, &diagnostics, true)) {
        if (error != nullptr) *error = firstDiagnostic(diagnostics);
        return false;
    }
    if (!ayt::io::File::atomicWrite(path, encoded.data(), encoded.size())) {
        if (error != nullptr) *error = "Atomic UI Flow save failed.";
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

void EditorUiFlowDocument::updateTitle(const std::string& displayPath)
{
    const std::string& source = displayPath.empty() ? _path : displayPath;
    if (source.empty()) {
        _title = "Untitled UI Flow";
        return;
    }
    const std::string fileName = std::filesystem::path(source).filename().string();
    _title = fileName.empty() ? source : fileName;
}

void EditorUiFlowDocument::refreshDiagnostics()
{
    _diagnostics.clear();
    _valid = ayt::ui::validateUIFlow(_flow, &_diagnostics);
}

EditorUiFlowDocument::Snapshot EditorUiFlowDocument::snapshot() const
{
    return {_flow, _selection};
}

void EditorUiFlowDocument::commitMutation(Snapshot before, std::string label)
{
    (void)_history.recordApplied(std::make_unique<SnapshotCommand>(
        *this, std::move(before), snapshot(), std::move(label)));
}

void EditorUiFlowDocument::restore(Snapshot value)
{
    _flow = std::move(value.flow);
    _selection = std::move(value.selection);
}

bool EditorUiFlowDocument::undo()
{
    return _history.undo();
}

bool EditorUiFlowDocument::redo()
{
    return _history.redo();
}

bool EditorUiFlowDocument::handlesCommand(
    const std::string& commandId) const
{
    return commandId == "file.save" || commandId == "edit.undo"
        || commandId == "edit.redo";
}

bool EditorUiFlowDocument::canExecuteCommand(
    const std::string& commandId) const
{
    if (commandId == "file.save") {
        return isDirty() && !_path.empty();
    }
    if (commandId == "edit.undo") return canUndo();
    if (commandId == "edit.redo") return canRedo();
    return false;
}

bool EditorUiFlowDocument::executeCommand(const std::string& commandId)
{
    if (commandId == "file.save") {
        std::string error;
        return save(&error);
    }
    if (commandId == "edit.undo") return undo();
    if (commandId == "edit.redo") return redo();
    return false;
}

void EditorUiFlowDocument::onHistoryChanged()
{
    ++_revision;
    refreshDiagnostics();
    notifyChanged();
}

bool EditorUiFlowDocument::selectionExists(
    const EditorUiFlowSelection& selection) const
{
    switch (selection.kind) {
    case EditorUiFlowObjectKind::Document: return true;
    case EditorUiFlowObjectKind::Layer:
        return findById(_flow.layers, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Slot:
        return findById(_flow.slots, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Screen:
        return findById(_flow.screens, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Context:
        return findById(_flow.contexts, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Entry:
        return findById(_flow.entries, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Signal:
        return findById(_flow.signals, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Action:
        return findById(_flow.actions, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Region:
        return findById(_flow.regions, selection.id) != nullptr;
    case EditorUiFlowObjectKind::State: {
        const auto* region = findById(_flow.regions, selection.ownerId);
        return region != nullptr && findById(region->states, selection.id) != nullptr;
    }
    case EditorUiFlowObjectKind::Transition:
        return findById(_flow.transitions, selection.id) != nullptr;
    case EditorUiFlowObjectKind::Graph:
        return findById(_flow.graphs, selection.id) != nullptr;
    }
    return false;
}

bool EditorUiFlowDocument::select(EditorUiFlowSelection selection)
{
    if (!selectionExists(selection)) return false;
    if (_selection == selection) return true;
    _selection = std::move(selection);
    ++_revision;
    notifyChanged();
    return true;
}

std::vector<EditorUiFlowOutlineItem> EditorUiFlowDocument::outline() const
{
    std::vector<EditorUiFlowOutlineItem> result;
    result.push_back({{}, "Flow — " + _flow.id, 0});
    const auto append = [&result](EditorUiFlowObjectKind kind,
                                  const auto& values,
                                  std::string_view prefix) {
        for (const auto& value : values) {
            result.push_back({{kind, value.id, {}},
                              std::string(prefix) + "  " + value.id, 0});
        }
    };
    append(EditorUiFlowObjectKind::Layer, _flow.layers, "Layer");
    append(EditorUiFlowObjectKind::Slot, _flow.slots, "Slot");
    append(EditorUiFlowObjectKind::Screen, _flow.screens, "Screen");
    append(EditorUiFlowObjectKind::Context, _flow.contexts, "Context");
    append(EditorUiFlowObjectKind::Entry, _flow.entries, "Entry");
    append(EditorUiFlowObjectKind::Signal, _flow.signals, "Signal");
    append(EditorUiFlowObjectKind::Action, _flow.actions, "Action");
    for (const auto& region : _flow.regions) {
        result.push_back({{EditorUiFlowObjectKind::Region, region.id, {}},
                          "Region  " + region.id, 0});
        for (const auto& state : region.states) {
            result.push_back({{EditorUiFlowObjectKind::State,
                               state.id, region.id},
                              "    State  " + state.id, 1});
        }
    }
    append(EditorUiFlowObjectKind::Transition, _flow.transitions, "Transition");
    append(EditorUiFlowObjectKind::Graph, _flow.graphs, "Graph");
    return result;
}

EditorUiFlowProperties EditorUiFlowDocument::selectedProperties() const
{
    EditorUiFlowProperties result;
    result.id = _selection.kind == EditorUiFlowObjectKind::Document
        ? _flow.id : _selection.id;
    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Document:
        result.first = _flow.defaultEntry;
        break;
    case EditorUiFlowObjectKind::Layer:
        if (const auto* value = findById(_flow.layers, _selection.id)) {
            result.first = ayt::ui::uiFlowInputPolicyName(value->inputPolicy);
            result.second = std::to_string(value->maxActiveScreens);
            result.number = value->order;
            result.flag = value->blocksLowerInput;
        }
        break;
    case EditorUiFlowObjectKind::Slot:
        if (const auto* value = findById(_flow.slots, _selection.id)) {
            result.first = value->layer;
            result.number = static_cast<std::int32_t>(value->capacity);
            result.flag = value->restorePrevious;
        }
        break;
    case EditorUiFlowObjectKind::Screen:
        if (const auto* value = findById(_flow.screens, _selection.id)) {
            result.first = value->layoutAsset;
            result.second = value->layer;
            result.third = value->slot;
            result.fourth = ayt::ui::uiFlowScopeName(value->scope);
            result.fifth = value->enterAnimation;
            result.sixth = value->exitAnimation;
            result.seventh = joinScreenEvents(value->events);
        }
        break;
    case EditorUiFlowObjectKind::Context:
        if (const auto* value = findById(_flow.contexts, _selection.id)) {
            result.first = joinAssignments(value->slots);
            result.number = value->priority;
        }
        break;
    case EditorUiFlowObjectKind::Entry:
        if (const auto* value = findById(_flow.entries, _selection.id)) {
            result.first = joinList(value->contexts);
            result.second = value->actionGraph;
        }
        break;
    case EditorUiFlowObjectKind::Region:
        if (const auto* value = findById(_flow.regions, _selection.id)) {
            result.first = value->initialState;
        }
        break;
    case EditorUiFlowObjectKind::State:
        if (const auto* region = findById(_flow.regions, _selection.ownerId)) {
            if (const auto* value = findById(region->states, _selection.id)) {
                result.first = value->parent;
                result.second = value->initialChild;
                result.third = joinList(value->contexts);
                result.fourth = value->enterGraph + ", " + value->exitGraph;
            }
        }
        break;
    case EditorUiFlowObjectKind::Transition:
        if (const auto* value = findById(_flow.transitions, _selection.id)) {
            result.first = value->region;
            result.second = value->fromState;
            result.third = value->toState;
            result.fourth = value->triggerSignal;
            result.fifth = value->guardExpression;
            result.sixth = value->actionGraph;
            result.seventh = ayt::ui::uiFlowInterruptPolicyName(
                value->interruptPolicy);
            result.number = value->priority;
        }
        break;
    case EditorUiFlowObjectKind::Signal:
    case EditorUiFlowObjectKind::Action:
    case EditorUiFlowObjectKind::Graph:
        break;
    }
    return result;
}

EditorUiFlowPropertyLabels EditorUiFlowDocument::selectedPropertyLabels() const
{
    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Document:
        return {"Default Entry"};
    case EditorUiFlowObjectKind::Layer:
        return {"Input Policy", "Max Active Screens (0 = unlimited)",
                {}, {}, {}, {}, {}, "Order", "Block Lower"};
    case EditorUiFlowObjectKind::Slot:
        return {"Layer", {}, {}, {}, {}, {}, {}, "Capacity", "Restore"};
    case EditorUiFlowObjectKind::Screen:
        return {"Layout Asset", "Layer", "Slot", "Scope",
                "Enter Animation", "Exit Animation",
                "Widget Events (handler=signal; ...)"};
    case EditorUiFlowObjectKind::Context:
        return {"Assignments (slot=screen; !slot)", {}, {}, {}, {}, {}, {},
                "Priority"};
    case EditorUiFlowObjectKind::Entry:
        return {"Contexts (CSV)", "Action Graph"};
    case EditorUiFlowObjectKind::Region:
        return {"Initial State"};
    case EditorUiFlowObjectKind::State:
        return {"Parent", "Initial Child", "Contexts (CSV)",
                "Enter Graph, Exit Graph"};
    case EditorUiFlowObjectKind::Transition:
        return {"Region", "From State", "To State", "Trigger Signal",
                "Guard Expression", "Action Graph", "Interrupt Policy",
                "Priority"};
    case EditorUiFlowObjectKind::Signal:
        return {"Payload fields are preserved by the wire contract"};
    case EditorUiFlowObjectKind::Action:
        return {"Input fields are preserved by the wire contract"};
    case EditorUiFlowObjectKind::Graph:
        return {"Graph nodes are edited on the canvas"};
    }
    return {};
}

bool EditorUiFlowDocument::renameSelection(
    std::string newId, std::string* error)
{
    newId = trim(std::move(newId));
    if (newId.empty()) {
        if (error != nullptr) *error = "ID cannot be empty.";
        return false;
    }
    const std::string oldId = _selection.kind == EditorUiFlowObjectKind::Document
        ? _flow.id : _selection.id;
    if (newId == oldId) return true;

    const auto duplicate = [&](const auto& values) {
        return findById(values, newId) != nullptr;
    };
    bool exists = false;
    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Document: break;
    case EditorUiFlowObjectKind::Layer: exists = duplicate(_flow.layers); break;
    case EditorUiFlowObjectKind::Slot: exists = duplicate(_flow.slots); break;
    case EditorUiFlowObjectKind::Screen: exists = duplicate(_flow.screens); break;
    case EditorUiFlowObjectKind::Context: exists = duplicate(_flow.contexts); break;
    case EditorUiFlowObjectKind::Entry: exists = duplicate(_flow.entries); break;
    case EditorUiFlowObjectKind::Signal: exists = duplicate(_flow.signals); break;
    case EditorUiFlowObjectKind::Action: exists = duplicate(_flow.actions); break;
    case EditorUiFlowObjectKind::Region: exists = duplicate(_flow.regions); break;
    case EditorUiFlowObjectKind::State: {
        const auto* region = findById(_flow.regions, _selection.ownerId);
        exists = region != nullptr && duplicate(region->states);
        break;
    }
    case EditorUiFlowObjectKind::Transition:
        exists = duplicate(_flow.transitions); break;
    case EditorUiFlowObjectKind::Graph: exists = duplicate(_flow.graphs); break;
    }
    if (exists) {
        if (error != nullptr) *error = "ID already exists in this collection.";
        return false;
    }

    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Document:
        _flow.id = newId;
        break;
    case EditorUiFlowObjectKind::Layer:
        findById(_flow.layers, oldId)->id = newId;
        for (auto& value : _flow.slots) if (value.layer == oldId) value.layer = newId;
        for (auto& value : _flow.screens) if (value.layer == oldId) value.layer = newId;
        break;
    case EditorUiFlowObjectKind::Slot:
        findById(_flow.slots, oldId)->id = newId;
        for (auto& value : _flow.screens) if (value.slot == oldId) value.slot = newId;
        for (auto& context : _flow.contexts) {
            for (auto& value : context.slots) if (value.slot == oldId) value.slot = newId;
        }
        break;
    case EditorUiFlowObjectKind::Screen:
        findById(_flow.screens, oldId)->id = newId;
        for (auto& context : _flow.contexts) {
            for (auto& value : context.slots) if (value.screen == oldId) value.screen = newId;
        }
        break;
    case EditorUiFlowObjectKind::Context:
        findById(_flow.contexts, oldId)->id = newId;
        for (auto& entry : _flow.entries) replace(entry.contexts, oldId, newId);
        for (auto& region : _flow.regions) {
            for (auto& state : region.states) replace(state.contexts, oldId, newId);
        }
        break;
    case EditorUiFlowObjectKind::Entry:
        findById(_flow.entries, oldId)->id = newId;
        if (_flow.defaultEntry == oldId) _flow.defaultEntry = newId;
        break;
    case EditorUiFlowObjectKind::Signal:
        findById(_flow.signals, oldId)->id = newId;
        for (auto& value : _flow.transitions) {
            if (value.triggerSignal == oldId) value.triggerSignal = newId;
        }
        for (auto& screen : _flow.screens) {
            for (auto& event : screen.events) {
                if (event.signal == oldId) event.signal = newId;
            }
        }
        break;
    case EditorUiFlowObjectKind::Action:
        findById(_flow.actions, oldId)->id = newId;
        break;
    case EditorUiFlowObjectKind::Region:
        findById(_flow.regions, oldId)->id = newId;
        for (auto& value : _flow.transitions) {
            if (value.region == oldId) value.region = newId;
        }
        break;
    case EditorUiFlowObjectKind::State: {
        auto* region = findById(_flow.regions, _selection.ownerId);
        findById(region->states, oldId)->id = newId;
        if (region->initialState == oldId) region->initialState = newId;
        for (auto& state : region->states) {
            if (state.parent == oldId) state.parent = newId;
            if (state.initialChild == oldId) state.initialChild = newId;
        }
        for (auto& transition : _flow.transitions) {
            if (transition.region != region->id) continue;
            if (transition.fromState == oldId) transition.fromState = newId;
            if (transition.toState == oldId) transition.toState = newId;
        }
        break;
    }
    case EditorUiFlowObjectKind::Transition:
        findById(_flow.transitions, oldId)->id = newId;
        break;
    case EditorUiFlowObjectKind::Graph:
        findById(_flow.graphs, oldId)->id = newId;
        for (auto& entry : _flow.entries) if (entry.actionGraph == oldId) entry.actionGraph = newId;
        for (auto& region : _flow.regions) for (auto& state : region.states) {
            if (state.enterGraph == oldId) state.enterGraph = newId;
            if (state.exitGraph == oldId) state.exitGraph = newId;
        }
        for (auto& transition : _flow.transitions) {
            if (transition.actionGraph == oldId) transition.actionGraph = newId;
        }
        break;
    }
    if (_selection.kind != EditorUiFlowObjectKind::Document) {
        _selection.id = std::move(newId);
    }
    return true;
}

bool EditorUiFlowDocument::applySelectedProperties(
    const EditorUiFlowProperties& properties, std::string* error)
{
    if (!selectionExists(_selection)) {
        if (error != nullptr) *error = "Selected UI Flow object no longer exists.";
        return false;
    }
    const EditorUiFlowProperties beforeProperties = selectedProperties();
    if (beforeProperties.id == properties.id
        && beforeProperties.first == properties.first
        && beforeProperties.second == properties.second
        && beforeProperties.third == properties.third
        && beforeProperties.fourth == properties.fourth
        && beforeProperties.fifth == properties.fifth
        && beforeProperties.sixth == properties.sixth
        && beforeProperties.seventh == properties.seventh
        && beforeProperties.number == properties.number
        && beforeProperties.flag == properties.flag) {
        if (error != nullptr) error->clear();
        return true;
    }
    std::uint32_t parsedMaximum = 0u;
    std::vector<ayt::ui::UIFlowSlotAssignment> parsedAssignments;
    std::vector<ayt::ui::UIFlowScreenEventBinding> parsedScreenEvents;
    ayt::ui::UIFlowInputPolicy parsedInputPolicy{};
    ayt::ui::UIFlowScope parsedScope{};
    ayt::ui::UIFlowInterruptPolicy parsedInterruptPolicy{};
    if (_selection.kind == EditorUiFlowObjectKind::Layer
        && !parseUnsigned(properties.second, parsedMaximum)) {
        if (error != nullptr) *error = "Max Active Screens must be a non-negative integer.";
        return false;
    }
    if (_selection.kind == EditorUiFlowObjectKind::Context
        && !parseAssignments(properties.first, parsedAssignments, error)) {
        return false;
    }
    if (_selection.kind == EditorUiFlowObjectKind::Screen
        && !parseScreenEvents(
            properties.seventh, parsedScreenEvents, error)) {
        return false;
    }
    if (_selection.kind == EditorUiFlowObjectKind::Layer
        && !parseInputPolicy(properties.first, parsedInputPolicy)) {
        if (error != nullptr) {
            *error = "Input Policy must be passThrough, consumeHandled, or blockLower.";
        }
        return false;
    }
    if (_selection.kind == EditorUiFlowObjectKind::Screen
        && !parseScope(properties.fourth, parsedScope)) {
        if (error != nullptr) {
            *error = "Scope must be application, world, owner, or transient.";
        }
        return false;
    }
    if (_selection.kind == EditorUiFlowObjectKind::Transition
        && !parseInterruptPolicy(properties.seventh, parsedInterruptPolicy)) {
        if (error != nullptr) {
            *error = "Interrupt Policy must be queue, cancelPrevious, reversePrevious, ignoreIfRunning, or coalesce.";
        }
        return false;
    }
    Snapshot before = snapshot();
    if (!renameSelection(properties.id, error)) return false;

    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Document:
        _flow.defaultEntry = trim(properties.first);
        break;
    case EditorUiFlowObjectKind::Layer: {
        auto* value = findById(_flow.layers, _selection.id);
        value->inputPolicy = parsedInputPolicy;
        value->maxActiveScreens = parsedMaximum;
        value->order = properties.number;
        value->blocksLowerInput = properties.flag;
        break;
    }
    case EditorUiFlowObjectKind::Slot: {
        auto* value = findById(_flow.slots, _selection.id);
        value->layer = trim(properties.first);
        value->capacity = static_cast<std::uint32_t>(
            (std::max)(1, properties.number));
        value->restorePrevious = properties.flag;
        break;
    }
    case EditorUiFlowObjectKind::Screen: {
        auto* value = findById(_flow.screens, _selection.id);
        value->layoutAsset = trim(properties.first);
        value->layer = trim(properties.second);
        value->slot = trim(properties.third);
        value->scope = parsedScope;
        value->enterAnimation = trim(properties.fifth);
        value->exitAnimation = trim(properties.sixth);
        value->events = std::move(parsedScreenEvents);
        break;
    }
    case EditorUiFlowObjectKind::Context: {
        auto* value = findById(_flow.contexts, _selection.id);
        value->slots = std::move(parsedAssignments);
        value->priority = properties.number;
        break;
    }
    case EditorUiFlowObjectKind::Entry: {
        auto* value = findById(_flow.entries, _selection.id);
        value->contexts = splitList(properties.first);
        value->actionGraph = trim(properties.second);
        break;
    }
    case EditorUiFlowObjectKind::Region:
        findById(_flow.regions, _selection.id)->initialState = trim(properties.first);
        break;
    case EditorUiFlowObjectKind::State: {
        auto* region = findById(_flow.regions, _selection.ownerId);
        auto* value = findById(region->states, _selection.id);
        value->parent = trim(properties.first);
        value->initialChild = trim(properties.second);
        value->contexts = splitList(properties.third);
        const std::vector<std::string> graphs = splitList(properties.fourth);
        value->enterGraph = graphs.empty() ? std::string{} : graphs[0];
        value->exitGraph = graphs.size() < 2u ? std::string{} : graphs[1];
        break;
    }
    case EditorUiFlowObjectKind::Transition: {
        auto* value = findById(_flow.transitions, _selection.id);
        value->region = trim(properties.first);
        value->fromState = trim(properties.second);
        value->toState = trim(properties.third);
        value->triggerSignal = trim(properties.fourth);
        value->guardExpression = trim(properties.fifth);
        value->actionGraph = trim(properties.sixth);
        value->priority = properties.number;
        value->interruptPolicy = parsedInterruptPolicy;
        break;
    }
    case EditorUiFlowObjectKind::Signal:
    case EditorUiFlowObjectKind::Action:
    case EditorUiFlowObjectKind::Graph:
        break;
    }
    commitMutation(std::move(before), "Edit UI Flow Properties");
    if (error != nullptr) error->clear();
    return true;
}

std::string EditorUiFlowDocument::uniqueId(
    EditorUiFlowObjectKind kind, std::string_view ownerId) const
{
    std::string stem = lowerAscii(kindName(kind));
    if (kind == EditorUiFlowObjectKind::State) stem = "state";
    for (unsigned serial = 1u;; ++serial) {
        const std::string candidate = stem + "_" + std::to_string(serial);
        if (!selectionExists({kind, candidate, std::string(ownerId)})) {
            return candidate;
        }
    }
}

bool EditorUiFlowDocument::addObject(
    EditorUiFlowObjectKind kind,
    std::string ownerId,
    std::string* error)
{
    if (kind == EditorUiFlowObjectKind::Document) {
        if (error != nullptr) *error = "A Flow already has one document root.";
        return false;
    }
    if (kind == EditorUiFlowObjectKind::State && ownerId.empty()) {
        ownerId = _selection.kind == EditorUiFlowObjectKind::Region
            ? _selection.id : _selection.ownerId;
        if (ownerId.empty() && !_flow.regions.empty()) ownerId = _flow.regions[0].id;
        if (findById(_flow.regions, ownerId) == nullptr) {
            if (error != nullptr) *error = "Add a Region before adding a State.";
            return false;
        }
    }
    if ((kind == EditorUiFlowObjectKind::Slot
         || kind == EditorUiFlowObjectKind::Screen)
        && _flow.layers.empty()) {
        if (error != nullptr) *error = "Add a Layer first.";
        return false;
    }
    if (kind == EditorUiFlowObjectKind::Screen && _flow.slots.empty()) {
        if (error != nullptr) *error = "Add a Slot before adding a Screen.";
        return false;
    }

    Snapshot before = snapshot();
    const std::string id = uniqueId(kind, ownerId);
    switch (kind) {
    case EditorUiFlowObjectKind::Layer:
        _flow.layers.push_back({id, static_cast<std::int32_t>(_flow.layers.size() * 100u)});
        break;
    case EditorUiFlowObjectKind::Slot:
        _flow.slots.push_back({id, _flow.layers.front().id, 1u, true});
        break;
    case EditorUiFlowObjectKind::Screen: {
        const auto slot = std::find_if(_flow.slots.begin(), _flow.slots.end(),
            [&](const auto& value) { return value.layer == _flow.layers.front().id; });
        const auto& chosen = slot == _flow.slots.end() ? _flow.slots.front() : *slot;
        _flow.screens.push_back({id, "ui/" + id + ".ui.json",
            chosen.layer, chosen.id, ayt::ui::UIFlowScope::World});
        break;
    }
    case EditorUiFlowObjectKind::Context:
        _flow.contexts.push_back({id});
        break;
    case EditorUiFlowObjectKind::Entry:
        _flow.entries.push_back({id});
        if (_flow.defaultEntry.empty()) _flow.defaultEntry = id;
        break;
    case EditorUiFlowObjectKind::Signal:
        _flow.signals.push_back({id});
        break;
    case EditorUiFlowObjectKind::Action:
        _flow.actions.push_back({id});
        break;
    case EditorUiFlowObjectKind::Region: {
        const std::string initial = "idle";
        _flow.regions.push_back({id, initial, {{initial}}});
        break;
    }
    case EditorUiFlowObjectKind::State:
        findById(_flow.regions, ownerId)->states.push_back({id});
        break;
    case EditorUiFlowObjectKind::Transition: {
        if (_flow.regions.empty() || _flow.signals.empty()) {
            if (error != nullptr) {
                *error = "Add a Region and Signal before adding a Transition.";
            }
            return false;
        }
        const auto& region = _flow.regions.front();
        if (region.states.empty()) {
            if (error != nullptr) *error = "The target Region has no State.";
            return false;
        }
        _flow.transitions.push_back({id, region.id, region.states.front().id,
            region.states.front().id, _flow.signals.front().id});
        break;
    }
    case EditorUiFlowObjectKind::Graph:
        _flow.graphs.push_back({id});
        break;
    case EditorUiFlowObjectKind::Document:
        return false;
    }
    _selection = {kind, id, ownerId};
    commitMutation(std::move(before), "Add UI Flow Object");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiFlowDocument::addGraphNode(
    std::string graphId, std::string nodeType, std::string* error)
{
    graphId = trim(std::move(graphId));
    nodeType = trim(std::move(nodeType));
    auto* graph = findById(_flow.graphs, graphId);
    if (graph == nullptr) {
        if (error != nullptr) *error = "Select an existing Graph first.";
        return false;
    }
    if (nodeType.empty()) {
        if (error != nullptr) *error = "Graph node type cannot be empty.";
        return false;
    }
    std::string id;
    for (unsigned serial = 1u;; ++serial) {
        const std::string candidate = "node_" + std::to_string(serial);
        if (findById(graph->nodes, candidate) == nullptr) {
            id = candidate;
            break;
        }
    }
    Snapshot before = snapshot();
    graph->nodes.push_back({std::move(id), std::move(nodeType), {}});
    commitMutation(std::move(before), "Add UI Flow Graph Node");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiFlowDocument::connectGraphNodes(
    std::string graphId,
    std::string fromNode,
    std::string fromPin,
    std::string toNode,
    std::string toPin,
    std::string* error)
{
    graphId = trim(std::move(graphId));
    fromNode = trim(std::move(fromNode));
    fromPin = trim(std::move(fromPin));
    toNode = trim(std::move(toNode));
    toPin = trim(std::move(toPin));
    auto* graph = findById(_flow.graphs, graphId);
    if (graph == nullptr) {
        if (error != nullptr) *error = "Select an existing Graph first.";
        return false;
    }
    if (findById(graph->nodes, fromNode) == nullptr
        || findById(graph->nodes, toNode) == nullptr) {
        if (error != nullptr) *error = "Graph link endpoint node does not exist.";
        return false;
    }
    if (fromPin.empty() || toPin.empty()) {
        if (error != nullptr) *error = "Graph link endpoints require pin names.";
        return false;
    }
    const auto duplicate = std::find_if(graph->links.begin(), graph->links.end(),
        [&](const auto& value) {
            return value.fromNode == fromNode && value.fromPin == fromPin
                && value.toNode == toNode && value.toPin == toPin;
        });
    if (duplicate != graph->links.end()) {
        if (error != nullptr) *error = "Graph link already exists.";
        return false;
    }
    Snapshot before = snapshot();
    graph->links.push_back({std::move(fromNode), std::move(fromPin),
                            std::move(toNode), std::move(toPin)});
    commitMutation(std::move(before), "Connect UI Flow Graph Nodes");
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiFlowDocument::selectionIsReferenced(std::string& reference) const
{
    const std::string& id = _selection.id;
    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Layer:
        for (const auto& slot : _flow.slots) if (slot.layer == id) { reference = "Slot " + slot.id; return true; }
        for (const auto& screen : _flow.screens) if (screen.layer == id) { reference = "Screen " + screen.id; return true; }
        break;
    case EditorUiFlowObjectKind::Slot:
        for (const auto& screen : _flow.screens) if (screen.slot == id) { reference = "Screen " + screen.id; return true; }
        for (const auto& context : _flow.contexts) for (const auto& slot : context.slots) if (slot.slot == id) { reference = "Context " + context.id; return true; }
        break;
    case EditorUiFlowObjectKind::Screen:
        for (const auto& context : _flow.contexts) for (const auto& slot : context.slots) if (slot.screen == id) { reference = "Context " + context.id; return true; }
        break;
    case EditorUiFlowObjectKind::Context:
        for (const auto& entry : _flow.entries) if (contains(entry.contexts, id)) { reference = "Entry " + entry.id; return true; }
        for (const auto& region : _flow.regions) for (const auto& state : region.states) if (contains(state.contexts, id)) { reference = "State " + state.id; return true; }
        break;
    case EditorUiFlowObjectKind::Entry:
        if (_flow.defaultEntry == id) { reference = "defaultEntry"; return true; }
        break;
    case EditorUiFlowObjectKind::Signal:
        for (const auto& transition : _flow.transitions) if (transition.triggerSignal == id) { reference = "Transition " + transition.id; return true; }
        for (const auto& screen : _flow.screens) for (const auto& event : screen.events) if (event.signal == id) { reference = "Screen " + screen.id; return true; }
        break;
    case EditorUiFlowObjectKind::Region:
        for (const auto& transition : _flow.transitions) if (transition.region == id) { reference = "Transition " + transition.id; return true; }
        break;
    case EditorUiFlowObjectKind::State: {
        const auto* region = findById(_flow.regions, _selection.ownerId);
        if (region != nullptr && region->initialState == id) { reference = "Region initialState"; return true; }
        if (region != nullptr) for (const auto& state : region->states) {
            if (state.parent == id || state.initialChild == id) { reference = "State " + state.id; return true; }
        }
        for (const auto& transition : _flow.transitions) if (transition.region == _selection.ownerId && (transition.fromState == id || transition.toState == id)) { reference = "Transition " + transition.id; return true; }
        break;
    }
    case EditorUiFlowObjectKind::Graph:
        for (const auto& entry : _flow.entries) if (entry.actionGraph == id) { reference = "Entry " + entry.id; return true; }
        for (const auto& region : _flow.regions) for (const auto& state : region.states) if (state.enterGraph == id || state.exitGraph == id) { reference = "State " + state.id; return true; }
        for (const auto& transition : _flow.transitions) if (transition.actionGraph == id) { reference = "Transition " + transition.id; return true; }
        break;
    case EditorUiFlowObjectKind::Action:
    case EditorUiFlowObjectKind::Transition:
    case EditorUiFlowObjectKind::Document:
        break;
    }
    return false;
}

bool EditorUiFlowDocument::deleteSelection(std::string* error)
{
    if (_selection.kind == EditorUiFlowObjectKind::Document) {
        if (error != nullptr) *error = "The Flow document root cannot be deleted.";
        return false;
    }
    std::string reference;
    if (selectionIsReferenced(reference)) {
        if (error != nullptr) *error = "Object is still referenced by " + reference + ".";
        return false;
    }
    Snapshot before = snapshot();
    bool erased = false;
    switch (_selection.kind) {
    case EditorUiFlowObjectKind::Layer: erased = eraseById(_flow.layers, _selection.id); break;
    case EditorUiFlowObjectKind::Slot: erased = eraseById(_flow.slots, _selection.id); break;
    case EditorUiFlowObjectKind::Screen: erased = eraseById(_flow.screens, _selection.id); break;
    case EditorUiFlowObjectKind::Context: erased = eraseById(_flow.contexts, _selection.id); break;
    case EditorUiFlowObjectKind::Entry: erased = eraseById(_flow.entries, _selection.id); break;
    case EditorUiFlowObjectKind::Signal: erased = eraseById(_flow.signals, _selection.id); break;
    case EditorUiFlowObjectKind::Action: erased = eraseById(_flow.actions, _selection.id); break;
    case EditorUiFlowObjectKind::Region: erased = eraseById(_flow.regions, _selection.id); break;
    case EditorUiFlowObjectKind::State:
        if (auto* region = findById(_flow.regions, _selection.ownerId)) erased = eraseById(region->states, _selection.id);
        break;
    case EditorUiFlowObjectKind::Transition: erased = eraseById(_flow.transitions, _selection.id); break;
    case EditorUiFlowObjectKind::Graph: erased = eraseById(_flow.graphs, _selection.id); break;
    case EditorUiFlowObjectKind::Document: break;
    }
    if (!erased) {
        if (error != nullptr) *error = "Selected object no longer exists.";
        return false;
    }
    _selection = {};
    commitMutation(std::move(before), "Delete UI Flow Object");
    if (error != nullptr) error->clear();
    return true;
}

void EditorUiFlowDocument::notifyChanged()
{
    if (_changed != nullptr) _changed();
}

} // namespace ayt::editor
