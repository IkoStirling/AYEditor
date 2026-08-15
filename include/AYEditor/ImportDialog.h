#pragma once
// AYEditor/ImportDialog.h — Phase 1 ED-01.
//
// Minimal entry point that ED-02 / Inspector / EditorSession call when
// the user wants to import a file. The plan calls for "Win32
// GetOpenFileName + hard-coded path field for headless tests". This
// iteration ships only the headless path — the Win32 dialog is a
// one-liner around `importFromPath` once the menu bar wiring lands in
// a follow-up commit. The path-field shape lets unit tests drive the
// entire editor flow end-to-end without spawning Win32 UI.

#include "AYEditor/Importer.h"

namespace ayt::editor
{

class ImportDialog
{
public:
    // Headless import entry. Returns whatever `Importer::importFile`
    // returned.
    static Importer::Result importFromPath(const std::string& sourcePath,
                                           const std::string& destinationDir);

    // Phase 2a: Win32 file picker for the toolbar Import button.
    // Opens a standard Win32 "Open file" dialog filtered to FBX /
    // glTF / glTF-binary. `ownerWindowHandle` may be nullptr for a
    // modeless dialog; usually it's the editor's host HWND.
    // Returns the picked absolute path, or empty string when the
    // user cancelled or the OS call failed. On non-Windows
    // platforms the function returns empty (no-op stub) so the
    // call site compiles cross-platform even though the toolbar
    // button is wired unconditionally.
    static std::string showOpenFileDialog(void* ownerWindowHandle);
};

} // namespace ayt::editor
