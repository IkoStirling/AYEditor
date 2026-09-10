cmake_minimum_required(VERSION 3.21)
project(AYEditor
    VERSION 0.2.0
    DESCRIPTION "AYEditor — editor shell + Play/network client + Importer mapping"
)

set(MODULE_NAME AYEditor)

set(AY_RESOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYResource")
set(AY_SHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AYShader")
include("${AY_SHADER_ROOT}/cmake/AYBgfx.cmake")

if(NOT DEFINED AY_ENGINE_ASSETS_SOURCE_DIR)
    get_filename_component(_ay_engine_superproject_root
        "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(AY_ENGINE_ASSETS_SOURCE_DIR
        "${_ay_engine_superproject_root}/EngineAssets"
        CACHE PATH "Versioned engine assets installed with AYEditor")
endif()
if(NOT DEFINED AY_EDITOR_INSTALL_ROOT)
    get_filename_component(_ay_editor_build_tree_name "${CMAKE_BINARY_DIR}" NAME)
    set(AY_EDITOR_INSTALL_ROOT
        "${CMAKE_BINARY_DIR}/install/${_ay_editor_build_tree_name}/AYEditor"
        CACHE PATH "Local portable AYEditor install root")
endif()
option(AY_EDITOR_ENABLE_PRODUCT_INSTALL
    "Generate the portable AYEditor install/stage target" ON)
if(AY_EDITOR_ENABLE_PRODUCT_INSTALL AND
   NOT EXISTS "${AY_ENGINE_ASSETS_SOURCE_DIR}/AYEditor/ui/editor_shell.ui.json")
    message(FATAL_ERROR
        "AYEditor product install requires ${AY_ENGINE_ASSETS_SOURCE_DIR}")
endif()

set(SRC_FILES
    src/AYEditorWorldContext.cpp
    src/AYEditorSession.cpp
    src/AYEditorGameView.cpp
    src/AYEditorPlayRuntime.cpp
    src/AYEditorApp.cpp
    src/AYRegisterDefaultEditorModules.cpp
    src/AYEditorFreecam.cpp
    src/AYEditorSceneDocument.cpp
    src/AYEditorSelection.cpp
    src/AYEditorCommandStack.cpp
    src/AYEditorCommandSystem.cpp
    src/AYEditorDocumentManager.cpp
    src/AYEditorDockViewHost.cpp
    src/AYEditorExtensionRegistry.cpp
    src/AYEditorSelectionContext.cpp
    src/AYEditorWorkspace.cpp
    src/AYEditorTransformGizmo.cpp
    src/AYEditorVisualStyle.cpp
    src/AYEditorAssetDatabase.cpp
    src/AYEditorAssetImportQueue.cpp
    src/AYEditorAssetTilePresenter.cpp
    src/AYEditorAssetDeleteAnalysis.cpp
    src/AYEditorAssetTrash.cpp
    src/AYEditorAssetOperations.cpp
    src/AYEditorProjectAssetFactory.cpp
    src/AYEditorProjectDescriptor.cpp
    src/AYEditorProjectRunner.cpp
    src/AYEditorProjectRuntimeValidator.cpp
    src/AYEditorRecoveryStore.cpp
    src/AYEditorBuiltInExtensions.cpp
    src/AYEditorTilemapCanvas.cpp
    src/AYEditorTileAtlasPicker.cpp
    src/AYEditorComponentPolicy.cpp
    src/AYEditorShortcutRegistry.cpp
    src/EditorAssetPreviewCache.cpp
    src/AYEditorAssetListView.cpp
    src/AYEditorDslDocument.cpp
    src/AYEditorDslExtension.cpp
    src/AYEditorUiLayoutDocument.cpp
    src/AYEditorUiLayoutExtension.cpp
    src/AYEditorStartupSplash.cpp
    src/AYEditorProductPaths.cpp
    src/AYEditor2DViewportModel.cpp
    src/AYEditorSceneCamera.cpp
    src/AYEditorSceneViewWorkspace.cpp
    src/AYEditorTilemapDocument.cpp
    src/AYImporter.cpp
    src/AYImportDialog.cpp
    src/AYImportedCharacterMapper.cpp
    src/EditorChildWindowManager.cpp  # D5+.5 (2026-07-26): child WindowManager wiring
    src/EditorPlayerController.cpp  # INT-01 sample script host registration
    src/AYEditorComponentModule.cpp
    # PR-Dock-TearOff (2026-08-08): per-HWND GDI render backend for
    # promoted child windows. Moved from the demo target into the lib;
    # the whole file is #if defined(_WIN32) so non-Windows builds of
    # the lib stay clean.
    demo/GdiRenderBackend.cpp
    # Audio Editor session core (shared with AYAudio_AudioEditor demo).
    ../AYAudio/demo/audio_editor/AudioEditorSession.cpp
)

set(HEAD_FILES
    AYEditor.h
    include/AYEditor/EditorWorldContext.h
    include/AYEditor/EditorSession.h
    include/AYEditor/EditorGameView.h
    include/AYEditor/EditorPlayRuntime.h
    include/AYEditor/EditorApp.h
    include/AYEditor/EditorPreferences.h
    include/AYEditor/RegisterDefaultEditorModules.h
    include/AYEditor/EditorFreecam.h
    include/AYEditor/EditorSceneDocument.h
    include/AYEditor/EditorSelection.h
    include/AYEditor/EditorCommandStack.h
    include/AYEditor/EditorCommandSystem.h
    include/AYEditor/EditorDocumentManager.h
    include/AYEditor/EditorDockViewHost.h
    include/AYEditor/EditorExtension.h
    include/AYEditor/EditorExtensionRegistry.h
    include/AYEditor/EditorSelectionContext.h
    include/AYEditor/EditorTransformGizmo.h
    include/AYEditor/EditorVisualStyle.h
    include/AYEditor/EditorAssetDatabase.h
    include/AYEditor/EditorAssetImportQueue.h
    include/AYEditor/EditorAssetTilePresenter.h
    include/AYEditor/EditorAssetDeleteAnalysis.h
    include/AYEditor/EditorAssetTrash.h
    include/AYEditor/EditorAssetOperations.h
    include/AYEditor/EditorProjectAssetFactory.h
    include/AYEditor/EditorProjectDescriptor.h
    include/AYEditor/EditorProjectRunner.h
    include/AYEditor/EditorVersion.h
    include/AYEditor/EditorProjectRuntimeValidator.h
    include/AYEditor/EditorRecoveryStore.h
    include/AYEditor/EditorBuiltInExtensions.h
    include/AYEditor/EditorComponentPolicy.h
    include/AYEditor/EditorShortcutRegistry.h
    include/AYEditor/EditorAssetListView.h
    include/AYEditor/EditorDslDocument.h
    include/AYEditor/EditorDslExtension.h
    include/AYEditor/EditorUiLayoutDocument.h
    include/AYEditor/EditorUiLayoutExtension.h
    include/AYEditor/EditorStartupSplash.h
    include/AYEditor/EditorProductPaths.h
    include/AYEditor/Editor2DViewportModel.h
    include/AYEditor/EditorSceneCamera.h
    include/AYEditor/EditorSceneViewWorkspace.h
    include/AYEditor/EditorTilemapDocument.h
    include/AYEditor/Importer.h
    include/AYEditor/ImportDialog.h
    include/AYEditor/ImportedCharacterMapper.h
    include/AYEditor/EditorWorkspace.h
    ../AYDevice/include/AYDevice/WindowManager.h  # D5+.5 (2026-07-26)
    include/AYEditor/EditorPlayerController.h
    include/AYEditor/EditorComponentModule.h
    ../AYAudio/demo/audio_editor/AudioEditorSession.h
)

add_library(${MODULE_NAME} STATIC ${SRC_FILES} ${HEAD_FILES})

target_compile_definitions(${MODULE_NAME} PUBLIC
    AYEDITOR_SOURCE_ABI_VERSION=7)

file(TO_CMAKE_PATH "${AY_ENGINE_ASSETS_SOURCE_DIR}"
    AY_ENGINE_ASSETS_SOURCE_HINT_PATH)
target_compile_definitions(${MODULE_NAME} PRIVATE
    AY_ENGINE_ASSETS_SOURCE_HINT="${AY_ENGINE_ASSETS_SOURCE_HINT_PATH}"
)

target_include_directories(${MODULE_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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
    # Audio Editor session headers (AYAudio/demo/audio_editor).
    ${CMAKE_CURRENT_SOURCE_DIR}/../AYAudio/demo/audio_editor
)

target_link_libraries(${MODULE_NAME} PUBLIC AYUI AYGameLoop AYDevice AYApplication
    PUBLIC AY2DEditorCore
    PRIVATE AYUILayoutEditorCore AYProjectContentValidationCore
    PRIVATE AYEntity AYRenderer AYResource AYResourceImport AYIO AYProject AYScript AYShader AYPlatform AYNetwork
    # v0.3 PR-4: Editor 消费 host->scenes() + _editScene Scene 完整定义需要
    # AYScene 完整 link；PRIVATE 因为 AYGameLoop / AYApplication 不需要
    # 暴露 SceneManager 类型。
    PRIVATE AYScene
    # PhysicsRuntimeModule in the default Editor graph (shared contract with
    # Client/Server; direct helper remains for compatibility callers).
    PRIVATE AYPhysics
    # AudioSubSystem + Audio Editor Tools child (shared with Client).
    PRIVATE AYAudio
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
    target_link_libraries(${MODULE_NAME} PRIVATE comdlg32 user32 gdi32 msimg32)
endif()

add_executable(AYEditorProjectValidator
    tools/AYEditorProjectValidator.cpp)
target_link_libraries(AYEditorProjectValidator PRIVATE AYEditor)
target_link_libraries(AYEditorProjectValidator PRIVATE AYEntity)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/demo
)

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
    AYConfig
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

if(WIN32 AND AY_EDITOR_ENABLE_PRODUCT_INSTALL)
    # The root launcher has no third-party imports.  It starts the real host
    # from AYRuntime, where Windows can resolve every app-local DLL before the
    # host reaches WinMain.
    add_executable(AYEditorLauncher WIN32 demo/AYEditorLauncher.cpp)
    set_target_properties(AYEditorLauncher PROPERTIES OUTPUT_NAME AYEditor)
    target_link_libraries(AYEditorLauncher PRIVATE user32 shell32)
endif()

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

if(WIN32 AND AY_EDITOR_ENABLE_PRODUCT_INSTALL)
    set(AY_EDITOR_INSTALL_COMPONENT AYEditorProduct)

    # Normalize dependency paths before applying the Windows system-directory
    # filters below.  Without this policy, mixed slash/case paths can cause
    # GET_RUNTIME_DEPENDENCIES to recurse through System32.
    if(POLICY CMP0207)
        cmake_policy(SET CMP0207 NEW)
    endif()

    # EngineAssets and AYRuntime are immutable product-owned directories. A
    # reinstall mirrors them from source, while UserAssets and logs are only
    # created and are never removed or overwritten by this cleanup step.
    install(CODE [=[
        set(_ay_product_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        cmake_path(NORMAL_PATH _ay_product_root)
        if(_ay_product_root STREQUAL "" OR
           _ay_product_root STREQUAL "/" OR
           _ay_product_root MATCHES "^[A-Za-z]:/$")
            message(FATAL_ERROR
                "Refusing to stage AYEditor into an unsafe install prefix")
        endif()
        set(_ay_product_marker
            "${_ay_product_root}/.ayeditor-product-root")
        set(_ay_known_legacy_product FALSE)
        if(EXISTS "${_ay_product_root}/AYEditor.exe" AND
           EXISTS "${_ay_product_root}/AYRuntime/AYEditorShell_Demo.exe" AND
           EXISTS "${_ay_product_root}/EngineAssets/AYEditor/ui/editor_shell.ui.json")
            set(_ay_known_legacy_product TRUE)
        endif()
        if((EXISTS "${_ay_product_root}/EngineAssets" OR
            EXISTS "${_ay_product_root}/AYRuntime") AND
           NOT EXISTS "${_ay_product_marker}" AND
           NOT _ay_known_legacy_product)
            message(FATAL_ERROR
                "Refusing to replace unmarked EngineAssets/AYRuntime under "
                "${_ay_product_root}")
        endif()
        file(MAKE_DIRECTORY "${_ay_product_root}")
        file(WRITE "${_ay_product_marker}"
            "Aliyat AYEditor install root\n")
        file(REMOVE_RECURSE
            "${_ay_product_root}/EngineAssets"
            "${_ay_product_root}/AYRuntime")
        file(MAKE_DIRECTORY
            "${_ay_product_root}/UserAssets/Assets"
            "${_ay_product_root}/logs")
    ]=] COMPONENT ${AY_EDITOR_INSTALL_COMPONENT})

    install(TARGETS AYEditorLauncher
        RUNTIME DESTINATION "."
        COMPONENT ${AY_EDITOR_INSTALL_COMPONENT})

    install(TARGETS AYEditorShell_Demo
        RUNTIME_DEPENDENCY_SET AYEditorRuntimeDependencies
        RUNTIME DESTINATION "AYRuntime"
        COMPONENT ${AY_EDITOR_INSTALL_COMPONENT})

    install(RUNTIME_DEPENDENCY_SET AYEditorRuntimeDependencies
        DESTINATION "AYRuntime"
        COMPONENT ${AY_EDITOR_INSTALL_COMPONENT}
        PRE_EXCLUDE_REGEXES
            "[Aa][Pp][Ii]-[Mm][Ss]-[Ww][Ii][Nn]-.*"
            "[Ee][Xx][Tt]-[Mm][Ss]-.*"
            "[Aa]zure[Aa]ttest.*\\.dll"
            "[Hh]vsi[Ff]ile[Tt]rust\\.dll"
            "[Pp]dm[Uu]tilities\\.dll"
            "[Ww]paxholder\\.dll"
        POST_EXCLUDE_REGEXES
            ".*/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/.*"
            ".*/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Ww][Oo][Ww]64/.*")

    install(DIRECTORY "${AY_ENGINE_ASSETS_SOURCE_DIR}/"
        DESTINATION "EngineAssets"
        COMPONENT ${AY_EDITOR_INSTALL_COMPONENT})

    add_custom_target(AYEditorStage
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
            --config "$<CONFIG>"
            --component "${AY_EDITOR_INSTALL_COMPONENT}"
            --prefix "${AY_EDITOR_INSTALL_ROOT}"
        DEPENDS AYEditorLauncher AYEditorShell_Demo
        COMMENT "Stage portable AYEditor in ${AY_EDITOR_INSTALL_ROOT}"
        USES_TERMINAL
        VERBATIM)
endif()

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYEditor v${PROJECT_VERSION} (Play/runtime + net-client + D5.5 Card promotion + Importer mapping) — see ${CMAKE_CURRENT_SOURCE_DIR}/design.md")
