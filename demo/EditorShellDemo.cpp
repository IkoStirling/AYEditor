// EditorShellDemo.cpp — E2-composite entry: EditorApp + single-window bgfx composite
//
// Default character: Sour Miku FBX (MMD-origin, model only — no clip).
// First convert can take ~1–2 minutes; later launches reuse
// ayeditor_cache/.../Sour.aydep.json (bind-pose preview). Override with
// `--import <other.fbx>`, AY_EDITOR_FORCE_IMPORT=1, or
// AY_EDITOR_CHARACTER_SCALE=<float>.

#include "AYEditor/EditorApp.h"
#include "AYGameLoop.h"
#include "AYApplication/IEngineHost.h"      // defaultEngineHost() Meyers singleton (v0.3 PR-4)
#include "AYScene/SceneManager.h"   // SceneManager::canBeginPlay/isEditDirty (PR-4 日志块)

#include <AYIO/Env.h>

#include <cstdio>

namespace {

constexpr const char* kDefaultSourFbx =
    "D:/Projects/AliyatRenderer/assets/core/models/sour-miku-Creamy/Sour.fbx";

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // AY_EDITOR_NO_CONSOLE=1 keeps stdout/stderr on the parent process
    // pipe (no AllocConsole / CONOUT$ hijack) so `exe 2>file` captures
    // engine diagnostics. Default: interactive console as before.
    const bool keepPipe =
        ayt::io::env::get("AY_EDITOR_NO_CONSOLE").value_or("") == "1";
    if (!keepPipe) {
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }

    ayt::app::GameDesc desc{};
    desc.name = "AY Editor (E2-composite)";
    desc.width = 1280;
    desc.height = 720;
    desc.enableRenderThread = false;

    auto app = ayt::editor::EditorApp::create(desc);
    app->setDefaultImportPath(kDefaultSourFbx);
    std::fprintf(stderr,
                 "[EditorShellDemo] default import: %s\n"
                 "[EditorShellDemo] note: model-only FBX → bind-pose; "
                 "first convert ~1–2 min, then cache\n"
                 "[EditorShellDemo] net client: AYEditorShell_Demo.exe --net-client "
                 "[--net-host 127.0.0.1] (start server Play first)\n",
                 kDefaultSourFbx);

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
    return 0;
}
