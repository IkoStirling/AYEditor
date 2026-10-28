cmake_minimum_required(VERSION 3.21)
project(AYEditorReferenceRewriteFocused LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(AYEditorReferenceRewriteFocused
    ReferenceRewriteFocused.cpp
    ../../src/EditorAssetReferenceRewrite.cpp)
target_include_directories(AYEditorReferenceRewriteFocused PRIVATE ../../src)
enable_testing()
add_test(NAME AYEditorReferenceRewriteFocused
    COMMAND AYEditorReferenceRewriteFocused)
