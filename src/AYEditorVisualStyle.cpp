#include "AYEditor/EditorVisualStyle.h"

#include "AYUI/Button.h"
#include "AYUI/DockCard.h"
#include "AYUI/MenuBar.h"
#include "AYUI/TextLabel.h"
#include "AYUI/Style.h"
#include "AYUI/Theme.h"
#include "AYUI/TreeView.h"
#include "AYUI/UIManager.h"
#include "AYUI/Widget.h"

#include <algorithm>
#include <vector>

namespace ayt::editor {

namespace {

const EditorDensityMetrics kCompactMetrics{
    13, 12, 5.0f, 2.0f, 20.0f, 16.0f};
const EditorDensityMetrics kComfortableMetrics{
    14, 13, 7.0f, 3.0f, 22.0f, 18.0f};

ayt::ui::Theme makeAliyatEditorDarkTheme()
{
    ayt::ui::Theme theme;
    theme.setColorToken("color.bg.window",
                        ayt::math::FVector4(0.055f, 0.058f, 0.067f, 1.0f));
    theme.setColorToken("color.bg.surface",
                        ayt::math::FVector4(0.090f, 0.094f, 0.105f, 1.0f));
    theme.setColorToken("color.bg.elevated",
                        ayt::math::FVector4(0.145f, 0.150f, 0.170f, 1.0f));
    theme.setColorToken("color.bg.input",
                        ayt::math::FVector4(0.075f, 0.078f, 0.090f, 1.0f));
    theme.setColorToken("color.text.primary",
                        ayt::math::FVector4(0.90f, 0.92f, 0.95f, 1.0f));
    theme.setColorToken("color.text.muted",
                        ayt::math::FVector4(0.64f, 0.68f, 0.74f, 1.0f));
    theme.setColorToken("color.border",
                        ayt::math::FVector4(0.20f, 0.22f, 0.26f, 1.0f));
    theme.setColorToken("color.accent",
                        ayt::math::FVector4(0.16f, 0.40f, 0.70f, 1.0f));
    theme.setColorToken("color.accent.hover",
                        ayt::math::FVector4(0.22f, 0.49f, 0.82f, 1.0f));
    theme.setColorToken("color.danger",
                        ayt::math::FVector4(0.72f, 0.20f, 0.24f, 1.0f));
    theme.setFloatToken("space.xs", 2.0f);
    theme.setFloatToken("space.sm", 4.0f);
    theme.setFloatToken("space.md", 8.0f);
    theme.setFloatToken("radius.sm", 2.0f);

    ayt::ui::StyleSheet chrome;

    ayt::ui::WidgetStyle menuAnchor = ayt::ui::StyleBuilder::makeButton();
    menuAnchor.backgroundColor =
        ayt::math::FVector4(0.12f, 0.13f, 0.16f, 1.0f);
    menuAnchor.borderColor =
        ayt::math::FVector4(0.12f, 0.13f, 0.16f, 1.0f);
    menuAnchor.border.color = menuAnchor.borderColor;
    menuAnchor.border.width = 0.0f;
    menuAnchor.border.cornerRadius = 3.0f;
    menuAnchor.font.fontSize = 13;
    menuAnchor.textColor =
        ayt::math::FVector4(0.90f, 0.92f, 0.95f, 1.0f);
    menuAnchor.backgroundStates.enabled = true;
    menuAnchor.backgroundStates.normal = menuAnchor.backgroundColor;
    menuAnchor.backgroundStates.hovered =
        ayt::math::FVector4(0.19f, 0.21f, 0.25f, 1.0f);
    menuAnchor.backgroundStates.pressed =
        ayt::math::FVector4(0.10f, 0.29f, 0.50f, 1.0f);
    menuAnchor.backgroundStates.disabled =
        ayt::math::FVector4(0.10f, 0.11f, 0.13f, 0.7f);
    menuAnchor.backgroundTransition.enabled = true;
    menuAnchor.backgroundTransition.durationMs = 80.0f;
    chrome.setStyle("editor_menu_anchor", menuAnchor);

    ayt::ui::WidgetStyle toolLauncher = ayt::ui::StyleBuilder::makeButton();
    toolLauncher.backgroundColor =
        ayt::math::FVector4(0.090f, 0.094f, 0.105f, 1.0f);
    toolLauncher.borderColor =
        ayt::math::FVector4(0.20f, 0.22f, 0.26f, 1.0f);
    toolLauncher.border.color = toolLauncher.borderColor;
    toolLauncher.border.width = 1.0f;
    toolLauncher.border.cornerRadius = 4.0f;
    toolLauncher.font.fontSize = 13;
    toolLauncher.textColor =
        ayt::math::FVector4(0.90f, 0.92f, 0.95f, 1.0f);
    toolLauncher.backgroundStates.enabled = true;
    toolLauncher.backgroundStates.normal = toolLauncher.backgroundColor;
    toolLauncher.backgroundStates.hovered =
        ayt::math::FVector4(0.16f, 0.18f, 0.22f, 1.0f);
    toolLauncher.backgroundStates.pressed =
        ayt::math::FVector4(0.10f, 0.31f, 0.55f, 1.0f);
    toolLauncher.backgroundStates.disabled =
        ayt::math::FVector4(0.075f, 0.078f, 0.090f, 0.65f);
    toolLauncher.backgroundTransition.enabled = true;
    toolLauncher.backgroundTransition.durationMs = 90.0f;
    chrome.setStyle("editor_tool_launcher", toolLauncher);

    ayt::ui::WidgetStyle viewOverlay = toolLauncher;
    viewOverlay.backgroundColor =
        ayt::math::FVector4(0.105f, 0.110f, 0.125f, 1.0f);
    viewOverlay.backgroundStates.normal = viewOverlay.backgroundColor;
    viewOverlay.border.cornerRadius = 3.0f;
    chrome.setStyle("editor_view_overlay_button", viewOverlay);

    ayt::ui::WidgetStyle propertyButton =
        ayt::ui::StyleBuilder::makeButton();
    propertyButton.backgroundColor =
        ayt::math::FVector4(0.125f, 0.135f, 0.155f, 1.0f);
    propertyButton.borderColor =
        ayt::math::FVector4(0.27f, 0.29f, 0.34f, 1.0f);
    propertyButton.border.color = propertyButton.borderColor;
    propertyButton.border.width = 1.0f;
    propertyButton.border.cornerRadius = 3.0f;
    propertyButton.font.fontSize = 13;
    propertyButton.textColor =
        ayt::math::FVector4(0.90f, 0.92f, 0.95f, 1.0f);
    propertyButton.backgroundStates.enabled = true;
    propertyButton.backgroundStates.normal = propertyButton.backgroundColor;
    propertyButton.backgroundStates.hovered =
        ayt::math::FVector4(0.18f, 0.20f, 0.24f, 1.0f);
    propertyButton.backgroundStates.pressed =
        ayt::math::FVector4(0.10f, 0.31f, 0.55f, 1.0f);
    propertyButton.backgroundStates.disabled =
        ayt::math::FVector4(0.085f, 0.090f, 0.105f, 0.65f);
    propertyButton.backgroundTransition.enabled = true;
    propertyButton.backgroundTransition.durationMs = 80.0f;
    chrome.setStyle("editor_property_button", propertyButton);

    ayt::ui::WidgetStyle dangerButton = propertyButton;
    dangerButton.backgroundStates.hovered =
        ayt::math::FVector4(0.36f, 0.13f, 0.16f, 1.0f);
    dangerButton.backgroundStates.pressed =
        ayt::math::FVector4(0.55f, 0.14f, 0.18f, 1.0f);
    chrome.setStyle("editor_property_button_danger", dangerButton);

    ayt::ui::WidgetStyle propertySelector = propertyButton;
    propertySelector.backgroundColor =
        ayt::math::FVector4(0.075f, 0.078f, 0.090f, 1.0f);
    propertySelector.backgroundStates.normal =
        propertySelector.backgroundColor;
    chrome.setStyle("editor_property_selector", propertySelector);

    ayt::ui::WidgetStyle propertyInput = propertySelector;
    propertyInput.border.cornerRadius = 3.0f;
    propertyInput.padding = ayt::math::FVector4(4.0f, 2.0f, 4.0f, 2.0f);
    chrome.setStyle("editor_property_input", propertyInput);

    ayt::ui::WidgetStyle windowButton =
        ayt::ui::StyleBuilder::makeButton();
    windowButton.backgroundColor =
        ayt::math::FVector4(0.12f, 0.13f, 0.16f, 1.0f);
    windowButton.borderColor = windowButton.backgroundColor;
    windowButton.border.color = windowButton.borderColor;
    windowButton.border.width = 0.0f;
    windowButton.border.cornerRadius = 2.0f;
    windowButton.font.fontSize = 13;
    windowButton.textColor =
        ayt::math::FVector4(0.84f, 0.87f, 0.91f, 1.0f);
    windowButton.backgroundStates.enabled = true;
    windowButton.backgroundStates.normal = windowButton.backgroundColor;
    windowButton.backgroundStates.hovered =
        ayt::math::FVector4(0.20f, 0.22f, 0.26f, 1.0f);
    windowButton.backgroundStates.pressed =
        ayt::math::FVector4(0.14f, 0.16f, 0.19f, 1.0f);
    windowButton.backgroundStates.disabled = windowButton.backgroundColor;
    windowButton.backgroundTransition.enabled = true;
    windowButton.backgroundTransition.durationMs = 70.0f;
    chrome.setStyle("editor_window_button", windowButton);

    ayt::ui::WidgetStyle closeButton = windowButton;
    closeButton.backgroundStates.hovered =
        ayt::math::FVector4(0.72f, 0.16f, 0.20f, 1.0f);
    closeButton.backgroundStates.pressed =
        ayt::math::FVector4(0.56f, 0.10f, 0.14f, 1.0f);
    chrome.setStyle("editor_window_close_button", closeButton);

    theme.addSheetFragment("editor.chrome", chrome);
    return theme;
}

void setLabelFont(ayt::ui::UIManager& ui, const char* id, int size)
{
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(ui.findById(id))) {
        label->setFontSize(size);
    }
}

} // namespace

void installEditorTheme(const std::string& requestedThemeName)
{
    ayt::ui::ThemeManager& manager = ayt::ui::ThemeManager::get();
    manager.ensureDefaultThemes();
    manager.registerTheme(kAliyatEditorDarkTheme, makeAliyatEditorDarkTheme());
    const std::string active = manager.getTheme(requestedThemeName) != nullptr
        ? requestedThemeName : std::string(kAliyatEditorDarkTheme);
    manager.setActiveTheme(active);
}

const EditorDensityMetrics& editorDensityMetrics(EditorDensity density)
{
    return density == EditorDensity::Comfortable
        ? kComfortableMetrics : kCompactMetrics;
}

void applyEditorVisualStyle(ayt::ui::UIManager& ui, EditorDensity density)
{
    const EditorDensityMetrics& metrics = editorDensityMetrics(density);
    std::vector<ayt::ui::Widget*> pending;
    if (ui.root() != nullptr) pending.push_back(ui.root());
    while (!pending.empty()) {
        ayt::ui::Widget* widget = pending.back();
        pending.pop_back();
        if (widget == nullptr) continue;

        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setFontSize(metrics.bodyFontSize);
        }
        if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
            button->setPadding(metrics.buttonPaddingX, metrics.buttonPaddingY,
                               metrics.buttonPaddingX, metrics.buttonPaddingY);
        }
        if (auto* card = dynamic_cast<ayt::ui::DockCard*>(widget)) {
            card->setHeaderHeight(metrics.dockHeaderHeight);
        }
        if (auto* tree = dynamic_cast<ayt::ui::TreeView*>(widget)) {
            tree->setItemHeight(metrics.treeRowHeight);
        }
        for (ayt::ui::Widget* child : widget->getChildren()) {
            pending.push_back(child);
        }
    }

    // Secondary chrome should recede from content. These ids are stable shell
    // contracts, not visual one-offs, and therefore belong in the density
    // profile instead of bindToolbar().
    const char* secondaryLabels[] = {
        "lbl_document_title", "lbl_active_tool", "lbl_viewport_scene",
        "lbl_status_scene", "lbl_status_network", "lbl_status_renderer",
        "lbl_status_fps", "lbl_unsaved"
    };
    for (const char* id : secondaryLabels) {
        setLabelFont(ui, id, metrics.secondaryFontSize);
    }
    setLabelFont(ui, "lbl_workspace", metrics.secondaryFontSize);
    setLabelFont(ui, "lbl_mode", metrics.secondaryFontSize);
    if (auto* menuBar = dynamic_cast<ayt::ui::MenuBar*>(
            ui.findById("menubar"))) {
        for (ayt::ui::Widget* child : menuBar->getChildren()) {
            if (auto* anchor = dynamic_cast<ayt::ui::Button*>(child)) {
                const float verticalPadding = std::max(
                    1.0f, metrics.buttonPaddingY - 1.0f);
                anchor->setPadding(metrics.buttonPaddingX + 3.0f,
                                   verticalPadding,
                                   metrics.buttonPaddingX + 3.0f,
                                   verticalPadding);
            }
        }
    }

    ui.invalidateLayout();
    ui.layout();
}

ayt::math::FVector4 editorThemeColor(
    const std::string& token,
    const ayt::math::FVector4& fallback)
{
    const ayt::ui::Theme* theme =
        ayt::ui::ThemeManager::get().getActiveTheme();
    return theme != nullptr && theme->hasColorToken(token)
        ? theme->getColorToken(token) : fallback;
}

} // namespace ayt::editor
