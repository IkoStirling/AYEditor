#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>

namespace {

std::filesystem::path launcherPath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).lexically_normal();
}

std::wstring quoteArgument(std::wstring_view argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring forwardedCommandLine(const std::filesystem::path& hostPath)
{
    int argumentCount = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(
        ::GetCommandLineW(), &argumentCount);

    std::wstring commandLine = quoteArgument(hostPath.wstring());
    if (arguments != nullptr) {
        for (int index = 1; index < argumentCount; ++index) {
            commandLine.push_back(L' ');
            commandLine += quoteArgument(arguments[index]);
        }
        ::LocalFree(arguments);
    }
    return commandLine;
}

void showLaunchError(const std::wstring& message)
{
    ::MessageBoxW(nullptr, message.c_str(), L"AY Editor failed to start",
                  MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::filesystem::path executable = launcherPath();
    if (executable.empty()) {
        showLaunchError(L"Unable to resolve the AYEditor executable path.");
        return ERROR_PATH_NOT_FOUND;
    }

    const std::filesystem::path productRoot = executable.parent_path();
    const std::filesystem::path runtimeRoot = productRoot / L"AYRuntime";
    const std::filesystem::path hostPath =
        runtimeRoot / L"AYEditorShell_Demo.exe";
    if (!std::filesystem::is_regular_file(hostPath)) {
        showLaunchError(
            L"AYRuntime\\AYEditorShell_Demo.exe is missing. Reinstall AYEditor.");
        return ERROR_FILE_NOT_FOUND;
    }

    // The user-facing launcher deliberately links only Windows system DLLs.
    // The actual host and every app-local dependency share AYRuntime, so the
    // Windows loader can resolve imports before the host reaches WinMain.
    ::SetEnvironmentVariableW(
        L"AY_EDITOR_PRODUCT_ROOT", productRoot.c_str());

    std::wstring commandLine = forwardedCommandLine(hostPath);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(
        hostPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
        nullptr, productRoot.c_str(), &startup, &process);
    if (!created) {
        const DWORD error = ::GetLastError();
        showLaunchError(
            L"Windows could not launch the AYEditor runtime (error "
            + std::to_wstring(error) + L").");
        return static_cast<int>(error);
    }

    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    (void)::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
