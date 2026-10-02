#include "AYEditor/EditorAnimationDocument.h"
#include <AYAnimationEditor/AnimationAuthoring.h>
#include <AYAnimationEditor/AnimationTimeTransform.h>
#include <AYAnimation/KeySampler.h>

#include <AYIO/File.h>
#include <AYResource/assetsDefs/IAnimation.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYResource/assetsImpl/Animation.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>

namespace ayt::editor {
namespace {

using Json = nlohmann::json;
using ayt::anim::editor::AnimationPreviewBindings;
using ayt::anim::editor::AnimationPreviewMode;

using EditableClip = ayt::anim::editor::AuthoredAnimation;

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
    return ayt::anim::editor::copyAnimationForAuthoring(source);
}

std::shared_ptr<ayt::resource::Animation> buildAnimation(const EditableClip& source)
{
    return ayt::anim::editor::buildAuthoredAnimation(source);
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

bool parseAuthoringKeys(const std::vector<std::string>& ids,
    std::vector<ayt::anim::editor::AnimationKeyReference>& keys)
{
    for (const auto& id : ids) {
        ayt::anim::editor::AnimationKeyReference key;
        if (const auto index = parseIndex(id, "notify.")) {
            key.notify = true;
            key.key = *index;
        } else if (!parseKeyId(id, key.track, key.key)) return false;
        keys.push_back(key);
    }
    return !keys.empty();
}

std::vector<std::string> authoringKeyIds(
    const std::vector<ayt::anim::editor::AnimationKeyReference>& keys)
{
    std::vector<std::string> ids;
    for (const auto& key : keys) ids.push_back(key.notify
        ? "notify." + std::to_string(key.key)
        : "key." + std::to_string(key.track) + "." + std::to_string(key.key));
    return ids;
}

std::vector<float> sampledValue(const ayt::resource::AnimTrack& track,
                                float tick, double scalarValue, float ticksPerSecond)
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
    const float rate = ticksPerSecond > 0 ? ticksPerSecond : 1;
    auto times = track.times;
    for (auto& time : times) time /= rate;
    if (track.valueType == ayt::resource::AnimTrackType::Quaternion) {
        std::vector<ayt::math::FQuaternion> values, in, out;
        for (std::size_t key = 0; key < times.size(); ++key) {
            const auto v = track.values.data() + key * width;
            values.emplace_back(v[0], v[1], v[2], v[3]);
            for (auto pair : {std::pair{&in, &track.inTangents}, std::pair{&out, &track.outTangents}})
                if (pair.second->size() == track.values.size()) {
                    const auto tangent = pair.second->data() + key * width;
                    pair.first->emplace_back(tangent[0], tangent[1], tangent[2], tangent[3]);
                }
        }
        ayt::math::FQuaternion value;
        ayt::anim::sampleTrackQuaternion(values.data(), times.size(), times, tick / rate,
            value, track.interpolation, in.empty() ? nullptr : in.data(), out.empty() ? nullptr : out.data());
        result = {value.x, value.y, value.z, value.w};
    } else {
        for (std::size_t component = 0; component < width; ++component) {
            std::vector<float> values, in, out;
            for (std::size_t key = 0; key < times.size(); ++key) {
                values.push_back(track.values[key * width + component]);
                if (track.inTangents.size() == track.values.size()) in.push_back(track.inTangents[key * width + component]);
                if (track.outTangents.size() == track.values.size()) out.push_back(track.outTangents[key * width + component]);
            }
            ayt::anim::sampleTrackFloat(values.data(), times.size(), times, tick / rate,
                result[component], track.interpolation, in.empty() ? nullptr : in.data(), out.empty() ? nullptr : out.data());
        }
    }
    return result;
}

void generateAutoTangents(ayt::resource::AnimTrack& track,
                          float ticksPerSecond)
{
    const std::size_t width = valueWidth(track.valueType);
    const std::size_t count = track.times.size();
    track.inTangents.assign(track.values.size(), 0.0f);
    track.outTangents.assign(track.values.size(), 0.0f);
    if (count < 2u || track.values.size() != count * width) return;
    const float safeTps = ticksPerSecond > 0.0f ? ticksPerSecond : 1.0f;
    for (std::size_t key = 0u; key < count; ++key) {
        const std::size_t left = key == 0u ? 0u : key - 1u;
        const std::size_t right = key + 1u < count ? key + 1u : count - 1u;
        const float seconds = (track.times[right] - track.times[left]) / safeTps;
        if (seconds <= 1.0e-6f) continue;
        for (std::size_t component = 0u; component < width; ++component) {
            const float slope = (track.values[right * width + component]
                - track.values[left * width + component]) / seconds;
            track.inTangents[key * width + component] = slope;
            track.outTangents[key * width + component] = slope;
        }
    }
}

class AppliedAnimationRevisionCommand final : public IEditorCommand {
public:
    AppliedAnimationRevisionCommand(std::function<bool()> redoAction,
                                    std::function<bool()> undoAction,
                                    std::string label, std::string mergeKey)
        : _redo(std::move(redoAction)), _undo(std::move(undoAction)),
          _label(std::move(label)), _mergeKey(std::move(mergeKey)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override { return _redo != nullptr && _redo(); }
    bool undo() override { return _undo != nullptr && _undo(); }
    std::string mergeKey() const override { return _mergeKey; }
    bool mergeFrom(const IEditorCommand& newer) override {
        const auto* revision = dynamic_cast<const AppliedAnimationRevisionCommand*>(&newer);
        if (!revision || _mergeKey.empty() || _mergeKey != revision->_mergeKey) return false;
        _redo = revision->_redo;
        return true;
    }

private:
    std::function<bool()> _redo;
    std::function<bool()> _undo;
    std::string _label;
    std::string _mergeKey;
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
            *error = "Baked animation outputs are read-only. Edit the source .anm clip.";
        }
        return false;
    }
    if (resourcePath.extension() != ".anm") {
        if (error != nullptr) {
            *error = "Legacy animation aliases are read-only; migrate to the canonical .anm format.";
        }
        return false;
    }
    if (!_controlRigLoadError.empty()) {
        if (error) *error="Saved controls could not be loaded. Explicitly clear or replace them before saving: "+_controlRigLoadError;
        return false;
    }
    const auto previousBytes=ayt::io::File::readAllBytes(_path);
    if (_preview.controlRig() && previousBytes.empty()) {
        if (error) *error="Unable to snapshot the existing clip for control metadata save rollback.";
        return false;
    }
    if (!writeAnimationBytes(_path, error)) return false;
    if (!persistPreviewMetadata(error, true)) {
        if (!previousBytes.empty() && !ayt::io::File::atomicWrite(_path,previousBytes.data(),previousBytes.size()) && error)
            *error += " Clip rollback also failed; the document remains dirty.";
        return false;
    }
    return _history.markSaved();
}

bool EditorAnimationDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    if (_preview.controlRig() || !_controlRigLoadError.empty()) {
        if (error) *error="Control rig recovery is not supported by the single-file recovery format. Save the clip and project metadata explicitly.";
        return false;
    }
    return writeAnimationBytes(path, error);
}

bool EditorAnimationDocument::reload(std::string* error)
{
    if (!_preview.reloadAnimation(error)) return false;
    if (!resetEditHistory(error)) return false;
    (void)_preview.setControlRig(nullptr);
    loadPreviewMetadata();
    return true;
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
    const std::vector<float> sampled = sampledValue(track, tick, value, clip.ticksPerSecond);
    track.times.insert(position, tick);
    track.values.insert(track.values.begin() + key * width,
                        sampled.begin(), sampled.end());
    if (!track.inTangents.empty()) {
        track.inTangents.insert(track.inTangents.begin() + key * width,
                                width, 0.0f);
    }
    if (!track.outTangents.empty()) {
        track.outTangents.insert(track.outTangents.begin() + key * width,
                                 width, 0.0f);
    }
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::timelineMoveKeyframe(
    const std::string& keyframeId, double time)
{
    if (!std::isfinite(time)) return false;
    std::size_t track = 0, key = 0;
    const auto* animation = _preview.animation();
    if (!animation || !parseKeyId(keyframeId, track, key)
        || track >= animation->getTrackCount()
        || key >= animation->getTrackKeyframeCount(static_cast<std::uint32_t>(track))) return false;
    const auto* times = animation->getTrackTimes(static_cast<std::uint32_t>(track));
    if (!times) return false;
    const double rate = animation->getTicksPerSecond() > 0 ? animation->getTicksPerSecond() : 1;
    std::vector<std::string> ids{keyframeId};
    return transformAnimationKeyframes(ids,
        std::clamp(time, 0.0, static_cast<double>(animation->getDuration())) - times[key] / rate, 0, 0);
}

bool EditorAnimationDocument::timelineRemoveKeyframe(
    const std::string& keyframeId)
{
    return removeAnimationKeyframes({keyframeId});
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
    track.values = sampledValue(track, track.times.front(), 0.0, clip.ticksPerSecond);
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

bool EditorAnimationDocument::animationKeyframeValues(
    const std::string& keyframeId, std::vector<float>& values,
    ayt::resource::AnimTrackType* type) const
{
    std::size_t trackIndex = 0u;
    std::size_t keyIndex = 0u;
    const auto* animation = _preview.animation();
    if (!parseKeyId(keyframeId, trackIndex, keyIndex) || animation == nullptr
        || trackIndex >= animation->getTrackCount()
        || keyIndex >= animation->getTrackKeyframeCount(
            static_cast<std::uint32_t>(trackIndex))) return false;
    const auto trackType = animation->getTrackType(
        static_cast<std::uint32_t>(trackIndex));
    const std::size_t width = valueWidth(trackType);
    const float* raw = animation->getTrackValues(
        static_cast<std::uint32_t>(trackIndex));
    if (raw == nullptr) return false;
    values.assign(raw + keyIndex * width, raw + (keyIndex + 1u) * width);
    if (type != nullptr) *type = trackType;
    return true;
}

bool EditorAnimationDocument::setAnimationKeyframeValues(
    const std::string& keyframeId, const std::vector<float>& values)
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
    if (values.size() != width
        || !std::all_of(values.begin(), values.end(), [](float value) {
            return std::isfinite(value);
        })) return false;

    std::vector<float> normalized = values;
    if (track.valueType == ayt::resource::AnimTrackType::Quaternion) {
        double lengthSquared = 0;
        for (const float value : normalized) lengthSquared += static_cast<double>(value) * value;
        if (lengthSquared <= 1.0e-12) return false;
        for (float& value : normalized) value = static_cast<float>(value / std::sqrt(lengthSquared));
    }
    const auto first = track.values.begin() + keyIndex * width;
    if (std::equal(normalized.begin(), normalized.end(), first)) return false;
    std::copy(normalized.begin(), normalized.end(), first);
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::animationTrackInterpolation(
    const std::string& trackId,
    ayt::resource::AnimInterpolation& interpolation) const
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    interpolation = animation->getTrackInterpolation(
        static_cast<std::uint32_t>(*index));
    return true;
}

bool EditorAnimationDocument::setAnimationTrackInterpolation(
    const std::string& trackId,
    ayt::resource::AnimInterpolation interpolation)
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    auto& track = clip.tracks[*index];
    if (track.interpolation == interpolation) return false;
    track.interpolation = interpolation;
    if (interpolation == ayt::resource::AnimInterpolation::CubicHermite
        && (track.inTangents.size() != track.values.size()
            || track.outTangents.size() != track.values.size())) {
        generateAutoTangents(track, clip.ticksPerSecond);
    }
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::animationKeyframeTangents(
    const std::string& keyframeId, std::vector<float>& inTangents,
    std::vector<float>& outTangents) const
{
    std::size_t trackIndex = 0u;
    std::size_t keyIndex = 0u;
    const auto* animation = _preview.animation();
    if (!parseKeyId(keyframeId, trackIndex, keyIndex) || animation == nullptr
        || trackIndex >= animation->getTrackCount()
        || keyIndex >= animation->getTrackKeyframeCount(
            static_cast<std::uint32_t>(trackIndex))) return false;
    const std::size_t width = valueWidth(animation->getTrackType(
        static_cast<std::uint32_t>(trackIndex)));
    const float* incoming = animation->getTrackInTangents(
        static_cast<std::uint32_t>(trackIndex));
    const float* outgoing = animation->getTrackOutTangents(
        static_cast<std::uint32_t>(trackIndex));
    inTangents.assign(width, 0.0f);
    outTangents.assign(width, 0.0f);
    if (incoming != nullptr) {
        std::copy_n(incoming + keyIndex * width, width, inTangents.begin());
    }
    if (outgoing != nullptr) {
        std::copy_n(outgoing + keyIndex * width, width, outTangents.begin());
    }
    return true;
}

bool EditorAnimationDocument::setAnimationKeyframeTangents(
    const std::string& keyframeId, const std::vector<float>& inTangents,
    const std::vector<float>& outTangents)
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
    const auto finite = [](const std::vector<float>& values) {
        return std::all_of(values.begin(), values.end(), [](float value) {
            return std::isfinite(value);
        });
    };
    if (inTangents.size() != width || outTangents.size() != width
        || !finite(inTangents) || !finite(outTangents)) return false;
    if (track.inTangents.size() != track.values.size()) {
        track.inTangents.assign(track.values.size(), 0.0f);
    }
    if (track.outTangents.size() != track.values.size()) {
        track.outTangents.assign(track.values.size(), 0.0f);
    }
    const auto incoming = track.inTangents.begin() + keyIndex * width;
    const auto outgoing = track.outTangents.begin() + keyIndex * width;
    if (std::equal(inTangents.begin(), inTangents.end(), incoming)
        && std::equal(outTangents.begin(), outTangents.end(), outgoing)) {
        return false;
    }
    std::copy(inTangents.begin(), inTangents.end(), incoming);
    std::copy(outTangents.begin(), outTangents.end(), outgoing);
    track.interpolation = ayt::resource::AnimInterpolation::CubicHermite;
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::autoAnimationTrackTangents(
    const std::string& trackId)
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    auto& track = clip.tracks[*index];
    generateAutoTangents(track, clip.ticksPerSecond);
    track.interpolation = ayt::resource::AnimInterpolation::CubicHermite;
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::animationCurveTrack(
    const std::string& trackId, EditorAnimationCurveTrack& result) const
{
    const auto index = parseIndex(trackId, "animation.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getTrackCount()) {
        return false;
    }
    const auto trackIndex = static_cast<std::uint32_t>(*index);
    const std::size_t width = valueWidth(animation->getTrackType(trackIndex));
    const std::size_t keyCount = animation->getTrackKeyframeCount(trackIndex);
    const float* times = animation->getTrackTimes(trackIndex);
    const float* values = animation->getTrackValues(trackIndex);
    const float* incoming = animation->getTrackInTangents(trackIndex);
    const float* outgoing = animation->getTrackOutTangents(trackIndex);
    if ((keyCount != 0u && (times == nullptr || values == nullptr))
        || width == 0u) return false;

    result = {};
    result.id = trackId;
    result.valueType = animation->getTrackType(trackIndex);
    result.interpolation = animation->getTrackInterpolation(trackIndex);
    result.ticksPerSecond = animation->getTicksPerSecond() > 0.0f
        ? animation->getTicksPerSecond() : 1.0;
    result.keys.reserve(keyCount);
    for (std::size_t key = 0u; key < keyCount; ++key) {
        EditorAnimationCurveKey item;
        item.id = "key." + std::to_string(*index) + "."
            + std::to_string(key);
        item.timeSeconds = static_cast<double>(times[key])
            / result.ticksPerSecond;
        item.values.assign(values + key * width, values + (key + 1u) * width);
        item.inTangents.assign(width, 0.0f);
        item.outTangents.assign(width, 0.0f);
        if (incoming != nullptr) {
            std::copy_n(incoming + key * width, width,
                        item.inTangents.begin());
        }
        if (outgoing != nullptr) {
            std::copy_n(outgoing + key * width, width,
                        item.outTangents.begin());
        }
        result.keys.push_back(std::move(item));
    }
    return true;
}

bool EditorAnimationDocument::updateAnimationKeyframe(
    std::string& keyframeId, double timeSeconds,
    const std::vector<float>& values)
{
    if (!std::isfinite(timeSeconds)) return false;
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
    if (values.size() != width || !std::all_of(values.begin(), values.end(),
            [](float value) { return std::isfinite(value); })) return false;

    std::vector<float> normalized = values;
    if (track.valueType == ayt::resource::AnimTrackType::Quaternion) {
        double lengthSquared = 0;
        for (const float value : normalized) lengthSquared += static_cast<double>(value) * value;
        if (lengthSquared <= 1.0e-12) return false;
        for (float& value : normalized) value = static_cast<float>(value / std::sqrt(lengthSquared));
    }

    const float ticksPerSecond = clip.ticksPerSecond > 0.0f
        ? clip.ticksPerSecond : 1.0f;
    const float tick = static_cast<float>(std::clamp(
        timeSeconds, 0.0, static_cast<double>(clip.duration))) * ticksPerSecond;
    for (std::size_t index = 0u; index < track.times.size(); ++index) {
        if (index != keyIndex && std::fabs(track.times[index] - tick) < 1.0e-5f) {
            return false;
        }
    }
    const bool sameTime = std::fabs(track.times[keyIndex] - tick) < 1.0e-5f;
    const auto valueFirst = track.values.begin() + keyIndex * width;
    if (sameTime && std::equal(normalized.begin(), normalized.end(), valueFirst)) {
        return false;
    }

    std::vector<float> storedIn(width, 0.0f);
    std::vector<float> storedOut(width, 0.0f);
    const bool hasIn = track.inTangents.size() == track.values.size();
    const bool hasOut = track.outTangents.size() == track.values.size();
    if (hasIn) {
        std::copy_n(track.inTangents.begin() + keyIndex * width, width,
                    storedIn.begin());
        track.inTangents.erase(track.inTangents.begin() + keyIndex * width,
            track.inTangents.begin() + (keyIndex + 1u) * width);
    }
    if (hasOut) {
        std::copy_n(track.outTangents.begin() + keyIndex * width, width,
                    storedOut.begin());
        track.outTangents.erase(track.outTangents.begin() + keyIndex * width,
            track.outTangents.begin() + (keyIndex + 1u) * width);
    }
    track.values.erase(track.values.begin() + keyIndex * width,
                       track.values.begin() + (keyIndex + 1u) * width);
    track.times.erase(track.times.begin() + keyIndex);
    const auto position = std::lower_bound(track.times.begin(), track.times.end(), tick);
    const std::size_t destination = static_cast<std::size_t>(
        position - track.times.begin());
    track.times.insert(position, tick);
    track.values.insert(track.values.begin() + destination * width,
                        normalized.begin(), normalized.end());
    if (hasIn) {
        track.inTangents.insert(track.inTangents.begin() + destination * width,
                                storedIn.begin(), storedIn.end());
    }
    if (hasOut) {
        track.outTangents.insert(track.outTangents.begin() + destination * width,
                                 storedOut.begin(), storedOut.end());
    }
    const std::vector<std::string> previousIds{keyframeId};
    const std::vector<std::string> updatedIds{"key." + std::to_string(trackIndex) + "."
        + std::to_string(destination)};
    if (!commitEditedAnimation(buildAnimation(clip), nullptr, &updatedIds, &previousIds)) return false;
    keyframeId = updatedIds.front();
    return true;
}

bool EditorAnimationDocument::transformAnimationKeyframes(
    std::vector<std::string>& keyframeIds, double deltaTimeSeconds,
    std::size_t component, float deltaValue)
{
    const auto* animation = _preview.animation();
    if (!animation) return false;
    std::vector<ayt::anim::editor::AnimationKeyReference> keys;
    if (!parseAuthoringKeys(keyframeIds, keys)) return false;
    auto edit = ayt::anim::editor::translateAnimationKeys(
        *animation, keys, deltaTimeSeconds, component, deltaValue);
    const auto updatedIds = authoringKeyIds(edit.keys);
    if (!edit || !commitEditedAnimation(edit.animation, nullptr, &updatedIds, &keyframeIds)) return false;
    keyframeIds = updatedIds;
    return true;
}

bool EditorAnimationDocument::removeAnimationKeyframes(
    const std::vector<std::string>& keyframeIds)
{
    const auto* animation = _preview.animation();
    if (!animation) return false;
    std::vector<ayt::anim::editor::AnimationKeyReference> keys;
    if (!parseAuthoringKeys(keyframeIds, keys)) return false;
    auto edit = ayt::anim::editor::removeAnimationKeys(*animation, keys);
    const std::vector<std::string> cleared;
    return edit && commitEditedAnimation(edit.animation, nullptr, &cleared, &keyframeIds);
}

std::vector<EditorAnimationNotify>
EditorAnimationDocument::animationNotifies() const
{
    std::vector<EditorAnimationNotify> result;
    const auto* animation = _preview.animation();
    if (animation == nullptr) return result;
    result.reserve(animation->getNotifyCount());
    for (std::uint32_t index = 0u; index < animation->getNotifyCount(); ++index) {
        result.push_back({"notify." + std::to_string(index),
            animation->getNotifyName(index) != nullptr
                ? animation->getNotifyName(index) : "",
            animation->getNotifyTime(index), animation->getNotifyPayload(index)});
    }
    return result;
}

bool EditorAnimationDocument::authoringReadOnly() const
{
    const std::filesystem::path resourcePath(_path);
    return resourcePath.extension() != ".anm"
        || resourcePath.filename().string().find(".baked.") != std::string::npos;
}

bool EditorAnimationDocument::retimeAnimationKeyframes(
    std::vector<std::string>& ids, double anchorSeconds, double scale,
    std::string* error)
{
    std::vector<ayt::anim::editor::AnimationKeyReference> keys;
    const auto* animation = _preview.animation();
    if (!animation || !parseAuthoringKeys(ids, keys)) {
        if (error) *error = "No valid animation keys selected.";
        return false;
    }
    auto edit = ayt::anim::editor::retimeAnimationKeys(*animation, keys, anchorSeconds, scale);
    if (!edit) {
        if (error) *error = edit.error;
        return false;
    }
    const auto updatedIds = authoringKeyIds(edit.keys);
    if (!commitEditedAnimation(edit.animation, error, &updatedIds, &ids)) return false;
    ids = updatedIds;
    return true;
}

bool EditorAnimationDocument::reverseAnimationKeyframes(
    std::vector<std::string>& ids, std::string* error)
{
    double first = timelineDurationSeconds(), last = 0;
    for (const auto& key : timelineKeyframes())
        if (std::find(ids.begin(), ids.end(), key.id) != ids.end()) {
            first = std::min(first, key.timeSeconds);
            last = std::max(last, key.timeSeconds);
        }
    return retimeAnimationKeyframes(ids, first + (last - first) / 2, -1, error);
}

bool EditorAnimationDocument::copyAnimationKeyframes(
    const std::vector<std::string>& ids, EditorAnimationClipboard& clipboard,
    std::string* error) const
{
    const auto* animation = _preview.animation();
    std::vector<ayt::anim::editor::AnimationKeyReference> keys;
    if (!animation || !parseAuthoringKeys(ids, keys)) {
        if (error) *error = "No valid animation keys selected.";
        return false;
    }
    auto data = ayt::anim::editor::copyAnimationKeys(*animation, keys, error);
    if (!data) return false;
    clipboard = {std::move(*data), _preview.skeletonPath()};
    return true;
}

bool EditorAnimationDocument::cutAnimationKeyframes(
    const std::vector<std::string>& ids, EditorAnimationClipboard& clipboard,
    std::string* error)
{
    EditorAnimationClipboard prepared;
    if (authoringReadOnly()) {
        if (error) *error = "Animation is read-only.";
        return false;
    }
    if (!copyAnimationKeyframes(ids, prepared, error)) return false;
    if (!removeAnimationKeyframes(ids)) {
        if (error) *error = "Unable to commit cut.";
        return false;
    }
    clipboard = std::move(prepared);
    if (error) error->clear();
    return true;
}

bool EditorAnimationDocument::pasteAnimationKeyframes(
    const EditorAnimationClipboard& clipboard, double seconds,
    std::vector<std::string>& pastedIds, std::string* error)
{
    const auto* animation = _preview.animation();
    if (!animation || authoringReadOnly()
        || clipboard.skeletonPath != _preview.skeletonPath()) {
        if (error) *error = "Read-only or incompatible skeleton binding; retarget explicitly first.";
        return false;
    }
    auto edit = ayt::anim::editor::pasteAnimationKeys(*animation, clipboard.data, seconds);
    if (!edit) {
        if (error) *error = edit.error;
        return false;
    }
    const auto updatedIds = authoringKeyIds(edit.keys);
    if (!commitEditedAnimation(edit.animation, error, &updatedIds)) return false;
    pastedIds = updatedIds;
    return true;
}

bool EditorAnimationDocument::duplicateAnimationKeyframes(
    std::vector<std::string>& ids, std::string* error)
{
    EditorAnimationClipboard clipboard;
    if (!copyAnimationKeyframes(ids, clipboard, error)) return false;
    double first = timelineDurationSeconds();
    for (const auto& key : timelineKeyframes())
        if (std::find(ids.begin(), ids.end(), key.id) != ids.end())
            first = std::min(first, key.timeSeconds);
    const double rate = animationClipProperties().ticksPerSecond;
    return pasteAnimationKeyframes(clipboard,
        first + clipboard.data.spanSeconds + (rate > 0 ? 1 / rate : 1), ids, error);
}

bool EditorAnimationDocument::addAnimationNotify(
    const std::string& name, double timeSeconds, float payload)
{
    const auto* animation = _preview.animation();
    if (animation == nullptr || name.empty() || !std::isfinite(timeSeconds)
        || !std::isfinite(payload)) return false;
    EditableClip clip = readEditableClip(*animation);
    clip.notifies.push_back({name, static_cast<float>(std::clamp(
        timeSeconds, 0.0, static_cast<double>(clip.duration))), payload});
    std::stable_sort(clip.notifies.begin(), clip.notifies.end(),
        [](const auto& a, const auto& b) { return a.time < b.time; });
    return commitEditedAnimation(buildAnimation(clip));
}

bool EditorAnimationDocument::updateAnimationNotify(
    std::string& notifyId, const std::string& name,
    double timeSeconds, float payload)
{
    const auto index = parseIndex(notifyId, "notify.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getNotifyCount()
        || name.empty() || !std::isfinite(timeSeconds)
        || !std::isfinite(payload)) return false;
    EditableClip clip = readEditableClip(*animation);
    auto marker = clip.notifies[*index];
    const float clampedTime = static_cast<float>(std::clamp(
        timeSeconds, 0.0, static_cast<double>(clip.duration)));
    if (marker.name == name && std::fabs(marker.time - clampedTime) < 1.0e-6f
        && marker.payload == payload) return false;
    marker.name = name;
    marker.time = clampedTime;
    marker.payload = payload;
    clip.notifies.erase(clip.notifies.begin() + *index);
    const auto position = std::upper_bound(clip.notifies.begin(),
        clip.notifies.end(), marker.time,
        [](float time, const auto& candidate) { return time < candidate.time; });
    const std::size_t destination = static_cast<std::size_t>(
        position - clip.notifies.begin());
    clip.notifies.insert(position, std::move(marker));
    const std::vector<std::string> previousIds{notifyId};
    const std::vector<std::string> updatedIds{"notify." + std::to_string(destination)};
    if (!commitEditedAnimation(buildAnimation(clip), nullptr, &updatedIds, &previousIds)) return false;
    notifyId = updatedIds.front();
    return true;
}

bool EditorAnimationDocument::removeAnimationNotify(
    const std::string& notifyId)
{
    const auto index = parseIndex(notifyId, "notify.");
    const auto* animation = _preview.animation();
    if (!index || animation == nullptr || *index >= animation->getNotifyCount()) {
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    clip.notifies.erase(clip.notifies.begin() + *index);
    return commitEditedAnimation(buildAnimation(clip));
}

EditorAnimationClipProperties
EditorAnimationDocument::animationClipProperties() const
{
    const auto* animation = _preview.animation();
    if (animation == nullptr) return {};
    return {animation->getName() != nullptr ? animation->getName() : "",
            animation->getDuration(),
            animation->getTicksPerSecond() > 0.0f
                ? animation->getTicksPerSecond() : 1.0};
}

bool EditorAnimationDocument::setAnimationClipProperties(
    const EditorAnimationClipProperties& properties, std::string* error)
{
    const auto* animation = _preview.animation();
    if (animation == nullptr) {
        if (error != nullptr) *error = "No animation clip is open.";
        return false;
    }
    if (properties.name.empty() || !std::isfinite(properties.durationSeconds)
        || properties.durationSeconds <= 0.0
        || !std::isfinite(properties.ticksPerSecond)
        || properties.ticksPerSecond <= 0.0) {
        if (error != nullptr) {
            *error = "Clip name must be non-empty and duration/rate must be positive finite values.";
        }
        return false;
    }
    EditableClip clip = readEditableClip(*animation);
    const double oldRate = clip.ticksPerSecond > 0.0f
        ? clip.ticksPerSecond : 1.0;
    double latestContent = 0.0;
    for (const auto& track : clip.tracks) {
        if (!track.times.empty()) {
            latestContent = std::max(latestContent,
                static_cast<double>(track.times.back()) / oldRate);
        }
    }
    for (const auto& notify : clip.notifies) {
        latestContent = std::max(latestContent,
                                 static_cast<double>(notify.time));
    }
    if (properties.durationSeconds + 1.0e-6 < latestContent) {
        if (error != nullptr) {
            *error = "Clip duration cannot end before its last keyframe or notify.";
        }
        return false;
    }
    const bool same = clip.name == properties.name
        && std::fabs(static_cast<double>(clip.duration)
            - properties.durationSeconds) < 1.0e-6
        && std::fabs(static_cast<double>(clip.ticksPerSecond)
            - properties.ticksPerSecond) < 1.0e-6;
    if (same) {
        if (error != nullptr) error->clear();
        return false;
    }
    const double tickScale = properties.ticksPerSecond / oldRate;
    if (std::fabs(tickScale - 1.0) > 1.0e-9) {
        for (auto& track : clip.tracks) {
            for (float& tick : track.times) {
                tick = static_cast<float>(static_cast<double>(tick) * tickScale);
            }
        }
    }
    clip.name = properties.name;
    clip.duration = static_cast<float>(properties.durationSeconds);
    clip.ticksPerSecond = static_cast<float>(properties.ticksPerSecond);
    return commitEditedAnimation(buildAnimation(clip), error);
}

bool EditorAnimationDocument::beginAnimationEditGesture(
    const std::string& label)
{
    if (authoringReadOnly()) return false;
    if (!_history.beginTransaction(label)) return false;
    ++_gestureGeneration;
    return true;
}

bool EditorAnimationDocument::commitAnimationEditGesture()
{
    return _history.commitTransaction();
}

bool EditorAnimationDocument::cancelAnimationEditGesture()
{
    return _history.cancelTransaction();
}

bool EditorAnimationDocument::animationEditGestureActive() const noexcept
{
    return _history.transactionActive();
}

bool EditorAnimationDocument::bindSkeleton(const std::string& path,
                                           std::string* error)
{
    const auto before=_preview.skeletonPath();
    const bool retainHistory=_history.canUndo() || _history.canRedo();
    if (!_preview.bindSkeleton(path, error)) return false;
    if (retainHistory && before!=_preview.skeletonPath()
        && !recordSkeletonBinding(before,_preview.skeletonPath())) return false;
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
    std::shared_ptr<ayt::resource::Animation> animation, std::string* error,
    const std::vector<std::string>* afterIds,
    const std::vector<std::string>* beforeIds)
{
    if (authoringReadOnly()) {
        if (error) *error = "Animation is read-only; edit the canonical source .anm clip.";
        return false;
    }
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
    std::optional<ayt::ui::authoring::TimelineSelection> selectionBefore, selectionAfter;
    if (const auto selection = _authoringSelection.lock()) {
        selectionBefore = selectionAfter = *selection;
        if (afterIds) {
            auto& next = *selectionAfter;
            next.keyIds = *afterIds;
            const auto primary = beforeIds ? std::find(beforeIds->begin(), beforeIds->end(), next.primaryKeyId)
                : std::vector<std::string>::const_iterator{};
            const auto primaryIndex = beforeIds && primary != beforeIds->end()
                ? static_cast<std::size_t>(primary - beforeIds->begin()) : 0;
            next.primaryKeyId = next.keyIds.empty() ? "" : next.keyIds[std::min(primaryIndex, next.keyIds.size()-1)];
            std::size_t track = 0, key = 0;
            if (parseKeyId(next.primaryKeyId, track, key)) next.trackId = "animation." + std::to_string(track);
            else if (const auto index = parseIndex(next.primaryKeyId, "notify.")) next.trackId = "event." + std::to_string(*index);
        }
    }
    if (!_preview.replaceAnimation(std::move(animation), true, error)) return false;
    _currentBytes = bytes;
    const bool recorded = _history.recordApplied(
        std::make_unique<AppliedAnimationRevisionCommand>(
            [this, bytes, selectionAfter]() {
                if (!applySerializedRevision(bytes)) return false;
                if (const auto selection = _authoringSelection.lock(); selection && selectionAfter)
                    *selection = *selectionAfter;
                return true;
            },
            [this, before, selectionBefore]() {
                if (!applySerializedRevision(before)) return false;
                if (const auto selection = _authoringSelection.lock(); selection && selectionBefore)
                    *selection = *selectionBefore;
                return true;
            },
            "Edit animation tracks and keys", _history.transactionActive()
                ? "animation-revision." + std::to_string(_gestureGeneration) : ""));
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

bool EditorAnimationDocument::persistPreviewMetadata(std::string* error, bool includeControls) const
{
    if (_metadataPath.empty()) {
        if (includeControls && _preview.controlRig()) {
            if (error) *error="Configure a project before saving control rig authoring metadata.";
            return false;
        }
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
        const auto oldControls=entry.is_object() ? entry.value("controlRig", Json{}) : Json{};
        const auto savedSkeleton=entry.is_object() ? entry.value("skeleton", std::string{}) : std::string{};
        entry = {{"skeleton", encodeProjectPath(
                                  _preview.skeletonPath(), _projectRoot)},
                 {"mesh", encodeProjectPath(_preview.meshPath(), _projectRoot)},
                 {"material", encodeProjectPath(
                                  _preview.materialPath(), _projectRoot)},
                 {"mode", modeValue(_preview.requestedPreviewMode())},
                 {"loop", _preview.looping()},
                 {"speed", _preview.playRate()}};
        if (includeControls) {
            if (const auto* rig=_preview.controlRig()) entry["controlRig"]=Json::parse(rig->encode());
        } else if (!oldControls.is_null()) {
            entry["controlRig"]=oldControls;
            // An unsaved clear/rebind is authoring state. Keep the saved rig's
            // matching skeleton until the explicit Save commits both.
            entry["skeleton"]=savedSkeleton;
        }
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
    if (_preview.controlRig()) return; // Reconfiguring the host must not discard live authoring.
    _controlRigLoadError.clear();
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
        if (value->contains("controlRig")) {
            auto rig=std::make_shared<ayt::anim::editor::HumanoidControlRig>();
            if (!_preview.skeleton()) _controlRigLoadError="Bind the saved skeleton to restore its controls.";
            else if (!rig->restore(*_preview.skeleton(),value->at("controlRig").dump(),&_controlRigLoadError)
                || !_preview.setControlRig(rig,&_controlRigLoadError)) {
                if (_controlRigLoadError.empty()) _controlRigLoadError="Saved controls are invalid.";
            }
        }
        _selectedBone = _preview.bones().empty() ? -1 : 0;
    } catch (...) {
        // Corrupt editor metadata must never prevent the cooked asset opening.
    }
}

} // namespace ayt::editor
