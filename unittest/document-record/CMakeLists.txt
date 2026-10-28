cmake_minimum_required(VERSION 3.25)
project(AYEditorDocumentRecordFocused LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(AYEditorDocumentRecordFocused
    DocumentRecordFocused.cpp
    ../../src/AYEditorDocumentManager.cpp
    ../../src/AYEditorExtensionRegistry.cpp)
target_include_directories(AYEditorDocumentRecordFocused PRIVATE
    ../../include
    ../../../AYUI/include
    ../../../../AYFoundation/AYMath/include)
if(MSVC)
    target_compile_options(AYEditorDocumentRecordFocused PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME AYEditorDocumentRecordFocused COMMAND AYEditorDocumentRecordFocused)
