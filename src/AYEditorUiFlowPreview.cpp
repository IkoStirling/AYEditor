#include "AYEditor/EditorUiFlowPreview.h"

#include <AYApplication/UIFlowGraphExecutor.h>
#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIManagerFlowScreenHost.h>

#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace ayt::editor {
namespace {

ayt::app::UIFlowPayload samplePayload(
    const ayt::ui::UIFlowSignalDefinition& signal)
{
    ayt::app::UIFlowPayload payload;
    for (const ayt::ui::UIFlowFieldDefinition& field : signal.payload) {
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            payload[field.id] = field.defaultValue;
            continue;
        }
        switch (field.type) {
        case ayt::ui::UIFlowValueType::Boolean: payload[field.id] = false; break;
        case ayt::ui::UIFlowValueType::Integer: payload[field.id] = std::int64_t{0}; break;
        case ayt::ui::UIFlowValueType::Number: payload[field.id] = 0.0; break;
        case ayt::ui::UIFlowValueType::String:
        case ayt::ui::UIFlowValueType::Entity:
        case ayt::ui::UIFlowValueType::Asset:
            payload[field.id] = std::string{};
            break;
        }
    }
    return payload;
}

ayt::app::UIFlowPayload sampleInputs(
    const ayt::ui::UIFlowActionDefinition& action)
{
    ayt::app::UIFlowPayload inputs;
    for (const ayt::ui::UIFlowFieldDefinition& field : action.inputs) {
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            inputs[field.id] = field.defaultValue;
            continue;
        }
        switch (field.type) {
        case ayt::ui::UIFlowValueType::Boolean: inputs[field.id] = false; break;
        case ayt::ui::UIFlowValueType::Integer: inputs[field.id] = std::int64_t{0}; break;
        case ayt::ui::UIFlowValueType::Number: inputs[field.id] = 0.0; break;
        case ayt::ui::UIFlowValueType::String:
        case ayt::ui::UIFlowValueType::Entity:
        case ayt::ui::UIFlowValueType::Asset:
            inputs[field.id] = std::string{};
            break;
        }
    }
    return inputs;
}

std::string stringInput(const ayt::app::UIFlowPayload& inputs,
                        std::initializer_list<std::string_view> names)
{
    for (const std::string_view name : names) {
        const auto found = inputs.find(std::string(name));
        if (found == inputs.end()) continue;
        if (const auto* value = std::get_if<std::string>(&found->second.data)) {
            return *value;
        }
    }
    return {};
}

std::string debugValue(const ayt::ui::UIFlowValue& value)
{
    if (std::holds_alternative<std::monostate>(value.data)) return "null";
    if (const auto* item = std::get_if<bool>(&value.data)) {
        return *item ? "true" : "false";
    }
    if (const auto* item = std::get_if<std::int64_t>(&value.data)) {
        return std::to_string(*item);
    }
    if (const auto* item = std::get_if<double>(&value.data)) {
        std::ostringstream stream;
        stream << *item;
        return stream.str();
    }
    if (const auto* item = std::get_if<std::string>(&value.data)) {
        return '"' + *item + '"';
    }
    if (const auto* item = std::get_if<ayt::ui::UIFlowValue::Array>(
            &value.data)) {
        return "array[" + std::to_string(item->size()) + "]";
    }
    const auto* item = std::get_if<ayt::ui::UIFlowValue::Object>(&value.data);
    return "object{" + std::to_string(item == nullptr ? 0u : item->size())
        + "}";
}

std::map<std::string, std::string> debugPayload(
    const ayt::app::UIFlowPayload& payload)
{
    std::map<std::string, std::string> result;
    for (const auto& [id, value] : payload) {
        result[id] = debugValue(value);
    }
    return result;
}

std::string debugPayloadText(const ayt::app::UIFlowPayload& payload)
{
    std::string result;
    for (const auto& [id, value] : debugPayload(payload)) {
        if (!result.empty()) result += ", ";
        result += id + "=" + value;
    }
    return result;
}

} // namespace

class EditorUiFlowPreview::Impl {
public:
    Impl()
    {
        host.changed = [this]() { ++presentationRevision; };
    }

    class Host final : public ayt::app::IUIFlowScreenHost {
    public:
        bool mountScreen(const ayt::app::UIFlowScreenMountRequest& request,
                         std::string& error) override
        {
            if (visual != nullptr) {
                ayt::app::UIFlowScreenMountRequest visualRequest = request;
                visualRequest.mountId = nextVisualMountId++;
                if (!visual->mountScreen(visualRequest, error)) return false;
                visualMountIds[request.mountId] = visualRequest.mountId;
            }
            screens.push_back({request.mountId, request.screenId,
                request.layerId, request.slotId, request.contextId,
                request.layerOrder, request.orderInLayer});
            sort();
            notifyChanged();
            return true;
        }

        void unmountScreen(std::uint64_t mountId) noexcept override
        {
            const auto visualMount = visualMountIds.find(mountId);
            if (visual != nullptr && visualMount != visualMountIds.end()) {
                visual->unmountScreen(visualMount->second);
            }
            if (visualMount != visualMountIds.end()) {
                visualMountIds.erase(visualMount);
            }
            const std::size_t oldSize = screens.size();
            screens.erase(std::remove_if(screens.begin(), screens.end(),
                [mountId](const auto& value) {
                    return value.mountId == mountId;
                }), screens.end());
            if (screens.size() != oldSize) notifyChanged();
        }

        void setScreenOrder(std::uint64_t mountId,
                            int layerOrder,
                            std::uint32_t orderInLayer) noexcept override
        {
            if (visual != nullptr) {
                const auto visualMount = visualMountIds.find(mountId);
                if (visualMount != visualMountIds.end()) {
                    visual->setScreenOrder(
                        visualMount->second, layerOrder, orderInLayer);
                }
            }
            const auto found = std::find_if(screens.begin(), screens.end(),
                [mountId](const auto& value) {
                    return value.mountId == mountId;
                });
            if (found == screens.end()) return;
            if (found->layerOrder == layerOrder
                && found->orderInLayer == orderInLayer) return;
            found->layerOrder = layerOrder;
            found->orderInLayer = orderInLayer;
            sort();
            notifyChanged();
        }

        void setSignalEmitter(
            ayt::app::UIFlowScreenSignalEmitter emitter) override
        {
            signalEmitter = std::move(emitter);
            if (visual != nullptr) {
                visual->setSignalEmitter(signalEmitter);
            }
        }

        void sort()
        {
            std::stable_sort(screens.begin(), screens.end(),
                [](const auto& lhs, const auto& rhs) {
                    if (lhs.layerOrder != rhs.layerOrder) {
                        return lhs.layerOrder < rhs.layerOrder;
                    }
                    return lhs.orderInLayer < rhs.orderInLayer;
                });
        }

        void notifyChanged()
        {
            if (changed != nullptr) changed();
        }

        std::vector<EditorUiFlowPreviewScreen> screens;
        std::unique_ptr<ayt::app::UIManagerFlowScreenHost> visual;
        ayt::app::UIFlowScreenSignalEmitter signalEmitter;
        std::unordered_map<std::uint64_t, std::uint64_t> visualMountIds;
        std::uint64_t nextVisualMountId = 1;
        std::function<void()> changed;
    };

    void refreshStates()
    {
        std::map<std::string, std::string> next;
        if (runtime != nullptr && document != nullptr) {
            for (const auto& region : document->regions) {
                next[region.id] = std::string(runtime->activeState(region.id));
            }
        }
        if (next == states) return;
        states = std::move(next);
        ++presentationRevision;
    }

    void appendTrace(EditorUiFlowPreviewTrace value)
    {
        traces.push_back(std::move(value));
        ++presentationRevision;
    }

    void syncGraphDebug()
    {
        for (const auto& value : graphExecutor.trace()) {
            if (value.serial <= lastGraphTraceSerial) continue;
            std::string detail = value.detail;
            const std::string inputs = debugPayloadText(value.inputs);
            const std::string outputs = debugPayloadText(value.outputs);
            if (!inputs.empty()) detail += "  inputs: " + inputs;
            if (!outputs.empty()) detail += "  outputs: " + outputs;
            if (!value.graphId.empty()) {
                detail = value.graphId + "  ·  " + detail;
            }
            appendTrace({"Node", value.nodeId, std::move(detail)});
            lastGraphTraceSerial = value.serial;
        }
        const auto previousPause = debugPause;
        const auto* pause = graphExecutor.debugPause();
        if (pause == nullptr) {
            debugPause.reset();
        } else {
            debugPause = EditorUiFlowDebugPause{
                pause->graphExecutionId,
                pause->graphId,
                pause->nodeId,
                pause->nodeType,
                pause->reason,
                debugPayload(pause->inputs)};
        }
        const bool pauseChanged = previousPause.has_value()
                != debugPause.has_value()
            || (previousPause.has_value() && debugPause.has_value()
                && (previousPause->graphExecutionId
                        != debugPause->graphExecutionId
                    || previousPause->graphId != debugPause->graphId
                    || previousPause->nodeId != debugPause->nodeId
                    || previousPause->nodeType != debugPause->nodeType
                    || previousPause->reason != debugPause->reason
                    || previousPause->inputs != debugPause->inputs));
        if (pauseChanged) ++presentationRevision;
    }

    Host host;
    std::unique_ptr<ayt::app::UIFlowRuntime> runtime;
    ayt::app::UIFlowGraphExecutor graphExecutor;
    std::vector<ayt::ui::UIFlowGraphNodeTypeDefinition> graphNodeTypes;
    const ayt::ui::UIFlowDocument* document = nullptr;
    std::map<std::string, std::string> states;
    std::map<std::string, bool> guardResults;
    std::vector<EditorUiFlowPreviewTrace> traces;
    std::optional<EditorUiFlowDebugPause> debugPause;
    std::uint64_t lastGraphTraceSerial = 0;
    std::uint64_t presentationRevision = 1;
    std::string error;
};

EditorUiFlowPreview::EditorUiFlowPreview()
    : _impl(std::make_unique<Impl>())
{
}

EditorUiFlowPreview::~EditorUiFlowPreview() = default;

bool EditorUiFlowPreview::rebuild(
    const ayt::ui::UIFlowDocument& document,
    std::string_view entry,
    std::string* error)
{
    ++_impl->presentationRevision;
    _impl->host.screens.clear();
    _impl->states.clear();
    _impl->traces.clear();
    _impl->graphExecutor.reset();
    _impl->graphExecutor.clearTrace();
    _impl->lastGraphTraceSerial = 0;
    _impl->debugPause.reset();
    _impl->graphExecutor.clearNodeTypes();
    _impl->runtime = std::make_unique<ayt::app::UIFlowRuntime>(_impl->host);
    _impl->document = &document;
    std::string localError;
    if (!_impl->runtime->load(document, &localError)) {
        _impl->error = std::move(localError);
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    _impl->graphExecutor.setDocument(_impl->runtime->document());
    for (const auto& type : _impl->graphNodeTypes) {
        const std::string nodeType = type.type;
        if (!_impl->graphExecutor.registerNodeType(type,
                [impl = _impl.get(), nodeType](
                    const ayt::app::UIFlowGraphNodeInvocation& invocation) {
                    if (nodeType == "flow.invokeAction"
                        || nodeType == "host.action") {
                        const std::string actionId = stringInput(
                            invocation.inputs, {"action", "actionId"});
                        if (!actionId.empty()) {
                            auto actionInputs = invocation.inputs;
                            actionInputs.erase("action");
                            actionInputs.erase("actionId");
                            const auto result = impl->runtime->invokeAction(
                                actionId, std::move(actionInputs));
                            if (!result.accepted) {
                                return ayt::app::UIFlowGraphNodeResult::failure(
                                    result.message);
                            }
                        }
                        return ayt::app::UIFlowGraphNodeResult::completed(
                            "completed", {{"accepted", true}});
                    } else if (nodeType == "flow.emitSignal") {
                        const std::string signalId = stringInput(
                            invocation.inputs, {"signal", "signalId"});
                        if (!signalId.empty()) {
                            auto payload = invocation.inputs;
                            payload.erase("signal");
                            payload.erase("signalId");
                            std::string signalError;
                            if (!impl->runtime->emitSignal(
                                    signalId, std::move(payload), &signalError)) {
                                return ayt::app::UIFlowGraphNodeResult::failure(
                                    std::move(signalError));
                            }
                        }
                    }
                    return ayt::app::UIFlowGraphNodeResult::completed();
                }, false, &localError)) {
            _impl->error = std::move(localError);
            _impl->graphExecutor.setDocument(nullptr);
            _impl->runtime.reset();
            if (error != nullptr) *error = _impl->error;
            return false;
        }
    }
    for (const auto& action : document.actions) {
        _impl->runtime->registerAction(action.id,
            [impl = _impl.get()](const ayt::app::UIFlowActionInvocation& value) {
                impl->appendTrace({"Action", value.actionId,
                    "mock accepted (" + std::to_string(value.inputs.size())
                        + " input fields)"});
                return ayt::app::UIFlowActionResult::success();
            });
    }
    _impl->runtime->setGuardEvaluator(
        [impl = _impl.get()](std::string_view expression,
                             const ayt::app::UIFlowPayload&,
                             std::string&) {
            const auto found = impl->guardResults.find(std::string(expression));
            const bool result = found == impl->guardResults.end()
                ? true : found->second;
            impl->appendTrace({"Guard", std::string(expression),
                result ? "true" : "false"});
            return result;
        });
    _impl->graphExecutor.setCompletionHandler(
        [impl = _impl.get()](ayt::app::UIFlowGraphExecutionId executionId,
                             bool succeeded,
                             std::string message) {
            if (impl->runtime != nullptr) {
                (void)impl->runtime->completeGraphExecution(
                    executionId, succeeded, std::move(message));
            }
        });
    _impl->runtime->setAsyncGraphRequestHandler(
        [impl = _impl.get()](
            const ayt::app::UIFlowGraphExecutionRequest& request) {
            impl->appendTrace({"Graph", request.graph.graphId,
                request.graph.transitionId.empty()
                    ? "executing" : "transition "
                        + request.graph.transitionId});
            return impl->graphExecutor.start(request);
        },
        [impl = _impl.get()](ayt::app::UIFlowGraphExecutionId executionId,
                             ayt::app::UIFlowGraphInterrupt interrupt) {
            (void)impl->graphExecutor.interruptGraph(executionId, interrupt);
        });
    _impl->runtime->subscribeSignal("*",
        [impl = _impl.get()](std::string_view signal,
                             const ayt::app::UIFlowPayload& payload) {
            impl->appendTrace({"Signal", std::string(signal),
                std::to_string(payload.size()) + " payload fields"});
        });
    if (!_impl->runtime->start(entry, &localError)
        || !_impl->runtime->beginScope(
            ayt::ui::UIFlowScope::World, "preview-world", &localError)) {
        _impl->graphExecutor.setDocument(nullptr);
        _impl->error = std::move(localError);
        _impl->runtime.reset();
        _impl->host.screens.clear();
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    _impl->refreshStates();
    _impl->syncGraphDebug();
    _impl->error.clear();
    _impl->appendTrace({"Preview", std::string(entry), "runtime started"});
    if (error != nullptr) error->clear();
    return true;
}

void EditorUiFlowPreview::stop() noexcept
{
    ++_impl->presentationRevision;
    _impl->graphExecutor.reset();
    _impl->graphExecutor.setDocument(nullptr);
    _impl->runtime.reset();
    _impl->document = nullptr;
    _impl->host.screens.clear();
    _impl->states.clear();
    _impl->debugPause.reset();
}

void EditorUiFlowPreview::setGraphNodeTypes(
    std::vector<ayt::ui::UIFlowGraphNodeTypeDefinition> types)
{
    stop();
    _impl->graphNodeTypes = std::move(types);
}

void EditorUiFlowPreview::configureVisualHost(
    ayt::ui::UIManager& manager,
    ayt::ui::Widget& parent,
    std::string assetRoot)
{
    stop();
    _impl->host.visualMountIds.clear();
    _impl->host.nextVisualMountId = 1;
    _impl->host.visual =
        std::make_unique<ayt::app::UIManagerFlowScreenHost>(
            manager, std::move(assetRoot), &parent);
}

void EditorUiFlowPreview::clearVisualHost() noexcept
{
    stop();
    _impl->host.visual.reset();
    _impl->host.visualMountIds.clear();
}

void EditorUiFlowPreview::tick(float deltaSeconds)
{
    _impl->graphExecutor.update(static_cast<double>(deltaSeconds));
    if (_impl->host.visual != nullptr) {
        _impl->host.visual->update(deltaSeconds);
    }
    _impl->refreshStates();
    _impl->syncGraphDebug();
}

bool EditorUiFlowPreview::isRunning() const noexcept
{
    return _impl->runtime != nullptr && _impl->runtime->isStarted();
}

bool EditorUiFlowPreview::emitSignal(
    std::string_view signalId, std::string* error)
{
    if (!isRunning() || _impl->document == nullptr) {
        _impl->error = "UI Flow preview is not running.";
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    const auto* signal = _impl->document->findSignal(signalId);
    if (signal == nullptr) {
        _impl->error = "Unknown preview Signal.";
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    std::string localError;
    if (!_impl->runtime->emitSignal(
            signalId, samplePayload(*signal), &localError)) {
        _impl->error = std::move(localError);
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    _impl->refreshStates();
    _impl->syncGraphDebug();
    _impl->error.clear();
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiFlowPreview::invokeAction(
    std::string_view actionId, std::string* error)
{
    if (!isRunning() || _impl->document == nullptr) {
        _impl->error = "UI Flow preview is not running.";
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    const auto* action = _impl->document->findAction(actionId);
    if (action == nullptr) {
        _impl->error = "Unknown preview Action.";
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    const ayt::app::UIFlowActionResult result = _impl->runtime->invokeAction(
        actionId, sampleInputs(*action));
    if (!result.accepted) {
        _impl->error = result.message;
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    _impl->error.clear();
    if (error != nullptr) error->clear();
    return true;
}

void EditorUiFlowPreview::setGuardResult(std::string expression, bool value)
{
    _impl->guardResults[std::move(expression)] = value;
}

void EditorUiFlowPreview::clearTrace()
{
    const bool changed = !_impl->traces.empty();
    _impl->traces.clear();
    _impl->graphExecutor.clearTrace();
    _impl->lastGraphTraceSerial = 0;
    if (changed) ++_impl->presentationRevision;
}

bool EditorUiFlowPreview::setBreakpoint(
    std::string graphId, std::string nodeId, bool enabled)
{
    return _impl->graphExecutor.setBreakpoint(
        std::move(graphId), std::move(nodeId), enabled);
}

void EditorUiFlowPreview::clearBreakpoints()
{
    _impl->graphExecutor.clearBreakpoints();
}

void EditorUiFlowPreview::requestPause()
{
    _impl->graphExecutor.requestPause();
}

bool EditorUiFlowPreview::continueExecution(std::string* error)
{
    const bool result = _impl->graphExecutor.continueExecution(error);
    _impl->refreshStates();
    _impl->syncGraphDebug();
    return result;
}

bool EditorUiFlowPreview::stepExecution(std::string* error)
{
    const bool result = _impl->graphExecutor.stepExecution(error);
    _impl->refreshStates();
    _impl->syncGraphDebug();
    return result;
}

bool EditorUiFlowPreview::isPaused() const noexcept
{
    return _impl->graphExecutor.isPaused();
}

const EditorUiFlowDebugPause* EditorUiFlowPreview::debugPause() const noexcept
{
    return _impl->debugPause.has_value() ? &*_impl->debugPause : nullptr;
}

const std::vector<EditorUiFlowPreviewScreen>&
EditorUiFlowPreview::mountedScreens() const
{
    return _impl->host.screens;
}

const std::map<std::string, std::string>&
EditorUiFlowPreview::activeStates() const
{
    return _impl->states;
}

const std::vector<EditorUiFlowPreviewTrace>& EditorUiFlowPreview::trace() const
{
    return _impl->traces;
}

std::uint64_t EditorUiFlowPreview::presentationRevision() const noexcept
{
    return _impl->presentationRevision;
}

std::string_view EditorUiFlowPreview::lastError() const noexcept
{
    return _impl->error;
}

} // namespace ayt::editor
