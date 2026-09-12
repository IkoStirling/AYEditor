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
    Test_EditorAssetTilePresenter.cpp
    Test_EditorDslDocument.cpp
    Test_Editor2DTools.cpp
    Test_EditorFramework.cpp
    Test_EditorProjectWorkflow.cpp
    Test_EditorUiFlowProjectDescriptor.cpp
    Test_EditorUiFlowEditor.cpp
    Test_EditorGameFlowAssetIntegration.cpp
    Test_EditorGameFlowDocument.cpp
    Test_EditorGameFlowPreview.cpp
    Test_EditorUiDesignerWorkflow.cpp
    Test_EditorRecoveryAndTrash.cpp
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
    AY_EDITOR_TEST_SOURCE_DIR="${AY_ENGINE_ASSETS_SOURCE_DIR}/AYEditor"
    AY_EDITOR_GAMEFLOW_TEST_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

# Keep registration symbols in this test-only binary. Production binaries keep
# their normal linker optimization settings.
if(MSVC)
    target_link_options(${MODULE_NAME} PRIVATE "/OPT:NOREF")
endif()

ay_add_test(${MODULE_NAME})
ay_add_test(${MODULE_NAME}
    NAME AYEditor_2DToolsBaseline
    SUITE Editor2DToolsTests
    LABELS "2d-baseline;ayeditor")
ay_add_test(${MODULE_NAME}
    NAME AYEditor_2DProjectBaseline
    SUITE AYEditor_ProjectWorkflow
    LABELS "2d-baseline;ayeditor")
ay_add_test(${MODULE_NAME}
    NAME AYEditor_MixedSceneBaseline
    SUITE Editor2DToolsTests
    LABELS "2d-3d-mixed;ayeditor")

# Keep the project/Flow wire contract independently buildable. The broad
# aggregation target intentionally includes integration suites and can be
# blocked by an unrelated Scene API transition.
add_executable(AYEditor_UIFlowContractTests UIFlowContractTestMain.cpp)
target_link_libraries(AYEditor_UIFlowContractTests PRIVATE AYEditor AYUI AYTest)
target_include_directories(AYEditor_UIFlowContractTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(AYEditor_UIFlowContractTests PRIVATE
    AY_EDITOR_TEST_SOURCE_DIR="${AY_ENGINE_ASSETS_SOURCE_DIR}/AYEditor"
)
if(MSVC)
    target_link_options(AYEditor_UIFlowContractTests PRIVATE "/OPT:NOREF")
endif()
ay_add_test(AYEditor_UIFlowContractTests
    LABELS "ui-flow;ayeditor")

# Keep GameFlow authoring independently buildable while other editor surfaces
# are changing. The source test file is included by the small registration
# main, matching the aggregate test binary's static-registration strategy.
add_executable(AYEditor_GameFlowTests EditorGameFlowDocumentTestMain.cpp)
set_property(SOURCE EditorGameFlowDocumentTestMain.cpp APPEND PROPERTY
    OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Test_EditorGameFlowDocument.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Test_EditorGameFlowPreview.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Test_EditorGameFlowAssetIntegration.cpp")
target_link_libraries(AYEditor_GameFlowTests PRIVATE
    AYEditor AYApplicationGameFlow AYTest AYIO)
target_include_directories(AYEditor_GameFlowTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(AYEditor_GameFlowTests PRIVATE
    AY_EDITOR_GAMEFLOW_TEST_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(MSVC)
    target_link_options(AYEditor_GameFlowTests PRIVATE "/OPT:NOREF")
endif()
ay_add_test(AYEditor_GameFlowTests LABELS "game-flow;ayeditor")

# Crash recovery has a small standalone target so suffix/restore regressions
# remain testable while unrelated editor integration surfaces are in flight.
add_executable(AYEditor_RecoveryStoreTests EditorRecoveryStoreTestMain.cpp)
set_property(SOURCE EditorRecoveryStoreTestMain.cpp APPEND PROPERTY
    OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Test_EditorRecoveryAndTrash.cpp")
target_link_libraries(AYEditor_RecoveryStoreTests PRIVATE AYEditor AYTest)
target_include_directories(AYEditor_RecoveryStoreTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
if(MSVC)
    target_link_options(AYEditor_RecoveryStoreTests PRIVATE "/OPT:NOREF")
endif()
ay_add_test(AYEditor_RecoveryStoreTests LABELS "recovery;ayeditor")

# Keep default Editor module assembly isolated from the broad UI/editor suite.
# The process-wide ComponentRegistry is sealed as part of the startup contract.
add_executable(DefaultEditorModuleAssemblyTest
    DefaultEditorModuleAssemblyTestMain.cpp
    Test_DefaultEditorModuleAssembly.cpp
)

target_link_libraries(DefaultEditorModuleAssemblyTest PRIVATE
    AYEditor
    AYApplication
    AYEntity
    AYTest
)

target_compile_options(DefaultEditorModuleAssemblyTest PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

if(MSVC)
    target_link_options(DefaultEditorModuleAssemblyTest PRIVATE
        "/FORCE:MULTIPLE"
        "/IGNORE:4005"
        "/IGNORE:4006")
endif()

ay_add_test(DefaultEditorModuleAssemblyTest)
