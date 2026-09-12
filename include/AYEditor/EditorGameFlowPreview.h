#pragma once

#include <AYApplication/GameFlowCoordinator.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::editor
{

struct EditorGameFlowActionInvocation
{
    ayt::app::GameFlowGeneration generation = 0;
    ayt::app::GameFlowActionExecutionId executionId = 0;
    std::string transitionId;
    std::string actionId;
};

// Executes the production normalized plan and coordinator with deterministic
// mock host actions. It is deliberately renderer independent so the visual
// editor and headless authoring tests observe the same transition semantics.
class EditorGameFlowPreview
{
public:
    EditorGameFlowPreview();
    ~EditorGameFlowPreview();

    EditorGameFlowPreview(const EditorGameFlowPreview&) = delete;
    EditorGameFlowPreview& operator=(const EditorGameFlowPreview&) = delete;

    bool rebuild(
        const ayt::app::GameFlowDocument& document,
        const ayt::app::GameFlowActionRegistry& authoringRegistry,
        std::string* error = nullptr);
    void stop() noexcept;

    ayt::app::GameFlowRequestResult request(
        std::string_view intent,
        ayt::app::GameFlowPayload payload = {});
    void update(double deltaSeconds = 0.0);
    bool completePending(std::string* error = nullptr);
    bool failPending(std::string message = {},
                     std::string* error = nullptr);
    bool cancelActive(std::string message = {});

    void setGuardResult(std::string guardId, bool accepted);
    void clearGuardResults();

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] ayt::app::GameFlowCoordinatorSnapshot snapshot() const;
    [[nodiscard]] const std::vector<ayt::app::GameFlowTraceEntry>& trace()
        const noexcept;
    [[nodiscard]] const std::vector<EditorGameFlowActionInvocation>&
        actionInvocations() const noexcept;
    void clearTrace();
    [[nodiscard]] std::string_view lastError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::editor
