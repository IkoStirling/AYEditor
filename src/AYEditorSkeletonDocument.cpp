#include "AYEditor/EditorSkeletonDocument.h"

#include <AYResource/assetsDefs/IAnimation.h>

#include <algorithm>
#include <filesystem>

namespace ayt::editor {

bool EditorSkeletonDocument::initialize(const EditorOpenRequest& request,
                                        std::string& error)
{
    if (!_core.open(request.resourcePath, &error)) return false;
    _path = _core.skeletonPath();
    const std::string display = request.displayPath.empty()
        ? _path : request.displayPath;
    _title = std::filesystem::path(display).filename().string();
    return true;
}

bool EditorSkeletonDocument::save(std::string* error)
{
    return _core.saveMapping(error);
}

bool EditorSkeletonDocument::reload(std::string* error)
{
    return _core.reload(error);
}

bool EditorSkeletonDocument::handlesCommand(const std::string& commandId) const
{
    return commandId == "file.save" || commandId == "edit.undo"
        || commandId == "edit.redo";
}

bool EditorSkeletonDocument::canExecuteCommand(const std::string& commandId) const
{
    if (commandId == "file.save") return _core.isDirty();
    if (commandId == "edit.undo") return _core.canUndo();
    if (commandId == "edit.redo") return _core.canRedo();
    return false;
}

bool EditorSkeletonDocument::executeCommand(const std::string& commandId)
{
    if (!canExecuteCommand(commandId)) return false;
    if (commandId == "file.save") return save();
    if (commandId == "edit.undo") return _core.undo();
    if (commandId == "edit.redo") return _core.redo();
    return false;
}

double EditorSkeletonDocument::timelineDurationSeconds() const noexcept
{
    return _core.duration();
}

double EditorSkeletonDocument::timelinePositionSeconds() const noexcept
{
    return _core.time();
}

bool EditorSkeletonDocument::setTimelinePositionSeconds(double seconds)
{
    return _core.setTime(static_cast<float>(seconds));
}

std::vector<EditorTimelineTrack> EditorSkeletonDocument::timelineTracks() const
{
    std::vector<EditorTimelineTrack> result;
    const ayt::resource::IAnimation* animation = _core.animation();
    if (animation == nullptr) return result;
    result.reserve(animation->getTrackCount());
    for (std::uint32_t index = 0; index < animation->getTrackCount(); ++index) {
        const char* node = animation->getTrackNodeName(index);
        const char* property = animation->getTrackProperty(index);
        EditorTimelineTrack track;
        track.id = "track-" + std::to_string(index);
        track.name = (node != nullptr ? node : "<unnamed>");
        if (property != nullptr && *property != '\0') {
            track.name += ".";
            track.name += property;
        }
        track.kind = EditorTimelineTrackKind::Animation;
        track.startSeconds = 0.0;
        track.endSeconds = animation->getDuration();
        result.push_back(std::move(track));
    }
    return result;
}

std::vector<EditorTimelineKeyframe>
EditorSkeletonDocument::timelineKeyframes() const
{
    std::vector<EditorTimelineKeyframe> result;
    const ayt::resource::IAnimation* animation = _core.animation();
    if (animation == nullptr) return result;
    const float ticksPerSecond = std::max(animation->getTicksPerSecond(), 1.0e-5f);
    for (std::uint32_t track = 0; track < animation->getTrackCount(); ++track) {
        const std::uint32_t count = animation->getTrackKeyframeCount(track);
        const float* times = animation->getTrackTimes(track);
        for (std::uint32_t key = 0; key < count && times != nullptr; ++key) {
            result.push_back({"track-" + std::to_string(track) + "-key-"
                + std::to_string(key), "track-" + std::to_string(track),
                static_cast<double>(times[key] / ticksPerSecond), 0.0});
        }
    }
    return result;
}

bool EditorSkeletonDocument::timelinePlaying() const noexcept
{
    return _core.isPlaying();
}

void EditorSkeletonDocument::timelinePlay() { _core.play(); }
void EditorSkeletonDocument::timelinePause() { _core.pause(); }
void EditorSkeletonDocument::timelineStop() { _core.stop(); }
void EditorSkeletonDocument::timelineTick(double seconds) {
    _core.tick(static_cast<float>(seconds));
}
bool EditorSkeletonDocument::timelineCanUndo() const noexcept { return _core.canUndo(); }
bool EditorSkeletonDocument::timelineCanRedo() const noexcept { return _core.canRedo(); }
bool EditorSkeletonDocument::timelineUndo() { return _core.undo(); }
bool EditorSkeletonDocument::timelineRedo() { return _core.redo(); }

} // namespace ayt::editor
