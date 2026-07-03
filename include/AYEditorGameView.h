#pragma once

#include <IAYGameLoop.h>
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
    void setMode(EditorMode mode);
    void stepOnce();

    using ModeChangedCallback = std::function<void(EditorMode)>;
    void setModeChangedCallback(ModeChangedCallback callback);

private:
    void applyMode(EditorMode mode);

    ayt::game::IGameLoop& _loop;
    EditorPlayRuntime& _runtime;
    EditorMode _mode = EditorMode::Edit;
    ModeChangedCallback _modeChanged;
};

} // namespace ayt::editor
