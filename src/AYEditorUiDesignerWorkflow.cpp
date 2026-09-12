#include "AYEditor/EditorUiDesignerWorkflow.h"

#include <AYUI/UIFlow.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace ayt::editor {

using nlohmann::json;
namespace fs = std::filesystem;

struct EditorUiDesignerWorkflow::LayoutRecord {
    std::string path;
    std::string sourceText;
    std::vector<EditorUiLayoutHandler> handlers;
};

struct EditorUiDesignerWorkflow::FlowRecord {
    std::string path;
    std::string sourceText;
    ayt::ui::UIFlowDocument document;
};

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

std::string readText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

bool writeText(const fs::path& path, const std::string& text,
               std::string* error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(error, "Could not open " + path.string());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return stream.good() || fail(error, "Could not write " + path.string());
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string normalizedPath(const fs::path& value) {
    std::error_code error;
    fs::path absolute = fs::absolute(value, error);
    if (error) absolute = value;
    std::string result = absolute.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return result;
}

std::string displayPath(const fs::path& value) {
    std::error_code error;
    const fs::path absolute = fs::absolute(value, error);
    return (error ? value : absolute).lexically_normal().string();
}

void collectLayoutHandlers(const json& value,
                           std::vector<EditorUiLayoutHandler>& out) {
    if (!value.is_object()) return;
    const std::string widgetId = value.value("id", std::string{});
    if (value.contains("events") && value["events"].is_object()) {
        for (auto it = value["events"].begin(); it != value["events"].end(); ++it) {
            if (it.value().is_string()
                && !it.value().get_ref<const std::string&>().empty()) {
                out.push_back({widgetId, it.key(), it.value().get<std::string>()});
            }
        }
    }
    if (value.contains("onClick") && value["onClick"].is_string()
        && !value["onClick"].get_ref<const std::string&>().empty()) {
        out.push_back({widgetId, "onClick", value["onClick"].get<std::string>()});
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "events" || it.key() == "reusable"
            || it.key() == "animations") continue;
        if (it.value().is_object()) collectLayoutHandlers(it.value(), out);
        else if (it.value().is_array()) {
            for (const json& child : it.value()) {
                if (child.is_object()) collectLayoutHandlers(child, out);
            }
        }
    }
}

std::string suggestedSignalId(const std::string& screen,
                              const std::string& handler) {
    std::string value = screen.empty() ? handler : screen + "." + handler;
    for (char& ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (!(std::isalnum(byte) || ch == '_' || ch == '.' || ch == '-')) ch = '_';
    }
    return value;
}

bool selected(const std::vector<std::string>& filter,
              const std::string& value) {
    return filter.empty() || std::find(filter.begin(), filter.end(), value)
        != filter.end();
}

json* layoutRoot(json& document) {
    if (document.is_object() && document.contains("root")
        && document["root"].is_object()) return &document["root"];
    return document.is_object() ? &document : nullptr;
}

const json* layoutRoot(const json& document) {
    if (document.is_object() && document.contains("root")
        && document["root"].is_object()) return &document["root"];
    return document.is_object() ? &document : nullptr;
}

void countWidgetIds(const json& value, const std::string& oldId,
                    const std::string& newId, std::size_t& oldCount,
                    std::size_t& newCount) {
    if (!value.is_object()) return;
    if (value.value("id", std::string{}) == oldId) ++oldCount;
    if (value.value("id", std::string{}) == newId) ++newCount;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "reusable" || it.key() == "animations") continue;
        if (it.value().is_object()) {
            countWidgetIds(it.value(), oldId, newId, oldCount, newCount);
        } else if (it.value().is_array()) {
            for (const json& child : it.value()) if (child.is_object()) {
                countWidgetIds(child, oldId, newId, oldCount, newCount);
            }
        }
    }
}

std::size_t renameWidgetId(json& value, const std::string& oldId,
                           const std::string& newId) {
    if (!value.is_object()) return 0u;
    std::size_t count = 0u;
    if (value.value("id", std::string{}) == oldId) {
        value["id"] = newId;
        ++count;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "reusable" || it.key() == "animations") continue;
        if (it.value().is_object()) count += renameWidgetId(it.value(), oldId, newId);
        else if (it.value().is_array()) {
            for (json& child : it.value()) if (child.is_object()) {
                count += renameWidgetId(child, oldId, newId);
            }
        }
    }
    return count;
}

std::size_t renameHandler(json& value, const std::string& oldName,
                          const std::string& newName) {
    if (!value.is_object()) return 0u;
    std::size_t count = 0u;
    if (value.contains("events") && value["events"].is_object()) {
        for (auto& event : value["events"].items()) {
            if (event.value().is_string()
                && event.value().get_ref<const std::string&>() == oldName) {
                event.value() = newName;
                ++count;
            }
        }
    }
    if (value.value("onClick", std::string{}) == oldName) {
        value["onClick"] = newName;
        ++count;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "events" || it.key() == "reusable"
            || it.key() == "animations") continue;
        if (it.value().is_object()) count += renameHandler(it.value(), oldName, newName);
        else if (it.value().is_array()) {
            for (json& child : it.value()) if (child.is_object()) {
                count += renameHandler(child, oldName, newName);
            }
        }
    }
    return count;
}

bool serializeFlow(const ayt::ui::UIFlowDocument& flow, std::string& text,
                   std::string& diagnostic) {
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (ayt::ui::UIFlowSerializer::serialize(flow, text, &diagnostics, true)) return true;
    diagnostic = diagnostics.empty() ? "Flow validation failed"
        : diagnostics.front().path + ": " + diagnostics.front().message;
    return false;
}

bool applyFileTransaction(const std::vector<EditorUiFileEdit>& edits,
                          std::string* error) {
    if (edits.empty()) return true;
    static std::atomic<unsigned long long> serial{0u};
    const std::string suffix = ".ayui-workflow-" + std::to_string(++serial);
    struct Staged { fs::path target; fs::path temporary; fs::path backup; };
    std::vector<Staged> staged;
    staged.reserve(edits.size());

    for (const EditorUiFileEdit& edit : edits) {
        const fs::path target(edit.path);
        if (readText(target) != edit.beforeText) {
            for (const Staged& value : staged) {
                std::error_code ignored;
                fs::remove(value.temporary, ignored);
            }
            return fail(error, "File changed since rename planning: " + edit.path);
        }
        Staged value{target, fs::path(edit.path + suffix + ".tmp"),
                     fs::path(edit.path + suffix + ".bak")};
        std::error_code ignored;
        fs::remove(value.temporary, ignored);
        fs::remove(value.backup, ignored);
        if (!writeText(value.temporary, edit.afterText, error)) {
            for (const Staged& prior : staged) fs::remove(prior.temporary, ignored);
            return false;
        }
        staged.push_back(std::move(value));
    }

    std::size_t backedUp = 0u;
    std::error_code fsError;
    for (; backedUp < staged.size(); ++backedUp) {
        fs::rename(staged[backedUp].target, staged[backedUp].backup, fsError);
        if (fsError) break;
    }
    if (fsError) {
        for (std::size_t i = backedUp; i > 0u; --i) {
            std::error_code ignored;
            fs::rename(staged[i - 1u].backup, staged[i - 1u].target, ignored);
        }
        for (const Staged& value : staged) {
            std::error_code ignored;
            fs::remove(value.temporary, ignored);
        }
        return fail(error, "Could not stage rename transaction: " + fsError.message());
    }

    std::size_t installed = 0u;
    for (; installed < staged.size(); ++installed) {
        fs::rename(staged[installed].temporary, staged[installed].target, fsError);
        if (fsError) break;
    }
    if (fsError) {
        for (std::size_t i = 0u; i < installed; ++i) {
            std::error_code ignored;
            fs::remove(staged[i].target, ignored);
        }
        for (std::size_t i = staged.size(); i > 0u; --i) {
            std::error_code ignored;
            fs::rename(staged[i - 1u].backup, staged[i - 1u].target, ignored);
            fs::remove(staged[i - 1u].temporary, ignored);
        }
        return fail(error, "Could not install rename transaction: " + fsError.message());
    }
    for (const Staged& value : staged) {
        std::error_code ignored;
        fs::remove(value.backup, ignored);
    }
    return true;
}

} // namespace

EditorUiDesignerWorkflow::EditorUiDesignerWorkflow(std::string assetRoot)
    : _assetRoot(std::move(assetRoot)) {}

EditorUiDesignerWorkflow::~EditorUiDesignerWorkflow() = default;

void EditorUiDesignerWorkflow::setAssetRoot(std::string assetRoot) {
    if (_assetRoot == assetRoot) return;
    _assetRoot = std::move(assetRoot);
    _layouts.clear();
    _flows.clear();
    _screenLinks.clear();
    _diagnostics.clear();
    _fileStamps.clear();
    _revision = 0u;
}

bool EditorUiDesignerWorkflow::captureFileStamps(
    std::unordered_map<std::string, FileStamp>& stamps,
    std::string* error) const {
    stamps.clear();
    std::error_code fsError;
    if (_assetRoot.empty() || !fs::is_directory(_assetRoot, fsError)) {
        return fail(error, "UI workflow asset root is not a directory");
    }
    for (fs::recursive_directory_iterator it(
             _assetRoot, fs::directory_options::skip_permission_denied, fsError), end;
         it != end; it.increment(fsError)) {
        if (fsError) {
            fsError.clear();
            continue;
        }
        if (!it->is_regular_file(fsError)) continue;
        const std::string generic = it->path().generic_string();
        if (!endsWith(generic, ".ui.json")
            && !endsWith(generic, ".uiflow.json")) {
            continue;
        }
        const std::string key = normalizedPath(it->path());
        const std::uintmax_t size = it->file_size(fsError);
        if (fsError) {
            fsError.clear();
            continue;
        }
        const auto writeTime = it->last_write_time(fsError);
        if (fsError) {
            fsError.clear();
            continue;
        }
        stamps.emplace(key, FileStamp{
            size, static_cast<std::int64_t>(writeTime.time_since_epoch().count())});
    }
    return true;
}

bool EditorUiDesignerWorkflow::refresh(std::string* error) {
    _layouts.clear();
    _flows.clear();
    _screenLinks.clear();
    _diagnostics.clear();
    std::unordered_map<std::string, FileStamp> nextStamps;
    if (!captureFileStamps(nextStamps, error)) return false;
    std::error_code fsError;
    for (fs::recursive_directory_iterator it(
             _assetRoot, fs::directory_options::skip_permission_denied, fsError), end;
         it != end; it.increment(fsError)) {
        if (fsError) {
            _diagnostics.push_back("Asset scan: " + fsError.message());
            fsError.clear();
            continue;
        }
        if (!it->is_regular_file(fsError)) continue;
        const std::string generic = it->path().generic_string();
        const std::string text = readText(it->path());
        if (endsWith(generic, ".uiflow.json")) {
            ayt::ui::UIFlowDocument flow;
            std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
            if (!ayt::ui::UIFlowSerializer::deserialize(text, flow, &diagnostics)) {
                _diagnostics.push_back(displayPath(it->path())
                    + ": invalid UI Flow document");
                continue;
            }
            _flows.push_back({displayPath(it->path()), text, std::move(flow)});
        } else if (endsWith(generic, ".ui.json")) {
            try {
                const json document = json::parse(text);
                const json* root = layoutRoot(document);
                if (root == nullptr) throw std::runtime_error("missing Widget root");
                LayoutRecord record;
                record.path = displayPath(it->path());
                record.sourceText = text;
                collectLayoutHandlers(*root, record.handlers);
                std::sort(record.handlers.begin(), record.handlers.end(),
                    [](const auto& a, const auto& b) {
                        if (a.handler != b.handler) return a.handler < b.handler;
                        if (a.widgetId != b.widgetId) return a.widgetId < b.widgetId;
                        return a.eventName < b.eventName;
                    });
                record.handlers.erase(std::unique(record.handlers.begin(),
                    record.handlers.end(), [](const auto& a, const auto& b) {
                        return a.handler == b.handler && a.widgetId == b.widgetId
                            && a.eventName == b.eventName;
                    }), record.handlers.end());
                _layouts.push_back(std::move(record));
            } catch (const std::exception& ex) {
                _diagnostics.push_back(displayPath(it->path()) + ": " + ex.what());
            }
        }
    }
    std::sort(_layouts.begin(), _layouts.end(), [](const auto& a, const auto& b) {
        return normalizedPath(a.path) < normalizedPath(b.path);
    });
    std::sort(_flows.begin(), _flows.end(), [](const auto& a, const auto& b) {
        return normalizedPath(a.path) < normalizedPath(b.path);
    });
    for (const FlowRecord& flow : _flows) {
        for (const ayt::ui::UIFlowScreenDefinition& screen : flow.document.screens) {
            fs::path layout(screen.layoutAsset);
            if (!layout.is_absolute()) layout = fs::path(_assetRoot) / layout;
            _screenLinks.push_back({flow.path, screen.id, screen.layoutAsset,
                                    displayPath(layout)});
        }
    }
    _fileStamps = std::move(nextStamps);
    ++_revision;
    return true;
}

bool EditorUiDesignerWorkflow::refreshIfChanged(
    bool* changed, std::vector<std::string>* changedPaths,
    std::string* error) {
    if (changed != nullptr) *changed = false;
    if (changedPaths != nullptr) changedPaths->clear();
    std::unordered_map<std::string, FileStamp> current;
    if (!captureFileStamps(current, error)) return false;
    if (current == _fileStamps) return true;

    if (changedPaths != nullptr) {
        for (const auto& [path, stamp] : current) {
            const auto old = _fileStamps.find(path);
            if (old == _fileStamps.end() || !(old->second == stamp)) {
                changedPaths->push_back(path);
            }
        }
        for (const auto& [path, stamp] : _fileStamps) {
            (void)stamp;
            if (current.find(path) == current.end()) {
                changedPaths->push_back(path);
            }
        }
        std::sort(changedPaths->begin(), changedPaths->end());
        changedPaths->erase(
            std::unique(changedPaths->begin(), changedPaths->end()),
            changedPaths->end());
    }
    if (!refresh(error)) return false;
    if (changed != nullptr) *changed = true;
    return true;
}

const EditorUiDesignerWorkflow::LayoutRecord*
EditorUiDesignerWorkflow::findLayout(const std::string& path) const {
    const std::string key = normalizedPath(path);
    const auto found = std::find_if(_layouts.begin(), _layouts.end(),
        [&key](const LayoutRecord& value) {
            return normalizedPath(value.path) == key;
        });
    return found != _layouts.end() ? &*found : nullptr;
}

const EditorUiDesignerWorkflow::FlowRecord*
EditorUiDesignerWorkflow::findFlow(const std::string& path) const {
    const std::string key = normalizedPath(path);
    const auto found = std::find_if(_flows.begin(), _flows.end(),
        [&key](const FlowRecord& value) {
            return normalizedPath(value.path) == key;
        });
    return found != _flows.end() ? &*found : nullptr;
}

std::vector<EditorUiScreenLayoutLink>
EditorUiDesignerWorkflow::screensForLayout(const std::string& layoutPath) const {
    const std::string key = normalizedPath(layoutPath);
    std::vector<EditorUiScreenLayoutLink> result;
    for (const EditorUiScreenLayoutLink& link : _screenLinks) {
        if (normalizedPath(link.resolvedLayoutPath) == key) result.push_back(link);
    }
    return result;
}

std::vector<EditorUiLayoutHandler>
EditorUiDesignerWorkflow::handlersForLayout(const std::string& layoutPath) const {
    const LayoutRecord* layout = findLayout(layoutPath);
    return layout != nullptr ? layout->handlers
                             : std::vector<EditorUiLayoutHandler>{};
}

std::vector<EditorUiHandlerCompletion>
EditorUiDesignerWorkflow::handlerCompletions(
    const std::string& flowPath, const std::string& screenId) const {
    const FlowRecord* flow = findFlow(flowPath);
    if (flow == nullptr) return {};
    const ayt::ui::UIFlowScreenDefinition* screen = flow->document.findScreen(screenId);
    if (screen == nullptr) return {};
    fs::path layoutPath(screen->layoutAsset);
    if (!layoutPath.is_absolute()) layoutPath = fs::path(_assetRoot) / layoutPath;
    const LayoutRecord* layout = findLayout(layoutPath.string());
    if (layout == nullptr) return {};

    std::unordered_set<std::string> mapped;
    for (const auto& binding : screen->events) mapped.insert(binding.handler);
    std::unordered_set<std::string> emitted;
    std::unordered_set<std::string> reservedSignals;
    for (const auto& signal : flow->document.signals) {
        reservedSignals.insert(signal.id);
    }
    std::vector<EditorUiHandlerCompletion> result;
    for (const EditorUiLayoutHandler& value : layout->handlers) {
        if (mapped.find(value.handler) != mapped.end()
            || !emitted.insert(value.handler).second) continue;
        std::string signal = value.handler;
        bool creates = reservedSignals.find(signal) == reservedSignals.end();
        if (creates) signal = suggestedSignalId(screenId, value.handler);
        creates = reservedSignals.find(signal) == reservedSignals.end();
        if (creates) reservedSignals.insert(signal);
        result.push_back({flow->path, screenId, layout->path, value.handler,
                          std::move(signal), creates});
    }
    return result;
}

bool EditorUiDesignerWorkflow::applyHandlerCompletions(
    const std::string& flowPath, const std::string& screenId,
    const std::vector<std::string>& handlers, std::size_t* applied,
    std::string* error) {
    if (applied != nullptr) *applied = 0u;
    const FlowRecord* record = findFlow(flowPath);
    if (record == nullptr) return fail(error, "Flow is not in the workflow index");
    ayt::ui::UIFlowDocument flow = record->document;
    ayt::ui::UIFlowScreenDefinition* screen = nullptr;
    for (auto& value : flow.screens) if (value.id == screenId) { screen = &value; break; }
    if (screen == nullptr) return fail(error, "Screen does not exist");
    const std::vector<EditorUiHandlerCompletion> completions =
        handlerCompletions(flowPath, screenId);
    std::size_t count = 0u;
    for (const EditorUiHandlerCompletion& completion : completions) {
        if (!selected(handlers, completion.handler)) continue;
        if (completion.createsSignal) flow.signals.push_back({completion.suggestedSignal});
        screen->events.push_back({completion.handler, completion.suggestedSignal});
        ++count;
    }
    if (count == 0u) return true;
    std::string after;
    std::string diagnostic;
    if (!serializeFlow(flow, after, diagnostic)) return fail(error, diagnostic);
    if (!applyFileTransaction({{record->path, record->sourceText, after, count}}, error)) {
        return false;
    }
    if (applied != nullptr) *applied = count;
    return refresh(error);
}

EditorUiRenamePlan EditorUiDesignerWorkflow::planRename(
    const EditorUiRenameRequest& request) const {
    EditorUiRenamePlan plan;
    plan.request = request;
    if (request.oldValue.empty() || request.newValue.empty()) {
        plan.diagnostics.push_back("Rename values cannot be empty");
        return plan;
    }
    if (request.oldValue == request.newValue) {
        plan.diagnostics.push_back("Rename values are identical");
        return plan;
    }
    if ((request.kind == EditorUiRenameKind::WidgetId
        || request.kind == EditorUiRenameKind::WidgetHandler)
        && request.scopePath.empty()) {
        plan.diagnostics.push_back("Widget rename requires a layout scope path");
        return plan;
    }

    if (request.kind == EditorUiRenameKind::LayoutAsset
        || request.kind == EditorUiRenameKind::FlowSignal) {
        for (const FlowRecord& record : _flows) {
            if (!request.scopePath.empty()
                && normalizedPath(request.scopePath) != normalizedPath(record.path)) continue;
            ayt::ui::UIFlowDocument flow = record.document;
            std::size_t count = 0u;
            if (request.kind == EditorUiRenameKind::LayoutAsset) {
                for (auto& screen : flow.screens) if (screen.layoutAsset == request.oldValue) {
                    screen.layoutAsset = request.newValue;
                    ++count;
                }
            } else {
                if (flow.findSignal(request.newValue) != nullptr) {
                    if (flow.findSignal(request.oldValue) != nullptr) {
                        plan.diagnostics.push_back(record.path
                            + ": replacement Signal already exists");
                    }
                    continue;
                }
                for (auto& signal : flow.signals) if (signal.id == request.oldValue) {
                    signal.id = request.newValue;
                    ++count;
                }
                if (count != 0u) {
                    for (auto& transition : flow.transitions) {
                        if (transition.triggerSignal == request.oldValue) {
                            transition.triggerSignal = request.newValue;
                            ++count;
                        }
                    }
                    for (auto& screen : flow.screens) for (auto& event : screen.events) {
                        if (event.signal == request.oldValue) {
                            event.signal = request.newValue;
                            ++count;
                        }
                    }
                }
            }
            if (count == 0u) continue;
            std::string after;
            std::string diagnostic;
            if (!serializeFlow(flow, after, diagnostic)) {
                plan.diagnostics.push_back(record.path + ": " + diagnostic);
                continue;
            }
            plan.edits.push_back({record.path, record.sourceText,
                                  std::move(after), count});
        }
    } else {
        const LayoutRecord* record = findLayout(request.scopePath);
        if (record == nullptr) {
            plan.diagnostics.push_back("Layout scope is not in the workflow index");
            return plan;
        }
        try {
            json document = json::parse(record->sourceText);
            json* root = layoutRoot(document);
            std::size_t count = 0u;
            if (request.kind == EditorUiRenameKind::WidgetId) {
                std::size_t oldCount = 0u;
                std::size_t newCount = 0u;
                countWidgetIds(*root, request.oldValue, request.newValue,
                               oldCount, newCount);
                if (oldCount != 1u) {
                    plan.diagnostics.push_back(oldCount == 0u
                        ? "Widget ID does not exist" : "Widget ID is ambiguous");
                    return plan;
                }
                if (newCount != 0u) {
                    plan.diagnostics.push_back("Replacement Widget ID already exists");
                    return plan;
                }
                count += renameWidgetId(*root, request.oldValue, request.newValue);
                if (document.contains("animations") && document["animations"].is_array()) {
                    for (json& clip : document["animations"]) {
                        if (!clip.is_object() || !clip.contains("tracks")
                            || !clip["tracks"].is_array()) continue;
                        for (json& track : clip["tracks"]) if (track.is_object()
                            && track.value("target", std::string{}) == request.oldValue) {
                            track["target"] = request.newValue;
                            ++count;
                        }
                    }
                }
            } else {
                const auto collides = std::find_if(
                    record->handlers.begin(), record->handlers.end(),
                    [&](const EditorUiLayoutHandler& handler) {
                        return handler.handler == request.newValue;
                    });
                if (collides != record->handlers.end()) {
                    plan.diagnostics.push_back(
                        "Replacement Widget handler already exists in Layout");
                    return plan;
                }
                count += renameHandler(*root, request.oldValue, request.newValue);
                if (count == 0u) {
                    plan.diagnostics.push_back(
                        "Widget handler does not exist in Layout");
                    return plan;
                }
            }
            if (count != 0u) plan.edits.push_back({record->path, record->sourceText,
                                                   document.dump(4), count});
            if (request.kind == EditorUiRenameKind::WidgetHandler) {
                const std::string layoutKey = normalizedPath(record->path);
                for (const FlowRecord& flowRecord : _flows) {
                    ayt::ui::UIFlowDocument flow = flowRecord.document;
                    std::size_t flowCount = 0u;
                    for (auto& screen : flow.screens) {
                        fs::path path(screen.layoutAsset);
                        if (!path.is_absolute()) path = fs::path(_assetRoot) / path;
                        if (normalizedPath(path) != layoutKey) continue;
                        for (auto& event : screen.events) {
                            if (event.handler == request.oldValue) {
                                event.handler = request.newValue;
                                ++flowCount;
                            }
                        }
                    }
                    if (flowCount == 0u) continue;
                    std::string after;
                    std::string diagnostic;
                    if (!serializeFlow(flow, after, diagnostic)) {
                        plan.diagnostics.push_back(flowRecord.path + ": " + diagnostic);
                        continue;
                    }
                    plan.edits.push_back({flowRecord.path, flowRecord.sourceText,
                                          std::move(after), flowCount});
                }
            }
        } catch (const std::exception& ex) {
            plan.diagnostics.push_back(std::string("Layout rename failed: ") + ex.what());
        }
    }
    if (plan.edits.empty()) plan.diagnostics.push_back("No matching references were found");
    plan.safe = !plan.edits.empty() && plan.diagnostics.empty();
    return plan;
}

bool EditorUiDesignerWorkflow::applyRename(const EditorUiRenamePlan& plan,
                                           std::string* error) {
    if (!plan.safe) return fail(error, "Rename plan is not safe to apply");
    if (!applyFileTransaction(plan.edits, error)) return false;
    return refresh(error);
}

} // namespace ayt::editor
