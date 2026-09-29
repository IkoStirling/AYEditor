#include <AYAnimationEditor/SkeletonEditorCore.h>
#include <AYEditor/EditorAnimationDocument.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYUI/Authoring/TimelineModel.h>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <optional>

namespace ayt::editor {
using ayt::anim::editor::HumanoidControlRig;
namespace {
class RigRevisionCommand final : public IEditorCommand {
public:
  RigRevisionCommand(std::function<bool()> redo, std::function<bool()> undo,
                     std::string key)
      : _redo(std::move(redo)), _undo(std::move(undo)), _key(std::move(key)) {}
  const std::string &label() const noexcept override { return _label; }
  bool execute() override { return _redo(); }
  bool undo() override { return _undo(); }
  std::string mergeKey() const override { return _key; }
  bool mergeFrom(const IEditorCommand &newer) override {
    const auto *revision = dynamic_cast<const RigRevisionCommand *>(&newer);
    if (!revision || _key.empty() || _key != revision->_key)
      return false;
    _redo = revision->_redo;
    return true;
  }

private:
  std::function<bool()> _redo, _undo;
  std::string _key, _label = "Edit humanoid controls";
};
} // namespace
bool EditorAnimationDocument::createControlRig(const std::string &profile,
                                               std::string *error) {
  if (authoringReadOnly() || !_preview.skeleton()) {
    if (error)
      *error = "Editable clip and bound skeleton required.";
    return false;
  }
  ayt::anim::HumanoidBoneMap mapping;
  if (profile.empty()) {
    for (const auto &spec : ayt::anim::getHumanoidBoneSpecs()) {
      const auto name = std::string(spec.canonicalName);
      const int index = _preview.skeleton()->findBone(name.c_str());
      if (index >= 0)
        (void)mapping.bind(spec.role, index);
    }
  } else {
    ayt::anim::editor::SkeletonEditorCore core;
    auto profilePath = std::filesystem::path(profile);
    if (profilePath.is_relative() && !_projectRoot.empty())
      profilePath = std::filesystem::path(_projectRoot) / profilePath;
    if (!core.open(profilePath.string(), error))
      return false;
    std::error_code pathError;
    if (!std::filesystem::equivalent(core.skeletonPath(),
                                     _preview.skeletonPath(), pathError) ||
        pathError) {
      if (error)
        *error = "Mapping profile belongs to another skeleton.";
      return false;
    }
    mapping = core.mapping();
  }
  HumanoidControlRig generated;
  if (!generated.bind(*_preview.skeleton(), mapping, error))
    return false;
  generated.enabled = true;
  const auto before =
      _preview.controlRig()
          ? std::make_shared<HumanoidControlRig>(*_preview.controlRig())
          : nullptr;
  const auto after = std::make_shared<HumanoidControlRig>(std::move(generated));
  const auto beforeError = _controlRigLoadError;
  if (!_preview.setControlRig(after, error))
    return false;
  if (_history.recordApplied(std::make_unique<RigRevisionCommand>(
          [this, after] {
            if (!_preview.setControlRig(after))
              return false;
            _controlRigLoadError.clear();
            return true;
          },
          [this, before, beforeError] {
            if (!_preview.setControlRig(before))
              return false;
            _controlRigLoadError = beforeError;
            return true;
          },
          ""))) {
    _controlRigLoadError.clear();
    return true;
  }
  (void)_preview.setControlRig(before);
  return false;
}
bool EditorAnimationDocument::clearControlRig(std::string *error) {
  if (authoringReadOnly() ||
      (!_preview.controlRig() && _controlRigLoadError.empty()))
    return false;
  const auto before =
      _preview.controlRig()
          ? std::make_shared<HumanoidControlRig>(*_preview.controlRig())
          : nullptr;
  const auto beforeError = _controlRigLoadError;
  if (!_preview.setControlRig(nullptr, error))
    return false;
  if (_history.recordApplied(std::make_unique<RigRevisionCommand>(
          [this] {
            if (!_preview.setControlRig(nullptr))
              return false;
            _controlRigLoadError.clear();
            return true;
          },
          [this, before, beforeError] {
            if (!_preview.setControlRig(before))
              return false;
            _controlRigLoadError = beforeError;
            return true;
          },
          ""))) {
    _controlRigLoadError.clear();
    return true;
  }
  (void)_preview.setControlRig(before);
  return false;
}
bool EditorAnimationDocument::recordSkeletonBinding(const std::string &before,
                                                    const std::string &after) {
  const auto apply = [this](const std::string &path) {
    if (path.empty())
      _preview.unbindSkeleton();
    else if (!_preview.bindSkeleton(path))
      return false;
    _selectedBone = _preview.bones().empty() ? -1 : 0;
    (void)persistPreviewMetadata();
    return true;
  };
  if (_history.recordApplied(std::make_unique<RigRevisionCommand>(
          [apply, after] { return apply(after); },
          [apply, before] { return apply(before); }, "")))
    return true;
  (void)apply(before);
  return false;
}
bool EditorAnimationDocument::editControlRig(
    const std::function<bool(HumanoidControlRig &)> &edit, std::string *error,
    const std::vector<std::string> *afterIds) {
  const auto *rig = _preview.controlRig();
  if (authoringReadOnly() || !rig || !edit) {
    if (error)
      *error = "No editable control rig.";
    return false;
  }
  auto before = std::make_shared<HumanoidControlRig>(*rig);
  auto after = std::make_shared<HumanoidControlRig>(*rig);
  if (!edit(*after) || before->encode() == after->encode() ||
      !_preview.setControlRig(after, error))
    return false;
  std::optional<ayt::ui::authoring::TimelineSelection> selectionBefore,
      selectionAfter;
  if (const auto selection = _controlRigSelection.lock()) {
    selectionBefore = selectionAfter = *selection;
    auto &next = *selectionAfter;
    std::vector<std::string> mapped;
    if (afterIds)
      mapped = *afterIds;
    else
      for (const auto &id : selection->keyIds) {
        std::size_t index = 0;
        if (!id.starts_with("rig.key."))
          continue;
        const auto parsed =
            std::from_chars(id.data() + 8, id.data() + id.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() ||
            index >= before->keys().size())
          continue;
        for (std::size_t i = 0; i < after->keys().size(); ++i)
          if (std::fabs(after->keys()[i].seconds -
                        before->keys()[index].seconds) < 1e-6) {
            mapped.push_back("rig.key." + std::to_string(i));
            break;
          }
      }
    const auto primary =
        std::find(selection->keyIds.begin(), selection->keyIds.end(),
                  selection->primaryKeyId);
    const auto position =
        primary == selection->keyIds.end()
            ? 0u
            : std::size_t(primary - selection->keyIds.begin());
    next.keyIds = std::move(mapped);
    next.primaryKeyId =
        next.keyIds.empty()
            ? ""
            : next.keyIds[std::min(position, next.keyIds.size() - 1)];
    next.trackId = "rig.pose";
  }
  const auto key = _history.transactionActive()
                       ? "control-rig." + std::to_string(_gestureGeneration)
                       : "";
  if (_history.recordApplied(std::make_unique<RigRevisionCommand>(
          [this, after, selectionAfter] {
            if (!_preview.setControlRig(after))
              return false;
            if (auto s = _controlRigSelection.lock(); s && selectionAfter)
              *s = *selectionAfter;
            return true;
          },
          [this, before, selectionBefore] {
            if (!_preview.setControlRig(before))
              return false;
            if (auto s = _controlRigSelection.lock(); s && selectionBefore)
              *s = *selectionBefore;
            return true;
          },
          key))) {
    if (auto s = _controlRigSelection.lock(); s && selectionAfter)
      *s = *selectionAfter;
    return true;
  }
  (void)_preview.setControlRig(before);
  return false;
}
bool EditorAnimationDocument::switchControlRigLimb(
    ayt::anim::editor::RigLimb limb, bool ik, std::string *error) {
  const auto displayed = _preview.poseWorldMatrices();
  return editControlRig(
      [&](auto &rig) { return rig.switchLimb(limb, ik, displayed, error); },
      error);
}
bool EditorAnimationDocument::recordControlRigKey(std::string *error) {
  if (!_preview.controlRig() || !_preview.controlRig()->enabled) {
    if (error)
      *error = "Enable controls before recording their displayed pose.";
    return false;
  }
  const auto displayed = _preview.poseWorldMatrices();
  const auto time = timelinePositionSeconds(),
             duration = timelineDurationSeconds();
  return editControlRig(
      [&](auto &rig) {
        return rig.captureFK(displayed) && rig.record(time, duration);
      },
      error);
}
bool EditorAnimationDocument::bakeControlRig(double sampleRate,
                                             std::string *error) {
  const auto *rig = _preview.controlRig();
  const auto *clip = _preview.animation();
  if (!rig || !rig->enabled || !clip || authoringReadOnly() ||
      _history.transactionActive())
    return false;
  const auto baked = rig->bake(*clip, sampleRate, error);
  if (!baked || !beginAnimationEditGesture("Bake humanoid controls"))
    return false;
  if (!commitEditedAnimation(baked, error) || !editControlRig(
                                                  [](auto &value) {
                                                    value.enabled = false;
                                                    return true;
                                                  },
                                                  error)) {
    (void)cancelAnimationEditGesture();
    return false;
  }
  return commitAnimationEditGesture();
}
} // namespace ayt::editor
