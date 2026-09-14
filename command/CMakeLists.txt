set(AY_EDITOR_COMMAND_CORE AYEditorCommandCore)

add_library(${AY_EDITOR_COMMAND_CORE} STATIC
    src/EditorCommandHistory.cpp
    include/AYEditorCommand/EditorCommandHistory.h
    include/AYEditorCommand/EditorCommandVersion.h
)
add_library(AYEditor::CommandCore ALIAS ${AY_EDITOR_COMMAND_CORE})

target_include_directories(${AY_EDITOR_COMMAND_CORE} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(${AY_EDITOR_COMMAND_CORE} PUBLIC
    AYEDITOR_COMMAND_SOURCE_ABI_VERSION=1
)

target_compile_features(${AY_EDITOR_COMMAND_CORE} PUBLIC cxx_std_23)

if(BUILD_TESTING)
    add_subdirectory(unittest)
endif()
