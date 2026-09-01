#include "AYTest.h"
#include "AYGameLoop.h"

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
#include "Test_EditorDslDocument.cpp"           // Phoskia/Logia dock editor
#include "Test_Editor2DTools.cpp"               // 2D viewport + tilemap authoring model

int main(int argc, char* argv[]) {
    const int result = argc > 1
        ? ayt::test::runSuite(argv[1])
        : ayt::test::runAllTests("AYEditor");
    ayt::game::GameLoop::instance().shutdown();
    return result;
}
