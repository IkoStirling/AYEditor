#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <string>

namespace ayt::editor {

inline constexpr const char* kEditorSpriteAnimationExtensionId =
    "ayeditor.tool.sprite-animation";

bool registerEditorSpriteAnimationExtension(
    EditorExtensionRegistry& registry,
    std::string* error = nullptr);

} // namespace ayt::editor
