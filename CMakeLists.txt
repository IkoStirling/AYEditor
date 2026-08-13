cmake_minimum_required(VERSION 3.12)
project(AYEditor
    VERSION 0.1.0
    DESCRIPTION "AYEditor — editor shell + Play/network client + Importer mapping"
)

set(MODULE_NAME AYEditor)

set(AY_RESOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYResource")
set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader")

set(SRC_FILES
    src/AYEditorSession.cpp
    src/AYEditorGameView.cpp
    src/AYEditorPlayRuntime.cpp
    src/AYEditorApp.cpp
    src/AYRegisterDefaultEditorModules.cpp
    src/AYEditorFreecam.cpp
    src/AYImporter.cpp
    src/AYImportDialog.cpp
    src/AYImportedCharacterMapper.cpp
    src/EditorChildWindowManager.cpp  # D5+.5 (2026-07-26): child WindowManager wiring
    src/EditorPlayerController.cpp  # INT-01 sample script host + AYReflect registration
    # PR-Dock-TearOff (2026-08-08): per-HWND GDI render backend for
    # promoted child windows. Moved from the demo target into the lib;
    # the whole file is #if defined(_WIN32) so non-Windows builds of
    # the lib stay clean.
    demo/GdiRenderBackend.cpp
    # UI Layout Editor session core (shared with AYUI_LayoutEditor demo).
    ../AYUI/demo/layout_editor/LayoutEditorSession.cpp
)

set(HEAD_FILES
    include/AYEditorSession.h
    include/AYEditorGameView.h
    include/AYEditorPlayRuntime.h
    include/AYEditorApp.h
    include/AYRegisterDefaultEditorModules.h
    include/AYEditorFreecam.h
    include/AYImporter.h
    include/AYImportDialog.h
    include/AYImportedCharacterMapper.h
    include/EditorChildWindowManager.h  # D5+.5 (2026-07-26)
    include/EditorPlayerController.h
    ../AYUI/demo/layout_editor/LayoutEditorSession.h
)

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${HEAD_FILES})

target_include_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(${MODULE_NAME} PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
    # D5+.5 (2026-07-26): nlohmann/json single-header lives in
    # AYUI's thirdparty_/; reuse instead of pulling a new dep into
    # AYEditor. Matches the AYUI Loader/UILayoutLoader.cpp pattern.
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYUI/thirdparty_/nlohmann
    # UI Layout Editor session headers (demo/layout_editor).
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYUI/demo/layout_editor
)

target_link_libraries(${MODULE_NAME} PUBLIC AYUI AYGameLoop AYDevice AYApplication
    PRIVATE AYEntity AYRenderer AYResource AYIO AYScript AYPlatform AYNetwork
    # v0.3 PR-4: Editor 消费 host->scenes() + _editScene Scene 完整定义需要
    # AYScene 完整 link；PRIVATE 因为 AYGameLoop / AYApplication 不需要
    # 暴露 SceneManager 类型。
    PRIVATE AYScene
    # INT-02 (2026-07-15): DeviceInputProvider lives in
    # AYDeviceSubSystem (not AYDevice core, by dep-graph design).
    # EditorApp.cpp references the type to install it into the Logia
    # bridge; PRIVATE because EditorSession / EditorPlayRuntime don't
    # need it and we don't want AYGameLoop dependency to leak.
    PRIVATE AYDeviceSubSystem)

# Phase 2a: toolbar Import button uses the Win32 file picker
# (AYImportDialog::showOpenFileDialog -> GetOpenFileNameA from
# <commdlg.h>), which lives in the comdlg32.lib Windows SDK library.
# Linux/macOS builds of AYEditor don't import this header (the
# non-Windows branch returns empty), so the link is gated. The demo
# and shutdown-repro targets inherit the link transitively.
if(WIN32)
    target_link_libraries(${MODULE_NAME} PRIVATE comdlg32)
endif()

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

if(NOT DEFINED AY_SHADER_BGFX_COMMON_DIR)
    set(_ays_bgfx_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/bgfx/examples/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../thirdparty/bgfx/examples/common"
    )
    set(AY_SHADER_BGFX_COMMON_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_search)
        if(EXISTS "${_dir}/common.sh")
            set(AY_SHADER_BGFX_COMMON_DIR "${_dir}")
            break()
        endif()
    endforeach()
endif()

if(NOT DEFINED AY_SHADER_BGFX_SRC_DIR AND AY_SHADER_BGFX_COMMON_DIR)
    set(AY_SHADER_BGFX_SRC_DIR "${AY_SHADER_BGFX_COMMON_DIR}/../../src")
endif()

if(NOT DEFINED AY_BGFX_SHADER_INCLUDE_DIR)
    set(_ays_bgfx_include_search
        "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader/thirdParty/bgfx-install/debug/include/bgfx"
    )
    set(AY_BGFX_SHADER_INCLUDE_DIR "")
    foreach(_dir IN LISTS _ays_bgfx_include_search)
        if(EXISTS "${_dir}/bgfx_shader.sh")
            set(AY_BGFX_SHADER_INCLUDE_DIR "${_dir}")
            break()
        endif()
    endforeach()
endif()

add_executable(AYEditorShell_Demo WIN32
    demo/AYEditorDebugHeapEarly.cpp
    demo/EditorShellDemo.cpp
)

target_include_directories(AYEditorShell_Demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
    # EditorShellDemo PR-4 启动日志调用 host->scenes()（SceneManager 类型
    # 定义在 AYScene/include；AYEditor lib 对 AYScene 是 PRIVATE link，
    # 不传递给 demo target）。
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYScene/include
)

target_link_libraries(AYEditorShell_Demo PRIVATE
    ${MODULE_NAME}
    AYRenderer
    AYEntity
    AYGameLoop
    AYResource
    AYUI
    AYDevice
)

target_include_directories(AYEditorShell_Demo PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

if(WIN32)
    target_link_libraries(AYEditorShell_Demo PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYEditorShell_Demo PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_BGFX_SHADER_INCLUDE_HINT="${AY_BGFX_SHADER_INCLUDE_DIR}"
)

add_custom_command(TARGET AYEditorShell_Demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:AYEditorShell_Demo>/assets
    COMMENT "Copy AYEditor assets next to AYEditorShell_Demo"
)

add_executable(AYEditorShutdownRepro WIN32
    demo/AYEditorDebugHeapEarly.cpp
    demo/ShutdownRepro.cpp
)

target_include_directories(AYEditorShutdownRepro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

target_link_libraries(AYEditorShutdownRepro PRIVATE
    ${MODULE_NAME}
    AYRenderer
    AYEntity
    AYGameLoop
    AYResource
    AYUI
    AYDevice
)

target_include_directories(AYEditorShutdownRepro PRIVATE
    ${AY_RESOURCE_ROOT}/include/assetsImpl
    ${AY_RESOURCE_ROOT}/interface
    ${AY_RESOURCE_ROOT}/interface/assetsDefs
)

if(WIN32)
    target_link_libraries(AYEditorShutdownRepro PRIVATE user32 gdi32)
endif()

target_compile_definitions(AYEditorShutdownRepro PRIVATE
    AY_SHADER_SHADERC_HINT="${AY_SHADER_SHADERC_PATH}"
    AY_SHADER_BGFX_COMMON_HINT="${AY_SHADER_BGFX_COMMON_DIR}"
    AY_SHADER_BGFX_SRC_HINT="${AY_SHADER_BGFX_SRC_DIR}"
    AY_BGFX_SHADER_INCLUDE_HINT="${AY_BGFX_SHADER_INCLUDE_DIR}"
)

add_custom_command(TARGET AYEditorShutdownRepro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:AYEditorShutdownRepro>/assets
    COMMENT "Copy AYEditor assets next to AYEditorShutdownRepro"
)

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYEditor v0.1.0 (Play/runtime + net-client + D5.5 Card promotion + Importer mapping) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
