// EditorShellDemo.cpp — E3 entry: EditorApp + AYDevice host window

#include "AYEditorApp.h"
#include "AYGameLoop.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ayt::app::GameDesc desc{};
    desc.name = "AY Editor (E3)";
    desc.width = 1280;
    desc.height = 720;
    desc.enableRenderThread = false;

    auto app = ayt::editor::EditorApp::create(desc);
    app->run();
    return 0;
}
