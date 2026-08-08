# =============================================================================
# SdlSoundManager - Virtools SDL3 Sound Manager Plugin
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(SdlSoundManager
        VERSION 2.1.0.14
        DESCRIPTION "Virtools SDL3 Sound Manager Plugin"
        LANGUAGES C CXX
)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    set(SDLSOUND_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
else ()
    if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(SDLSOUND_IS_TOP_LEVEL ON)
    else ()
        set(SDLSOUND_IS_TOP_LEVEL OFF)
    endif ()
endif ()

if (SDLSOUND_IS_TOP_LEVEL AND CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed. Run CMake from a separate directory: cmake -B build")
endif ()

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif ()
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(SDLSOUND_BUILD_STATIC "Build static library" OFF)
option(SDLSOUND_BUILD_SHARED "Build shared library" ON)
option(SDLSOUND_INSTALL "Generate install target" ${SDLSOUND_IS_TOP_LEVEL})

include(GNUInstallDirs)

if (SDLSOUND_IS_TOP_LEVEL)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMakeTargets")

    get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if (NOT IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
        message(STATUS "[SdlSoundManager] Setting build type to 'Release'")
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
    endif ()

    if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/install" CACHE PATH "Installation directory" FORCE)
        message(STATUS "[SdlSoundManager] Setting install directory to ${CMAKE_INSTALL_PREFIX}")
    endif ()

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif ()

if (MSVC)
    add_compile_definitions(
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
    )
endif ()

if (NOT TARGET SDL3::SDL3)
    find_package(SDL3 CONFIG REQUIRED)
endif ()

if (NOT TARGET VxMath AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VxMath/CMakeLists.txt")
    if (SDLSOUND_BUILD_STATIC AND NOT SDLSOUND_BUILD_SHARED)
        set(VXMATH_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(VXMATH_BUILD_STATIC ON CACHE BOOL "" FORCE)
    endif ()
    if (SDLSOUND_INSTALL)
        set(VXMATH_INSTALL ON CACHE BOOL "Generate install target" FORCE)
    endif ()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../VxMath" "${CMAKE_CURRENT_BINARY_DIR}/VxMath")
endif ()

if (NOT TARGET CK2 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CK2/CMakeLists.txt")
    if (SDLSOUND_BUILD_STATIC AND NOT SDLSOUND_BUILD_SHARED)
        set(CK2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(CK2_BUILD_STATIC ON CACHE BOOL "" FORCE)
    endif ()
    if (SDLSOUND_INSTALL)
        set(CK2_INSTALL ON CACHE BOOL "Generate install target" FORCE)
    endif ()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../CK2" "${CMAKE_CURRENT_BINARY_DIR}/CK2")
endif ()

if (NOT TARGET VxMath OR NOT TARGET CK2)
    message(FATAL_ERROR "SdlSoundManager requires CK2 and VxMath targets.")
endif ()

set(SDLSOUND_SOURCES
        SdlSoundManager.cpp
        SdlSoundManager.h
)

function(sdlsound_configure_target TARGET_NAME)
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_ck2_dep CK2)
    set(_vxmath_dep VxMath)
    get_target_property(_target_type ${TARGET_NAME} TYPE)
    if (_target_type STREQUAL "STATIC_LIBRARY")
        if (TARGET CK2Static)
            set(_ck2_dep CK2Static)
        endif ()
        if (TARGET VxMathStatic)
            set(_vxmath_dep VxMathStatic)
        endif ()
    endif ()
    target_link_libraries(${TARGET_NAME} PRIVATE ${_ck2_dep} ${_vxmath_dep} SDL3::SDL3)
endfunction()

if (SDLSOUND_BUILD_SHARED)
    add_library(SdlSoundManager SHARED ${SDLSOUND_SOURCES})
    sdlsound_configure_target(SdlSoundManager)
    set_target_properties(SdlSoundManager PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif ()

if (SDLSOUND_BUILD_STATIC)
    add_library(SdlSoundManagerStatic STATIC ${SDLSOUND_SOURCES})
    sdlsound_configure_target(SdlSoundManagerStatic)
    set_target_properties(SdlSoundManagerStatic PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
endif ()

if (SDLSOUND_INSTALL)
    if (TARGET SdlSoundManager)
        install(TARGETS SdlSoundManager
                RUNTIME DESTINATION Managers COMPONENT Runtime
                LIBRARY DESTINATION Managers COMPONENT Runtime
        )
    endif ()

    if (SDLSOUND_BUILD_STATIC AND TARGET SdlSoundManagerStatic)
        install(TARGETS SdlSoundManagerStatic
                ARCHIVE DESTINATION lib COMPONENT Development
        )
    endif ()
endif ()

if (SDLSOUND_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "SdlSoundManager Configuration Summary")
    message(STATUS "============================================================")
    message(STATUS "  Version:              ${PROJECT_VERSION}")
    message(STATUS "  Install:              ${SDLSOUND_INSTALL}")
    message(STATUS "  Install Prefix:       ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "============================================================")
    message(STATUS "")
endif ()
