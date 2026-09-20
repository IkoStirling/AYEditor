#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <string>

namespace ayt::editor {

EditorDescriptor makeEditorAnimationDescriptor();
bool registerEditorAnimationExtension(EditorExtensionRegistry& registry,
                                      std::string* error = nullptr);

} // namespace ayt::editor
