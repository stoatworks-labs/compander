cmake_minimum_required(VERSION 3.15)

project(Compander
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too,
# and the error gives no hint what happened.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(COMPANDER_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group,
# and the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX
# plugin alone, with no GL loader anywhere in the configure -- which is what the
# Linux job does, because the FFGL SDK has no Linux path.
option(COMPANDER_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

# ---------------------------------------------------------------------------
# The model. No GL, no FFGL, no host: the compander's laws, the detector, the
# control mapping and the preset table.
#
# Its own library so the OpenFX build and the harness can link exactly this and
# nothing else, and so it is obvious at a glance that none of it can reach for a
# texture. Everything in here is what `cmtest` tests without a GL context.
# ---------------------------------------------------------------------------
add_library(compander_model OBJECT
    source/Compander.h
    source/Compander.cpp
    source/Chain.h
    source/Chain.cpp
    source/Detector.h
    source/Detector.cpp
    source/Controls.h
    source/Controls.cpp
    source/Presets.h
    source/Diag.h
    source/Diag.cpp
)
target_include_directories(compander_model PUBLIC source)
target_compile_definitions(compander_model PUBLIC COMPANDER_VERSION="${PROJECT_VERSION}")
set_target_properties(compander_model PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(COMPANDER_BUILD_FFGL)
# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins. See AGENTS.md for what is wrong with it.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The GL half.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Plugin.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(compander_core OBJECT
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/Common.cpp
    source/shaders/Passes.cpp
    source/Plugin.h
    source/Plugin.cpp
)
target_include_directories(compander_core PUBLIC source)
target_link_libraries(compander_core PUBLIC ffgl::sdk compander_model)
set_target_properties(compander_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too -- for the include path, and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json,
# which nothing else in this file mentions: delete it and every macOS build
# stays green while the Windows job fails at CONFIGURE time.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(compander_core PUBLIC GLEW::GLEW)
endif()

# ⚠️ BOTH object libraries, explicitly. CMake does not propagate an OBJECT
# library's objects transitively THROUGH another object library, so linking only
# compander_core compiles everything, links nothing from compander_model, and
# fails with undefined symbols for the whole model -- which reads like a missing
# source file rather than a linkage rule.
add_library(CompanderEffect MODULE source/PluginEntry.cpp)
target_link_libraries(CompanderEffect PRIVATE compander_core compander_model)

if(APPLE)
    set_target_properties(CompanderEffect PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Compander"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Compander"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.compander"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(CompanderEffect PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
else()
    set_target_properties(CompanderEffect PROPERTIES OUTPUT_NAME "Compander")
endif()

# ---------------------------------------------------------------------------
# Offline harness. Where the plugin is verified rather than merely previewed.
# The model tests need no GL at all; the shader tests drive the real plugin
# class through the real FFGL sequence in a headless core-profile context.
# ---------------------------------------------------------------------------
if(COMPANDER_BUILD_TOOLS AND APPLE)
    add_executable(cmtest tools/cmtest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(cmtest PRIVATE compander_core compander_model z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS CompanderEffect
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

endif()  # COMPANDER_BUILD_FFGL
