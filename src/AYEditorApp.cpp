#include "AYEditor/EditorApp.h"

#include "AYEditor/EditorHeapDebug.h"
#include "AYEditor/EditorPlayRuntime.h"
#include "AYEditor/EditorSession.h"
#include "AYEditor/RegisterDefaultEditorModules.h"
#include "AYGameLoop.h"
#include "AYDevice/DeviceManager.h"
#include "AYDevice/DeviceInputProvider.h"
#include "AYEditor/ImportedCharacterMapper.h"
#include "AYEditor/Importer.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYScript/ScriptSubSystem.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYUI/UIKeyCode.h"

#include <AYApplication/IEngineHost.h>

#include <AYEventSystem/EventBus.h>
#include <AYIO/Env.h>
#include <AYPlatform/Console.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

namespace ayt::editor {

namespace {

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
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

std::string resolveLayoutPath()
{
    const std::vector<std::string> candidates = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
        "../AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
        "../../AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return candidates.front();
}

struct EditorHostState {
    EditorSession* session = nullptr;
    int clientWidth = 0;
    int clientHeight = 0;
};

int uiKeyFromVirtualKey(std::uintptr_t virtualKey) noexcept
{
    // Most UIKeyCode values intentionally match Win32 VK values. PageUp and
    // PageDown are the documented exceptions and must be translated at the
    // host boundary instead of being passed through as raw VK_PRIOR/VK_NEXT.
    switch (virtualKey) {
    case VK_PRIOR: return ayt::ui::UIKey_PageUp;
    case VK_NEXT:  return ayt::ui::UIKey_PageDown;
    default:       return static_cast<int>(virtualKey);
    }
}

std::intptr_t handleHostMessage(HWND hwnd, EditorHostState* state, unsigned msg,
                                std::uintptr_t wParam, std::intptr_t lParam, bool& handled)
{
    handled = false;
    if (state == nullptr || state->session == nullptr) {
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        state->clientWidth  = LOWORD(lParam);
        state->clientHeight = HIWORD(lParam);
        state->session->setClientSize(static_cast<float>(state->clientWidth),
                                      static_cast<float>(state->clientHeight));
        handled = true;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        handled = state->session->onKeyDown(uiKeyFromVirtualKey(wParam));
        return handled ? 0 : 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        handled = state->session->onKeyUp(uiKeyFromVirtualKey(wParam));
        return handled ? 0 : 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT trackLeave{};
        trackLeave.cbSize = sizeof(trackLeave);
        trackLeave.dwFlags = TME_LEAVE;
        trackLeave.hwndTrack = hwnd;
        TrackMouseEvent(&trackLeave);

        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        state->session->onMouseMove(x, y);
        switch (state->session->getUiCursorHint()) {
        case ayt::ui::UiCursorHint::Hand:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            break;
        case ayt::ui::UiCursorHint::SizeHorizontal:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            break;
        case ayt::ui::UiCursorHint::Move:
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            break;
        default:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            break;
        }
        handled = true;
        return 0;
    }
    case WM_MOUSELEAVE:
        if (GetCapture() != hwnd) {
            state->session->onMouseLeave();
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        handled = true;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            switch (state->session->getUiCursorHint()) {
            case ayt::ui::UiCursorHint::Hand:
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                handled = true;
                return TRUE;
            case ayt::ui::UiCursorHint::SizeHorizontal:
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                handled = true;
                return TRUE;
            case ayt::ui::UiCursorHint::Move:
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                handled = true;
                return TRUE;
            default:
                break;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        // Capture for UI drag OR freecam look (both return true).
        if (state->session->onMouseButtonDown(x, y, 0)) {
            SetCapture(hwnd);
        }
        handled = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        state->session->onMouseButtonUp(x, y, 0);
        ReleaseCapture();
        handled = true;
        return 0;
    }
    default:
        break;
    }

    return 0;
}

// G2: split the Win32 command line into whitespace-separated tokens.
// Returns an empty vector on null input or all-whitespace input.
// Phase 1 only: does NOT honor double-quoted strings. Use
// `--import <path-without-spaces>` form. Quoted paths with spaces
// fall back to the cube (logged as a parse failure).
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
    for (const char* p = cmdLine; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (std::isspace(c) != 0) {
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
    // INT-02 (2026-07-15): reset provider BEFORE devices. ScriptSubSystem
    // is owned by GameLoop and survives this dtor (GameLoop clears
    // SubSystems on its own shutdown); if _inputProvider outlived
    // _devices, the bridge would hold a dangling DeviceManager*.
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
    // Engine-host: shared Editor assembly + service table
    // (AYApplication/docs/engine-host.md).
    EditorModuleOptions opts{};
    opts.enableAudio = !_cmdLine.noAudio;
    registerDefaultEditorModules(opts);
    ayt::app::bindBuiltinHostServices(engineHost());

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

    AY_EDITOR_HEAP_DEBUG_INIT();
    AY_EDITOR_HEAP_CHECK("startup");
    attachDebugConsole();

    // Import before registering/initializing runtime and rendering systems.
    // Asset conversion is a pure offline operation and must not share its
    // STL allocations with live renderer/device/background services.  This
    // also gives the debug heap a precise boundary for import corruption.
    ImportedCharacter importedCharacter;
    const std::vector<std::string> cmdTokens =
        tokenizeCommandLine(::GetCommandLineA());
    const bool netClientMode = hasNetClientFlag(cmdTokens);
    const std::string netConnectHost = findNetConnectHost(cmdTokens);
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

    // A model source owns render assets and the target skeleton. A separate
    // animation source contributes only .ayanm clips, preventing re-exported
    // animation FBX files from replacing valid model textures/materials.
    if (!netClientMode && importedCharacter.isValid()) {
        std::string animationImportPath = findAnimationImportPath(cmdTokens);
        if (animationImportPath.empty()) {
            animationImportPath = _defaultAnimationImportPath;
        }
        if (!animationImportPath.empty()) {
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

    onInit();
    // INT-02 (2026-07-15): _devices is a member (was stack-local
    // before). Lifetime == *this so the Logia InputProvider that
    // registerSubSystems() installs can hold a raw pointer into it.
    _devices = std::make_unique<ayt::device::DeviceManager>();
    ayt::device::DeviceConfig deviceConfig{};
    deviceConfig.window.title = _desc.name != nullptr ? _desc.name : "AY Editor";
    deviceConfig.window.width = static_cast<int>(_desc.width);
    deviceConfig.window.height = static_cast<int>(_desc.height);
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

    EditorHostState hostState{};
    hostState.clientWidth  = static_cast<int>(_desc.width);
    hostState.clientHeight = static_cast<int>(_desc.height);

    auto uiBackend = std::make_unique<ayt::render::UIRenderBackend>();
    ayt::render::RendererSubSystem* rendererSub = nullptr;

    {
        EditorSession session;
        hostState.session = &session;

        const std::string layoutPath = resolveLayoutPath();

    EditorSessionDesc sessionDesc{};
    sessionDesc.uiBackend = uiBackend.get();
    sessionDesc.importedCharacter = importedCharacter;
    sessionDesc.layoutPath = layoutPath;
    sessionDesc.hostWindow = hwnd;
    sessionDesc.netClientMode = netClientMode;
    sessionDesc.netConnectHost = netConnectHost;
    sessionDesc.childWindowManager = &_devices->window();

    if (!session.initialize(sessionDesc)) {
        std::fprintf(stderr, "[EditorApp] failed to load layout: %s\n", layoutPath.c_str());
        _devices->shutdown();
        return;
    }

    // Primary-window TextInput/IME bridge. Child windows already install the
    // same bridge through EditorChildWindowManager.
    session.ui().onTextEditingFocusChanged = [this](bool editing) {
        _devices->textInput().setEnabled(editing);
    };
    _devices->textInput().onCommit = [&session](const std::string& chunk) {
        if (!chunk.empty()) {
            session.ui().onDeviceChar(chunk.data(), static_cast<int>(chunk.size()));
        }
    };
    _devices->textInput().onCompositionUpdate =
        [&session, this](const std::string& text, int caret) {
            if (text.empty() && !_devices->textInput().isComposing()) {
                session.ui().onDeviceCompositionEnd("");
                return;
            }
            session.ui().onDeviceCompositionUpdate(text, caret);
        };

    session.setClientSize(static_cast<float>(hostState.clientWidth),
                          static_cast<float>(hostState.clientHeight));

    if (!session.ensurePresentationReady()) {
        std::fprintf(stderr, "[EditorApp] presentation bootstrap failed\n");
        session.shutdown();
        _devices->shutdown();
        return;
    }

    if (netClientMode) {
        session.autoEnterNetClientPlay();
    } else if (_autoPlayImportedAnimation
               && importedCharacter.isValid()
               && !importedCharacter.animationPath.empty()) {
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

    bool running = true;
    bool loggedFirstFrameHeap = false;
    window.setWindowCloseCallback([&running]() { running = false; });
    window.setWindowFocusCallback([&session](bool focused) {
        session.onWindowFocusChanged(focused);
    });
    window.setWindowMessageCallback(
        [hwnd, &hostState](unsigned msg, std::uintptr_t wParam, std::intptr_t lParam,
                           bool& handled) -> std::intptr_t {
            return handleHostMessage(hwnd, &hostState, msg, wParam, lParam, handled);
        });

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

            if (!passCaptureBase.empty()) {
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

    onPreShutdown();
    hostState.session = nullptr;

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

    _devices->shutdown();
    onShutdown();

    // Release GPU-owned state while handles are still valid; heap object avoids
    // MSVC stack-cookie trips on the run() stack frame when class layout drifts.
    uiBackend->shutdown();
    uiBackend.reset();
    AY_EDITOR_HEAP_CHECK("after_ui_backend_destroyed");
}

} // namespace ayt::editor
