#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

namespace ayt::editor {

inline constexpr const char* kEditorDslExtensionId = "ayeditor.dsl";
inline constexpr const char* kEditorDslCompileCommand = "dsl.compile";
inline constexpr const char* kEditorLogiaCompleteCommand = "logia.complete";
inline constexpr const char* kEditorLogiaDefinitionCommand = "logia.definition";

EditorDescriptor makeEditorDslDescriptor();
bool registerEditorDslExtension(EditorExtensionRegistry& registry,
                                std::string* error = nullptr);

} // namespace ayt::editor
