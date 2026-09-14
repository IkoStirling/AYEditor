#include "AYEditor/EditorApp.h"

#if defined(_DEBUG) && defined(_MSC_VER)
#include "AYEditor/EditorHeapDebug.h"
#endif
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorSession.h"
#include "AYEditor/EditorStartupSplash.h"
#include "AYEditor/RegisterDefaultEditorModules.h"
#include "resources/AYEditorResourceIds.h"
#include "AYGameLoop.h"
#include "AYDevice/DeviceManager.h"
#include "AYDevice/DeviceInputProvider.h"
#include "AYEditor/ImportedCharacterMapper.h"
#include "AYEditor/Importer.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYScript/ScriptSubSystem.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYUI/DeviceInputBridge.h"
#include "AYUI/UIKeyCode.h"
#include "AYEntity.h"
#include <AYEntity/components/MeshComponent.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/EngineRuntimeScope.h>
#include <AYEntity/ComponentRegistry.h>

#include <AYEventSystem/EventBus.h>
#include <AYIO/Env.h>
#include <AYProject/Project.h>
#include <AYPlatform/Console.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace ayt::editor {

namespace {

constexpr int kEditorChromeHeight = 24;
constexpr int kEditorChromeDragLeft = 360;
constexpr int kEditorChromeButtonsWidth = 92;
constexpr int kEditorResizeBorder = 6;

void installEditorWindowIcon(HWND hwnd)
{
    if (hwnd == nullptr) {
        return;
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return;
    }

    const auto loadIcon = [instance](int width, int height) -> HICON {
        return static_cast<HICON>(::LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_AYEDITOR_APP),
            IMAGE_ICON,
            width,
            height,
            LR_DEFAULTCOLOR | LR_SHARED));
    };

    if (HICON largeIcon = loadIcon(
            ::GetSystemMetrics(SM_CXICON),
            ::GetSystemMetrics(SM_CYICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(largeIcon));
    }
    if (HICON smallIcon = loadIcon(
            ::GetSystemMetrics(SM_CXSMICON),
            ::GetSystemMetrics(SM_CYSMICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(smallIcon));
    }
}

void renderEditorWarmupFrame(EditorSession& session,
                             ayt::render::RendererSubSystem& rendererSub,
                             ayt::render::UIRenderBackend& uiBackend)
{
    rendererSub.renderCompositeFrame(
        session.shouldCompositeViewport(), &uiBackend,
        [&session](bool skipViewportPanel,
                   ayt::render::CompositeUiPhase phase) {
            if (phase == ayt::render::CompositeUiPhase::Populate) {
                session.populateFrame(skipViewportPanel);
            } else {
                session.flushFrame();
            }
        });
}

std::intptr_t handleEditorBorderlessMessage(HWND hwnd, unsigned msg,
                                             std::uintptr_t wParam,
                                             std::intptr_t lParam,
                                             bool& handled)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        // The whole HWND is editor client area in every window state. Do not
        // put rcWork here: a maximized HWND still spans rcMonitor, so shrinking
        // only its client rectangle leaves the taskbar-height remainder as an
        // unpainted non-client strip. WM_GETMINMAXINFO below constrains the
        // HWND itself to the monitor work area instead.
        handled = true;
        return 0;

    case WM_NCHITTEST: {
        POINT screenPoint{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
        RECT windowRect{};
        ::GetWindowRect(hwnd, &windowRect);
        if (::IsZoomed(hwnd) == FALSE) {
            const bool left = screenPoint.x < windowRect.left + kEditorResizeBorder;
            const bool right = screenPoint.x >= windowRect.right - kEditorResizeBorder;
            const bool top = screenPoint.y < windowRect.top + kEditorResizeBorder;
            const bool bottom = screenPoint.y >= windowRect.bottom - kEditorResizeBorder;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }

        POINT clientPoint = screenPoint;
        ::ScreenToClient(hwnd, &clientPoint);
        RECT clientRect{};
        ::GetClientRect(hwnd, &clientRect);
        const bool inDragRegion = clientPoint.y >= 0
            && clientPoint.y < kEditorChromeHeight
            && clientPoint.x >= kEditorChromeDragLeft
            && clientPoint.x < clientRect.right - kEditorChromeButtonsWidth;
        handled = true;
        return inDragRegion ? HTCAPTION : HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits != nullptr) {
            limits->ptMinTrackSize.x = 960;
            limits->ptMinTrackSize.y = 600;

            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(monitorInfo);
            const HMONITOR monitor = ::MonitorFromWindow(
                hwnd, MONITOR_DEFAULTTONEAREST);
            if (::GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
                const RECT& monitorRect = monitorInfo.rcMonitor;
                const RECT& workRect = monitorInfo.rcWork;
                limits->ptMaxPosition.x = workRect.left - monitorRect.left;
                limits->ptMaxPosition.y = workRect.top - monitorRect.top;
                limits->ptMaxSize.x = workRect.right - workRect.left;
                limits->ptMaxSize.y = workRect.bottom - workRect.top;
            }
        }
        handled = true;
        return 0;
    }

    case WM_ERASEBKGND: {
        // The AYDevice main-window class has a COLOR_WINDOW brush for generic
        // applications. Before the first GPU present that would expose a white
        // client area. Paint the editor's bootstrap colour instead; after the
        // renderer starts this remains a harmless resize/failure fallback.
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT client{};
        if (dc != nullptr && ::GetClientRect(hwnd, &client) != FALSE) {
            ::SetDCBrushColor(dc, RGB(0x14, 0x16, 0x1B));
            ::FillRect(dc, &client,
                       reinterpret_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
        }
        handled = true;
        return TRUE;
    }

    default:
        return 0;
    }
}

void applyEditorDwmFrame(HWND hwnd)
{
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr DWORD kDwmwcpDoNotRound = 1;
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr DWORD kDwmwaCaptionColor = 35;
    const BOOL darkMode = TRUE;
    const DWORD corner = kDwmwcpDoNotRound;
    const COLORREF frameColor = RGB(0x18, 0x19, 0x1C);
    using DwmSetWindowAttributeFn =
        HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll")) {
        if (auto* setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
                ::GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            setAttribute(hwnd, kDwmwaUseImmersiveDarkMode,
                         &darkMode, sizeof(darkMode));
            setAttribute(hwnd, kDwmwaWindowCornerPreference,
                         &corner, sizeof(corner));
            setAttribute(hwnd, kDwmwaBorderColor,
                         &frameColor, sizeof(frameColor));
            setAttribute(hwnd, kDwmwaCaptionColor,
                         &frameColor, sizeof(frameColor));
        }
        ::FreeLibrary(dwm);
    }
}

bool installEditorBorderlessChrome(ayt::device::WindowManager& window,
                                   HWND hwnd, int width, int height)
{
    if (hwnd == nullptr) return false;
    window.setWindowMessageCallback(
        [hwnd](unsigned msg, std::uintptr_t wParam, std::intptr_t lParam,
               bool& handled) {
            return handleEditorBorderlessMessage(
                hwnd, msg, wParam, lParam, handled);
        });

    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style == 0 && ::GetLastError() != ERROR_SUCCESS) return false;
    style &= ~static_cast<LONG_PTR>(WS_CAPTION);
    style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    ::SetLastError(ERROR_SUCCESS);
    if (::SetWindowLongPtrW(hwnd, GWL_STYLE, style) == 0
        && ::GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    applyEditorDwmFrame(hwnd);
    return ::SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
                              | SWP_FRAMECHANGED) != FALSE;
}

void attachDebugConsole()
{
    if (AllocConsole() == 0) {
        return;
    }
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    ayt::platform::ensureConsoleUtf8();
    std::fprintf(stdout, "[EditorApp] debug console attached\n");
}

std::string resolveLayoutPath(const EditorProductPaths& productPaths)
{
    return productPaths.editorAsset("ui/editor_shell.ui.json").string();
}

bool isEditorIconRoot(const std::filesystem::path& root)
{
    std::error_code error;
    const bool hasOutline = std::filesystem::is_regular_file(
        root / "outline/x.svg", error);
    error.clear();
    const bool hasFilled = std::filesystem::is_regular_file(
        root / "filled/player-play.svg", error);
    return hasOutline && hasFilled;
}

std::string resolveEditorIconRoot(const EditorProductPaths& productPaths)
{
    std::vector<std::filesystem::path> candidates;
    if (const auto configured = ayt::io::env::get("AY_EDITOR_ICON_ROOT");
        configured.has_value() && !configured->empty()) {
        const std::filesystem::path root(*configured);
        candidates.push_back(root);
        candidates.push_back(root / "icons");
        candidates.push_back(root / "tabler-icons-3.46.0/icons");
    }
    candidates.push_back(
        productPaths.engineAsset("Icons/Tabler"));

    for (const std::filesystem::path& candidate : candidates) {
        if (isEditorIconRoot(candidate)) {
            return candidate.lexically_normal().string();
        }
    }
    return {};
}

// Split the Win32 command line while preserving quoted paths. This remains a
// deliberately small parser (Windows already supplies the complete command
// line here), but project/import paths containing spaces are now first-class.
std::vector<std::string> tokenizeCommandLine(const char* cmdLine)
{
    std::vector<std::string> out;
    if (cmdLine == nullptr) {
        return out;
    }
    std::string token;
    auto flush = [&]() {
        if (!token.empty()) {
            out.push_back(token);
            token.clear();
        }
    };
    bool quoted = false;
    for (const char* p = cmdLine; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && std::isspace(c) != 0) {
            flush();
        } else {
            token.push_back(static_cast<char>(c));
        }
    }
    flush();
    return out;
}

// G2: scan tokens for the literal "--import" flag and return the
// following token (if any). Returns an empty string when the flag is
// absent or followed by nothing. Does not handle "--import=path" form
// in Phase 1; that variant passes through and the unknownArgs path
// would carry it (we don't read AppCommandLine here, so it's just
// ignored).
std::string findImportPath(const std::vector<std::string>& tokens)
{
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "--import") {
            if (i + 1 < tokens.size()) {
                return tokens[i + 1];
            }
            return std::string{};
        }
    }
    return std::string{};
}

std::string findAnimationImportPath(const std::vector<std::string>& tokens)
{
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "--animation") {
            return i + 1 < tokens.size() ? tokens[i + 1] : std::string{};
        }
    }
    return std::string{};
}

std::string findProjectRoot(const std::vector<std::string>& tokens)
{
    constexpr const char* prefix = "--project=";
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "--project") {
            return i + 1 < tokens.size() ? tokens[i + 1] : std::string{};
        }
        if (tokens[i].compare(0, std::strlen(prefix), prefix) == 0) {
            return tokens[i].substr(std::strlen(prefix));
        }
    }
    return {};
}

std::string detectEditorProjectRoot()
{
    std::vector<std::filesystem::path> starts;
    std::error_code ec;
    starts.push_back(std::filesystem::current_path(ec));
    char modulePath[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        starts.push_back(std::filesystem::path(modulePath).parent_path());
    }
    for (std::filesystem::path cursor : starts) {
        for (int depth = 0; depth < 10 && !cursor.empty(); ++depth) {
            if (std::filesystem::exists(cursor / "AYRuntime" / "AYEditor", ec)
                && std::filesystem::exists(cursor / "CMakeLists.txt", ec)) {
                return cursor.lexically_normal().string();
            }
            const std::filesystem::path parent = cursor.parent_path();
            if (parent == cursor) break;
            cursor = parent;
        }
    }
    return std::filesystem::current_path(ec).string();
}

bool hasNetClientFlag(const std::vector<std::string>& tokens)
{
    for (const std::string& token : tokens) {
        if (token == "--net-client") {
            return true;
        }
    }
    return false;
}

std::string findNetConnectHost(const std::vector<std::string>& tokens)
{
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == "--net-host") {
            return tokens[i + 1];
        }
    }
    return "127.0.0.1";
}

} // namespace

class EditorAppRuntime final {
public:
    std::unique_ptr<ayt::app::EngineRuntimeScope> services;
    std::unique_ptr<ayt::app::EngineModuleRuntime> modules;
};

static_assert(sizeof(std::unique_ptr<EditorAppRuntime>) ==
              sizeof(std::unique_ptr<ayt::app::EngineModuleRuntime>));
static_assert(alignof(std::unique_ptr<EditorAppRuntime>) ==
              alignof(std::unique_ptr<ayt::app::EngineModuleRuntime>));

EditorApp::EditorApp(const ayt::app::GameDesc& desc) : _desc(desc) {}

EditorApp::EditorApp(const ayt::app::GameDesc& desc, const ayt::app::AppCommandLine& cmdLine)
    : _desc(desc), _cmdLine(cmdLine)
{
    if (_cmdLine.width > 0) {
        _desc.width = _cmdLine.width;
    }
    if (_cmdLine.height > 0) {
        _desc.height = _cmdLine.height;
    }
    if (_cmdLine.fps > 0.0f) {
        _desc.targetFPS = _cmdLine.fps;
    }
}

EditorApp::~EditorApp()
{
    if (_runtime && _runtime->modules) {
        _runtime->modules->shutdown();
        _runtime->modules.reset();
    }
    _runtime.reset();
    // INT-02 (2026-07-15): reset provider BEFORE devices. ScriptSubSystem is
    // normally withdrawn by the module runtime above; the explicit ordering
    // also protects compatibility paths where GameLoop still owns it. If
    // _inputProvider outlived _devices, the bridge would hold a dangling
    // DeviceManager*.
    // ScriptSubSystem::shutdown() also defensively calls
    // setInputProvider(nullptr), but explicit ordering here keeps
    // the invariant local to the type that owns the pointer.
    _inputProvider.reset();
    _devices.reset();
}

std::unique_ptr<EditorApp> EditorApp::create(const ayt::app::GameDesc& desc)
{
    return std::make_unique<EditorApp>(desc);
}

std::unique_ptr<EditorApp> EditorApp::create(const ayt::app::GameDesc& desc,
                                             const ayt::app::AppCommandLine& cmdLine)
{
    return std::make_unique<EditorApp>(desc, cmdLine);
}

ayt::game::GameLoop& EditorApp::getGameLoop()
{
    return ayt::game::GameLoop::instance();
}

ayt::event::EventBus& EditorApp::eventBus()
{
    return ayt::event::EventBus::instance();
}

void EditorApp::registerSubSystems()
{
    if (!_runtime || !_runtime->modules) {
        // Engine-host: shared Editor assembly + service table
        // (AYApplication/docs/engine-host.md).
        EditorModuleOptions opts{};
        opts.enableAudio = !_cmdLine.noAudio;

        auto state = std::make_unique<EditorAppRuntime>();
        state->services = std::make_unique<ayt::app::EngineRuntimeScope>(
            engineHost());
        auto runtime = std::make_unique<ayt::app::EngineModuleRuntime>(
            engineHost());
        auto require = [](const ayt::module::ModuleResult& result,
                          const char* phase) {
            if (!result) {
                throw ayt::app::AppException(
                    ayt::app::AppException::Code::SubSystemInitFailed,
                    std::string("Editor module ") + phase + " failed: " +
                        result.message());
            }
        };

        require(configureDefaultEditorModules(*runtime, opts),
                "configuration");
        require(runtime->prepare(), "type registration");
        runtime->context().componentRegistry().seal();
        require(runtime->install(), "installation");
        state->modules = std::move(runtime);
        state->services->refresh();
        _runtime = std::move(state);
    }

    // INT-02: Script ← Editor-owned DeviceManager (not DeviceSubSystem).
    if (_devices && !_inputProvider) {
        auto* sub = engineHost().findSubSystem("ayt.script.runtime");
        if (auto* scriptSub = dynamic_cast<ayt::script::ScriptSubSystem*>(sub)) {
            _inputProvider = std::make_unique<ayt::device::DeviceInputProvider>(
                _devices.get());
            scriptSub->bridge().setInputProvider(_inputProvider.get());
        }
    }
}

void EditorApp::onInit()
{
    registerSubSystems();
}

void EditorApp::onPreShutdown() {}

void EditorApp::onShutdown() {}

void EditorApp::run()
{
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());

    EditorStartupSplash startupSplash;
    (void)startupSplash.show();
    startupSplash.update(0.02f, L"Starting AY Editor...");

    AY_EDITOR_HEAP_DEBUG_INIT();
    AY_EDITOR_HEAP_CHECK("startup");
    attachDebugConsole();
    std::fprintf(stderr,
                 "[EditorApp] product root: %s\n"
                 "[EditorApp] engine assets: %s (%s)\n"
                 "[EditorApp] user workspace: %s\n",
                 _productPaths.productRoot.string().c_str(),
                 _productPaths.engineAssetsRoot.string().c_str(),
                 _productPaths.portableLayout ? "installed" : "development",
                 _productPaths.userWorkspaceRoot.string().c_str());

    // Import before registering/initializing runtime and rendering systems.
    // Asset conversion is a pure offline operation and must not share its
    // STL allocations with live renderer/device/background services.  This
    // also gives the debug heap a precise boundary for import corruption.
    ImportedCharacter importedCharacter;
    const std::vector<std::string> cmdTokens =
        tokenizeCommandLine(::GetCommandLineA());
    const bool netClientMode = hasNetClientFlag(cmdTokens);
    const std::string netConnectHost = findNetConnectHost(cmdTokens);
    std::string projectRoot = findProjectRoot(cmdTokens);
    startupSplash.update(0.06f, L"Resolving project workspace...");
    if (projectRoot.empty()) projectRoot = _projectRoot;
    if (projectRoot.empty()) {
        projectRoot = ayt::io::env::get("AY_EDITOR_PROJECT_ROOT").value_or("");
    }
    if (projectRoot.empty() && _productPaths.portableLayout) {
        projectRoot = _productPaths.userWorkspaceRoot.string();
    }
    if (projectRoot.empty()) projectRoot = detectEditorProjectRoot();
    ayt::project::Project::instance().setRoot(projectRoot);
    projectRoot = ayt::project::Project::instance().root();
    std::fprintf(stderr, "[EditorApp] project root: %s\n", projectRoot.c_str());
    if (netClientMode) {
        std::fprintf(stderr,
            "[EditorApp] net client mode (connect to %s)\n",
            netConnectHost.c_str());
    }
    {
        std::string importPath = findImportPath(cmdTokens);
        if (!netClientMode && importPath.empty() && !_defaultImportPath.empty()) {
            importPath = _defaultImportPath;
            std::fprintf(stderr,
                         "[EditorApp] no --import; using default: %s\n",
                         importPath.c_str());
        }
        if (!importPath.empty()) {
            startupSplash.update(0.10f, L"Importing startup character...");
            const std::string cacheRoot =
                EditorPlayRuntime::resolvePersistentCacheRoot();
            const std::string assetRoot = cacheRoot + "assets\\";

            Importer::MaterialPolicy materialPolicy;
            materialPolicy.tag = _materialPolicyTag;
            materialPolicy.opaqueIndices = _opaqueMaterialIndices;
            materialPolicy.maskIndices = _maskMaterialIndices;
            materialPolicy.blendIndices = _blendMaterialIndices;
            materialPolicy.doubleSidedIndices = _doubleSidedMaterialIndices;
            materialPolicy.opaqueNames = _opaqueMaterialNames;
            materialPolicy.maskNames = _maskMaterialNames;
            materialPolicy.blendNames = _blendMaterialNames;
            materialPolicy.doubleSidedNames = _doubleSidedMaterialNames;
            materialPolicy.normalMapYSign = _normalMapYSign;
            Importer::Result result =
                Importer::importFile(importPath, assetRoot, materialPolicy,
                                     _sourceCoordinates);
            if (!result.success) {
                std::fprintf(stderr,
                             "[EditorApp] import failed: %s (falling back to cube)\n",
                             result.errorMessage.c_str());
            } else {
                if (result.usedCache) {
                    std::fprintf(stderr,
                                 "[EditorApp] import cache hit (no FBX convert)\n");
                }
                ImportedCharacterMapDiagnostics diag;
                importedCharacter = mapConversionToImportedCharacter(
                    result.conversion, cacheRoot, diag);
                if (!diag.success) {
                    std::string missing;
                    for (size_t i = 0; i < diag.missing.size(); ++i) {
                        if (i > 0) missing += ", ";
                        missing += diag.missing[i];
                    }
                    std::fprintf(stderr,
                                 "[EditorApp] import produced no skinned character: "
                                 "missing [%s] (falling back to cube)\n",
                                 missing.c_str());
                    importedCharacter = ImportedCharacter{};
                } else {
                    if (!diag.missing.empty()) {
                        std::string optional;
                        for (size_t i = 0; i < diag.missing.size(); ++i) {
                            if (i > 0) optional += ", ";
                            optional += diag.missing[i];
                        }
                        std::fprintf(stderr,
                                     "[EditorApp] import optional missing [%s] "
                                     "(bind-pose / default lit OK)\n",
                                     optional.c_str());
                    }
                    std::fprintf(stderr,
                                 "[EditorApp] imported character ready "
                                 "(mesh=%s, extraMeshes=%zu, skel=%s, anim=%s)\n",
                                 importedCharacter.meshPath.c_str(),
                                 importedCharacter.additionalMeshPaths.size(),
                                 importedCharacter.skeletonPath.c_str(),
                                 importedCharacter.animationPath.empty()
                                     ? "(none/bind-pose)"
                                     : importedCharacter.animationPath.c_str());
                }
            }
        }
    }
    startupSplash.update(0.24f, L"Preparing runtime modules...");

    // A model source owns render assets and the target skeleton. A separate
    // animation source contributes only .ayanm clips, preventing re-exported
    // animation FBX files from replacing valid model textures/materials.
    if (!netClientMode && importedCharacter.isValid()) {
        std::string animationImportPath = findAnimationImportPath(cmdTokens);
        if (animationImportPath.empty()) {
            animationImportPath = _defaultAnimationImportPath;
        }
        if (!animationImportPath.empty()) {
            startupSplash.update(0.25f, L"Importing startup animation...");
            const std::string cacheRoot =
                EditorPlayRuntime::resolvePersistentCacheRoot();
            const std::string assetRoot = cacheRoot + "assets\\";
            std::fprintf(stderr,
                         "[EditorApp] importing animation source: %s\n",
                         animationImportPath.c_str());
            const Importer::Result animationResult =
                Importer::importAnimationFile(animationImportPath, assetRoot,
                                              _sourceCoordinates);
            if (!animationResult.success) {
                std::fprintf(stderr,
                             "[EditorApp] animation import failed: %s "
                             "(character remains in bind pose)\n",
                             animationResult.errorMessage.c_str());
                importedCharacter.animationPath.clear();
            } else {
                const std::string clipPath = mapFirstAnimationPath(
                    animationResult.conversion, cacheRoot);
                if (clipPath.empty()) {
                    std::fprintf(stderr,
                                 "[EditorApp] animation source produced no clip "
                                 "(character remains in bind pose)\n");
                    importedCharacter.animationPath.clear();
                } else {
                    importedCharacter.animationPath = clipPath;
                    std::fprintf(stderr,
                                 "[EditorApp] external animation ready: %s%s\n",
                                 clipPath.c_str(),
                                 animationResult.usedCache ? " (cache)" : "");
                }
            }
        }
    }
    AY_EDITOR_HEAP_CHECK("after_import_before_runtime_init");

    startupSplash.update(0.32f, L"Registering engine modules...");
    onInit();
    // INT-02 (2026-07-15): _devices is a member (was stack-local
    // before). Lifetime == *this so the Logia InputProvider that
    // registerSubSystems() installs can hold a raw pointer into it.
    _devices = std::make_unique<ayt::device::DeviceManager>();
    ayt::device::DeviceConfig deviceConfig{};
    deviceConfig.window.title = _desc.name != nullptr ? _desc.name : "AY Editor";
    deviceConfig.window.width = static_cast<int>(_desc.width);
    deviceConfig.window.height = static_cast<int>(_desc.height);
    // Configure the native surface before its first visible frame. The editor
    // installs self-painted chrome immediately after creation, avoiding a
    // one-frame flash of the Win32 caption.
    deviceConfig.window.hidden = true;
    deviceConfig.enableTouch = true;
    startupSplash.update(0.40f, L"Creating editor window...");
    if (!_devices->initialize(deviceConfig)) {
        std::fprintf(stderr, "[EditorApp] DeviceManager initialize failed\n");
        _devices.reset();
        return;
    }
    // Provider needs to be installed now that _devices is valid;
    // onInit() ran before _devices existed so we wire here too.
    {
        auto* sub = engineHost().findSubSystem("ayt.script.runtime");
        if (auto* scriptSub = dynamic_cast<ayt::script::ScriptSubSystem*>(sub)) {
            if (!_inputProvider) {
                _inputProvider = std::make_unique<ayt::device::DeviceInputProvider>(
                    _devices.get());
                scriptSub->bridge().setInputProvider(_inputProvider.get());
            }
        }
    }
    ayt::device::WindowManager& window = _devices->window();
    HWND hwnd = static_cast<HWND>(window.getWindowHandle());
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[EditorApp] host window unavailable\n");
        _devices->shutdown();
        _devices.reset();
        return;
    }

    installEditorWindowIcon(hwnd);

    if (!installEditorBorderlessChrome(
            window, hwnd, static_cast<int>(_desc.width),
            static_cast<int>(_desc.height))) {
        std::fprintf(stderr,
                     "[EditorApp] failed to install borderless editor chrome\n");
    }
    int clientWidth = window.getWidth();
    int clientHeight = window.getHeight();

    auto uiBackend = std::make_unique<ayt::render::UIRenderBackend>();
    ayt::render::RendererSubSystem* rendererSub = nullptr;

    {
        EditorSession session;

        const std::string layoutPath = resolveLayoutPath(_productPaths);

    EditorSessionDesc sessionDesc{};
    sessionDesc.uiBackend = uiBackend.get();
    sessionDesc.createAssetPreviewTexture =
        [backend = uiBackend.get()](std::uint16_t width,
                                    std::uint16_t height,
                                    const void* bgraPixels) -> void* {
            return backend != nullptr
                ? backend->createUiTexture(width, height, bgraPixels)
                : nullptr;
        };
    sessionDesc.releaseAssetPreviewTexture =
        [backend = uiBackend.get()](void* handle) {
            if (backend != nullptr) backend->releaseUiTexture(handle);
        };
    sessionDesc.importedCharacter = importedCharacter;
    sessionDesc.editorTestSceneEnabled = _editorTestSceneEnabled;
    sessionDesc.layoutPath = layoutPath;
    sessionDesc.projectRoot = projectRoot;
    sessionDesc.engineAssetsRoot = _productPaths.engineAssetsRoot.string();
    sessionDesc.iconRootPath = resolveEditorIconRoot(_productPaths);
    sessionDesc.hostWindow = hwnd;
    sessionDesc.deviceManager = _devices.get();
    sessionDesc.netClientMode = netClientMode;
    sessionDesc.netConnectHost = netConnectHost;
    sessionDesc.childWindowManager = &_devices->window();
    sessionDesc.preferences = _editorPreferences;
    sessionDesc.viewportOrientationAxisVisible =
        _editorPreferences.viewportOrientationAxisVisible;
    sessionDesc.onViewportOrientationAxisVisibilityChanged =
        _onViewportOrientationAxisVisibilityChanged;
    auto persistEditorPreferences =
        [this, &window, hwnd](const EditorPreferences& current) {
            EditorPreferences preferences = current;
            preferences.windowMaximized = window.isTopLevelMaximized(hwnd);
            if (!preferences.windowMaximized) {
                preferences.windowWidth = window.getWidth();
                preferences.windowHeight = window.getHeight();
            } else {
                // Session state intentionally does not churn on every host
                // resize. Preserve the last normal-window dimensions held by
                // the app while maximized, so a later toolbar/render change
                // cannot overwrite the restore size with stale startup data.
                preferences.windowWidth = _editorPreferences.windowWidth;
                preferences.windowHeight = _editorPreferences.windowHeight;
            }
            _editorPreferences = preferences;
            _viewportOrientationAxisVisible =
                preferences.viewportOrientationAxisVisible;
            if (_onEditorPreferencesChanged) {
                _onEditorPreferencesChanged(preferences);
            }
        };
    sessionDesc.onPreferencesChanged = persistEditorPreferences;
    sessionDesc.onStartupProgress = [&startupSplash](float progress,
                                                      const wchar_t* stage) {
        startupSplash.update(0.42f + std::clamp(progress, 0.0f, 1.0f) * 0.20f,
                             stage != nullptr ? stage : L"Loading editor...");
    };

    if (!session.initialize(sessionDesc)) {
        std::fprintf(stderr, "[EditorApp] failed to load layout: %s\n", layoutPath.c_str());
        _devices->shutdown();
        return;
    }

    session.setClientSize(static_cast<float>(clientWidth),
                          static_cast<float>(clientHeight));

    startupSplash.update(0.66f, L"Initializing renderer...");
    if (!session.ensurePresentationReady()) {
        std::fprintf(stderr, "[EditorApp] presentation bootstrap failed\n");
        session.shutdown();
        _devices->shutdown();
        return;
    }
    startupSplash.update(0.86f, L"Preparing render pipeline...");

    if (netClientMode) {
        session.autoEnterNetClientPlay();
    } else if (ayt::io::env::get("AY_EDITOR_SELECTION_CAPTURE_BASE")
                   .has_value()
               || (_autoPlayImportedAnimation
               && importedCharacter.isValid()
               && !importedCharacter.animationPath.empty())) {
        session.autoEnterImportedAnimationPlay();
    }

        rendererSub = ayt::render::RendererSubSystem::findRegistered();
        if (rendererSub == nullptr) {
        std::fprintf(stderr, "[EditorApp] renderer subsystem unavailable\n");
        session.shutdown();
        _devices->shutdown();
        return;
    }

    if (!uiBackend->initialize(rendererSub->renderer())) {
        std::fprintf(stderr, "[EditorApp] UIRenderBackend initialize failed\n");
        uiBackend->shutdown();
        session.shutdown();
        _devices->shutdown();
        return;
    }
    // AI-1 (2026-07-20) — inject the backend into RenderPipeline's
    // UIPass so the RenderPass dispatch can call backend->flushBatches
    // inside UIPass::execute. The dispatch order in
    // RendererSubSystem::renderCompositeFrame is now:
    //   uiPass(Populate) → renderScenePass (incl UIPass.flush) →
    //   uiPass(Flush). This replaces the pre-AI-1 path where the
    //   flush lived entirely in the host lambda.
    rendererSub->renderer().setUiBackend(uiBackend.get());
    AY_EDITOR_HEAP_CHECK("after_full_init");
    startupSplash.update(0.93f, L"Connecting editor input...");

    bool running = true;
    bool loggedFirstFrameHeap = false;
    bool windowPreferencesDirty = false;
    float windowPreferencesSaveCountdown = 0.0f;
    window.setWindowCloseCallback([&running]() { running = false; });
    window.setWindowResizeCallback([&session, &clientWidth, &clientHeight,
                                    &windowPreferencesDirty,
                                    &windowPreferencesSaveCountdown](int width,
                                                                      int height) {
        clientWidth = width;
        clientHeight = height;
        session.setClientSize(static_cast<float>(width), static_cast<float>(height));
        if (width > 0 && height > 0) {
            windowPreferencesDirty = true;
            windowPreferencesSaveCountdown = 0.5f;
        }
    });
    window.setWindowFocusCallback([&session](bool focused) {
        session.onWindowFocusChanged(focused);
    });

    ayt::ui::DeviceInputBridge::Callbacks inputCallbacks{};
    inputCallbacks.onMouseMove = [&session, &window](float x, float y) {
        const bool handled = session.onMouseMove(x, y);
        window.setCursorShape(ayt::ui::systemCursorFromUi(session.getUiCursorHint()));
        return handled;
    };
    inputCallbacks.onMouseLeave = [&session, &window]() {
        session.onMouseLeave();
        window.setCursorShape(ayt::device::SystemCursorShape::Arrow);
    };
    inputCallbacks.onMouseButton = [&session, &window](float x, float y,
                                                        int button, bool pressed) {
        const bool handled = pressed
            ? session.onMouseButtonDown(x, y, button)
            : session.onMouseButtonUp(x, y, button);
        window.setCursorShape(ayt::ui::systemCursorFromUi(session.getUiCursorHint()));
        return handled;
    };
    inputCallbacks.onMouseWheel = [&session](float x, float y, float deltaY) {
        return session.onMouseWheel(x, y, deltaY);
    };
    inputCallbacks.onKey = [&session](ayt::device::KeyCode key, bool pressed,
                                      bool /*repeat*/) {
        const int uiKey = static_cast<int>(ayt::ui::fromDeviceKey(key));
        return pressed ? session.onKeyDown(uiKey) : session.onKeyUp(uiKey);
    };
    inputCallbacks.onTextCommit = [&session](const std::string& text) {
        return !text.empty()
            && session.ui().onDeviceChar(text.data(), static_cast<int>(text.size()));
    };
    inputCallbacks.onComposition = [&session](
        ayt::device::DeviceInputEventType type, const std::string& text, int caret) {
        switch (type) {
        case ayt::device::DeviceInputEventType::CompositionStart:
            session.ui().onDeviceCompositionStart(text, caret);
            break;
        case ayt::device::DeviceInputEventType::CompositionUpdate:
            session.ui().onDeviceCompositionUpdate(text, caret);
            break;
        case ayt::device::DeviceInputEventType::CompositionEnd:
            session.ui().onDeviceCompositionEnd("");
            break;
        default:
            break;
        }
    };
    ayt::ui::DeviceInputBridge inputBridge(std::move(inputCallbacks));
    inputBridge.connect(*_devices);
    inputBridge.bindTextInputFocus(session.ui());
    window.setCursorShape(ayt::ui::systemCursorFromUi(session.getUiCursorHint()));

    // Submit a complete frame against the final editor HWND while it is still
    // hidden. The splash remains responsive on its own message thread during
    // shader warm-up. Only after this Present boundary do we reveal the main
    // window, eliminating the previous COLOR_WINDOW/white interval.
    startupSplash.update(0.97f, L"Rendering editor workspace...");
    session.syncViewportIfChanged();
    renderEditorWarmupFrame(session, *rendererSub, *uiBackend);
    startupSplash.update(1.0f, L"Editor ready");
    startupSplash.close();

    ::ShowWindow(hwnd, _editorPreferences.windowMaximized
        ? SW_MAXIMIZE : SW_SHOW);
    ::SetForegroundWindow(hwnd);

    // Diagnostic: per-frame timing print when AY_EDITOR_FRAME_TIMING=1.
    // Reports ms for pollEvents / update / syncViewport / render per
    // frame. Logged once a second (every ~60 frames at 60 FPS, less
    // often at lower FPS) to keep stderr readable. Disabled by
    // default. Set in the shell: set AY_EDITOR_FRAME_TIMING=1 before
    // launching AYEditorShell_Demo.exe.
    using Clock = std::chrono::steady_clock;
    auto previousHostFrame = Clock::now();
    const bool frameTiming =
        ayt::io::env::get("AY_EDITOR_FRAME_TIMING").has_value();
    // TEMPORARY PASS/GPU VALIDATION HOOK — remove with the backend override in
    // EditorShellDemo after the Pass audit. When the environment variable is
    // absent this is a zero-behavior branch. When set, one paused D3D11 run
    // captures stable Bloom off/on, Shadow Bias min/max, and ambient-only SSAO
    // off/strong/default pairs through Renderer::captureScreenshot; no UI
    // automation is involved. The caller supplies a path *base* without an
    // extension.
    const std::string passCaptureBase =
        ayt::io::env::get("AY_EDITOR_PASS_CAPTURE_BASE").value_or("");
    // Deterministic visual regression hook for editor selection. The normal
    // interactive path is unchanged when the variable is absent. A capture
    // run enters Play above, freezes the scene after its first presentation,
    // and records the same camera with no selection, the scaled ground, and
    // the opaque cube selected. This intentionally uses the real backbuffer
    // and production pass graph rather than a headless contract-only test.
    const std::string selectionCaptureBase =
        ayt::io::env::get("AY_EDITOR_SELECTION_CAPTURE_BASE").value_or("");
    if (!passCaptureBase.empty()) {
        ayt::render::Renderer& validationRenderer = rendererSub->renderer();
        validationRenderer.setDepthHazeEnabled(false);
        validationRenderer.setDepthHazeStrength(0.0f);
        validationRenderer.setSsaoEnabled(false);
        validationRenderer.setSsaoStrength(0.0f);
        validationRenderer.setPostProcessBloomStrength(0.0f);
        // Pin the Editor validation threshold so a later preset drift cannot
        // silently return this ordinary, non-emissive scene to a no-op Bloom.
        validationRenderer.setPostProcessBloomThreshold(0.25f);
        std::fprintf(stderr,
                     "[EditorApp] TEMP pass capture sequence armed: %s_*\n",
                     passCaptureBase.c_str());
    }
    uint64_t frameIndex = 0;
    double compositeMs = 0.0;
    // uiPassMs measures the host-side lambda that drives
    // UIManager::populateFrame + UIManager::flushFrame around the
    // RenderPass dispatch. AI-1 splits the lambda into two phases
    // so UIPass::execute can flush pending text in between; this
    // timer therefore accumulates the populate + flush cost
    // end-to-end (both halves happen via this lambda).
    double uiPassMs = 0.0;
    while (running && window.isWindowValid()) {
        // Per-frame value. This used to accumulate for the lifetime of the
        // process, producing misleading uiPass=20000ms diagnostics.
        uiPassMs = 0.0;
        const auto hostFrameNow = Clock::now();
        float editorDeltaSeconds = std::chrono::duration<float>(
            hostFrameNow - previousHostFrame).count();
        previousHostFrame = hostFrameNow;
        // Do not inject an arbitrarily large UI/freecam movement after a
        // debugger break or a dragged modal dialog. Simulation has its own
        // FrameManager and independently samples the monotonic clock.
        if (editorDeltaSeconds < 0.0f) {
            editorDeltaSeconds = 0.0f;
        } else if (editorDeltaSeconds > 0.25f) {
            editorDeltaSeconds = 0.25f;
        }

        const auto t0 = frameTiming ? Clock::now() : Clock::time_point{};
        _devices->pollEvents();
        const auto t1 = frameTiming ? Clock::now() : Clock::time_point{};
        ayt::game::HostedFrameContext hostFrame;
        hostFrame.realWallDeltaTime = editorDeltaSeconds;
        hostFrame.hostFrameIndex = frameIndex + 1;
        // DeviceManager::pollEvents() completed immediately above; this
        // identifies the stable device state observed by Play subsystems.
        hostFrame.inputFrameIndex = hostFrame.hostFrameIndex;
        session.update(hostFrame);
        if (windowPreferencesDirty) {
            windowPreferencesSaveCountdown -= editorDeltaSeconds;
            if (windowPreferencesSaveCountdown <= 0.0f) {
                persistEditorPreferences(session.currentPreferences());
                windowPreferencesDirty = false;
            }
        }
        const auto t2 = frameTiming ? Clock::now() : Clock::time_point{};
        session.syncViewportIfChanged();
        const auto t3 = frameTiming ? Clock::now() : Clock::time_point{};

        if (rendererSub != nullptr) {
            const bool renderScene = session.shouldCompositeViewport();
            const auto tRenderBegin = frameTiming ? Clock::now() : Clock::time_point{};
            // Inner timing: measure just the UI render pass (uiPass
            // lambda). If this is near-zero ms while the outer
            // render time is 60-80ms, the slow path is
            // beginFrame/endFrame/pollShaderHotReload inside
            // renderCompositeFrame (bgfx + shader pipeline), not
            // the UI pass. Disabled by default; same env var as
            // the outer diag.
            rendererSub->renderCompositeFrame(
                renderScene, uiBackend.get(),
                [&session, frameTiming, &uiPassMs](
                    bool skipViewportPanel, ayt::render::CompositeUiPhase phase) {
                    // AI-1 (2026-07-20): phase-aware dispatch. The
                    // lambda is now called TWICE per frame — once
                    // with Populate (before Renderer::render, so the
                    // widget walk accumulates batches that
                    // UIPass::execute will flush), once with Flush
                    // (after Renderer::render, to close the
                    // IRenderBackend lifecycle). The skipViewportPanel
                    // toggle is applied once at populate and
                    // reverted once at flush; the old single-call
                    // session.render(skipViewportPanel) is preserved
                    // by EditorSession::render(bool) for back-compat.
                    if (frameTiming) {
                        const auto tU0 = Clock::now();
                        if (phase == ayt::render::CompositeUiPhase::Populate) {
                            session.populateFrame(skipViewportPanel);
                        } else {
                            session.flushFrame();
                        }
                        const auto tU1 = Clock::now();
                        uiPassMs += std::chrono::duration<double, std::milli>(
                            tU1 - tU0).count();
                    } else {
                        if (phase == ayt::render::CompositeUiPhase::Populate) {
                            session.populateFrame(skipViewportPanel);
                        } else {
                            session.flushFrame();
                        }
                    }
                });
            const auto tRenderEnd = frameTiming ? Clock::now() : Clock::time_point{};
            if (frameTiming) {
                compositeMs = std::chrono::duration<double, std::milli>(
                    tRenderEnd - tRenderBegin).count();
            }

            if (!selectionCaptureBase.empty()) {
                ayt::render::Renderer& validationRenderer =
                    rendererSub->renderer();
                auto queueSelectionCapture = [&](const char* suffix) {
                    const std::string base = selectionCaptureBase + suffix;
                    const bool queued = validationRenderer.captureScreenshot(base);
                    std::fprintf(stderr,
                                 "[EditorApp] selection capture %s: %s\n",
                                 queued ? "queued" : "FAILED", base.c_str());
                };
                auto selectCaptureTarget = [&](const char* materialNeedle) {
                    ayt::entity::World* world = session.worldContext().world(
                        EditorWorldSlot::Play, true);
                    ayt::entity::Entity* target = nullptr;
                    if (world != nullptr) {
                        for (ayt::entity::Entity* entity : world->getAllEntities()) {
                            auto* mesh = entity != nullptr
                                ? entity->getComponent<ayt::entity::MeshComponent>()
                                : nullptr;
                            if (mesh == nullptr) {
                                continue;
                            }
                            mesh->outlineHull = false;
                            if (materialNeedle != nullptr
                                && mesh->materialPath.find(materialNeedle)
                                    != std::string::npos) {
                                target = entity;
                            }
                        }
                    }
                    if (target != nullptr) {
                        target->getComponent<ayt::entity::MeshComponent>()
                            ->outlineHull = true;
                    }
                    std::fprintf(stderr,
                                 "[EditorApp] selection capture target '%s': %s\n",
                                 materialNeedle != nullptr ? materialNeedle : "none",
                                 target != nullptr ? "found" : "not found");
                };

                if (frameIndex == 30) {
                    selectCaptureTarget(nullptr);
                } else if (frameIndex == 35) {
                    ayt::game::GameLoop::instance().pause();
                } else if (frameIndex == 50) {
                    queueSelectionCapture("_none");
                } else if (frameIndex == 60) {
                    ayt::game::GameLoop::instance().resume();
                    selectCaptureTarget("ground_shadow.aymat");
                } else if (frameIndex == 65) {
                    ayt::game::GameLoop::instance().pause();
                } else if (frameIndex == 80) {
                    queueSelectionCapture("_ground");
                } else if (frameIndex == 90) {
                    ayt::game::GameLoop::instance().resume();
                    selectCaptureTarget("cube_shadow.aymat");
                } else if (frameIndex == 95) {
                    ayt::game::GameLoop::instance().pause();
                } else if (frameIndex == 105) {
                    queueSelectionCapture("_cube");
                } else if (frameIndex == 125) {
                    running = false;
                }
            } else if (!passCaptureBase.empty()) {
                ayt::render::Renderer& validationRenderer =
                    rendererSub->renderer();
                auto queueCapture = [&](const char* suffix) {
                    const std::string base = passCaptureBase + suffix;
                    const bool queued = validationRenderer.captureScreenshot(base);
                    std::fprintf(stderr,
                                 "[EditorApp] TEMP pass capture %s: %s\n",
                                 queued ? "queued" : "FAILED", base.c_str());
                };

                // Leave several submitted frames between every state change
                // and capture so bgfx readback cannot sample the next state.
                if (frameIndex == 30) {
                    // Let render systems publish a real Scene before freezing;
                    // pausing at frame zero leaves the viewport intentionally
                    // empty because no presentation snapshot exists yet.
                    ayt::game::GameLoop::instance().pause();
                } else if (frameIndex == 60) {
                    queueCapture("_bloom_off");
                } else if (frameIndex == 70) {
                    validationRenderer.setPostProcessBloomStrength(1.5f);
                } else if (frameIndex == 85) {
                    queueCapture("_bloom_on");
                } else if (frameIndex == 95) {
                    validationRenderer.setPostProcessBloomStrength(0.0f);
                    validationRenderer.setShadowBias(0.0f);
                } else if (frameIndex == 110) {
                    queueCapture("_shadow_bias_0000");
                } else if (frameIndex == 120) {
                    validationRenderer.setShadowBias(0.02f);
                } else if (frameIndex == 135) {
                    queueCapture("_shadow_bias_0020");
                } else if (frameIndex == 145) {
                    // Isolate ambient occlusion from direct-light changes.
                    validationRenderer.setSsaoEnabled(false);
                    validationRenderer.setSsaoStrength(0.0f);
                    validationRenderer.setSceneLights(nullptr);
                    validationRenderer.setDirectionalLight(
                        ayt::math::FVector3(0.35f, -0.85f, -0.40f),
                        ayt::math::FVector3(0.0f, 0.0f, 0.0f));
                    validationRenderer.setAmbientStrength(1.5f);
                } else if (frameIndex == 160) {
                    queueCapture("_ssao_off");
                } else if (frameIndex == 170) {
                    validationRenderer.setSsaoEnabled(true);
                    validationRenderer.setSsaoStrength(1.0f);
                    validationRenderer.setSsaoParams(0.8f, 0.02f);
                } else if (frameIndex == 185) {
                    queueCapture("_ssao_on");
                } else if (frameIndex == 195) {
                    // Match the values shipped by editor_shell.ui.json after
                    // the strong profile has established spatial correctness.
                    validationRenderer.setSsaoStrength(0.45f);
                    validationRenderer.setSsaoParams(0.4f, 0.04f);
                } else if (frameIndex == 210) {
                    queueCapture("_ssao_default_on");
                } else if (frameIndex == 225) {
                    running = false;
                }
            }
        }
        const auto t4 = frameTiming ? Clock::now() : Clock::time_point{};

        if (!loggedFirstFrameHeap) {
            AY_EDITOR_HEAP_CHECK("after_first_frame");
            loggedFirstFrameHeap = true;
        }

        Sleep(1);
        ++frameIndex;

        if (frameTiming && (frameIndex % 60) == 0) {
            using ms = std::chrono::duration<double, std::milli>;
            const ayt::render::RenderFrameStats* renderStats =
                rendererSub != nullptr
                    ? &rendererSub->renderer().getFrameStats()
                    : nullptr;
            std::fprintf(stderr,
                "[EditorApp frame %llu] poll=%5.2fms update=%5.2fms "
                "syncViewport=%5.2fms render=%6.2fms (uiPass=%5.2fms) "
                "total=%6.2fms fps=%5.1f avg=%5.2fms p95=%5.2fms "
                "p99=%5.2fms gpu=%5.2fms dc=%u/%u blit=%u\n",
                static_cast<unsigned long long>(frameIndex),
                std::chrono::duration_cast<ms>(t1 - t0).count(),
                std::chrono::duration_cast<ms>(t2 - t1).count(),
                std::chrono::duration_cast<ms>(t3 - t2).count(),
                compositeMs,
                uiPassMs,
                std::chrono::duration_cast<ms>(t4 - t0).count(),
                renderStats ? renderStats->fps : 0.0f,
                renderStats ? renderStats->avgFrameTimeMs : 0.0f,
                renderStats ? renderStats->p95FrameTimeMs : 0.0f,
                renderStats ? renderStats->p99FrameTimeMs : 0.0f,
                renderStats ? renderStats->gpuFrameTimeMs : 0.0f,
                renderStats ? renderStats->drawCalls : 0u,
                renderStats ? renderStats->backendDrawCalls : 0u,
                renderStats ? renderStats->backendBlitCalls : 0u);
            if (renderStats != nullptr) {
                for (const auto& pass : renderStats->passes) {
                    std::fprintf(stderr,
                                 "  [Pass %-14s] dc=%3u cpu=%6.2fms gpu=%6.2fms\n",
                                 pass.name.c_str(), pass.drawCalls,
                                 pass.cpuTimeMs, pass.gpuTimeMs);
                }
            }
        }
    }

    persistEditorPreferences(session.currentPreferences());
    onPreShutdown();

    // UI GPU resources (font atlas, UiGpuContext) must be released while bgfx is
    // still alive. session.shutdown() calls endPlaySession() which shuts down
    // RendererSubSystem and destroys bgfx — do that only after this step.
    AY_EDITOR_HEAP_CHECK("before_ui_backend_shutdown");
    if (rendererSub != nullptr) {
        rendererSub->renderer().shutdownUiRenderBackend(*uiBackend);
    } else {
        uiBackend->shutdown();
    }
    AY_EDITOR_HEAP_CHECK("after_ui_backend_shutdown");

    AY_EDITOR_HEAP_CHECK("before_session_shutdown");
    session.shutdown();
    AY_EDITOR_HEAP_CHECK("after_session_shutdown");
    } // ~EditorSession — destroy before GameLoop teardown

    ayt::game::GameLoop::instance().shutdown();
    AY_EDITOR_HEAP_CHECK("after_gameloop_shutdown");

    if (_runtime && _runtime->modules) {
        _runtime->modules->shutdown();
        _runtime->modules.reset();
    }
    _runtime.reset();

    _devices->shutdown();
    onShutdown();

    // Release GPU-owned state while handles are still valid; heap object avoids
    // MSVC stack-cookie trips on the run() stack frame when class layout drifts.
    uiBackend->shutdown();
    uiBackend.reset();
    AY_EDITOR_HEAP_CHECK("after_ui_backend_destroyed");
}

} // namespace ayt::editor
