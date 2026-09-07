#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

namespace ayt::editor {

inline constexpr const char* kEditorDslExtensionId = "ayeditor.dsl";
inline constexpr const char* kEditorDslCompileCommand = "dsl.compile";

EditorDescriptor makeEditorDslDescriptor();
bool registerEditorDslExtension(EditorExtensionRegistry& registry,
                                std::string* error = nullptr);

} // namespace ayt::editor
