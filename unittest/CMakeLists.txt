cmake_minimum_required(VERSION 3.12)

set(MODULE_NAME AYEditor_UnitTests)

add_executable(${MODULE_NAME} main.cpp)

# The test sources are intentionally included by main.cpp so AYTest's static
# registration cannot be discarded by MSVC. Ninja does not reliably emit
# dependency edges for included files whose extension is .cpp, so make those
# edges explicit; editing any case must rebuild the aggregation unit.
set(AY_EDITOR_EMBEDDED_TEST_SOURCES
    Test_EditorShell.cpp
    Test_EditorImporter.cpp
    Test_EditorPlayRuntime.cpp
    Test_EditorWorldContext.cpp
    Test_EditorChildWindowManager.cpp
    Test_ImportedCharacterMapper.cpp
    Test_EditorTransportDirtyPrompt.cpp
    Test_EditorHierarchy.cpp
    Test_EditorSceneBridge.cpp
    Test_EditorP0Core.cpp
    Test_EditorTransformGizmo.cpp
    Test_EditorAssetBrowser.cpp
)
list(TRANSFORM AY_EDITOR_EMBEDDED_TEST_SOURCES
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set_property(SOURCE main.cpp APPEND PROPERTY
    OBJECT_DEPENDS "${AY_EDITOR_EMBEDDED_TEST_SOURCES}")

target_link_libraries(${MODULE_NAME} PRIVATE AYEditor AYUI AYTest AYResource
    AYIO AYProject AYEntity AYScene)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYEntity
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYEntity/include
)

# AYTest runs every case from its temporary directory.  Give editor tests a
# stable source-tree anchor so suites that load the real shell layout do not
# silently return before executing their assertions.
target_compile_definitions(${MODULE_NAME} PRIVATE
    AY_EDITOR_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# Keep registration symbols in this test-only binary. Production binaries keep
# their normal linker optimization settings.
if(MSVC)
    target_link_options(${MODULE_NAME} PRIVATE "/OPT:NOREF")
endif()

ay_add_test(${MODULE_NAME})
