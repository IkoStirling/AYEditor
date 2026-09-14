// AYImportDialog.cpp - Phase 1 ED-01 + Phase 2a toolbar wiring.
//
// importFromPath is a thin shim over Importer::importFile (kept for
// the EditorShell_Demo --import argv path and for unit tests).
//
// showOpenFileDialog wraps Win32 GetOpenFileNameW from <commdlg.h>
// and is bound to the toolbar Import button. It is statically guarded
// so non-Windows builds compile (returns empty string there).
//
// B-17 (ayeditor audit 2026-09-14): switched from GetOpenFileNameA to
// GetOpenFileNameW so UTF-8 paths (Asian character filenames, emoji,
// accented characters) round-trip correctly. The ANSI variants mangle
// any non-system-codepage byte to '?', which corrupts asset references
// the moment a user picks a file outside the system ANSI code page.
// We now convert filter strings UTF-8 -> UTF-16 at call time, grow the
// path buffer to UNICODE_PATH_MAX (32 Ki chars) so we can receive long
// \\?\ UNC paths, and call CommDlgExtendedError() on FALSE return to
// distinguish user-cancel from a real dialog failure -- the prior code
// silently swallowed every FALSE as "cancel".

#include "AYEditor/ImportDialog.h"

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

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ayt::editor
{

Importer::Result ImportDialog::importFromPath(const std::string& sourcePath,
                                              const std::string& destinationDir)
{
    return Importer::importFile(sourcePath, destinationDir,
                                Importer::MaterialPolicy{},
                                ayt::resource::SourceCoordinatePolicy{});
}

#if defined(_WIN32)

namespace
{

// Convert a UTF-8 byte sequence to a UTF-16 wide string for the Win32
// "W" APIs. Returns an empty string on conversion failure; the caller
// is responsible for treating empty as "fail this dialog" -- we never
// want to silently fall back to a wrong-codepage buffer.
std::wstring utf8ToWide(std::string_view utf8)
{
    if (utf8.empty()) return std::wstring{};
    const int sourceLength = static_cast<int>(utf8.size());
    // MultiByteToWideChar with cchWideChar=0 returns the required buffer
    // size (including the terminating NUL).
    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), sourceLength, nullptr, 0);
    if (required <= 0) return std::wstring{};
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), sourceLength, output.data(), required);
    if (written != required) return std::wstring{};
    return output;
}

// Buffer size in TCHARs (UTF-16 code units, including the terminating NUL).
// 32 Ki is the modern cap for the \\?\ long-path prefix; well above the
// legacy MAX_PATH (260) and large enough to hold typical asset paths.
constexpr DWORD kWidePathBuffer = 32768;

// Builds a double-NUL-terminated UTF-16 filter string out of a sequence
// of UTF-8 "Display\0Pattern" pairs. The Win32 OPENFILENAMEW filter
// expects pairs of NUL-terminated wide strings followed by an extra
// terminating NUL.
std::wstring buildWideFilter(
    const std::vector<std::pair<std::string, std::string>>& pairs)
{
    std::wstring output;
    for (const auto& pair : pairs) {
        std::wstring display = utf8ToWide(pair.first);
        std::wstring pattern = utf8ToWide(pair.second);
        if (display.empty() || pattern.empty()) return std::wstring{};
        output.append(display);
        output.push_back(L'\0');
        output.append(pattern);
        output.push_back(L'\0');
    }
    output.push_back(L'\0'); // double-NUL terminator
    return output;
}

// Result of a Win32 dialog call. Used by all three showOpen* helpers so
// they can uniformly convert UTF-16 -> UTF-8 and report CommDlg failures.
struct DialogResult
{
    std::string pickedPath;     // UTF-8; empty when user cancelled.
    std::string extendedError;  // non-empty when the dialog actually failed.
    bool wasCancelled() const noexcept
    {
        return pickedPath.empty() && extendedError.empty();
    }
};

DialogResult runOpenDialog(
    void* ownerWindowHandle,
    const std::vector<std::pair<std::string, std::string>>& filterPairs,
    DWORD filterIndex)
{
    DialogResult result;
    const std::wstring filter = buildWideFilter(filterPairs);
    if (filter.empty()) {
        result.extendedError = "Could not convert file filter to UTF-16.";
        return result;
    }

    std::vector<wchar_t> path(kWidePathBuffer, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = static_cast<HWND>(ownerWindowHandle);
    ofn.lpstrFile    = path.data();
    ofn.nMaxFile     = kWidePathBuffer;
    ofn.lpstrFilter  = filter.c_str();
    ofn.nFilterIndex = filterIndex;
    // OFN_FILEMUSTEXIST  : never return a non-existent path.
    // OFN_PATHMUSTEXIST  : never return a path with a missing dir.
    // OFN_NOCHANGEDIR    : do not mutate the CWD (we read asset
    //                       roots from EditorPlayRuntime and would
    //                       not want the file dialog to break it).
    // OFN_EXPLORER       : request the modern Explorer-style dialog
    //                       (the default on Vista+; explicit here so a
    //                       future caller can't accidentally get the
    //                       legacy dialog by failing to set it).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
              | OFN_NOCHANGEDIR  | OFN_EXPLORER;

    if (!::GetOpenFileNameW(&ofn)) {
        // Distinguish "user cancelled" from "dialog failed":
        //   - CommDlgExtendedError() == 0  : user pressed Cancel
        //     (or otherwise dismissed the dialog without picking).
        //   - any non-zero code           : an actual dialog failure
        //     (CDERR_DIALOGFAILURE, FNERR_BUFFERTOOSMALL, ...).
        // We surface the extended error in the result so callers can
        // log it; previously the entire FALSE return was treated as
        // cancel, which silently lost real failures.
        const DWORD extended = ::CommDlgExtendedError();
        if (extended != 0) {
            char message[64] = {};
            std::snprintf(message, sizeof(message),
                "Common dialog error 0x%08lX", static_cast<unsigned long>(extended));
            result.extendedError = message;
        }
        return result;
    }

    const std::wstring widePath(path.data());
    if (widePath.empty()) return result;
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        widePath.c_str(), static_cast<int>(widePath.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        result.extendedError = "Could not convert picked path to UTF-8.";
        return result;
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        widePath.c_str(), static_cast<int>(widePath.size()),
        utf8.data(), required, nullptr, nullptr);
    if (written != required) {
        result.extendedError = "Could not convert picked path to UTF-8.";
        return result;
    }
    result.pickedPath = std::move(utf8);
    return result;
}

} // anonymous namespace

#endif // _WIN32

std::string ImportDialog::showOpenFileDialog(void* ownerWindowHandle)
{
#if !defined(_WIN32)
    (void)ownerWindowHandle;
    return std::string{};
#else
    // Filter pairs: "Display\0Pattern\0", each pair null-terminated,
    // list double-null-terminated. The first pair is the default.
    // We lead with "3D Model" (FBX + glTF + glTF-binary) which is
    // what AYResource::IConverter's extension switch understands;
    // the per-format filters are exposed as follow-ups. "All files"
    // is the GetOpenFileName idiom for allowing type override.
    const DialogResult result = runOpenDialog(ownerWindowHandle, {
        {"3D Model (*.fbx;*.gltf;*.glb)", "*.fbx;*.gltf;*.glb"},
        {"FBX (*.fbx)",                    "*.fbx"},
        {"glTF / glTF-binary (*.gltf;*.glb)", "*.gltf;*.glb"},
        {"All files (*.*)",                "*.*"},
    }, 1);
    return result.pickedPath;
#endif
}

std::string ImportDialog::showOpenAssetFileDialog(void* ownerWindowHandle)
{
#if !defined(_WIN32)
    (void)ownerWindowHandle;
    return std::string{};
#else
    const DialogResult result = runOpenDialog(ownerWindowHandle, {
        {"Importable assets (*.fbx;*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.wav;*.ogg;*.mp3)",
            "*.fbx;*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.wav;*.ogg;*.mp3"},
        {"3D models (*.fbx;*.gltf;*.glb)", "*.fbx;*.gltf;*.glb"},
        {"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)",
            "*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds"},
        {"Audio (*.wav;*.ogg;*.mp3)",      "*.wav;*.ogg;*.mp3"},
        {"All files (*.*)",                "*.*"},
    }, 1);
    return result.pickedPath;
#endif
}

std::string ImportDialog::showOpenImageFileDialog(void* ownerWindowHandle)
{
#if !defined(_WIN32)
    (void)ownerWindowHandle;
    return std::string{};
#else
    const DialogResult result = runOpenDialog(ownerWindowHandle, {
        {"PNG tile sheets (*.png)",        "*.png"},
        {"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp)",
            "*.png;*.jpg;*.jpeg;*.tga;*.bmp"},
        {"All files (*.*)",                "*.*"},
    }, 1);
    return result.pickedPath;
#endif
}

} // namespace ayt::editor
