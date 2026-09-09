#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {

inline constexpr const char* kEditorTilemapExtensionId =
    "ayeditor.tilemap";
inline constexpr const char* kEditorAudioToolExtensionId =
    "ayeditor.tool.audio";
inline constexpr const char* kEditorTimelineToolExtensionId =
    "ayeditor.tool.timeline";
inline constexpr const char* kEditorAnimationTimelineExtensionId =
    "ayeditor.timeline.animation";
inline constexpr const char* kEditorAudioTimelineExtensionId =
    "ayeditor.timeline.audio";

enum class EditorTimelineTrackKind : std::uint8_t {
    Animation,
    Audio,
    Event,
};

struct EditorTimelineTrack {
    std::string id;
    std::string name;
    EditorTimelineTrackKind kind = EditorTimelineTrackKind::Animation;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool enabled = true;
};

struct EditorTimelineKeyframe {
    std::string id;
    std::string trackId;
    double timeSeconds = 0.0;
    double value = 0.0;
};

struct EditorTimelineClip {
    std::string id;
    std::string trackId;
    std::string sourcePath;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    float gain = 1.0f;
};

// Optional document capability consumed by the contextual Timeline panel.
// It deliberately carries time-domain state only; animation, audio and future
// sequencer documents can expose it without depending on each other.
class IEditorTimelineSource {
public:
    virtual ~IEditorTimelineSource() = default;
    virtual double timelineDurationSeconds() const noexcept = 0;
    virtual double timelinePositionSeconds() const noexcept = 0;
    virtual bool setTimelinePositionSeconds(double seconds) = 0;
    virtual std::vector<EditorTimelineTrack> timelineTracks() const {
        return {};
    }
    virtual bool timelinePlaying() const noexcept { return false; }
    virtual void timelinePlay() {}
    virtual void timelinePause() {}
    virtual void timelineStop() { (void)setTimelinePositionSeconds(0.0); }
    virtual void timelineTick(double) {}
    virtual std::vector<EditorTimelineKeyframe> timelineKeyframes() const {
        return {};
    }
    virtual std::vector<EditorTimelineClip> timelineClips() const {
        return {};
    }
    // Normalized min/max envelope pairs used by timeline waveform painters.
    virtual std::vector<float> timelineWaveformPeaks(
        const std::string&) const { return {}; }
    virtual bool timelineAddKeyframe(const std::string&, double, double) {
        return false;
    }
    virtual bool timelineMoveKeyframe(const std::string&, double) {
        return false;
    }
    virtual bool timelineRemoveKeyframe(const std::string&) { return false; }
    virtual bool timelineAddClip(const EditorTimelineClip&) { return false; }
    virtual bool timelineMoveClip(const std::string&, double) { return false; }
    virtual bool timelineRemoveClip(const std::string&) { return false; }
    virtual bool timelineCanUndo() const noexcept { return false; }
    virtual bool timelineCanRedo() const noexcept { return false; }
    virtual bool timelineUndo() { return false; }
    virtual bool timelineRedo() { return false; }
};

struct EditorBuiltInExtensionConfig {
    std::function<void()> openAudioMixer;
};

bool registerEditorBuiltInExtensions(
    EditorExtensionRegistry& registry,
    EditorBuiltInExtensionConfig config = {},
    std::string* error = nullptr);

} // namespace ayt::editor
