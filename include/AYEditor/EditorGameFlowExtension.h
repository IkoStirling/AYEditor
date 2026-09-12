#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <AYApplication/GameFlowActionRegistry.h>

#include <functional>

namespace ayt::editor
{

inline constexpr const char* kEditorGameFlowExtensionId =
    "ayeditor.game-flow";
inline constexpr const char* kEditorGameFlowValidateCommand =
    "gameflow.validate";
inline constexpr const char* kEditorGameFlowDeleteCommand =
    "gameflow.delete";

// The editor owns only action/guard metadata. Product composition roots use
// this callback to add the same schemas that their runtime installs, without
// introducing runtime handlers into the authoring process.
struct EditorGameFlowExtensionConfig
{
    std::function<void(ayt::app::GameFlowActionRegistry&)>
        configureRegistry;
};

EditorDescriptor makeEditorGameFlowDescriptor(
    EditorGameFlowExtensionConfig config = {});
bool registerEditorGameFlowExtension(
    EditorExtensionRegistry& registry,
    EditorGameFlowExtensionConfig config = {},
    std::string* error = nullptr);

} // namespace ayt::editor
