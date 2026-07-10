#include "AYTest.h"
#include "AYGameLoop.h"

#include "Test_EditorShell.cpp"
#include "Test_EditorImporter.cpp"
#include "Test_EditorPlayRuntime.cpp"
#include "Test_ImportedCharacterMapper.cpp"

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
