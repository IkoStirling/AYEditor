#pragma once
#include <algorithm>
#include <cmath>

namespace ayt::editor {
// Input is dispatched every host iteration. Only the focused workbench's
// telemetry/UI presentation is paced; never catch up by drawing many frames.
class EditorAudioFramePacer {
public:
    struct Refresh { bool due = false; float deltaSeconds = 0; };
    Refresh advance(bool focused, float wallDeltaSeconds) {
        if (!focused) { _active = false; _elapsed = 0; return {}; }
        const float dt = std::isfinite(wallDeltaSeconds)
            ? std::clamp(wallDeltaSeconds, 0.0f, 0.1f) : 0.0f;
        if (!_active) { _active = true; _elapsed = 0; return {true, dt}; }
        _elapsed += dt;
        if (_elapsed < 1.0f / 60.0f) return {};
        const float elapsed = _elapsed;
        _elapsed = 0;
        return {true, elapsed};
    }
private:
    bool _active = false;
    float _elapsed = 0;
};
} // namespace ayt::editor
