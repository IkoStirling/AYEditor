#include "AYEditor/EditorProductPaths.h"

#include <AYIO/Env.h>

#include <system_error>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

#ifndef AY_ENGINE_ASSETS_SOURCE_HINT
#  define AY_ENGINE_ASSETS_SOURCE_HINT ""
#endif

namespace ayt::editor {
namespace {

std::filesystem::path normalizedAbsolute(std::filesystem::path path)
{
    if (path.empty()) {
        return {};
    }
    std::error_code error;
    if (path.is_relative()) {
        path = std::filesystem::absolute(path, error);
        if (error) {
            return path.lexically_normal();
        }
    }
    return path.lexically_normal();
}

std::filesystem::path pathFromEnvironment(const char* name)
{
    const auto value = ayt::io::env::get(name);
    return value.has_value() && !value->empty()
        ? normalizedAbsolute(std::filesystem::path(*value))
        : std::filesystem::path{};
}

std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return normalizedAbsolute(
            std::filesystem::path(buffer).parent_path());
    }
#endif
    std::error_code error;
    return normalizedAbsolute(std::filesystem::current_path(error));
}

bool hasEditorAssets(const std::filesystem::path& root)
{
    if (root.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(
        root / "AYEditor" / "ui" / "editor_shell.ui.json", error);
}

} // namespace

EditorProductPaths EditorProductPaths::detect()
{
    EditorProductPaths paths;
    const std::filesystem::path moduleDirectory = executableDirectory();

    paths.productRoot = pathFromEnvironment("AY_EDITOR_PRODUCT_ROOT");
    if (paths.productRoot.empty()) {
        if (hasEditorAssets(moduleDirectory / "EngineAssets")) {
            paths.productRoot = moduleDirectory;
        } else if (hasEditorAssets(
                       moduleDirectory.parent_path() / "EngineAssets")) {
            // Installed host executable lives in <product>/AYRuntime.
            paths.productRoot = moduleDirectory.parent_path();
        } else {
            paths.productRoot = moduleDirectory;
        }
    }

    paths.engineAssetsRoot =
        pathFromEnvironment("AY_EDITOR_ENGINE_ASSETS_ROOT");
    if (paths.engineAssetsRoot.empty()) {
        const std::filesystem::path installed =
            paths.productRoot / "EngineAssets";
        if (hasEditorAssets(installed)) {
            paths.engineAssetsRoot = normalizedAbsolute(installed);
        }
    }
    if (paths.engineAssetsRoot.empty()) {
        const std::filesystem::path sourceHint(
            AY_ENGINE_ASSETS_SOURCE_HINT);
        if (hasEditorAssets(sourceHint)) {
            paths.engineAssetsRoot = normalizedAbsolute(sourceHint);
        }
    }

    paths.userWorkspaceRoot =
        pathFromEnvironment("AY_EDITOR_USER_WORKSPACE_ROOT");
    if (paths.userWorkspaceRoot.empty()) {
        paths.userWorkspaceRoot =
            normalizedAbsolute(paths.productRoot / "UserAssets");
    }
    paths.runtimeRoot = normalizedAbsolute(paths.productRoot / "AYRuntime");
    paths.logsRoot = normalizedAbsolute(paths.productRoot / "logs");

    const std::filesystem::path installedAssets =
        normalizedAbsolute(paths.productRoot / "EngineAssets");
    std::error_code error;
    paths.portableLayout = hasEditorAssets(installedAssets)
        && std::filesystem::equivalent(
            installedAssets, paths.engineAssetsRoot, error);
    if (error) {
        paths.portableLayout =
            installedAssets.lexically_normal()
            == paths.engineAssetsRoot.lexically_normal();
    }
    return paths;
}

std::filesystem::path EditorProductPaths::engineAsset(
    const std::filesystem::path& relative) const
{
    return (engineAssetsRoot / relative).lexically_normal();
}

std::filesystem::path EditorProductPaths::editorAsset(
    const std::filesystem::path& relative) const
{
    return engineAsset(std::filesystem::path("AYEditor") / relative);
}

} // namespace ayt::editor
