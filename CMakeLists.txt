cmake_minimum_required(VERSION 3.12)
project(AYEditor)

set(MODULE_NAME AYEditor)

set(AY_RESOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYResource")
set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader")

set(SRC_FILES
    src/AYEditorSession.cpp
    src/AYEditorGameView.cpp
    src/AYEditorPlayRuntime.cpp
    src/AYEditorApp.cpp
)

set(HEAD_FILES
    include/AYEditorSession.h
    include/AYEditorGameView.h
    include/AYEditorPlayRuntime.h
    include/AYEditorApp.h
)

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${HEAD_FILES})

target_include_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${MODULE_NAME} PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

target_link_libraries(${MODULE_NAME} PUBLIC AYUI AYGameLoop AYDevice AYApplication
    PRIVATE AYEntity AYRenderer AYResource AYIO)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

if(NOT DEFINED AY_SHADER_SHADERC_PATH)
    if(WIN32)
        set(_ays_shaderc_name "shaderc.exe")
    else()
        set(_ays_shaderc_name "shaderc")
    endif()
    set(AY_SHADER_SHADERC_PATH
        "${AY_SHADER_ROOT}/thirdParty/bgfx-install/debug/bin/${_ays_shaderc_name}")
endif()

add_executable(AYEditorShell_Demo WIN32
    demo/EditorShellDemo.cpp
    demo/GdiRenderBackend.cpp
)

target_include_directories(AYEditorShell_Demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

target_link_libraries(AYEditorShell_Demo PRIVATE
    ${MODULE_NAME}
    AYUI
    AYDevice
)

if(WIN32)
    target_link_libraries(AYEditorShell_Demo PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYEditorShell_Demo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
)

add_custom_command(TARGET AYEditorShell_Demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:AYEditorShell_Demo>/assets
    COMMENT "Copy AYEditor assets next to AYEditorShell_Demo"
)

enable_testing()
add_subdirectory(unittest)
