#include "AYTest.h"

#include <AYEditor/EditorGameFlowDocument.h>
#include <AYIO/File.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <variant>

#ifndef AY_EDITOR_GAMEFLOW_TEST_ASSET_ROOT
#  define AY_EDITOR_GAMEFLOW_TEST_ASSET_ROOT ""
#endif

namespace editor_game_flow_document_test
{
namespace fs = std::filesystem;

struct TempFile
{
    explicit TempFile(const char* stem)
        : path(fs::temp_directory_path()
               / (std::string("ayeditor_gameflow_") + stem
                  + ".gameflow.json"))
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    ~TempFile()
    {
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    fs::path path;
};

fs::path fixture(std::string_view name)
{
    return fs::path(AY_EDITOR_GAMEFLOW_TEST_ASSET_ROOT)
        / "gameflow" / std::string(name);
}

bool hasDiagnostic(const ayt::editor::EditorGameFlowDocument& document,
                   ayt::app::GameFlowDiagnosticSeverity severity,
                   std::string_view text)
{
    return std::any_of(document.diagnostics().begin(),
        document.diagnostics().end(), [&](const auto& diagnostic) {
            return diagnostic.severity == severity
                && diagnostic.message.find(text) != std::string::npos;
        });
}

const ayt::app::GameFlowValue* argument(
    const ayt::app::GameFlowDocument& document,
    std::string_view id)
{
    if (document.transitions.empty()
        || document.transitions.front().actions.empty()) return nullptr;
    const auto& arguments =
        document.transitions.front().actions.front().arguments;
    const auto found = arguments.find(id);
    return found == arguments.end() ? nullptr : &found->second;
}

} // namespace editor_game_flow_document_test

using namespace ayt::app;
using namespace ayt::editor;

TEST_SUITE(AYEditor_GameFlowDocument)

TEST_CASE(new_document_edits_ids_and_references_atomically)
{
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(error.empty());
    CHECK(document.isValid());
    CHECK(document.flow().initialState == "Boot");
    CHECK(document.flow().findIntent("app.start") != nullptr);

    CHECK(document.addObject(EditorGameFlowObjectKind::State, {}, &error));
    EditorGameFlowProperties state = document.selectedProperties();
    state.id = "Gameplay";
    CHECK(document.applySelectedProperties(state, &error));
    CHECK(document.addObject(
        EditorGameFlowObjectKind::Transition, {}, &error));
    EditorGameFlowProperties transition = document.selectedProperties();
    transition.id = "start-game";
    transition.third = "Gameplay";
    CHECK(document.applySelectedProperties(transition, &error));
    CHECK(document.isValid());

    CHECK(document.select(
        {EditorGameFlowObjectKind::State, "Gameplay"}));
    state = document.selectedProperties();
    state.id = "InGame";
    CHECK(document.applySelectedProperties(state, &error));
    CHECK(document.flow().transitions.front().toState == "InGame");
    CHECK_FALSE(document.deleteSelection(&error));
    CHECK(error.find("referenced") != std::string::npos);

    CHECK(document.undo());
    CHECK(document.flow().findState("Gameplay") != nullptr);
    CHECK(document.flow().transitions.front().toState == "Gameplay");
    CHECK(document.redo());
    CHECK(document.flow().findState("InGame") != nullptr);
    CHECK(document.flow().transitions.front().toState == "InGame");
}

TEST_CASE(intent_field_ids_are_unique_within_their_owner)
{
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(document.addObject(
        EditorGameFlowObjectKind::IntentField, "app.start", &error));
    CHECK(document.selection().id == "field_1");
    CHECK(document.addObject(
        EditorGameFlowObjectKind::IntentField, "app.start", &error));
    CHECK(document.selection().id == "field_2");
    CHECK(document.flow().findIntent("app.start")->payload.size() == 2u);
}

TEST_CASE(registry_drives_argument_defaults_validation_and_plan_building)
{
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(document.actionRegistry().registerActionType({"test.open", {
        {"world", GameFlowValueType::String, true, GameFlowValue("Main")},
        {"fade", GameFlowValueType::Number, false, GameFlowValue(0.25)},
    }}, false, &error));
    CHECK(document.actionRegistry().registerActionType(
        {"test.finish", {}}, false, &error));
    document.actionRegistryChanged();

    CHECK(document.addObject(EditorGameFlowObjectKind::State, {}, &error));
    const std::string targetState = document.selection().id;
    CHECK(document.addObject(
        EditorGameFlowObjectKind::Transition, {}, &error));
    const std::string transitionId = document.selection().id;
    EditorGameFlowProperties transition = document.selectedProperties();
    transition.third = targetState;
    CHECK(document.applySelectedProperties(transition, &error));
    CHECK(document.addAction(transitionId, "test.open", &error));

    const auto defaults = document.selectedArguments();
    CHECK(defaults.size() == 2u);
    CHECK(defaults[0].id == "world");
    CHECK_FALSE(defaults[0].authored);
    CHECK(std::get<std::string>(defaults[0].value.data) == "Main");
    CHECK_FALSE(document.setSelectedArgument(
        "fade", GameFlowValue("wrong"), &error));
    CHECK(document.setSelectedArgument(
        "world", GameFlowValue("Arena"), &error));
    CHECK(document.selection().kind
          == EditorGameFlowObjectKind::ActionArgument);

    CHECK(document.addAction(transitionId, "test.finish", &error));
    CHECK(document.moveSelectedAction(-1, &error));
    CHECK(document.flow().transitions.front().actions.front().action
          == "test.finish");
    CHECK(document.undo());
    CHECK(document.flow().transitions.front().actions.front().action
          == "test.open");
    CHECK(document.redo());

    GameFlowPlan plan;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK(document.buildPlan(plan, &diagnostics));
    CHECK(diagnostics.empty());
    CHECK(plan.document.transitions.front().actions[1].arguments.contains(
        "fade"));
    CHECK(std::get<double>(plan.document.transitions.front()
        .actions[1].arguments.at("fade").data) == 0.25);
    CHECK(std::get<std::string>(plan.document.transitions.front()
        .actions[1].arguments.at("world").data) == "Arena");
}

TEST_CASE(unknown_custom_types_roundtrip_without_losing_nested_arguments)
{
    const auto sourcePath = editor_game_flow_document_test::fixture(
        "unknown-custom.gameflow.json");
    CHECK(std::filesystem::exists(sourcePath));
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize(
        sourcePath.string(), "Unknown Custom", &error));
    CHECK(error.empty());
    CHECK(document.isValid());
    CHECK(editor_game_flow_document_test::hasDiagnostic(document,
        GameFlowDiagnosticSeverity::Warning, "studio.entitled"));
    CHECK(editor_game_flow_document_test::hasDiagnostic(document,
        GameFlowDiagnosticSeverity::Warning, "studio.telemetry"));

    const auto* metadata =
        editor_game_flow_document_test::argument(document.flow(), "metadata");
    const auto* weights =
        editor_game_flow_document_test::argument(document.flow(), "weights");
    CHECK(metadata != nullptr);
    CHECK(weights != nullptr);
    CHECK(metadata != nullptr
          && std::holds_alternative<GameFlowValue::Object>(metadata->data));
    CHECK(weights != nullptr
          && std::holds_alternative<GameFlowValue::Array>(weights->data));

    editor_game_flow_document_test::TempFile saved("custom_roundtrip");
    CHECK(document.saveAs(saved.path.string(), &error));
    EditorGameFlowDocument loaded;
    CHECK(loaded.initialize(saved.path.string(), saved.path.string(), &error));
    CHECK(loaded.isValid());
    CHECK(loaded.flow().transitions.front().guard.arguments
          == document.flow().transitions.front().guard.arguments);
    CHECK(loaded.flow().transitions.front().actions.front().arguments
          == document.flow().transitions.front().actions.front().arguments);

    GameFlowPlan unavailable;
    std::vector<GameFlowDiagnostic> diagnostics;
    CHECK_FALSE(loaded.buildPlan(unavailable, &diagnostics));
    CHECK_FALSE(diagnostics.empty());
}

TEST_CASE(save_recovery_and_reload_keep_document_identity_and_dirty_state)
{
    editor_game_flow_document_test::TempFile saved("source");
    editor_game_flow_document_test::TempFile recovery("recovery");
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(document.saveAs(saved.path.string(), &error));
    CHECK_FALSE(document.isDirty());
    const std::string sourceIdentity = document.path();

    CHECK(document.addObject(EditorGameFlowObjectKind::Intent, {}, &error));
    CHECK(document.isDirty());
    CHECK(document.writeRecoveryCopy(recovery.path.string(), &error));
    CHECK(document.path() == sourceIdentity);
    CHECK(document.isDirty());
    CHECK(ayt::io::File::exists(recovery.path.string()));

    CHECK(document.reload(&error));
    CHECK_FALSE(document.isDirty());
    CHECK(document.flow().intents.size() == 1u);
}

TEST_CASE(normal_save_rejects_known_schema_errors_but_recovery_preserves_them)
{
    editor_game_flow_document_test::TempFile saved("validation_gate");
    editor_game_flow_document_test::TempFile recovery("invalid_recovery");
    EditorGameFlowDocument document;
    std::string error;
    CHECK(document.initialize({}, {}, &error));
    CHECK(document.actionRegistry().registerActionType({"test.required", {
        {"token", GameFlowValueType::String, true, {}},
    }}, false, &error));
    document.actionRegistryChanged();
    CHECK(document.addObject(
        EditorGameFlowObjectKind::Transition, {}, &error));
    const std::string transitionId = document.selection().id;
    CHECK(document.addAction(transitionId, "test.required", &error));
    CHECK_FALSE(document.isValid());

    CHECK_FALSE(document.saveAs(saved.path.string(), &error));
    CHECK(error.find("Required authored argument") != std::string::npos);
    CHECK(document.path().empty());
    CHECK(document.writeRecoveryCopy(recovery.path.string(), &error));
    CHECK(ayt::io::File::exists(recovery.path.string()));

    CHECK(document.setSelectedArgument(
        "token", GameFlowValue("ready"), &error));
    CHECK(document.isValid());
    CHECK(document.saveAs(saved.path.string(), &error));
    CHECK_FALSE(document.isDirty());
}

TEST_SUITE_END
