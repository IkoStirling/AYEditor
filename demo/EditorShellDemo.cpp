// EditorShellDemo.cpp — E2-composite entry: EditorApp + single-window bgfx composite
//
// Default character: Sour.fbx owns render assets; SourWithAnim.fbx contributes
// only the first baked animation clip. Later launches reuse both sidecars.
// Override with `--import <model.fbx> --animation <clip.fbx>`,
// AY_EDITOR_FORCE_IMPORT=1, or
// AY_EDITOR_CHARACTER_SCALE=<float>.

#include "AYEditor/EditorApp.h"
#include "AYGameLoop.h"
#include "AYApplication/IEngineHost.h"      // defaultEngineHost() Meyers singleton (v0.3 PR-4)
#include "AYScene/SceneManager.h"   // SceneManager::canBeginPlay/isEditDirty (PR-4 日志块)

#include <AYConfig.h>
#include <AYIO/Env.h>
#include <AYLog.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <share.h>
#endif

namespace {

constexpr const char* kEditorConfigRelativePath = "assets/config/editor.json";
constexpr const char* kDefaultImportPathKey = "Editor.DefaultImportPath";
constexpr const char* kDefaultAnimationImportPathKey =
    "Editor.DefaultAnimationImportPath";
constexpr const char* kAutoPlayImportedAnimationKey =
    "Editor.AutoPlayImportedAnimation";
constexpr const char* kMaterialPolicyTagKey = "Editor.MaterialPolicy.Tag";
constexpr const char* kOpaqueMaterialsKey = "Editor.MaterialPolicy.Opaque";
constexpr const char* kMaskMaterialsKey = "Editor.MaterialPolicy.Mask";
constexpr const char* kBlendMaterialsKey = "Editor.MaterialPolicy.Blend";
constexpr const char* kDoubleSidedMaterialsKey = "Editor.MaterialPolicy.DoubleSided";
constexpr const char* kOpaqueMaterialNamesKey = "Editor.MaterialPolicy.OpaqueNames";
constexpr const char* kMaskMaterialNamesKey = "Editor.MaterialPolicy.MaskNames";
constexpr const char* kBlendMaterialNamesKey = "Editor.MaterialPolicy.BlendNames";
constexpr const char* kDoubleSidedMaterialNamesKey = "Editor.MaterialPolicy.DoubleSidedNames";
constexpr const char* kSourceCoordinateModeKey = "Editor.Import.SourceCoordinates.Mode";
constexpr const char* kSourceUpAxisKey = "Editor.Import.SourceCoordinates.Up";
constexpr const char* kSourceForwardAxisKey = "Editor.Import.SourceCoordinates.Forward";
constexpr const char* kSourceHandednessKey = "Editor.Import.SourceCoordinates.Handedness";
constexpr const char* kSourceUvOriginKey = "Editor.Import.SourceCoordinates.UVOrigin";
constexpr const char* kSourceMetersPerUnitKey = "Editor.Import.SourceCoordinates.MetersPerUnit";
constexpr const char* kSourceCoordinateTagKey = "Editor.Import.SourceCoordinates.Tag";
constexpr const char* kNormalMapYKey = "Editor.Import.Materials.NormalMapY";
constexpr const char* kEditorLogFileEnv = "AY_EDITOR_LOG_FILE";
constexpr const char* kEditorLogRelativePath = "logs/AYEditorShell_Demo.log";
FILE* g_editorLogStream = nullptr;
HANDLE g_logPipeRead = INVALID_HANDLE_VALUE;
HANDLE g_consoleOutput = INVALID_HANDLE_VALUE;
std::thread g_logPumpThread;

ayt::resource::ImportAxis parseImportAxis(const std::string& text,
                                          ayt::resource::ImportAxis fallback)
{
    if (text == "+X" || text == "X") return ayt::resource::ImportAxis::PositiveX;
    if (text == "-X") return ayt::resource::ImportAxis::NegativeX;
    if (text == "+Y" || text == "Y") return ayt::resource::ImportAxis::PositiveY;
    if (text == "-Y") return ayt::resource::ImportAxis::NegativeY;
    if (text == "+Z" || text == "Z") return ayt::resource::ImportAxis::PositiveZ;
    if (text == "-Z") return ayt::resource::ImportAxis::NegativeZ;
    return fallback;
}

ayt::resource::SourceCoordinatePolicy sourceCoordinatePolicy(
    const ayt::config::Config& config)
{
    ayt::resource::SourceCoordinatePolicy policy;
    const std::string mode = config.getString(kSourceCoordinateModeKey, "Auto");
    policy.mode = mode == "Manual" || mode == "manual"
        ? ayt::resource::SourceCoordinateMode::Manual
        : ayt::resource::SourceCoordinateMode::Auto;
    policy.up = parseImportAxis(config.getString(kSourceUpAxisKey, "+Y"),
                                ayt::resource::ImportAxis::PositiveY);
    policy.forward = parseImportAxis(
        config.getString(kSourceForwardAxisKey, "+Z"),
        ayt::resource::ImportAxis::PositiveZ);
    const std::string handedness =
        config.getString(kSourceHandednessKey, "Left");
    policy.handedness = handedness == "Right" || handedness == "right"
        ? ayt::resource::ImportHandedness::Right
        : ayt::resource::ImportHandedness::Left;
    const std::string uvOrigin =
        config.getString(kSourceUvOriginKey, "TopLeft");
    policy.uvOrigin = uvOrigin == "BottomLeft" || uvOrigin == "bottom-left"
        || uvOrigin == "bottomleft"
        ? ayt::resource::ImportUvOrigin::BottomLeft
        : ayt::resource::ImportUvOrigin::TopLeft;
    policy.metersPerUnit = static_cast<float>(
        config.getFloat(kSourceMetersPerUnitKey, 0.0));
    policy.tag = config.getString(kSourceCoordinateTagKey);
    return policy;
}

void pumpLogToFileAndConsole()
{
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(g_logPipeRead, buffer, sizeof(buffer), &bytesRead, nullptr)
           && bytesRead != 0) {
        if (g_editorLogStream != nullptr) {
            std::fwrite(buffer, 1, bytesRead, g_editorLogStream);
            std::fflush(g_editorLogStream);
        }
        if (g_consoleOutput != INVALID_HANDLE_VALUE) {
            DWORD bytesWritten = 0;
            WriteFile(g_consoleOutput, buffer, bytesRead, &bytesWritten, nullptr);
        }
    }
}

void shutdownPersistentLog()
{
    ayt::log::flush();
    std::fflush(stdout);
    std::fflush(stderr);

    // Closing both CRT writers lets the blocking pump drain the pipe and exit.
    std::fclose(stdout);
    std::fclose(stderr);
    if (g_logPumpThread.joinable()) {
        g_logPumpThread.join();
    }
    if (g_logPipeRead != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logPipeRead);
        g_logPipeRead = INVALID_HANDLE_VALUE;
    }
    if (g_consoleOutput != INVALID_HANDLE_VALUE) {
        CloseHandle(g_consoleOutput);
        g_consoleOutput = INVALID_HANDLE_VALUE;
    }
    if (g_editorLogStream != nullptr) {
        std::fclose(g_editorLogStream);
        g_editorLogStream = nullptr;
    }
}

std::filesystem::path moduleDirectory()
{
    char modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::string editorConfigPath()
{
    return (moduleDirectory() / kEditorConfigRelativePath).string();
}

std::string editorLogPath()
{
    const std::string overridePath =
        ayt::io::env::get(kEditorLogFileEnv).value_or("");
    if (!overridePath.empty()) {
        return std::filesystem::absolute(overridePath).string();
    }
    return (moduleDirectory() / kEditorLogRelativePath).string();
}

bool initializePersistentLog(const std::string& logFile)
{
    const std::filesystem::path logPath(logFile);
    std::error_code ec;
    if (!logPath.parent_path().empty()) {
        std::filesystem::create_directories(logPath.parent_path(), ec);
    }

    // A WIN32-subsystem process launched without a debugger may begin with
    // invalid CRT stdout/stderr FILE objects.  freopen_s on those objects can
    // trip the UCRT write.cpp handle assertion.  Bootstrap valid CRT streams
    // through a console first and only then redirect both streams to the
    // persistent file. Keep a console created by us visible: the shell demo is
    // a diagnostic executable, and persistent logging must not change its
    // existing interactive diagnostics behavior.
    if (GetConsoleWindow() == nullptr) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            AllocConsole();
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    FILE* stream = nullptr;
    const bool stdoutReady =
        freopen_s(&stream, "CONOUT$", "w", stdout) == 0;
    const bool stderrReady =
        freopen_s(&stream, "CONOUT$", "w", stderr) == 0;
    if (!stdoutReady || !stderrReady) {
        return false;
    }

    // Preserve a console handle before stdout/stderr are routed through the
    // tee pipe. The pump is the only log-file writer and mirrors every byte to
    // this console handle.
    g_consoleOutput = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    g_editorLogStream = _fsopen(logFile.c_str(), "a", _SH_DENYWR);
    HANDLE pipeWrite = INVALID_HANDLE_VALUE;
    const bool pipeReady =
        CreatePipe(&g_logPipeRead, &pipeWrite, nullptr, 0) != FALSE;
    if (pipeReady) {
        SetHandleInformation(g_logPipeRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(pipeWrite, HANDLE_FLAG_INHERIT, 0);
    }

    const int pipeFd = pipeReady
        ? _open_osfhandle(reinterpret_cast<std::intptr_t>(pipeWrite),
                         _O_WRONLY | _O_BINARY)
        : -1;
    const int stdoutFd = _fileno(stdout);
    const int stderrFd = _fileno(stderr);
    const bool streamsCaptured = g_editorLogStream != nullptr && pipeFd >= 0
        && stdoutFd >= 0 && stderrFd >= 0
        && _dup2(pipeFd, stdoutFd) == 0
        && _dup2(pipeFd, stderrFd) == 0;
    if (pipeFd >= 0) {
        _close(pipeFd);
    }
    if (!streamsCaptured) {
        if (g_logPipeRead != INVALID_HANDLE_VALUE) {
            CloseHandle(g_logPipeRead);
            g_logPipeRead = INVALID_HANDLE_VALUE;
        }
        if (g_consoleOutput != INVALID_HANDLE_VALUE) {
            CloseHandle(g_consoleOutput);
            g_consoleOutput = INVALID_HANDLE_VALUE;
        }
        if (g_editorLogStream != nullptr) {
            fclose(g_editorLogStream);
            g_editorLogStream = nullptr;
        }
        return false;
    }

    // Diagnostics must be visible while the process is still running.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    g_logPumpThread = std::thread(pumpLogToFileAndConsole);

    ayt::log::LogConfig logConfig;
    logConfig.consoleEnabled = true;
    logConfig.consoleLevel = ayt::log::LogLevel::Trace;
    logConfig.consoleColorEnabled = false;
    logConfig.fileEnabled = false;
    logConfig.flush.policy = ayt::log::FlushPolicy::OnError;
    ayt::log::initialize(logConfig);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(stderr,
                 "\n========== AYEditorShell_Demo session "
                 "%04u-%02u-%02u %02u:%02u:%02u pid=%lu ==========\n"
                 "[EditorShellDemo] persistent log: %s\n",
                 now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond,
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 logFile.c_str());
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::string logFile = editorLogPath();
    if (!initializePersistentLog(logFile)) {
        // Last-resort interactive diagnostics if file setup is unavailable.
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        std::fprintf(stderr,
                     "[EditorShellDemo] persistent log initialization failed: %s\n",
                     logFile.c_str());
    }

    ayt::app::GameDesc desc{};
    desc.name = "AY Editor (E2-composite)";
    desc.width = 1280;
    desc.height = 720;
    desc.enableRenderThread = false;

    const std::string configPath = editorConfigPath();
    ayt::config::Config editorConfig;
    const bool configLoaded = editorConfig.loadFromFile(configPath);
    const std::string defaultImportPath =
        editorConfig.getString(kDefaultImportPathKey);
    const std::string defaultAnimationImportPath =
        editorConfig.getString(kDefaultAnimationImportPathKey);

    auto app = ayt::editor::EditorApp::create(desc);
    app->setDefaultImportPath(defaultImportPath);
    app->setDefaultAnimationImportPath(defaultAnimationImportPath);
    const bool autoPlayImportedAnimation =
        editorConfig.getBool(kAutoPlayImportedAnimationKey, false);
    app->setAutoPlayImportedAnimation(autoPlayImportedAnimation);
    const ayt::resource::SourceCoordinatePolicy coordinates =
        sourceCoordinatePolicy(editorConfig);
    app->setDefaultSourceCoordinates(coordinates);
    const std::string normalMapY = editorConfig.getString(kNormalMapYKey, "+Y");
    const float normalMapYSign =
        normalMapY == "-Y" || normalMapY == "DirectX" || normalMapY == "directx"
            ? -1.0f : 1.0f;
    app->setDefaultNormalMapYSign(normalMapYSign);
    app->setDefaultMaterialPolicy(
        editorConfig.getString(kMaterialPolicyTagKey),
        editorConfig.getString(kOpaqueMaterialsKey),
        editorConfig.getString(kMaskMaterialsKey),
        editorConfig.getString(kBlendMaterialsKey),
        editorConfig.getString(kDoubleSidedMaterialsKey),
        editorConfig.getString(kOpaqueMaterialNamesKey),
        editorConfig.getString(kMaskMaterialNamesKey),
        editorConfig.getString(kBlendMaterialNamesKey),
        editorConfig.getString(kDoubleSidedMaterialNamesKey));
    std::fprintf(stderr,
                 "[EditorShellDemo] config: %s (%s)\n"
                 "[EditorShellDemo] default import: %s\n"
                 "[EditorShellDemo] default animation: %s\n"
                 "[EditorShellDemo] auto-play imported animation: %s\n"
                 "[EditorShellDemo] source coordinates: %s tag='%s'\n"
                 "[EditorShellDemo] source UV origin: %s -> engine TopLeft\n"
                 "[EditorShellDemo] normal map convention: %s\n"
                 "[EditorShellDemo] note: first imported animation clip "
                 "auto-plays; model-only FBX stays in bind pose\n"
                 "[EditorShellDemo] net client: AYEditorShell_Demo.exe --net-client "
                 "[--net-host 127.0.0.1] (start server Play first)\n",
                 configPath.c_str(), configLoaded ? "loaded" : "not found",
                 defaultImportPath.empty() ? "(none; cube fallback)"
                                           : defaultImportPath.c_str(),
                 defaultAnimationImportPath.empty() ? "(none; bind pose)"
                                                    : defaultAnimationImportPath.c_str(),
                 autoPlayImportedAnimation ? "enabled" : "disabled",
                 coordinates.mode == ayt::resource::SourceCoordinateMode::Manual
                     ? "manual" : "auto",
                 ayt::resource::sourceCoordinatePolicyCacheTag(coordinates).c_str(),
                 coordinates.uvOrigin == ayt::resource::ImportUvOrigin::BottomLeft
                     ? "BottomLeft" : "TopLeft",
                 normalMapYSign < 0.0f ? "DirectX (-Y)" : "OpenGL/Blender (+Y)");

    // v0.3 PR-4 — 启动日志验证 host->scenes() wiring 通（design §4.2.x）
    // 不影响 demo 行为；仅 stderr 状态打印，便于 v0.3 验收 + 后续 PR debug。
    // v0.3 PR-4 API 变化：defaultEngineHost() 是 Meyers 单例（永不为
    // null），返回 IEngineHost& 而非指针。
    ayt::app::IEngineHost& host = ayt::app::defaultEngineHost();
    if (auto* sm = host.scenes()) {
        std::fprintf(stderr,
                     "[EditorShellDemo] PR-4 host->scenes() wiring: OK\n"
                     "[EditorShellDemo]   canBeginPlay=%s isEditDirty=%s\n",
                     sm->canBeginPlay() ? "true" : "false",
                     sm->isEditDirty()  ? "true" : "false");
    } else {
        std::fprintf(stderr,
                     "[EditorShellDemo] PR-4 host->scenes() missing — "
                     "bindBuiltinHostServices not called?\n");
    }

    app->run();
    shutdownPersistentLog();
    return 0;
}
