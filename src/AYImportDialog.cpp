// AYImportDialog.cpp — Phase 1 ED-01.
//
// Currently a thin shim over `Importer::importFile`. When the Win32
// `GetOpenFileName` wiring lands (Phase 2 menu bar) this file grows
// the popup construction; the headless test path stays as is.

#include "AYImportDialog.h"

namespace ayt::editor
{

Importer::Result ImportDialog::importFromPath(const std::string& sourcePath,
                                              const std::string& destinationDir)
{
    return Importer::importFile(sourcePath, destinationDir);
}

} // namespace ayt::editor
