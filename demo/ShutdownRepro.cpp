// ShutdownRepro.cpp — minimal heap-corruption isolation (L0-L4).
// Usage: AYEditorShutdownRepro.exe [--level N] [--frames N]
//   level 0: DeviceManager init/shutdown
//   level 1: + entity bootstrap (onInit)
//   level 2: + EditorSession layout load, no presentation / no GPU UI (default)
//   level 3: + ensurePresentationReady + UIRenderBackend init, no frames
//   level 4: + renderCompositeFrame for --frames N (default 1)

#include "AYEditorHeapDebug.h"
#include "AYEditorSession.h"
#include "AYEntityModule.h"
#include "AYGameLoop.h"
#include "AYDeviceManager.h"
#include "AYRendererSubSystem.h"
#include "AYUIRenderBackend.h"

#include <cstdio>
#include <cstdlib>
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
    std::fprintf(stdout, "[ShutdownRepro] debug console attached\n");
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

int parseLevel(PWSTR cmdLine)
{
    int level = 2;
    if (cmdLine == nullptr) {
        return level;
    }

    const std::wstring args(cmdLine);
    const size_t pos = args.find(L"--level");
    if (pos == std::wstring::npos) {
        return level;
    }

    const size_t valueStart = args.find_first_not_of(L" \t", pos + 7);
    if (valueStart == std::wstring::npos) {
        return level;
    }

    level = _wtoi(args.c_str() + valueStart);
    if (level < 0) {
        level = 0;
    }
    if (level > 4) {
        level = 4;
    }
    return level;
}

int parseFrames(PWSTR cmdLine)
{
    int frames = 1;
    if (cmdLine == nullptr) {
        return frames;
    }

    const std::wstring args(cmdLine);
    const size_t pos = args.find(L"--frames");
    if (pos == std::wstring::npos) {
        return frames;
    }

    const size_t valueStart = args.find_first_not_of(L" \t", pos + 8);
    if (valueStart == std::wstring::npos) {
        return frames;
    }

    frames = _wtoi(args.c_str() + valueStart);
    if (frames < 1) {
        frames = 1;
    }
    return frames;
}

void shutdownSession(ayt::editor::EditorSession& session, ayt::render::UIRenderBackend* uiBackend,
                     ayt::render::RendererSubSystem* rendererSub)
{
    AY_EDITOR_HEAP_CHECK("before_ui_shutdown");
    session.ui().shutdown();
    if (uiBackend != nullptr) {
        if (rendererSub != nullptr) {
            rendererSub->renderer().shutdownUiRenderBackend(*uiBackend);
        } else {
            uiBackend->shutdown();
        }
    }
    session.shutdown();
    AY_EDITOR_HEAP_CHECK("after_session_shutdown");
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int)
{
    attachDebugConsole();
    AY_EDITOR_HEAP_DEBUG_INIT();

    const int level  = parseLevel(cmdLine);
    const int frames = parseFrames(cmdLine);
    std::fprintf(stderr, "[ShutdownRepro] level=%d frames=%d\n", level, frames);

    ayt::device::DeviceManager devices;
    ayt::device::DeviceConfig deviceConfig{};
    deviceConfig.window.title = "AY Editor Shutdown Repro";
    deviceConfig.window.width = 1280;
    deviceConfig.window.height = 720;

    if (!devices.initialize(deviceConfig)) {
        std::fprintf(stderr, "[ShutdownRepro] DeviceManager initialize failed\n");
        return 1;
    }
    AY_EDITOR_HEAP_CHECK("after_devices_init");

    if (level >= 1) {
        ayt::entity::bootstrapModule();
        AY_EDITOR_HEAP_CHECK("after_entity_bootstrap");
    }

    ayt::editor::EditorSession session;
    AY_EDITOR_HEAP_CHECK("after_editor_session_ctor");
    AY_EDITOR_TRACE("EditorSession constructed");

    ayt::render::UIRenderBackend uiBackend;
    ayt::render::RendererSubSystem* rendererSub = nullptr;
    const std::string layoutPath = resolveLayoutPath();

    if (level >= 2) {
        HWND hwnd = static_cast<HWND>(devices.window().getWindowHandle());
        ayt::editor::EditorSessionDesc sessionDesc{};
        sessionDesc.uiBackend = nullptr;
        sessionDesc.layoutPath = layoutPath;
        sessionDesc.hostWindow = hwnd;

        const bool useMinimal =
            cmdLine != nullptr && wcsstr(cmdLine, L"--minimal") != nullptr;
        std::fprintf(stderr, "[ShutdownRepro] layout path: %s minimal=%d\n", layoutPath.c_str(),
                     useMinimal ? 1 : 0);

        if (!useMinimal) {
            if (!session.initialize(sessionDesc)) {
                std::fprintf(stderr, "[ShutdownRepro] layout load failed: %s\n",
                             layoutPath.c_str());
                devices.shutdown();
                return 1;
            }
        } else {
            sessionDesc.layoutPath.clear();
            if (!session.initialize(sessionDesc)) {
                std::fprintf(stderr, "[ShutdownRepro] session init failed (minimal)\n");
                devices.shutdown();
                return 1;
            }
            static const char kMinimalJson[] =
                R"({"type":"TextLabel","id":"minimal_label","text":"Hi","size":{"w":100,"h":24}})";
            if (!session.ui().loadFromString(kMinimalJson)) {
                std::fprintf(stderr, "[ShutdownRepro] minimal layout load failed\n");
                devices.shutdown();
                return 1;
            }
            AY_EDITOR_HEAP_CHECK("after_minimal_layout_load");
        }

        if (!useMinimal) {
            session.setClientSize(1280.0f, 720.0f);
            AY_EDITOR_HEAP_CHECK("after_layout_load");
        }
    }

    if (level >= 3) {
        if (!session.ensurePresentationReady()) {
            std::fprintf(stderr, "[ShutdownRepro] presentation bootstrap failed\n");
            shutdownSession(session, nullptr, nullptr);
            devices.shutdown();
            return 1;
        }
        AY_EDITOR_HEAP_CHECK("after_presentation_ready");

        rendererSub = ayt::render::RendererSubSystem::findRegistered();
        if (rendererSub == nullptr) {
            std::fprintf(stderr, "[ShutdownRepro] renderer subsystem unavailable\n");
            shutdownSession(session, nullptr, nullptr);
            devices.shutdown();
            return 1;
        }

        if (!uiBackend.initialize(rendererSub->renderer())) {
            std::fprintf(stderr, "[ShutdownRepro] UIRenderBackend initialize failed\n");
            uiBackend.shutdown();
            shutdownSession(session, nullptr, nullptr);
            devices.shutdown();
            return 1;
        }
        AY_EDITOR_HEAP_CHECK("after_ui_backend_init");
    }

    if (level >= 4 && rendererSub != nullptr) {
        uiBackend.setFramebufferSize(1280, 720);
        constexpr float kDeltaSeconds = 1.0f / 60.0f;
        for (int i = 0; i < frames; ++i) {
            session.update(kDeltaSeconds);
            const bool renderScene = session.shouldCompositeViewport();
            rendererSub->renderCompositeFrame(
                renderScene, &uiBackend,
                [&session](bool skipViewportPanel) { session.render(skipViewportPanel); });
            devices.pollEvents();
        }
        AY_EDITOR_HEAP_CHECK("after_render_frames");
    }

    if (level >= 2) {
        shutdownSession(session, level >= 3 ? &uiBackend : nullptr, rendererSub);
    }

    devices.shutdown();
    ayt::game::GameLoop::instance().shutdown();
    AY_EDITOR_HEAP_CHECK("after_full_shutdown");

    std::fprintf(stderr, "[ShutdownRepro] completed level=%d\n", level);
    std::fflush(stderr);
    return 0;
}
