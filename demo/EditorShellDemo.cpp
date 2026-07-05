// EditorShellDemo.cpp — E2-composite entry: EditorApp + single-window bgfx composite

#include "AYEditorApp.h"
#include "AYGameLoop.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ayt::app::GameDesc desc{};
    desc.name = "AY Editor (E2-composite)";
    desc.width = 1280;
    desc.height = 720;
    desc.enableRenderThread = false;

    auto app = ayt::editor::EditorApp::create(desc);
    app->run();
    return 0;
}
