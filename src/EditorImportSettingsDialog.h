#pragma once
#include <AYResource/ImportJob.h>
#include <AYUI/ModalDialog.h>
#include <functional>
#include <string>
#include <unordered_map>

namespace ayt::ui { class TextLabel; class Tooltip; }
namespace ayt::ui::authoring { class PropertyField; }

namespace ayt::editor {
class EditorImportSettingsDialog final : public ayt::ui::ModalDialog {
public:
    using Apply = std::function<bool(const ayt::resource::ImportOptions&, std::string&)>;
    using Localize = std::function<std::wstring(const char*, const wchar_t*)>;
    using PickSource = std::function<std::string()>;
    EditorImportSettingsDialog(ayt::resource::ImportOptions options, Apply apply, Localize localize,
                              PickSource pickSource = {});
    ~EditorImportSettingsDialog() override;
private:
    bool applyChanges();
    void chooseSource();
    void updateSourceDisplay();
    ayt::resource::ImportOptions _original;
    Apply _apply;
    Localize _localize;
    PickSource _pickSource;
    std::string _sourcePath;
    std::unordered_map<std::string, ayt::ui::authoring::PropertyField*> _fields;
    ayt::ui::TextLabel* _error = nullptr;
    ayt::ui::Tooltip* _sourceTooltip = nullptr;
};
}
