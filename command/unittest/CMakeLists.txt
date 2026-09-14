add_executable(AYEditorCommandCore_UnitTests main.cpp)

set_property(SOURCE main.cpp APPEND PROPERTY
    OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Test_EditorCommandHistory.cpp")

target_link_libraries(AYEditorCommandCore_UnitTests PRIVATE
    AYEditor::CommandCore
    AYTest
)

ay_add_test(AYEditorCommandCore_UnitTests
    LABELS "editor-command-core;ayeditor")
