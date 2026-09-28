#pragma once
#include "AYEditor/EditorBuiltInExtensions.h"
#include <AYUI/Authoring/PlaybackControls.h>

namespace ayt::editor {
struct TimelinePlaybackExtras {
    std::function<bool()> looping;
    std::function<void(bool)> setLooping;
    std::function<float()> rate;
    std::function<void(float)> setRate;
};
// Acquire a shared document on every operation, including contextual source switches.
// No Tick here: the document view / contextual tool retains its existing authority.
class TimelinePlaybackSource final : public ayt::ui::authoring::IPlaybackSource {
public:
    using Acquire = std::function<std::shared_ptr<IEditorTimelineSource>()>;
    TimelinePlaybackSource(Acquire acquire, TimelinePlaybackExtras extras = {})
        : _acquire(std::move(acquire)), _extras(std::move(extras)) {}
    ayt::ui::authoring::PlaybackState playbackState() const override {
        const auto owner = _acquire();
        if (!owner) return {};
        ayt::ui::authoring::PlaybackState result{true, owner->timelinePlaying(),
            owner->timelinePositionSeconds(), owner->timelineDurationSeconds(), {}, {}};
        if (_extras.looping) result.looping = _extras.looping();
        if (_extras.rate) result.rate = _extras.rate();
        return result;
    }
    void play() override { if (auto owner = _acquire()) owner->timelinePlay(); }
    void pause() override { if (auto owner = _acquire()) owner->timelinePause(); }
    void stop() override { if (auto owner = _acquire()) owner->timelineStop(); }
    bool seek(double seconds) override {
        auto owner = _acquire(); return owner && owner->setTimelinePositionSeconds(seconds);
    }
    bool setLooping(bool value) override {
        if (!_acquire() || !_extras.setLooping) return false;
        _extras.setLooping(value); return true;
    }
    bool setRate(float value) override {
        if (!_acquire() || !_extras.setRate) return false;
        _extras.setRate(value); return true;
    }
private:
    Acquire _acquire;
    TimelinePlaybackExtras _extras;
};
} // namespace ayt::editor
