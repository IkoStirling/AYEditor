#include "AYEditor/EditorAnimationDocument.h"

#include <AYIO/File.h>
#include <AYResource/assetsDefs/IAnimation.h>
#include <AYResource/assetsImpl/Animation.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ayt::editor {
namespace {

using Json = nlohmann::json;
using ayt::anim::editor::AnimationPreviewBindings;
using ayt::anim::editor::AnimationPreviewMode;

struct EditableClip {
    std::string name;
    float duration = 0.0f;
    float ticksPerSecond = 30.0f;
    ayt::math::FGuid guid;
    std::vector<ayt::resource::AnimTrack> tracks;
    std::vector<ayt::resource::AnimNotifyMarker> notifies;
};

std::size_t valueWidth(ayt::resource::AnimTrackType type) noexcept
{
    switch (type) {
    case ayt::resource::AnimTrackType::Vector3: return 3u;
    case ayt::resource::AnimTrackType::Quaternion: return 4u;
    case ayt::resource::AnimTrackType::Float: return 1u;
    }
    return 1u;
}

EditableClip readEditableClip(const ayt::resource::IAnimation& source)
{
    EditableClip result;
    result.name = source.getName() != nullptr ? source.getName() : "";
    result.duration = source.getDuration();
    result.ticksPerSecond = source.getTicksPerSecond();
    if (const auto* concrete =
            dynamic_cast<const ayt::resource::Animation*>(&source)) {
        result.guid = concrete->getGuid();
    }
    result.tracks.reserve(source.getTrackCount());
    for (std::uint32_t index = 0; index < source.getTrackCount(); ++index) {
        ayt::resource::AnimTrack track;
        track.nodeName = source.getTrackNodeName(index) != nullptr
            ? source.getTrackNodeName(index) : "";
        track.property = source.getTrackProperty(index) != nullptr
            ? source.getTrackProperty(index) : "";
        track.valueType = source.getTrackType(index);
        track.blendMode = source.getTrackBlendMode(index);
        const std::size_t keyCount = source.getTrackKeyframeCount(index);
        if (const float* times = source.getTrackTimes(index)) {
            track.times.assign(times, times + keyCount);
        }
        const std::size_t count = keyCount * valueWidth(track.valueType);
        if (const float* values = source.getTrackValues(index)) {
            track.values.assign(values, values + count);
        }
        result.tracks.push_back(std::move(track));
    }
    result.notifies.reserve(source.getNotifyCount());
    for (std::uint32_t index = 0; index < source.getNotifyCount(); ++index) {
        result.notifies.push_back({
            source.getNotifyName(index) != nullptr ? source.getNotifyName(index) : "",
            source.getNotifyTime(index), source.getNotifyPayload(index)});
    }
    return result;
}

std::shared_ptr<ayt::resource::Animation> buildAnimation(
    const EditableClip& source)
{
    auto result = std::make_shared<ayt::resource::Animation>();
    result->setName(source.name);
    result->setDuration(source.duration);
    result->setTicksPerSecond(source.ticksPerSecond);
    result->setGuid(source.guid);
    for (const auto& track : source.tracks) result->addTrack(track);
    for (const auto& notify : source.notifies) result->addNotify(notify);
    return result;
}

std::optional<std::size_t> parseIndex(std::string_view value,
                                      std::string_view prefix)
{
    if (!value.starts_with(prefix)) return std::nullopt;
    value.remove_prefix(prefix.size());
    std::size_t result = 0u;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

bool parseKeyId(std::string_view value, std::size_t& track,
                std::size_t& key) noexcept
{
    static constexpr std::string_view prefix = "key.";
    if (!value.starts_with(prefix)) return false;
    value.remove_prefix(prefix.size());
    const std::size_t separator = value.find('.');
    if (separator == std::string_view::npos) return false;
    const auto first = value.substr(0u, separator);
    const auto second = value.substr(separator + 1u);
    const auto parsedTrack = std::from_chars(
        first.data(), first.data() + first.size(), track);
    const auto parsedKey = std::from_chars(
        second.data(), second.data() + second.size(), key);
    return parsedTrack.ec == std::errc{}
        && parsedTrack.ptr == first.data() + first.size()
        && parsedKey.ec == std::errc{}
        && parsedKey.ptr == second.data() + second.size();
}

std::vector<float> sampledValue(const ayt::resource::AnimTrack& track,
                                float tick, double scalarValue)
{
    const std::size_t width = valueWidth(track.valueType);
    std::vector<float> result(width, 0.0f);
    if (track.valueType == ayt::resource::AnimTrackType::Quaternion) {
        result[3] = 1.0f;
    } else if (track.valueType == ayt::resource::AnimTrackType::Vector3
               && track.property == "scale") {
        std::fill(result.begin(), result.end(), 1.0f);
    }
    if (track.valueType == ayt::resource::AnimTrackType::Float) {
        result[0] = static_cast<float>(scalarValue);
        return result;
    }
    if (track.times.empty() || track.values.size() < track.times.size() * width) {
        return result;
    }
    const auto upper = std::lower_bound(track.times.begin(), track.times.end(), tick);
    if (upper == track.times.begin()) {
        std::copy_n(track.values.begin(), width, result.begin());
        return result;
    }
    if (upper == track.times.end()) {
        std::copy_n(track.values.begin() + (track.times.size() - 1u) * width,
                    width, result.begin());
        return result;
    }
    const std::size_t right = static_cast<std::size_t>(upper - track.times.begin());
    const std::size_t left = right - 1u;
    const float span = track.times[right] - track.times[left];
    const float alpha = span > 1.0e-6f
        ? std::clamp((tick - track.times[left]) / span, 0.0f, 1.0f) : 0.0f;
    for (std::size_t component = 0; component < width; ++component) {
        const float a = track.values[left * width + component];
        const float b = track.values[right * width + component];
        result[component] = a + (b - a) * alpha;
    }
    return result;
}

class AppliedAnimationRevisionCommand final : public IEditorCommand {
public:
    AppliedAnimationRevisionCommand(std::function<bool()> redoAction,
                                    std::function<bool()> undoAction,
                                    std::string label)
        : _redo(std::move(redoAction)), _undo(std::move(undoAction)),
          _label(std::move(label)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override { return _redo != nullptr && _redo(); }
    bool undo() override { return _undo != nullptr && _undo(); }

private:
    std::function<bool()> _redo;
    std::function<bool()> _undo;
    std::string _label;
};

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

EditorAnimationDocument::EditorAnimationDocument() : _history(256u) {}

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
    if (!resetEditHistory(&error)) return false;
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

bool EditorAnimationDocument::isDirty() const noexcept
{
    return _history.isDirty();
}

bool EditorAnimationDocument::save(std::string* error)
{
    const std::filesystem::path resourcePath(_path);
    const std::string filename = resourcePath.filename().string();
    if (filename.find(".baked.") != std::string::npos) {
        if (error != nullptr) {
            *error = "Baked animation outputs are read-only. Edit the source .ayanm clip.";
        }
        return false;
    }
    if (resourcePath.extension() != ".ayanm") {
        if (error != nullptr) {
            *error = "Legacy animation aliases are read-only; migrate to the canonical .ayanm format.";
        }
        return false;
    }
    if (!writeAnimationBytes(_path, error)) return false;
    return _history.markSaved();
}

bool EditorAnimationDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    return writeAnimationBytes(path, error);
}

bool EditorAnimationDocument::reload(std::string* error)
{
    if (!_preview.reloadAnimation(error)) return false;
    return resetEditHistory(error);
}

bool EditorAnimationDocument::handlesCommand(
    const std::string& commandId) const
{
    return commandId == "file.save" || commandId == "edit.undo"
        || commandId == "edit.redo";
}

bool EditorAnimationDocument::canExecuteCommand(
    const std::string& commandId) const
{
    if (commandId == "file.save") return isDirty();
    if (commandId == "edit.undo") return timelineCanUndo();
    if (commandId == "edit.redo") return timelineCanRedo();
    return false;
}

bool EditorAnimationDocument::executeCommand(const std::string& commandId)
{
    if (!canExecuteCommand(commandId)) return false;
    if (commandId == "file.save") return save();
    if (commandId == "edit.undo") return timelineUndo();
    if (commandId == "edit.redo") return timelineRedo();
    return false;
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
        const float* values = animation->getTrackValues(track);
        const std::size_t width = valueWidth(animation->getTrackType(track));
        for (std::uint32_t key = 0; key < animation->getTrackKeyframeCount(track);
             ++key) {
            result.push_back({"key." + std::to_string(track) + "."
                    + std::to_string(key),
                "animation." + std::to_string(track),
                times != nullptr ? static_cast<double>(times[key]) * tickScale : 0.0,
                values != nullptr ? values[key * width] : 0.0});
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

bool EditorAnimationDocument::timelineAddKeyframe(
    const std::string& trackId, double time, double value)
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    auto& track = clip.tracks[*index];
    const float ticksPerSecond = clip.ticksPerSecond > 0.0f
        ? clip.ticksPerSecond : 1.0f;
    const float tick = static_cast<float>(std::clamp(
        time, 0.0, static_cast<double>(clip.duration))) * ticksPerSecond;
    const auto position = std::lower_bound(track.times.begin(), track.times.end(), tick);
    if (position != track.times.end() && std::fabs(*position - tick) < 1.0e-5f) {
        return false;
    }
    const std::size_t key = static_cast<std::size_t>(position - track.times.begin());
    const std::size_t width = valueWidth(track.valueType);
    const std::vector<float> sampled = sampledValue(track, tick, value);
    track.times.insert(position, tick);
    track.values.insert(track.values.begin() + key * width,
                        sampled.begin(), sampled.end());
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::timelineMoveKeyframe(
    const std::string& keyframeId, double time)
{
    std::size_t trackIndex = 0u;
    std::size_t keyIndex = 0u;
    const auto* animation = _preview.animation();
    if (!parseKeyId(keyframeId, trackIndex, keyIndex) || animation == nullptr
        || trackIndex >= animation->getTrackCount()
        || keyIndex >= animation->getTrackKeyframeCount(
            static_cast<std::uint32_t>(trackIndex))) return false;
    EditableClip clip = readEditableClip(*animation);
    auto& track = clip.tracks[trackIndex];
    const float ticksPerSecond = clip.ticksPerSecond > 0.0f
        ? clip.ticksPerSecond : 1.0f;
    const float tick = static_cast<float>(std::clamp(
        time, 0.0, static_cast<double>(clip.duration))) * ticksPerSecond;
    for (std::size_t index = 0u; index < track.times.size(); ++index) {
        if (index != keyIndex && std::fabs(track.times[index] - tick) < 1.0e-5f) {
            return false;
        }
    }
    if (std::fabs(track.times[keyIndex] - tick) < 1.0e-5f) return false;
    const std::size_t width = valueWidth(track.valueType);
    const auto valueFirst = track.values.begin() + keyIndex * width;
    const std::vector<float> stored(valueFirst, valueFirst + width);
    track.values.erase(valueFirst, valueFirst + width);
    track.times.erase(track.times.begin() + keyIndex);
    const auto position = std::lower_bound(track.times.begin(), track.times.end(), tick);
    const std::size_t destination = static_cast<std::size_t>(
        position - track.times.begin());
    track.times.insert(position, tick);
    track.values.insert(track.values.begin() + destination * width,
                        stored.begin(), stored.end());
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::timelineRemoveKeyframe(
    const std::string& keyframeId)
{
    std::size_t trackIndex = 0u;
    std::size_t keyIndex = 0u;
    const auto* animation = _preview.animation();
    if (!parseKeyId(keyframeId, trackIndex, keyIndex) || animation == nullptr
        || trackIndex >= animation->getTrackCount()
        || keyIndex >= animation->getTrackKeyframeCount(
            static_cast<std::uint32_t>(trackIndex))) return false;
    EditableClip clip = readEditableClip(*animation);
    auto& track = clip.tracks[trackIndex];
    const std::size_t width = valueWidth(track.valueType);
    track.times.erase(track.times.begin() + keyIndex);
    track.values.erase(track.values.begin() + keyIndex * width,
                       track.values.begin() + (keyIndex + 1u) * width);
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::timelineCanUndo() const noexcept
{
    return _history.canUndo();
}

bool EditorAnimationDocument::timelineCanRedo() const noexcept
{
    return _history.canRedo();
}

bool EditorAnimationDocument::timelineUndo()
{
    return _history.undo();
}

bool EditorAnimationDocument::timelineRedo()
{
    return _history.redo();
}

bool EditorAnimationDocument::addAnimationTrack(
    const std::string& nodeName, const std::string& property,
    ayt::resource::AnimTrackType type)
{
    const auto* animation = _preview.animation();
    if (animation == nullptr || nodeName.empty() || property.empty()) return false;
    EditableClip clip = readEditableClip(*animation);
    if (std::any_of(clip.tracks.begin(), clip.tracks.end(),
            [&](const auto& track) {
                return track.nodeName == nodeName && track.property == property;
            })) return false;
    ayt::resource::AnimTrack track;
    track.nodeName = nodeName;
    track.property = property;
    track.valueType = type;
    const float ticksPerSecond = clip.ticksPerSecond > 0.0f
        ? clip.ticksPerSecond : 1.0f;
    track.times.push_back(static_cast<float>(std::clamp(
        timelinePositionSeconds(), 0.0,
        static_cast<double>(clip.duration))) * ticksPerSecond);
    track.values = sampledValue(track, track.times.front(), 0.0);
    clip.tracks.push_back(std::move(track));
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::removeAnimationTrack(const std::string& trackId)
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    clip.tracks.erase(clip.tracks.begin() + *index);
    return commitEditedAnimation(buildAnimation(clip));
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

bool EditorAnimationDocument::resetEditHistory(std::string* error)
{
    const auto* animation = dynamic_cast<const ayt::resource::Animation*>(
        _preview.animation());
    std::vector<std::uint8_t> bytes;
    if (animation == nullptr || !animation->saveToBinary(bytes)) {
        if (error != nullptr) *error = "Unable to snapshot animation for editing.";
        return false;
    }
    _currentBytes = std::move(bytes);
    _history.discardHistory(EditorHistoryDiscardState::MarkClean);
    if (error != nullptr) error->clear();
    return true;
}

bool EditorAnimationDocument::commitEditedAnimation(
    std::shared_ptr<ayt::resource::Animation> animation, std::string* error)
{
    std::vector<std::uint8_t> bytes;
    if (animation == nullptr || !animation->saveToBinary(bytes)) {
        if (error != nullptr) *error = "Unable to serialize edited animation.";
        return false;
    }
    ayt::resource::Animation verification;
    if (bytes.empty() || !verification.loadFromBinary(bytes.data(), bytes.size())) {
        if (error != nullptr) *error = "Edited animation failed validation.";
        return false;
    }
    if (bytes == _currentBytes) {
        if (error != nullptr) error->clear();
        return false;
    }
    const std::vector<std::uint8_t> before = _currentBytes;
    if (!_preview.replaceAnimation(std::move(animation), true, error)) return false;
    _currentBytes = bytes;
    const bool recorded = _history.recordApplied(
        std::make_unique<AppliedAnimationRevisionCommand>(
            [this, bytes]() { return applySerializedRevision(bytes); },
            [this, before]() { return applySerializedRevision(before); },
            "Edit animation tracks and keys"));
    if (recorded) return true;
    (void)applySerializedRevision(before, nullptr);
    if (error != nullptr) *error = "Unable to record animation edit history.";
    return false;
}

bool EditorAnimationDocument::applySerializedRevision(
    const std::vector<std::uint8_t>& bytes, std::string* error)
{
    auto animation = std::make_shared<ayt::resource::Animation>();
    if (bytes.empty() || !animation->loadFromBinary(bytes.data(), bytes.size())) {
        if (error != nullptr) *error = "Animation undo revision is invalid.";
        return false;
    }
    if (!_preview.replaceAnimation(std::move(animation), true, error)) return false;
    _currentBytes = bytes;
    return true;
}

bool EditorAnimationDocument::writeAnimationBytes(
    const std::string& path, std::string* error) const
{
    if (_currentBytes.empty() || path.empty()) {
        if (error != nullptr) *error = "No editable animation is available.";
        return false;
    }
    const auto& bytes = _currentBytes;
    ayt::resource::Animation verification;
    if (bytes.empty() || !verification.loadFromBinary(bytes.data(), bytes.size())
        || !ayt::io::File::atomicWrite(path, bytes.data(), bytes.size())) {
        if (error != nullptr) *error = "Unable to validate or write animation resource.";
        return false;
    }
    if (error != nullptr) error->clear();
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
