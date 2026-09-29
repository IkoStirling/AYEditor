#include <AYEditor/EditorSequenceDocument.h>
#include <AYEntity.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYResource.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYScene.h>
#include <AYSequence/AnimationSequenceDriver.h>
#include <AYSequence/SequenceAssetIO.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ayt::editor {
using namespace ayt::sequence;
namespace {
class SnapshotCommand final : public IEditorCommand {
  public:
    SnapshotCommand(std::shared_ptr<SequenceAuthoring> owner,
                    std::shared_ptr<const CompiledSequence> before,
                    std::shared_ptr<const CompiledSequence> after, std::string label)
        : _owner(owner), _before(std::move(before)), _after(std::move(after)),
          _label(std::move(label)) {}
    const std::string &label() const noexcept override { return _label; }
    bool isAlive() const noexcept override { return !_owner.expired(); }
    bool execute() override { return apply(_after); }
    bool undo() override { return apply(_before); }

  private:
    bool apply(const std::shared_ptr<const CompiledSequence> &value) {
        auto owner = _owner.lock();
        return owner && owner->replace(value->data());
    }
    std::weak_ptr<SequenceAuthoring> _owner;
    std::shared_ptr<const CompiledSequence> _before, _after;
    std::string _label;
};
bool diagnosticResult(bool result, const Diagnostic &d, std::string *error) {
    if (error)
        *error = result ? std::string{} : d.location + ": " + d.message;
    return result;
}
// Reject paths that escape the declared Assets tree, including symlink escapes.
std::string assetPath(const std::string &root, const std::string &relative) {
    if (root.empty() || relative.empty() || std::filesystem::path(relative).is_absolute())
        return {};
    std::error_code ec;
    const auto base = std::filesystem::weakly_canonical(std::filesystem::path(root) / "Assets", ec);
    if (ec)
        return {};
    const auto full = std::filesystem::weakly_canonical(base / relative, ec);
    if (ec)
        return {};
    const auto rel = full.lexically_relative(base);
    if (rel.empty() || *rel.begin() == "..")
        return {};
    return full.string();
}
} // namespace
EditorSequenceDocument::EditorSequenceDocument() : _model(std::make_shared<SequenceAuthoring>()) {}
EditorSequenceDocument::~EditorSequenceDocument() { closePreview(); }
bool EditorSequenceDocument::initialize(const EditorOpenRequest &request, std::string &error) {
    Sequence data;
    Diagnostic d;
    if (!loadSequence(request.resourcePath, data, &d))
        return diagnosticResult(false, d, &error);
    invalidatePreview();
    _model->replace(std::move(data));
    _history.discardHistory();
    _gesture.reset();
    _path = request.resourcePath;
    _title = std::filesystem::path(_path).filename().string();
    error.clear();
    return true;
}
void EditorSequenceDocument::configureProjectRoot(std::string root) {
    if (_projectRoot != root) {
        invalidatePreview();
        _projectRoot = std::move(root);
    }
}
bool EditorSequenceDocument::isDirty() const noexcept {
    return _history.isDirty() || (_gesture && _gesture->data() != _model->snapshot()->data());
}
bool EditorSequenceDocument::writeRecoveryCopy(const std::string &path, std::string *error) const {
    Diagnostic d;
    return diagnosticResult(saveSequence(path, _model->snapshot()->data(), &d), d, error);
}
bool EditorSequenceDocument::save(std::string *error) {
    if (_gesture) {
        if (error)
            *error = "Finish/cancel the current edit before saving.";
        return false;
    }
    if (!writeRecoveryCopy(_path, error))
        return false;
    return _history.markSaved();
}
bool EditorSequenceDocument::reload(std::string *error) {
    if (_gesture) {
        if (error)
            *error = "Finish/cancel the current edit before reload.";
        return false;
    }
    Sequence data;
    Diagnostic d;
    if (!loadSequence(_path, data, &d))
        return diagnosticResult(false, d, error);
    invalidatePreview();
    _model->replace(std::move(data));
    _history.discardHistory();
    if (error)
        error->clear();
    return true;
}
bool EditorSequenceDocument::handlesCommand(const std::string &id) const {
    return id == "file.save" || id == "edit.undo" || id == "edit.redo";
}
bool EditorSequenceDocument::canExecuteCommand(const std::string &id) const {
    if (_gesture)
        return false;
    if (id == "file.save")
        return !_path.empty();
    if (id == "edit.undo")
        return _history.canUndo();
    if (id == "edit.redo")
        return _history.canRedo();
    return false;
}
bool EditorSequenceDocument::executeCommand(const std::string &id) {
    if (!canExecuteCommand(id))
        return false;
    if (id == "file.save")
        return save(&_diagnostic);
    invalidatePreview();
    return id == "edit.undo" ? _history.undo() : _history.redo();
}
bool EditorSequenceDocument::edit(const std::string &label,
                                  const std::function<bool(Sequence &)> &mutation) {
    const auto before = _model->snapshot();
    Diagnostic d;
    if (!_model->edit(mutation, &d)) {
        _diagnostic = d.location + ": " + d.message;
        return false;
    }
    if (before == _model->snapshot())
        return true;
    invalidatePreview();
    _diagnostic.clear();
    if (_gesture)
        return true;
    if (_history.recordApplied(
            std::make_unique<SnapshotCommand>(_model, before, _model->snapshot(), label)))
        return true;
    _model->replace(before->data());
    return false;
}
bool EditorSequenceDocument::beginEdit(const std::string &label) {
    if (_gesture)
        return false;
    _gesture = _model->snapshot();
    _gestureLabel = label;
    pause();
    return true;
}
bool EditorSequenceDocument::endEdit(bool cancel) {
    if (!_gesture)
        return false;
    if (cancel) {
        invalidatePreview();
        _model->replace(_gesture->data());
        _gesture.reset();
        return true;
    }
    if (_gesture->data() == _model->snapshot()->data()) {
        _gesture.reset();
        return true;
    }
    if (!_history.recordApplied(
            std::make_unique<SnapshotCommand>(_model, _gesture, _model->snapshot(), _gestureLabel)))
        return false;
    _gesture.reset();
    return true;
}
void EditorSequenceDocument::closePreview() {
    if (_session)
        _session->stop();
    _session.reset();
    _scene.reset();
    _player.stop();
    _previewRevision = ~std::uint64_t{};
}
void EditorSequenceDocument::invalidatePreview() {
    closePreview();
    _player.bind(_model->snapshot());
}
bool EditorSequenceDocument::preparePreview() {
    if (_previewRevision == revision())
        return true;
    closePreview();
    _player.bind(_model->snapshot());
    const auto &data = _model->snapshot()->data();
    if (data.scenePath.empty()) {
        _previewRevision = revision();
        _diagnostic = "Value-only preview: set a saved Scene path to preview entities.";
        return true;
    }
    const auto path = assetPath(_projectRoot, data.scenePath);
    if (path.empty()) {
        _diagnostic = "Scene path is outside the configured project Assets root.";
        return false;
    }
    auto scene = std::make_unique<ayt::scene::Scene>(ayt::scene::SceneMode::Edit,
                                                     "Sequence isolated preview");
    ayt::serializer::SerializeError error;
    if (!scene->load(path, &error)) {
        _diagnostic = "Scene preview load failed: " + error.message;
        return false;
    }
    std::map<std::string, std::uint32_t> ids;
    auto entities = scene->world().getAllEntities();
    auto &resources = ayt::resource::ResourceManager::instance();
    for (const auto &binding : data.bindings) {
        ayt::entity::Entity *found = nullptr;
        for (auto *entity : entities)
            if (!binding.entityName.empty() && binding.entityName == entity->getName()) {
                if (found) {
                    _diagnostic = "Ambiguous entity name for binding " + binding.id;
                    return false;
                }
                found = entity;
            }
        if (!found) {
            _diagnostic =
                "Missing entity locator for binding " + binding.id + ": " + binding.entityName;
            return false;
        }
        ids[binding.id] = found->getId();
        const bool animated = std::any_of(data.animations.begin(), data.animations.end(),
                                          [&](const auto &t) { return t.binding == binding.id; });
        if (!animated)
            continue;
        auto *skeleton = found->getComponent<ayt::entity::SkeletonComponent>();
        if (!skeleton) {
            _diagnostic = "Animation target has no SkeletonComponent: " + binding.id;
            return false;
        }
        const auto skeletonPath = assetPath(_projectRoot, skeleton->skeletonPath);
        if (skeletonPath.empty()) {
            _diagnostic = "Skeleton reference escapes Assets: " + binding.id;
            return false;
        }
        skeleton->skeleton = resources.load<ayt::resource::Skeleton>(skeletonPath);
        if (!skeleton->skeleton ||
            resources.getLoadState(skeletonPath) != ayt::resource::ResourceLoadState::Ready) {
            _diagnostic = "Missing/invalid skeleton: " + skeletonPath;
            return false;
        }
        Diagnostic skeletonError;
        if (!validateAnimationSequenceSkeleton(*skeleton->skeleton, &skeletonError)) {
            _diagnostic = binding.id + ": " + skeletonError.message;
            return false;
        }
        skeleton->jointCount = skeleton->skeleton->getBoneCount();
        skeleton->player->setSkeleton(skeleton->skeleton);
        skeleton->player->evaluate();
        const auto *rest = skeleton->player->getBoneSkinMatrices();
        for (std::size_t i = 0; i < skeleton->jointCount; ++i)
            for (const auto &row : rest[i].row)
                for (float value : {row.x, row.y, row.z, row.w})
                    if (!std::isfinite(value) || std::abs(value) > 1e6f) {
                        _diagnostic = binding.id + ": Rest pose matrices exceed preview limits.";
                        return false;
                    }
        delete[] skeleton->skinMatrices;
        skeleton->skinMatrices = new ayt::math::Float4x4[skeleton->jointCount];
        std::copy_n(skeleton->player->getBoneSkinMatrices(), skeleton->jointCount,
                    skeleton->skinMatrices);
        skeleton->loaded = true;
    }
    auto session = std::make_shared<SceneSequenceSession>();
    Diagnostic d;
    bool bound = false;
    if (data.animations.empty())
        bound = session->bind(scene->world(), _model->snapshot(), ids, &d);
    else
        bound = session->bind(
            scene->world(), _model->snapshot(), ids,
            makeAnimationSequenceDriver([this, &resources](const std::string &relative)
                                            -> std::shared_ptr<const ayt::resource::Animation> {
                const auto full = assetPath(_projectRoot, relative);
                if (full.empty())
                    return {};
                auto clip = resources.load<ayt::resource::Animation>(full);
                return resources.getLoadState(full) == ayt::resource::ResourceLoadState::Ready
                           ? clip
                           : nullptr;
            }),
            &d);
    if (!bound) {
        _diagnostic = d.location + ": " + d.message;
        return false;
    }
    _scene = std::move(scene);
    _session = std::move(session);
    _previewRevision = revision();
    _diagnostic.clear();
    return true;
}
bool EditorSequenceDocument::seek(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > _model->snapshot()->data().duration)
        return false;
    if (!preparePreview())
        return false;
    Diagnostic d;
    SceneFrame sceneFrame;
    PlaybackFrame pure;
    const bool result =
        _session ? _session->seek(seconds, sceneFrame, &d) : _player.seek(seconds, pure, &d);
    if (!result)
        _diagnostic = d.location + ": " + d.message;
    return result;
}
bool EditorSequenceDocument::play() {
    if (_gesture || !preparePreview())
        return false;
    Diagnostic d;
    const bool result = _session ? _session->play(&d) : _player.play();
    if (!result)
        _diagnostic = d.message;
    return result;
}
void EditorSequenceDocument::pause() {
    if (_session)
        _session->pause();
    else
        _player.pause();
}
void EditorSequenceDocument::stop() {
    if (_session)
        _session->stop();
    _player.stop();
}
void EditorSequenceDocument::tick(double seconds) {
    if (!playing())
        return;
    Diagnostic d;
    SceneFrame sceneFrame;
    PlaybackFrame pure;
    if (!(_session ? _session->advance(seconds, sceneFrame, &d)
                   : _player.advance(seconds, pure, &d))) {
        _diagnostic = d.location + ": " + d.message;
        stop();
    }
    // Intentionally discard events. Editor preview never invokes project code.
}
double EditorSequenceDocument::position() const noexcept {
    return _session ? _session->position() : _player.position();
}
bool EditorSequenceDocument::playing() const noexcept {
    return (_session ? _session->state() : _player.state()) ==
           ayt::sequence::PlaybackState::Playing;
}
} // namespace ayt::editor
