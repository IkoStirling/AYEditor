#include "AYTest.h"

#include <AYEditor/EditorGameFlowPreview.h>

#include <string>
#include <utility>

namespace editor_game_flow_preview_test
{

ayt::app::GameFlowActionRegistry makeRegistry()
{
    using namespace ayt::app;
    GameFlowActionRegistry registry;
    CHECK(registry.registerActionType({"preview.mark", {}, false}));
    CHECK(registry.registerActionType({"preview.wait", {}, true}));
    CHECK(registry.registerGuardType({"preview.allow", {}}));
    return registry;
}

ayt::app::GameFlowDocument makeDocument()
{
    using namespace ayt::app;
    GameFlowDocument document;
    document.id = "preview";
    document.initialState = "menu";
    document.intents = {{"start", {}}, {"ready", {}}};
    document.states = {
        {"menu"}, {"loading"}, {"playing"}, {"failed"}};

    GameFlowTransitionDefinition start;
    start.id = "begin-loading";
    start.fromState = "menu";
    start.triggerIntent = "start";
    start.toState = "loading";
    start.actions = {{"preview.mark", {}}};
    document.transitions.push_back(std::move(start));

    GameFlowTransitionDefinition ready;
    ready.id = "finish-loading";
    ready.fromState = "loading";
    ready.triggerIntent = "ready";
    ready.toState = "playing";
    ready.guard.guard = "preview.allow";
    ready.actions = {{"preview.wait", {}}};
    ready.onFailureState = "failed";
    ready.onCancelState = "menu";
    document.transitions.push_back(std::move(ready));
    return document;
}

} // namespace editor_game_flow_preview_test

using namespace ayt::app;
using namespace ayt::editor;

TEST_SUITE(AYEditor_GameFlowPreview)

TEST_CASE(preview_uses_the_production_plan_and_reports_runtime_highlights)
{
    auto registry = editor_game_flow_preview_test::makeRegistry();
    EditorGameFlowPreview preview;
    std::string error;
    CHECK(preview.rebuild(
        editor_game_flow_preview_test::makeDocument(), registry, &error));
    CHECK(error.empty());
    CHECK(preview.snapshot().status == GameFlowCoordinatorStatus::Idle);
    CHECK(preview.snapshot().currentStateId == "menu");

    CHECK(preview.request("start"));
    preview.update();
    CHECK(preview.snapshot().currentStateId == "loading");
    CHECK(preview.actionInvocations().size() == 1u);

    preview.setGuardResult("preview.allow", false);
    CHECK(preview.request("ready"));
    preview.update();
    CHECK(preview.snapshot().status == GameFlowCoordinatorStatus::Idle);
    CHECK(preview.snapshot().currentStateId == "loading");

    preview.setGuardResult("preview.allow", true);
    CHECK(preview.request("ready"));
    preview.update();
    const GameFlowCoordinatorSnapshot pending = preview.snapshot();
    CHECK(pending.status == GameFlowCoordinatorStatus::WaitingForAction);
    CHECK(pending.activeTransitionId == "finish-loading");
    CHECK(pending.activeActionId == "preview.wait");
    CHECK(pending.activeActionIndex == 0u);
    CHECK(pending.executionId != 0u);
    CHECK(preview.completePending(&error));
    CHECK(preview.snapshot().currentStateId == "playing");
    CHECK_FALSE(preview.trace().empty());
}

TEST_CASE(preview_exposes_failure_and_cancel_routes_without_host_services)
{
    auto registry = editor_game_flow_preview_test::makeRegistry();
    EditorGameFlowPreview preview;
    std::string error;
    const auto enterPending = [&]() {
        CHECK(preview.rebuild(
            editor_game_flow_preview_test::makeDocument(), registry, &error));
        CHECK(preview.request("start"));
        preview.update();
        CHECK(preview.request("ready"));
        preview.update();
        CHECK(preview.snapshot().status
              == GameFlowCoordinatorStatus::WaitingForAction);
    };

    enterPending();
    CHECK(preview.failPending("simulated failure", &error));
    CHECK(preview.snapshot().currentStateId == "failed");

    enterPending();
    CHECK(preview.cancelActive("simulated cancel"));
    CHECK(preview.snapshot().currentStateId == "menu");
}

TEST_CASE(preview_rejects_documents_missing_registered_action_metadata)
{
    auto document = editor_game_flow_preview_test::makeDocument();
    GameFlowActionRegistry incomplete;
    CHECK(incomplete.registerActionType({"preview.mark", {}, false}));
    EditorGameFlowPreview preview;
    std::string error;
    CHECK_FALSE(preview.rebuild(document, incomplete, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(preview.isRunning());
}

TEST_CASE(rebuilding_preview_starts_a_new_diagnostic_trace)
{
    auto registry = editor_game_flow_preview_test::makeRegistry();
    const auto document = editor_game_flow_preview_test::makeDocument();
    EditorGameFlowPreview preview;
    CHECK(preview.rebuild(document, registry));
    CHECK(preview.request("start"));
    preview.update();
    CHECK(preview.trace().size() > 1u);

    CHECK(preview.rebuild(document, registry));
    CHECK(preview.trace().size() == 1u);
    if (!preview.trace().empty()) {
        CHECK(preview.trace().front().detail == "flow initialized");
    }
}

TEST_SUITE_END
