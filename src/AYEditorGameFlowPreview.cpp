#include "AYEditor/EditorGameFlowPreview.h"

#include <utility>

namespace ayt::editor
{

class EditorGameFlowPreview::Impl
{
public:
    ayt::app::GameFlowActionRegistry registry;
    ayt::app::GameFlowPlan plan;
    ayt::app::GameFlowCoordinator coordinator;
    std::map<std::string, bool, std::less<>> guardResults;
    std::vector<EditorGameFlowActionInvocation> invocations;
    std::string lastError;
    bool running = false;

    void fail(std::string message)
    {
        lastError = std::move(message);
    }
};

EditorGameFlowPreview::EditorGameFlowPreview()
    : _impl(std::make_unique<Impl>())
{
}

EditorGameFlowPreview::~EditorGameFlowPreview() = default;

bool EditorGameFlowPreview::rebuild(
    const ayt::app::GameFlowDocument& document,
    const ayt::app::GameFlowActionRegistry& authoringRegistry,
    std::string* error)
{
    stop();
    _impl->coordinator.clearTrace();
    _impl->registry.clear();
    _impl->invocations.clear();
    _impl->lastError.clear();

    for (auto definition : authoringRegistry.actionTypes()) {
        const bool asynchronous = definition.asynchronous;
        const std::string actionId = definition.id;
        std::string registrationError;
        if (!_impl->registry.registerAction(
                std::move(definition),
                [this, asynchronous, actionId](
                    const ayt::app::GameFlowActionInvocation& invocation) {
                    _impl->invocations.push_back({
                        invocation.generation,
                        invocation.executionId,
                        std::string(invocation.transitionId),
                        actionId,
                    });
                    return asynchronous
                        ? ayt::app::GameFlowActionResult::pending()
                        : ayt::app::GameFlowActionResult::succeeded();
                }, false, &registrationError)) {
            _impl->fail(std::move(registrationError));
            if (error != nullptr) *error = _impl->lastError;
            return false;
        }
    }
    for (auto definition : authoringRegistry.guardTypes()) {
        const std::string guardId = definition.id;
        std::string registrationError;
        if (!_impl->registry.registerGuard(
                std::move(definition),
                [this, guardId](const ayt::app::GameFlowGuardInvocation&) {
                    const auto found = _impl->guardResults.find(guardId);
                    return found == _impl->guardResults.end() || found->second;
                }, false, &registrationError)) {
            _impl->fail(std::move(registrationError));
            if (error != nullptr) *error = _impl->lastError;
            return false;
        }
    }

    std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
    if (!ayt::app::buildGameFlowPlan(
            document, _impl->registry, _impl->plan, &diagnostics)) {
        _impl->fail(diagnostics.empty()
            ? "GameFlow preview validation failed."
            : diagnostics.front().path + ": " + diagnostics.front().message);
        if (error != nullptr) *error = _impl->lastError;
        return false;
    }
    std::string coordinatorError;
    if (!_impl->coordinator.setPlan(
            &_impl->plan, &_impl->registry, &coordinatorError)) {
        _impl->fail(std::move(coordinatorError));
        if (error != nullptr) *error = _impl->lastError;
        return false;
    }
    _impl->running = true;
    if (error != nullptr) error->clear();
    return true;
}

void EditorGameFlowPreview::stop() noexcept
{
    _impl->coordinator.reset();
    _impl->running = false;
}

ayt::app::GameFlowRequestResult EditorGameFlowPreview::request(
    std::string_view intent, ayt::app::GameFlowPayload payload)
{
    auto result = _impl->coordinator.request(intent, std::move(payload));
    if (!result) _impl->fail(result.message);
    return result;
}

void EditorGameFlowPreview::update(double deltaSeconds)
{
    if (_impl->running) _impl->coordinator.update(deltaSeconds);
}

bool EditorGameFlowPreview::completePending(std::string* error)
{
    const auto current = _impl->coordinator.snapshot();
    if (current.executionId == 0) {
        if (error != nullptr) *error = "No preview action is pending.";
        return false;
    }
    return _impl->coordinator.completeAction(
        current.executionId,
        ayt::app::GameFlowActionResult::succeeded(), error);
}

bool EditorGameFlowPreview::failPending(
    std::string message, std::string* error)
{
    const auto current = _impl->coordinator.snapshot();
    if (current.executionId == 0) {
        if (error != nullptr) *error = "No preview action is pending.";
        return false;
    }
    if (message.empty()) message = "Preview action failed.";
    return _impl->coordinator.completeAction(
        current.executionId,
        ayt::app::GameFlowActionResult::failed(std::move(message)), error);
}

bool EditorGameFlowPreview::cancelActive(std::string message)
{
    return _impl->coordinator.cancelActive(std::move(message));
}

void EditorGameFlowPreview::setGuardResult(
    std::string guardId, bool accepted)
{
    _impl->guardResults[std::move(guardId)] = accepted;
}

void EditorGameFlowPreview::clearGuardResults()
{
    _impl->guardResults.clear();
}

bool EditorGameFlowPreview::isRunning() const noexcept
{
    return _impl->running;
}

ayt::app::GameFlowCoordinatorSnapshot
EditorGameFlowPreview::snapshot() const
{
    return _impl->coordinator.snapshot();
}

const std::vector<ayt::app::GameFlowTraceEntry>&
EditorGameFlowPreview::trace() const noexcept
{
    return _impl->coordinator.trace();
}

const std::vector<EditorGameFlowActionInvocation>&
EditorGameFlowPreview::actionInvocations() const noexcept
{
    return _impl->invocations;
}

void EditorGameFlowPreview::clearTrace()
{
    _impl->coordinator.clearTrace();
    _impl->invocations.clear();
}

std::string_view EditorGameFlowPreview::lastError() const noexcept
{
    return _impl->lastError;
}

} // namespace ayt::editor
