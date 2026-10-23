#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

namespace ayt::editor {

inline constexpr const char* kEditorDslExtensionId = "ayeditor.dsl";
inline constexpr const char* kEditorDslCompileCommand = "dsl.compile";
inline constexpr const char* kEditorLogiaCompleteCommand = "logia.complete";
inline constexpr const char* kEditorLogiaDefinitionCommand = "logia.definition";

/// Source document identity and strict save validation. With Sequence renderer
/// integration, .lshow receives a visual main-Deferred view with shared JSON
/// undo history; without it the validated source view remains available.
EditorDescriptor makeEditorDslDescriptor();
bool registerEditorDslExtension(EditorExtensionRegistry& registry,
                                std::string* error = nullptr);

} // namespace ayt::editor
