#pragma once

#include <memory>
#include <string_view>

namespace ayt::editor {

// Lightweight bootstrap UI shown before AYRenderer/AYUI are ready.  The
// implementation owns a dedicated Win32 message thread, so synchronous
// renderer and importer work on the editor thread cannot turn the splash into
// an unpainted or "Not responding" window.  It is deliberately not a render
// host: the final editor HWND is created separately and stays hidden until its
// first composite frame has been submitted.
class EditorStartupSplash final {
public:
    EditorStartupSplash();
    ~EditorStartupSplash();

    EditorStartupSplash(const EditorStartupSplash&) = delete;
    EditorStartupSplash& operator=(const EditorStartupSplash&) = delete;

    bool show();
    void update(float progress, std::wstring_view stage);
    void close();
    bool isVisible() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::editor
