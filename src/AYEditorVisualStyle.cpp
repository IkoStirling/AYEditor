#include "AYEditor/EditorVisualStyle.h"

#include "AYUI/Button.h"
#include "AYUI/DockCard.h"
#include "AYUI/TextLabel.h"
#include "AYUI/Style.h"
#include "AYUI/Theme.h"
#include "AYUI/TreeView.h"
#include "AYUI/UIManager.h"
#include "AYUI/Widget.h"

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
