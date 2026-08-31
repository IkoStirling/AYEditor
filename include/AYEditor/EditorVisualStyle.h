#pragma once

#include "AYEditor/EditorPreferences.h"
#include "AYMath/MathTypes.h"

#include <string>

namespace ayt::ui {
class UIManager;
}

namespace ayt::editor {

inline constexpr const char* kAliyatEditorDarkTheme = "aliyat-editor-dark";

struct EditorDensityMetrics {
    int bodyFontSize = 13;
    int secondaryFontSize = 12;
    float buttonPaddingX = 5.0f;
    float buttonPaddingY = 2.0f;
    float dockHeaderHeight = 20.0f;
    float treeRowHeight = 16.0f;
};

// Registers and activates the editor-owned palette. Unknown names fall back
// to Aliyat Editor Dark so a stale preference can never leave AYUI without an
// active theme.
void installEditorTheme(const std::string& requestedThemeName);

const EditorDensityMetrics& editorDensityMetrics(EditorDensity density);

// Applies typography and component metrics after the JSON tree is loaded.
// Color and density deliberately remain separate: theme swaps do not resize
// the workspace, and density changes do not rewrite the palette.
void applyEditorVisualStyle(ayt::ui::UIManager& ui, EditorDensity density);

ayt::math::FVector4 editorThemeColor(
    const std::string& token,
    const ayt::math::FVector4& fallback);

} // namespace ayt::editor
