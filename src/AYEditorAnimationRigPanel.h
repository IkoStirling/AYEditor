#pragma once
#include <AYEditor/EditorAnimationDocument.h>
#include <AYUI/Authoring/DopeSheet.h>
#include <AYUI/Authoring/NumericFields.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>

namespace ayt::editor {
// Private thin adapter. IDs: fk.<bone>.rotation/position,
// ik.<limb>.target/pole/rotation/weight.
class EditorAnimationRigPanel final : public ayt::ui::VBox {
public:
  EditorAnimationRigPanel(std::shared_ptr<EditorAnimationDocument>,
                          std::function<void(const std::string &)> selected,
                          std::function<void(const std::string &)> changed);
  void refresh();
  void refreshValues(); // Transport-only refresh; no list/snapshot rebuild.
  void selectControl(const std::string &);

private:
  void run(const std::function<bool(std::string *)> &);
  void applyValues();
  std::shared_ptr<EditorAnimationDocument> _document;
  std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> _source;
  std::function<void(const std::string &)> _selected, _changed;
  std::vector<std::string> _ids;
  std::string _control;
  ayt::ui::ComboBox *_picker = nullptr, *_limb = nullptr, *_mode = nullptr;
  ayt::ui::TextInput *_profile = nullptr;
  ayt::ui::authoring::NumericFields *_values = nullptr;
  ayt::ui::authoring::DopeSheet *_sheet = nullptr;
  ayt::ui::TextLabel *_status = nullptr;
  std::vector<ayt::ui::Button *> _editButtons;
  bool _syncing = false;
};
std::shared_ptr<ayt::ui::authoring::ICurveEditorSource>
    makeControlRigTimelineSource(std::shared_ptr<EditorAnimationDocument>);
} // namespace ayt::editor
