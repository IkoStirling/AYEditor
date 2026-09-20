#include "AYEditor/EditorAnimationDocument.h"

#include <AYIO/File.h>
#include <AYResource/assetsDefs/IAnimation.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ayt::editor {
namespace {

using Json = nlohmann::json;
using ayt::anim::editor::AnimationPreviewBindings;
using ayt::anim::editor::AnimationPreviewMode;

const char* modeValue(AnimationPreviewMode mode) noexcept
{
    switch (mode) {
    case AnimationPreviewMode::ModelAndSkeleton: return "model+skeleton";
    case AnimationPreviewMode::ModelOnly: return "model";
    case AnimationPreviewMode::SkeletonOnly: return "skeleton";
    }
    return "model+skeleton";
}

AnimationPreviewMode parseMode(const std::string& value) noexcept
{
    if (value == "model") return AnimationPreviewMode::ModelOnly;
    if (value == "skeleton") return AnimationPreviewMode::SkeletonOnly;
    return AnimationPreviewMode::ModelAndSkeleton;
}

bool isProjectRelative(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute()) return false;
    const auto first = path.begin();
    return first == path.end() || *first != std::filesystem::path("..");
}

std::string encodeProjectPath(const std::string& value,
                              const std::string& projectRoot)
{
    if (value.empty() || projectRoot.empty()) return value;
    std::error_code error;
    const auto relative = std::filesystem::relative(
        std::filesystem::path(value), std::filesystem::path(projectRoot), error);
    if (!error && isProjectRelative(relative)) {
        return relative.lexically_normal().generic_string();
    }
    return std::filesystem::path(value).lexically_normal().generic_string();
}

std::string resolveProjectPath(const std::string& value,
                               const std::string& projectRoot)
{
    if (value.empty()) return {};
    const std::filesystem::path path(value);
    if (path.is_absolute() || projectRoot.empty()) {
        return path.lexically_normal().generic_string();
    }
    return (std::filesystem::path(projectRoot) / path)
        .lexically_normal().generic_string();
}

double secondsPerTick(const ayt::resource::IAnimation& animation) noexcept
{
    const float rate = animation.getTicksPerSecond();
    return rate > 0.0f ? 1.0 / static_cast<double>(rate) : 1.0;
}

} // namespace

bool EditorAnimationDocument::initialize(const EditorOpenRequest& request,
                                         std::string& error)
{
    if (!_preview.openAnimation(request.resourcePath, &error)) return false;
    _path = _preview.animationPath();
    _title = std::filesystem::path(_path).filename().string();
    const AnimationPreviewBindings inferred = _preview.inferCompanionAssets();
    std::string ignored;
    (void)_preview.applyBindings(inferred, &ignored);
    _selectedBone = _preview.bones().empty() ? -1 : 0;
    error.clear();
    return true;
}

void EditorAnimationDocument::configureProjectRoot(
    const std::string& projectRoot)
{
    std::error_code error;
    _projectRoot = projectRoot.empty() ? std::string{}
        : std::filesystem::absolute(projectRoot, error)
            .lexically_normal().generic_string();
    if (error || _projectRoot.empty()) {
        _projectRoot.clear();
        _metadataPath.clear();
        return;
    }
    _metadataPath = (std::filesystem::path(_projectRoot) / ".ayeditor"
        / "animation-preview-bindings.json").generic_string();
    loadPreviewMetadata();
}

std::uint64_t EditorAnimationDocument::revision() const noexcept
{
    return _preview.revision();
}

bool EditorAnimationDocument::save(std::string* error)
{
    if (error != nullptr) {
        *error = "Cooked animation assets are read-only. Preview bindings are "
            "saved automatically as editor metadata.";
    }
    return false;
}

bool EditorAnimationDocument::reload(std::string* error)
{
    return _preview.reloadAnimation(error);
}

double EditorAnimationDocument::timelineDurationSeconds() const noexcept
{
    return _preview.duration();
}

double EditorAnimationDocument::timelinePositionSeconds() const noexcept
{
    return _preview.time();
}

bool EditorAnimationDocument::setTimelinePositionSeconds(double seconds)
{
    return _preview.setTime(static_cast<float>(seconds));
}

std::vector<EditorTimelineTrack> EditorAnimationDocument::timelineTracks() const
{
    std::vector<EditorTimelineTrack> result;
    const auto* animation = _preview.animation();
    if (animation == nullptr) return result;
    result.reserve(animation->getTrackCount() + animation->getNotifyCount());
    for (std::uint32_t index = 0; index < animation->getTrackCount(); ++index) {
        std::string name = animation->getTrackNodeName(index) != nullptr
            ? animation->getTrackNodeName(index) : "<unnamed>";
        const char* property = animation->getTrackProperty(index);
        if (property != nullptr && *property != '\0') name += " / " + std::string(property);
        result.push_back({"animation." + std::to_string(index), std::move(name),
            EditorTimelineTrackKind::Animation, 0.0, _preview.duration(), true});
    }
    for (std::uint32_t index = 0; index < animation->getNotifyCount(); ++index) {
        const char* name = animation->getNotifyName(index);
        const double time = animation->getNotifyTime(index);
        result.push_back({"event." + std::to_string(index),
            name != nullptr ? name : "<event>", EditorTimelineTrackKind::Event,
            time, time, true});
    }
    return result;
}

std::vector<EditorTimelineKeyframe>
EditorAnimationDocument::timelineKeyframes() const
{
    std::vector<EditorTimelineKeyframe> result;
    const auto* animation = _preview.animation();
    if (animation == nullptr) return result;
    const double tickScale = secondsPerTick(*animation);
    for (std::uint32_t track = 0; track < animation->getTrackCount(); ++track) {
        const float* times = animation->getTrackTimes(track);
        const float* values = animation->getTrackFloatValues(track);
        for (std::uint32_t key = 0; key < animation->getTrackKeyframeCount(track);
             ++key) {
            result.push_back({"key." + std::to_string(track) + "."
                    + std::to_string(key),
                "animation." + std::to_string(track),
                times != nullptr ? static_cast<double>(times[key]) * tickScale : 0.0,
                values != nullptr ? values[key] : 0.0});
        }
    }
    for (std::uint32_t index = 0; index < animation->getNotifyCount(); ++index) {
        result.push_back({"notify." + std::to_string(index),
            "event." + std::to_string(index), animation->getNotifyTime(index),
            animation->getNotifyPayload(index)});
    }
    return result;
}

bool EditorAnimationDocument::timelinePlaying() const noexcept
{
    return _preview.isPlaying();
}

void EditorAnimationDocument::timelinePlay() { _preview.play(); }
void EditorAnimationDocument::timelinePause() { _preview.pause(); }
void EditorAnimationDocument::timelineStop() { _preview.stop(); }
void EditorAnimationDocument::timelineTick(double seconds)
{
    _preview.tick(static_cast<float>(seconds));
}

bool EditorAnimationDocument::bindSkeleton(const std::string& path,
                                           std::string* error)
{
    if (!_preview.bindSkeleton(path, error)) return false;
    _selectedBone = _preview.bones().empty() ? -1 : 0;
    (void)persistPreviewMetadata(nullptr);
    return true;
}

bool EditorAnimationDocument::bindMesh(const std::string& path,
                                       std::string* error)
{
    if (!_preview.bindMesh(path, error)) return false;
    (void)persistPreviewMetadata(nullptr);
    return true;
}

void EditorAnimationDocument::setMaterialPath(const std::string& path)
{
    _preview.setMaterialPath(path);
    (void)persistPreviewMetadata(nullptr);
}

void EditorAnimationDocument::setPreviewMode(AnimationPreviewMode mode)
{
    _preview.setPreviewMode(mode);
    (void)persistPreviewMetadata(nullptr);
}

void EditorAnimationDocument::setLooping(bool looping)
{
    _preview.setLooping(looping);
    (void)persistPreviewMetadata(nullptr);
}

void EditorAnimationDocument::setPlayRate(float rate)
{
    _preview.setPlayRate(rate);
    (void)persistPreviewMetadata(nullptr);
}

bool EditorAnimationDocument::selectBone(int index) noexcept
{
    if (index < -1 || index >= static_cast<int>(_preview.bones().size())
        || _selectedBone == index) return false;
    _selectedBone = index;
    return true;
}

bool EditorAnimationDocument::persistPreviewMetadata(std::string* error) const
{
    if (_metadataPath.empty()) {
        if (error != nullptr) error->clear();
        return true;
    }
    try {
        Json root = Json::object();
        const std::string existing = ayt::io::File::readAllText(_metadataPath);
        if (!existing.empty()) root = Json::parse(existing);
        root["version"] = 2;
        const std::string animationKey = encodeProjectPath(_path, _projectRoot);
        auto& entry = root["bindings"][animationKey];
        entry = {{"skeleton", encodeProjectPath(
                                  _preview.skeletonPath(), _projectRoot)},
                 {"mesh", encodeProjectPath(_preview.meshPath(), _projectRoot)},
                 {"material", encodeProjectPath(
                                  _preview.materialPath(), _projectRoot)},
                 {"mode", modeValue(_preview.requestedPreviewMode())},
                 {"loop", _preview.looping()},
                 {"speed", _preview.playRate()}};
        const std::string encoded = root.dump(2) + "\n";
        const std::filesystem::path path(_metadataPath);
        std::error_code directoryError;
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError || !ayt::io::File::atomicWrite(
                _metadataPath, encoded.data(), encoded.size())) {
            if (error != nullptr) *error = "Unable to write animation preview metadata.";
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

void EditorAnimationDocument::loadPreviewMetadata()
{
    const std::string text = ayt::io::File::readAllText(_metadataPath);
    if (text.empty()) return;
    try {
        const Json root = Json::parse(text);
        const auto bindings = root.find("bindings");
        if (bindings == root.end() || !bindings->is_object()) return;
        const std::string animationKey = encodeProjectPath(_path, _projectRoot);
        auto value = bindings->find(animationKey);
        // Version 1 used absolute animation keys. Keep those files readable
        // while all newly persisted entries use project-relative paths.
        if (value == bindings->end()) value = bindings->find(_path);
        if (value == bindings->end() || !value->is_object()) return;
        AnimationPreviewBindings saved;
        saved.skeletonPath = resolveProjectPath(
            value->value("skeleton", std::string{}), _projectRoot);
        saved.meshPath = resolveProjectPath(
            value->value("mesh", std::string{}), _projectRoot);
        saved.materialPath = resolveProjectPath(
            value->value("material", std::string{}), _projectRoot);
        std::string ignored;
        if (!saved.skeletonPath.empty()) (void)_preview.bindSkeleton(saved.skeletonPath, &ignored);
        if (!saved.meshPath.empty()) (void)_preview.bindMesh(saved.meshPath, &ignored);
        if (!saved.materialPath.empty()) _preview.setMaterialPath(saved.materialPath);
        _preview.setPreviewMode(parseMode(value->value("mode", "model+skeleton")));
        _preview.setLooping(value->value("loop", true));
        _preview.setPlayRate(value->value("speed", 1.0f));
        _selectedBone = _preview.bones().empty() ? -1 : 0;
    } catch (...) {
        // Corrupt editor metadata must never prevent the cooked asset opening.
    }
}

} // namespace ayt::editor
