#include "AYEditorAnimationRigPanel.h"
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Authoring/TimelineSelectionOps.h>
#include <AYUI/UnicodeText.h>
#include <charconv>
#include <cmath>
#include <limits>

namespace ayt::editor {
using namespace ayt::ui;
using namespace ayt::ui::authoring;
using namespace ayt::anim::editor;
namespace {
bool keyIndex(const std::string &id, std::size_t &out) {
  if (!id.starts_with("rig.key."))
    return false;
  const auto r = std::from_chars(id.data() + 8, id.data() + id.size(), out);
  return r.ec == std::errc{} && r.ptr == id.data() + id.size();
}
class RigTimelineSource final : public ICurveEditorSource {
public:
  explicit RigTimelineSource(std::shared_ptr<EditorAnimationDocument> d)
      : document(std::move(d)) {
    document->bindControlRigSelection(selectionState());
  }
  std::uint64_t revision() const noexcept override {
    return document->revision();
  }
  double durationSeconds() const noexcept override {
    return document->timelineDurationSeconds();
  }
  double positionSeconds() const noexcept override {
    return document->timelinePositionSeconds();
  }
  bool seek(double time) override {
    return document->setTimelinePositionSeconds(time);
  }
  double snapTime(const std::string &, double time) const override {
    const auto rate = document->animationClipProperties().ticksPerSecond;
    return snapTimeToInterval(time, rate > 0 ? 1 / rate : 0);
  }
  std::shared_ptr<const CurveTrack>
  curveTrack(const std::string &) const override {
    return {};
  }
  std::shared_ptr<const TimelineSnapshot> timelineSnapshot() const override {
    if (cached && stamp == revision())
      return cached;
    auto next = std::make_shared<TimelineSnapshot>();
    next->tracks.push_back(
        {"rig.pose", "Controller pose", TimelineTrackKind::Event});
    if (const auto *rig = document->preview().controlRig())
      for (std::size_t i = 0; i < rig->keys().size(); ++i)
        next->keys.push_back({"rig.key." + std::to_string(i), "rig.pose",
                              rig->keys()[i].seconds});
    cached = next;
    stamp = revision();
    return next;
  }
  bool beginEdit(const std::string &label) override {
    return document->beginAnimationEditGesture(label);
  }
  bool endEdit(bool cancel) override {
    return cancel ? document->cancelAnimationEditGesture()
                  : document->commitAnimationEditGesture();
  }
  bool transformKeys(std::vector<std::string> &ids, double delta, std::size_t,
                     float value) override {
    const auto *rig = document->preview().controlRig();
    if (!rig || value != 0 || !std::isfinite(delta))
      return false;
    std::vector<std::size_t> indices;
    std::vector<double> times;
    for (const auto &id : ids) {
      std::size_t i;
      if (!keyIndex(id, i) || i >= rig->keys().size())
        return false;
      indices.push_back(i);
      times.push_back(rig->keys()[i].seconds + delta);
    }
    auto mapped = ids;
    if (!document->editControlRig(
            [&](auto &next) {
              if (!next.moveKeys(indices, delta, durationSeconds()))
                return false;
              const auto &keys = next.keys();
              for (std::size_t i = 0; i < ids.size(); ++i)
                for (std::size_t k = 0; k < keys.size(); ++k)
                  if (std::fabs(keys[k].seconds - times[i]) < 1e-6) {
                    mapped[i] = "rig.key." + std::to_string(k);
                    break;
                  }
              return true;
            },
            nullptr, &mapped))
      return false;
    ids = std::move(mapped);
    return true;
  }
  bool moveTimelineKey(std::string &, std::string &id, double time) override {
    std::size_t i;
    const auto *rig = document->preview().controlRig();
    if (!rig || !keyIndex(id, i) || i >= rig->keys().size())
      return false;
    std::vector<std::string> ids{id};
    if (!transformKeys(ids, time - rig->keys()[i].seconds, 0, 0))
      return false;
    id = ids[0];
    return true;
  }
  bool removeKeys(const std::vector<std::string> &ids) override {
    std::vector<std::size_t> indices;
    for (const auto &id : ids) {
      std::size_t i;
      if (!keyIndex(id, i))
        return false;
      indices.push_back(i);
    }
    std::sort(indices.begin(), indices.end(), std::greater<>());
    if (indices.empty() ||
        std::adjacent_find(indices.begin(), indices.end()) != indices.end())
      return false;
    const std::vector<std::string> cleared;
    return document->editControlRig(
        [&](auto &rig) {
          for (auto i : indices)
            if (!rig.removeKey(i))
              return false;
          return true;
        },
        nullptr, &cleared);
  }

private:
  std::shared_ptr<EditorAnimationDocument> document;
  mutable std::uint64_t stamp = std::numeric_limits<std::uint64_t>::max();
  mutable std::shared_ptr<const TimelineSnapshot> cached;
};
} // namespace
std::shared_ptr<ICurveEditorSource> makeControlRigTimelineSource(
    std::shared_ptr<EditorAnimationDocument> document) {
  return document ? std::make_shared<RigTimelineSource>(std::move(document))
                  : nullptr;
}
EditorAnimationRigPanel::EditorAnimationRigPanel(
    std::shared_ptr<EditorAnimationDocument> document,
    std::function<void(const std::string &)> selected,
    std::function<void(const std::string &)> changed)
    : _document(std::move(document)), _selected(std::move(selected)),
      _changed(std::move(changed)) {
  setId("animation_control_rig_panel");
  setSpacing(4);
  _status = new TextLabel();
  _status->setFontSize(11);
  addWidget(_status, 24);
  auto *create = new HBox();
  create->setSpacing(4);
  _profile = new TextInput();
  _profile->setId("animation_rig_profile");
  _profile->setPlaceholder(L"Optional mapping .rig; empty = canonical names");
  create->addWidget(_profile, 0);
  auto *generate = new Button();
  generate->setId("animation_rig_create");
  generate->setText(L"Create controls");
  generate->setOnClicked([this] {
    run([this](auto *error) {
      return _document->createControlRig(encodeUtf8Text(_profile->getText()),
                                         error);
    });
  });
  create->addWidget(generate, 110);
  addWidget(create, 28);
  auto *controls = new HBox();
  controls->setSpacing(4);
  _picker = new ComboBox();
  _picker->setId("animation_rig_control");
  _picker->setOnSelectionChanged([this](int i) {
    if (!_syncing && i >= 0 && std::size_t(i) < _ids.size())
      selectControl(_ids[i]);
  });
  controls->addWidget(_picker, 0);
  _limb = new ComboBox();
  _limb->setId("animation_rig_limb");
  _limb->setItems({L"Left arm", L"Right arm", L"Left leg", L"Right leg"});
  _limb->setOnSelectionChanged([this](int) {
    if (!_syncing)
      refresh();
  });
  controls->addWidget(_limb, 106);
  _mode = new ComboBox();
  _mode->setId("animation_rig_mode");
  _mode->setItems({L"FK", L"IK"});
  _mode->setOnSelectionChanged([this](int i) {
    if (_syncing || i < 0)
      return;
    run([&](auto *e) {
      return _document->switchControlRigLimb(RigLimb(_limb->getSelectedIndex()),
                                             i == 1, e);
    });
  });
  controls->addWidget(_mode, 60);
  addWidget(controls, 28);
  _values = new NumericFields({"animation_rig_x", "animation_rig_y",
                               "animation_rig_z", "animation_rig_w"},
                              {L"X", L"Y", L"Z", L"W"});
  _values->setOnSubmitted([this] { applyValues(); });
  addWidget(_values, 28);
  auto *actions = new HBox();
  actions->setSpacing(4);
  const auto button = [&](const char *id, const wchar_t *name,
                          std::function<void()> action) {
    auto *b = new Button();
    b->setId(id);
    b->setText(name);
    b->setOnClicked(std::move(action));
    actions->addWidget(b, 0);
    _editButtons.push_back(b);
  };
  button("animation_rig_apply", L"Apply", [this] { applyValues(); });
  button("animation_rig_record", L"Key pose", [this] {
    run([&](auto *e) { return _document->recordControlRigKey(e); });
  });
  button("animation_rig_toggle", L"Enable / Disable", [this] {
    run([&](auto *e) {
      return _document->editControlRig(
          [](auto &rig) {
            rig.enabled = !rig.enabled;
            return true;
          },
          e);
    });
  });
  button("animation_rig_bake", L"Bake to Clip", [this] {
    run([&](auto *e) { return _document->bakeControlRig(30, e); });
  });
  button("animation_rig_delete", L"Delete pose keys", [this] {
    run([&](auto *) {
      const auto ids = _source->selectionState()->keyIds;
      const bool ok = _source->removeKeys(ids);
      if (ok)
        TimelineSelectionOps::clear(*_source->selectionState());
      return ok;
    });
  });
  addWidget(actions, 28);
  auto *clear = new Button();
  clear->setId("animation_rig_clear");
  clear->setText(L"Clear controls (undoable)");
  clear->setOnClicked(
      [this] { run([&](auto *e) { return _document->clearControlRig(e); }); });
  addWidget(clear, 24);
  _editButtons.push_back(clear);
  _source = makeControlRigTimelineSource(_document);
  _sheet = new DopeSheet(_source);
  _sheet->setId("animation_rig_timeline");
  _sheet->setOnEdited([this] {
    if (_changed)
      _changed("");
    refresh();
  });
  addWidget(_sheet, 80);
  refresh();
}
void EditorAnimationRigPanel::run(
    const std::function<bool(std::string *)> &action) {
  std::string error;
  if (_document->authoringReadOnly() || !action(&error)) {
    if (_changed)
      _changed(error.empty() ? "Controller operation rejected." : error);
  } else if (_changed)
    _changed("");
  refresh();
}
void EditorAnimationRigPanel::selectControl(const std::string &id) {
  _control = id;
  refresh();
  if (_selected)
    _selected(id);
}
void EditorAnimationRigPanel::refresh() {
  _syncing = true;
  const auto *rig = _document->preview().controlRig();
  _ids.clear();
  std::vector<std::wstring> items;
  const auto add = [&](std::string id, std::wstring label) {
    _ids.push_back(std::move(id));
    items.push_back(std::move(label));
  };
  if (rig) {
    for (const auto &h : rig->handles()) {
      const auto name =
          decodeUtf8Text(std::string(ayt::anim::getHumanoidBoneName(h.role)));
      add("fk." + std::to_string(h.bone) + ".rotation", name + L" FK rotation");
      if (h.translation)
        add("fk." + std::to_string(h.bone) + ".position", name + L" position");
    }
    const wchar_t *names[] = {L"Left arm", L"Right arm", L"Left leg",
                              L"Right leg"};
    for (unsigned i = 0; i < 4; ++i)
      if (rig->limbBone(RigLimb(i), 0) >= 0)
        for (const auto *type : {"target", "pole", "rotation", "weight"})
          add("ik." + std::to_string(i) + "." + type,
              std::wstring(names[i]) + L" IK " + decodeUtf8Text(type));
  }
  _picker->setItems(items);
  auto found = std::find(_ids.begin(), _ids.end(), _control);
  const int index =
      found == _ids.end() ? (_ids.empty() ? -1 : 0) : int(found - _ids.begin());
  _picker->setSelectedIndex(index);
  _control = index >= 0 ? _ids[index] : "";
  if (_selected)
    _selected(_control);
  _status->setText(!rig ? L"CONTROL RIG — bind skeleton, then create controls"
                   : rig->enabled
                       ? L"CONTROL RIG — active · model-space · whole-pose keys"
                       : L"CONTROL RIG — disabled / baked");
  const bool readOnly = _document->authoringReadOnly();
  const int limb = std::max(0, _limb->getSelectedIndex());
  if (_limb->getSelectedIndex() < 0)
    _limb->setSelectedIndex(0);
  _mode->setEnabled(rig && rig->limbBone(RigLimb(limb), 0) >= 0 && !readOnly);
  _mode->setSelectedIndex(rig && rig->pose().limbs[limb].ik ? 1 : 0);
  for (auto *b : _editButtons)
    b->setEnabled(!readOnly &&
                  (rig || (b->getId() == "animation_rig_clear" &&
                           !_document->controlRigLoadError().empty())));
  if (!_document->controlRigLoadError().empty())
    _status->setText(L"CONTROL RIG ERROR: " +
                     decodeUtf8Text(_document->controlRigLoadError()));
  _sheet->markDirty();
  _syncing = false;
  refreshValues();
}
void EditorAnimationRigPanel::refreshValues() {
  const auto *rig = _document->preview().controlRig();
  const bool readOnly =
      _document->authoringReadOnly() || _document->timelinePlaying();
  const auto limb = RigLimb(std::max(0, _limb->getSelectedIndex()));
  _mode->setSelectedIndex(rig && rig->pose().limbs[unsigned(limb)].ik ? 1 : 0);
  _mode->setEnabled(rig && rig->limbBone(limb, 0) >= 0 && !readOnly);
  if (!_document->preview().controlRigError().empty())
    _status->setText(L"CONTROL RIG ERROR: " +
                     decodeUtf8Text(_document->preview().controlRigError()));
  std::vector<float> values;
  if (rig && !_control.empty()) {
    const auto dot = _control.find('.', 3);
    const unsigned i = unsigned(std::stoul(_control.substr(3, dot - 3)));
    const auto type = _control.substr(dot + 1);
    if (_control.starts_with("fk.")) {
      auto position = rig->pose().fk.positions[i];
      auto rotation = rig->pose().fk.rotations[i];
      if (!rig->pose().overrides[i]) {
        const auto &world = _document->preview().poseWorldMatrices();
        const auto &bones = _document->preview().bones();
        if (i < world.size() && i < bones.size()) {
          const auto local =
              bones[i].parentIndex < 0
                  ? world[i]
                  : world[std::size_t(bones[i].parentIndex)].inverse() *
                        world[i];
          ayt::math::FVector3 scale;
          (void)local.decompose(position, rotation, scale);
        }
      }
      if (type == "rotation") {
        const auto q = rotation;
        values = {q.x, q.y, q.z, q.w};
      } else {
        const auto v = position;
        values = {v.x, v.y, v.z};
      }
    } else {
      const auto &c = rig->pose().limbs[i];
      if (type == "weight")
        values = {c.weight};
      else if (type == "rotation")
        values = {c.tipRotation.x, c.tipRotation.y, c.tipRotation.z,
                  c.tipRotation.w};
      else {
        const auto v = type == "target" ? c.target : c.pole;
        values = {v.x, v.y, v.z};
      }
    }
  }
  _values->setValues(values, readOnly || !rig);
  _values->setUnit(L"Model space / local FK quaternion");
  _sheet->markDirty();
}
void EditorAnimationRigPanel::applyValues() {
  const auto *rig = _document->preview().controlRig();
  if (!rig || _control.empty())
    return;
  const auto dot = _control.find('.', 3);
  const unsigned i = unsigned(std::stoul(_control.substr(3, dot - 3)));
  const auto type = _control.substr(dot + 1);
  std::vector<float> v;
  const auto count = type == "rotation" ? 4u : type == "weight" ? 1u : 3u;
  if (!_values->readValues(count, v))
    return;
  const auto displayed = _document->preview().poseWorldMatrices();
  run([&](auto *error) {
    return _document->editControlRig(
        [&](auto &next) {
          if (_control.starts_with("fk.") && !next.captureFK(displayed))
            return false;
          auto p = next.pose();
          if (_control.starts_with("fk.")) {
            p.overrides[i] = true;
            if (type == "rotation")
              p.fk.rotations[i] = {v[0], v[1], v[2], v[3]};
            else
              p.fk.positions[i] = {v[0], v[1], v[2]};
          } else {
            auto &c = p.limbs[i];
            if (type == "weight")
              c.weight = v[0];
            else if (type == "rotation")
              c.tipRotation = {v[0], v[1], v[2], v[3]};
            else if (type == "target")
              c.target = {v[0], v[1], v[2]};
            else
              c.pole = {v[0], v[1], v[2]};
          }
          return next.setPose(std::move(p), error);
        },
        error);
  });
}
} // namespace ayt::editor
