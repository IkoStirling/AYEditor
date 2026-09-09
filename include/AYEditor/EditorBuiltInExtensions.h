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
};

struct EditorBuiltInExtensionConfig {
    std::function<void()> openAudioMixer;
};

bool registerEditorBuiltInExtensions(
    EditorExtensionRegistry& registry,
    EditorBuiltInExtensionConfig config = {},
    std::string* error = nullptr);

} // namespace ayt::editor
