cmake_minimum_required(VERSION 3.20)

project(DS3RawMouseFix VERSION 1.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "DS3RawMouseFix requires the Microsoft Visual C++ toolchain on Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "DS3RawMouseFix must be configured for Win32/x86 because Dead Space 3 is 32-bit.")
endif()

set(DS3_RAW_MOUSE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version written to the DS3 Raw Mouse Fix log")
option(DS3_RAW_MOUSE_BUILD_LEGACY_LOADER
    "Build the legacy post-launch injector for development comparisons" OFF)

add_library(ds3_raw_mouse MODULE DS3RawMouse.cpp)
target_compile_features(ds3_raw_mouse PRIVATE cxx_std_20)
target_compile_definitions(ds3_raw_mouse PRIVATE
    DS3_RAW_MOUSE_PRODUCT_BUILD=1
    DS3_RAW_MOUSE_VERSION="${DS3_RAW_MOUSE_VERSION}"
    DS3_RAW_MOUSE_LOG_FILENAME="DS3RawMouse.log")
set_target_properties(ds3_raw_mouse PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DS3RawMouse"
    SUFFIX ".asi"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(TARGET ds3_raw_mouse APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ds3_raw_mouse.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/dinput8.ini")
target_compile_options(ds3_raw_mouse PRIVATE /W4 /permissive-)

add_custom_command(TARGET ds3_raw_mouse POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/ds3_raw_mouse.ini"
        "$<TARGET_FILE_DIR:ds3_raw_mouse>/ds3_raw_mouse.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/dinput8.ini"
        "$<TARGET_FILE_DIR:ds3_raw_mouse>/dinput8.ini"
    VERBATIM)

if(DS3_RAW_MOUSE_BUILD_LEGACY_LOADER)
    add_executable(ds3_raw_mouse_loader DS3RawMouseLoader.cpp)
    target_compile_features(ds3_raw_mouse_loader PRIVATE cxx_std_20)
    target_compile_definitions(ds3_raw_mouse_loader PRIVATE
        UNICODE
        _UNICODE
        DS3_RAW_MOUSE_LOADER_BUILD=1
        DS3_RAW_MOUSE_DEFAULT_DLL=L"DS3RawMouse.asi")
    target_compile_options(ds3_raw_mouse_loader PRIVATE /W4 /permissive-)
    set_target_properties(ds3_raw_mouse_loader PROPERTIES
        OUTPUT_NAME "DS3RawMouseLoader"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
