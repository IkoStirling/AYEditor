#include "AYEditor/EditorProjectRunner.h"
#include "AYEditor/EditorProjectDescriptor.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

std::string normalized(const fs::path& path)
{
    std::error_code error;
    fs::path result = fs::absolute(path, error).lexically_normal();
    if (!error) {
        const fs::path canonical = fs::weakly_canonical(result, error);
        if (!error) result = canonical;
    }
    return result.string();
}

EditorProjectRunConfig loadRunOverride(const fs::path& root,
                                       std::string* error)
{
    const fs::path path = root / ".ayeditor" / "run.json";
    if (!fs::is_regular_file(path)) return {};
    try {
        std::ifstream input(path, std::ios::binary);
        nlohmann::json json;
        input >> json;
        EditorProjectRunConfig result;
        fs::path executable = json.value("executable", std::string{});
        if (executable.is_relative()) executable = root / executable;
        result.executable = normalized(executable);
        fs::path working = json.value("workingDirectory", std::string{});
        if (working.empty()) working = root;
        else if (working.is_relative()) working = root / working;
        result.workingDirectory = normalized(working);
        if (json.contains("arguments") && json["arguments"].is_array()) {
            for (const auto& argument : json["arguments"]) {
                if (argument.is_string()) {
                    result.arguments.push_back(argument.get<std::string>());
                }
            }
        }
        result.source = path.string();
        if (!fs::is_regular_file(result.executable)) {
            if (error != nullptr) *error =
                "Configured executable does not exist: " + result.executable;
            return {};
        }
        return result;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = std::string("Invalid run.json: ")
            + exception.what();
        return {};
    }
}

EditorProjectRunConfig loadProjectDescriptor(const fs::path& root,
                                             std::string* error)
{
    const fs::path descriptorPath = root / kEditorProjectDescriptorFile;
    if (!fs::is_regular_file(descriptorPath)) return {};
    std::string descriptorError;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(root.string(), &descriptorError);
    if (!descriptor) {
        if (error != nullptr) *error = descriptorError;
        return {};
    }
    if (descriptor.run.executable.empty()) return {};
    EditorProjectRunConfig result;
    fs::path executable = descriptor.run.executable;
    if (executable.is_relative()) executable = root / executable;
    result.executable = normalized(executable);
    fs::path working = descriptor.run.workingDirectory;
    if (working.empty()) working = root;
    else if (working.is_relative()) working = root / working;
    result.workingDirectory = normalized(working);
    result.arguments = descriptor.run.arguments;
    result.source = descriptor.sourcePath;
    if (!fs::is_regular_file(result.executable)) {
        if (error != nullptr) {
            *error = "Configured executable does not exist: " + result.executable;
        }
        return {};
    }
    return result;
}

#if defined(_WIN32)
std::wstring utf8(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring quote(std::wstring value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') {
            output.append(slashes * 2u + 1u, L'\\');
            output.push_back(L'\"');
        } else {
            output.append(slashes, L'\\');
            output.push_back(character);
        }
        slashes = 0;
    }
    output.append(slashes * 2u, L'\\');
    output.push_back(L'\"');
    return output;
}
#endif

} // namespace

EditorProjectRunConfig EditorProjectRunner::resolve(
    const std::string& projectRoot, std::string* error)
{
    if (error != nullptr) error->clear();
    const fs::path root = normalized(projectRoot.empty()
        ? fs::current_path() : fs::path(projectRoot));
    std::string manifestError;
    EditorProjectRunConfig configured = loadRunOverride(root, &manifestError);
    if (configured) return configured;
    if (!manifestError.empty()) {
        if (error != nullptr) *error = manifestError;
        return {};
    }
    configured = loadProjectDescriptor(root, &manifestError);
    if (configured) return configured;
    if (!manifestError.empty()) {
        if (error != nullptr) *error = manifestError;
        return {};
    }

    const std::string projectName = root.filename().string();
#if defined(_WIN32)
    const std::string executableName = projectName + ".exe";
#else
    const std::string executableName = projectName;
#endif
    const fs::path candidates[] = {
        root / "out" / "build" / "windows-client-debug" / executableName,
        root / "out" / "build" / "windows-debug" / executableName,
        root / "build" / executableName,
        root / executableName,
    };
    for (const fs::path& candidate : candidates) {
        if (fs::is_regular_file(candidate)) {
            return {candidate.lexically_normal().string(), root.string(), {},
                    "conventional build output"};
        }
    }

    const fs::path buildRoot = root / "out" / "build";
    std::error_code scanError;
    for (fs::recursive_directory_iterator it(buildRoot,
             fs::directory_options::skip_permission_denied, scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(scanError)
            && it->path().filename() == executableName) {
            return {it->path().lexically_normal().string(), root.string(), {},
                    "discovered build output"};
        }
    }
    if (error != nullptr) {
        *error = "No runnable " + executableName
            + " was found. Configure project.ayproject.json or "
              ".ayeditor/run.json.";
    }
    return {};
}

EditorProjectLaunchResult EditorProjectRunner::launch(
    const EditorProjectRunConfig& config)
{
    EditorProjectLaunchResult result;
    if (!config) {
        result.error = "Project run configuration is empty.";
        return result;
    }
#if defined(_WIN32)
    const std::wstring executable = fs::path(config.executable).wstring();
    std::wstring command = quote(executable);
    for (const std::string& argument : config.arguments) {
        command += L" ";
        command += quote(utf8(argument));
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring working = fs::path(config.workingDirectory).wstring();
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
            nullptr, FALSE, 0, nullptr,
            working.empty() ? nullptr : working.c_str(), &startup, &process)) {
        result.error = "Could not launch project (Windows error "
            + std::to_string(GetLastError()) + ").";
        return result;
    }
    result.processId = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    result.error = "Project launching is not implemented on this platform.";
#endif
    return result;
}

} // namespace ayt::editor
