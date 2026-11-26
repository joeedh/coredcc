# coredcc built as Unreal builds an Editor module (docs/plans/2026-11-25-p4-tools.md, "The
# Unreal spike"): coredcc as a subproject, so the host's flags are what count. UE 5.9's
# Win64 editor modules have exceptions on and RTTI off, and use the release CRT (/MD) without
# iterator debugging. tools/unreal-spike.mjs configures this with RelWithDebInfo.
cmake_minimum_required(VERSION 3.25)
project(coredcc_unreal_spike C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
string(REGEX REPLACE "/GR( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:CXX>:/GR-> /utf-8)
add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)

set(COREDCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(COREDCC_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. coredcc)
