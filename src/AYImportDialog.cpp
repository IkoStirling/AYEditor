// AYImportDialog.cpp - Phase 1 ED-01 + Phase 2a toolbar wiring.
//
// importFromPath is a thin shim over Importer::importFile (kept for
// the EditorShell_Demo --import argv path and for unit tests).
//
// showOpenFileDialog wraps Win32 GetOpenFileNameA from <commdlg.h>
// and is bound to the toolbar Import button. It is statically guarded
// so non-Windows builds compile (returns empty string there).

#include "AYImportDialog.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#  include <commdlg.h>
#endif

namespace ayt::editor
{

Importer::Result ImportDialog::importFromPath(const std::string& sourcePath,
                                              const std::string& destinationDir)
{
    return Importer::importFile(sourcePath, destinationDir);
}

std::string ImportDialog::showOpenFileDialog(void* ownerWindowHandle)
{
#if !defined(_WIN32)
    (void)ownerWindowHandle;
    return std::string{};
#else
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = static_cast<HWND>(ownerWindowHandle);
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = MAX_PATH;
    // Filter pairs: "Display\0Pattern\0", each pair null-terminated,
    // list double-null-terminated. The first pair is the default.
    // We lead with "3D Model" (FBX + glTF + glTF-binary) which is
    // what AYResource::IConverter's extension switch understands;
    // the per-format filters are exposed as follow-ups. "All files"
    // is the GetOpenFileName idiom for allowing type override.
    ofn.lpstrFilter =
        "3D Model (*.fbx;*.gltf;*.glb)\0*.fbx;*.gltf;*.glb\0"
        "FBX (*.fbx)\0*.fbx\0"
        "glTF / glTF-binary (*.gltf;*.glb)\0*.gltf;*.glb\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    // OFN_FILEMUSTEXIST  : never return a non-existent path.
    // OFN_PATHMUSTEXIST  : never return a path with a missing dir.
    // OFN_NOCHANGEDIR    : do not mutate the CWD (we read asset
    //                       roots from EditorPlayRuntime and would
    //                       not want the file dialog to break it).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameA(&ofn)) {
        // Zero return covers both "user cancelled" (CommDlgExtendedError
        // returns 0) and various failure modes. Either way the caller
        // treats empty as a no-op; we do not surface the extended
        // error code because no machine-parseable recovery is sensible
        // from a UI cancel.
        return std::string{};
    }

    return std::string(path);
#endif
}

} // namespace ayt::editor