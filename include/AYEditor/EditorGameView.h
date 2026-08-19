#pragma once

#include <AYGameLoop/IGameLoop.h>
#include <cstdint>
#include <functional>

namespace ayt::editor {

class EditorPlayRuntime;

enum class EditorMode : uint8_t {
    Edit,
    Play,
    Paused,
};

class EditorGameView {
public:
    EditorGameView(ayt::game::IGameLoop& loop, EditorPlayRuntime& runtime);

    EditorMode mode() const { return _mode; }
    // Compatibility command for existing toolbar/UI callers. It leaves the
    // current mode unchanged if the runtime transition cannot be completed.
    void setMode(EditorMode mode);
    // Transactional variant for hosts that need to surface a transition
    // failure to the user.
    [[nodiscard]] bool trySetMode(EditorMode mode);
    void stepOnce();

    using ModeChangedCallback = std::function<void(EditorMode)>;
    void setModeChangedCallback(ModeChangedCallback callback);

private:
    [[nodiscard]] bool applyMode(EditorMode mode);

    ayt::game::IGameLoop& _loop;
    EditorPlayRuntime& _runtime;
    EditorMode _mode = EditorMode::Edit;
    ModeChangedCallback _modeChanged;
};

} // namespace ayt::editor
