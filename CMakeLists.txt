cmake_minimum_required(VERSION 3.16)
project(cs_translate LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(cs_translate SHARED
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/dict.cpp
    src/utf8.cpp
    src/chat.cpp
    src/queue.cpp
    src/translate.cpp
    src/hooks.cpp
)

set_target_properties(cs_translate PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cs_translate"
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories(cs_translate PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(cs_translate PRIVATE winhttp)
target_compile_definitions(cs_translate PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cs_translate PRIVATE /utf-8 /W3)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/translate_test.cpp")
    add_executable(translate_test
        tools/translate_test.cpp
        src/log.cpp
        src/translate.cpp
        src/utf8.cpp
        src/chat.cpp
        src/config.cpp
        src/dict.cpp
    )
    target_include_directories(translate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(translate_test PRIVATE winhttp shell32)
    target_compile_definitions(translate_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(translate_test PRIVATE /utf-8 /W3)
    set_target_properties(translate_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

