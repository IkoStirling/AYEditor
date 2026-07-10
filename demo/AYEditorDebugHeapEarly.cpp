// Enable CRT debug heap before any other static initializers in the EXE.
// Without this, allocations during REGISTER_SUBSYSTEM / reflect static init
// happen before AY_EDITOR_HEAP_DEBUG_INIT() and _CrtCheckMemory() may fail
// at the first checkpoint in wWinMain / EditorApp::run().

#if defined(_DEBUG) && defined(_MSC_VER)

#include <crtdbg.h>

#pragma init_seg(compiler)

namespace {

struct AyEditorEarlyCrtHeap {
    AyEditorEarlyCrtHeap()
    {
        const int flag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        _CrtSetDbgFlag(flag | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    }
};

static AyEditorEarlyCrtHeap g_ayEditorEarlyCrtHeap;

} // namespace

#endif
