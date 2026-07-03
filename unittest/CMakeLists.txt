cmake_minimum_required(VERSION 3.12)

set(MODULE_NAME AYEditor_UnitTests)

add_executable(${MODULE_NAME}
    main.cpp
)

target_link_libraries(${MODULE_NAME} PRIVATE AYEditor AYUI AYTest)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ${MODULE_NAME} COMMAND ${MODULE_NAME})
