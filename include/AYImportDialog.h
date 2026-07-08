#pragma once
// AYImportDialog.h — Phase 1 ED-01.
//
// Minimal entry point that ED-02 / Inspector / EditorSession call when
// the user wants to import a file. The plan calls for "Win32
// GetOpenFileName + hard-coded path field for headless tests". This
// iteration ships only the headless path — the Win32 dialog is a
// one-liner around `importFromPath` once the menu bar wiring lands in
// a follow-up commit. The path-field shape lets unit tests drive the
// entire editor flow end-to-end without spawning Win32 UI.

#include "AYImporter.h"

namespace ayt::editor
{

class ImportDialog
{
public:
    // Headless import entry. Returns whatever `Importer::importFile`
    // returned.
    static Importer::Result importFromPath(const std::string& sourcePath,
                                           const std::string& destinationDir);
};

} // namespace ayt::editor
