#include "AYTest.h"
#include "AYGameLoop.h"
#include <AYEntity/EntityModule.h>

#include <array>
#include <cstdio>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Test_EditorShell.cpp"
#include "Test_EditorImporter.cpp"
#include "Test_EditorPlayRuntime.cpp"
#include "Test_EditorWorldContext.cpp"
// AYTest discovers cases through static registration. Aggregate every Editor
// test source into this one translation unit so MSVC cannot discard otherwise
// unreferenced registration initializers. CMake must compile only main.cpp.
#include "Test_EditorChildWindowManager.cpp"   // D5+.5 case definitions
#include "Test_ImportedCharacterMapper.cpp"     // master test cases
#include "Test_EditorTransportDirtyPrompt.cpp"  // v0.3 PR-4 case definitions
#include "Test_EditorHierarchy.cpp"            // v0.3+ PR-5 case definitions
#include "Test_EditorSceneBridge.cpp"          // v0.4 PR-1 Scene runtime bridge
#include "Test_EditorP0Core.cpp"               // P0 document/selection/command core
#include "Test_EditorTransformGizmo.cpp"        // transform gizmo ray/constraint math
#include "Test_EditorAssetBrowser.cpp"          // project asset index/browser/drag-drop
#include "Test_EditorAssetTilePresenter.cpp"    // asset tile display-only mapping
#include "Test_EditorSkeletonExtension.cpp"     // skeleton core thin-adapter integration
#include "Test_EditorAnimationExtension.cpp"    // animation preview + binding integration
#include "Test_EditorDslDocument.cpp"           // Phoskia/Logia dock editor
#include "Test_Editor2DTools.cpp"               // 2D viewport + tilemap authoring model
#include "Test_EditorFramework.cpp"             // unified editor workspace foundation
#include "Test_EditorProjectWorkflow.cpp"       // project creation/trash/tool registration
#include "Test_EditorNewProject.cpp"            // shared project initializer adapter
#include "Test_EditorUiFlowProjectDescriptor.cpp" // project Flow contract + legacy migration
#include "Test_EditorUiFlowEditor.cpp"          // UI Flow authoring + production-runtime preview
#include "Test_EditorUiDesignerWorkflow.cpp"    // cross-document UI authoring workflow
#include "Test_EditorGameFlowAssetIntegration.cpp" // GameFlow asset/project contract
#include "Test_EditorGameFlowDocument.cpp"      // GameFlow authoring model + validation
#include "Test_EditorGameFlowPreview.cpp"       // production coordinator preview diagnostics
#include "Test_EditorRecoveryAndTrash.cpp"      // selective recovery + trash browser services

namespace {

constexpr std::array<const char*, 27> kEditorTestSuites{
    "AYEditor_Shell",
    "AYEditor_Importer",
    "AYEditor_PlayRuntime",
    "AYEditor_WorldContext",
    "AYEditor_ChildWindowManager",
    "AYEditor_ImportedCharacterMapper",
    "AYEditor_TransportDirtyPrompt",
    "AYEditor_Hierarchy",
    "AYEditor_SceneBridge",
    "AYEditor_P0Core",
    "AYEditor_TransformGizmo",
    "AYEditor_AssetBrowser",
    "AYEditor_AssetTilePresenter",
    "AYEditor_SkeletonExtension",
    "AYEditor_AnimationExtension",
    "AYEditor_DslDocument",
    "Editor2DToolsTests",
    "AYEditor_Framework",
    "AYEditor_ProjectWorkflow",
    "AYEditor_NewProject",
    "AYEditor_UIFlowProjectContract",
    "AYEditor_UIFlowAuthoring",
    "EditorUiDesignerWorkflowTests",
    "AYEditor_GameFlowAssetIntegration",
    "AYEditor_GameFlowDocument",
    "AYEditor_GameFlowPreview",
    "AYEditor_RecoveryAndTrash",
};

int runIsolatedSuite(const char* executable, const char* suite)
{
#if defined(_WIN32)
    const char* arguments[]{executable, suite, nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, executable, arguments));
#else
    const pid_t child = fork();
    if (child == 0) {
        execl(executable, executable, suite, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) return -1;
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    // This executable is the host for Editor/Entity integration tests.
    // Register component metadata up front so typed addComponent<T>() calls
    // may create their World storage. Keep runtime subsystem registration out
    // of the test entry point; individual cases own that lifecycle explicitly.
    ayt::entity::registerEntityComponents();

    if (argc > 1) {
        const int result = ayt::test::runSuite(argv[1]);
        ayt::game::GameLoop::instance().shutdown();
        return result;
    }

    // Editor integration suites own process-wide UI, native-window, and
    // runtime singletons. Reusing those singletons across unrelated suites
    // creates order-dependent state that cannot occur in a real editor
    // process. Keep the aggregate CTest entry, but isolate each suite in a
    // child process so every suite receives the production startup baseline.
    int result = 0;
    for (const char* suite : kEditorTestSuites) {
        std::printf("\n[ISOLATED SUITE] %s\n", suite);
        const int suiteResult = runIsolatedSuite(argv[0], suite);
        if (suiteResult != 0) {
            std::fprintf(stderr,
                "[AYEditor tests] suite '%s' exited with code %d\n",
                suite, suiteResult);
            result = 1;
        }
    }
    ayt::game::GameLoop::instance().shutdown();
    return result;
}
