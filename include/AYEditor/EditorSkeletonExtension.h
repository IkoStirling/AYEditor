#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

namespace ayt::editor {

inline constexpr const char* kEditorSkeletonExtensionId =
    "ayeditor.skeleton";

EditorDescriptor makeEditorSkeletonDescriptor();
bool registerEditorSkeletonExtension(EditorExtensionRegistry& registry,
                                     std::string* error = nullptr);

} // namespace ayt::editor
