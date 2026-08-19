#include "AYTest.h"
#include "AYGameLoop.h"

#include "Test_EditorShell.cpp"
#include "Test_EditorImporter.cpp"
#include "Test_EditorPlayRuntime.cpp"
// D5+.5 (2026-07-26): D5 + ImportedMapper compile as their own TU
// (CMakeLists); kept #include would duplicate symbols.
//
// Pre-existing AYTest registration quirk: When Test_*.cpp files are
// built as their own TU, MSVC's linker strips them when nothing
// in main's TU references them. Workaround: pull each Test_*.cpp's
// case definitions via the cpp-as-header #include path, but ONLY
// the test functions (without their static _reg_* auto-init), and
// drive registration from main.cpp's own static initializer which
// the linker must keep.
//
// Concretely: we keep the regular cpp-as-header path (#include)
// for all 5 suites. This drops the `#include` requirement for D5 +
// ImportedMapper (per the CMakeLists split) — see the test files'
// TU build for those.
// Keep this aggregation explicit: CMake compiles main.cpp, so these included
// test sources share one test-registration translation unit.
//
// D5 child-window cases + Imported mapper test cases are registered
// below via direct `registerTest` calls from main() so the linker
// can't strip their references.
#include "Test_EditorChildWindowManager.cpp"   // D5+.5 case definitions
#include "Test_ImportedCharacterMapper.cpp"     // master test cases
#include "Test_EditorTransportDirtyPrompt.cpp"  // v0.3 PR-4 case definitions
#include "Test_EditorHierarchy.cpp"            // v0.3+ PR-5 case definitions
#include "Test_EditorSceneBridge.cpp"          // v0.4 PR-1 Scene runtime bridge

void runTest()
{
    ayt::test::runAllTests("AYEditor");
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    runTest();
    ayt::game::GameLoop::instance().shutdown();
    return 0;
}
