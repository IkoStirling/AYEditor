#include "AYEditor/EditorUiFlowPreview.h"

#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIManagerFlowScreenHost.h>

#include <algorithm>
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

} // namespace

class EditorUiFlowPreview::Impl {
public:
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
            screens.erase(std::remove_if(screens.begin(), screens.end(),
                [mountId](const auto& value) {
                    return value.mountId == mountId;
                }), screens.end());
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
            found->layerOrder = layerOrder;
            found->orderInLayer = orderInLayer;
            sort();
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

        std::vector<EditorUiFlowPreviewScreen> screens;
        std::unique_ptr<ayt::app::UIManagerFlowScreenHost> visual;
        ayt::app::UIFlowScreenSignalEmitter signalEmitter;
        std::unordered_map<std::uint64_t, std::uint64_t> visualMountIds;
        std::uint64_t nextVisualMountId = 1;
    };

    void refreshStates()
    {
        states.clear();
        if (runtime == nullptr || document == nullptr) return;
        for (const auto& region : document->regions) {
            states[region.id] = std::string(runtime->activeState(region.id));
        }
    }

    Host host;
    std::unique_ptr<ayt::app::UIFlowRuntime> runtime;
    const ayt::ui::UIFlowDocument* document = nullptr;
    std::map<std::string, std::string> states;
    std::map<std::string, bool> guardResults;
    std::vector<EditorUiFlowPreviewTrace> traces;
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
    _impl->host.screens.clear();
    _impl->states.clear();
    _impl->traces.clear();
    _impl->runtime = std::make_unique<ayt::app::UIFlowRuntime>(_impl->host);
    _impl->document = &document;
    std::string localError;
    if (!_impl->runtime->load(document, &localError)) {
        _impl->error = std::move(localError);
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    for (const auto& action : document.actions) {
        _impl->runtime->registerAction(action.id,
            [impl = _impl.get()](const ayt::app::UIFlowActionInvocation& value) {
                impl->traces.push_back({"Action", value.actionId,
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
            impl->traces.push_back({"Guard", std::string(expression),
                result ? "true" : "false"});
            return result;
        });
    _impl->runtime->setGraphRequestHandler(
        [impl = _impl.get()](const ayt::app::UIFlowGraphRequest& request) {
            impl->traces.push_back({"Graph", request.graphId,
                request.transitionId.empty()
                    ? "requested" : "transition " + request.transitionId});
        });
    _impl->runtime->subscribeSignal("*",
        [impl = _impl.get()](std::string_view signal,
                             const ayt::app::UIFlowPayload& payload) {
            impl->traces.push_back({"Signal", std::string(signal),
                std::to_string(payload.size()) + " payload fields"});
        });
    if (!_impl->runtime->start(entry, &localError)
        || !_impl->runtime->beginScope(
            ayt::ui::UIFlowScope::World, "preview-world", &localError)) {
        _impl->error = std::move(localError);
        _impl->runtime.reset();
        _impl->host.screens.clear();
        if (error != nullptr) *error = _impl->error;
        return false;
    }
    _impl->refreshStates();
    _impl->error.clear();
    _impl->traces.push_back({"Preview", std::string(entry), "runtime started"});
    if (error != nullptr) error->clear();
    return true;
}

void EditorUiFlowPreview::stop() noexcept
{
    _impl->runtime.reset();
    _impl->document = nullptr;
    _impl->host.screens.clear();
    _impl->states.clear();
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
    if (_impl->host.visual != nullptr) {
        _impl->host.visual->update(deltaSeconds);
    }
    _impl->refreshStates();
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
    _impl->traces.clear();
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

std::string_view EditorUiFlowPreview::lastError() const noexcept
{
    return _impl->error;
}

} // namespace ayt::editor
