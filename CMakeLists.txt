cmake_minimum_required(VERSION 3.25)
project(coredcc C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COREDCC_WERROR "Treat warnings in coredcc's own targets as errors" ${PROJECT_IS_TOP_LEVEL})
option(COREDCC_BUILD_TESTS "Build coredcc's tests" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CoredccHelpers)

# No exceptions and no RTTI, for litestl too, so _HAS_EXCEPTIONS agrees across every
# translation unit (Unreal builds this way). Only when we own the build: a host project
# such as ubertopo decides its own flags.
if(PROJECT_IS_TOP_LEVEL)
  coredcc_disable_exceptions_and_rtti()
endif()

# litestl lives in extern/, not source/: it adds its parent directory as an include root,
# and under source/ adding it SYSTEM would silence warnings in coredcc's own headers.
if(NOT TARGET util)
  set(LITESTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(extern/litestl EXCLUDE_FROM_ALL SYSTEM)
endif()
# When a host supplied litestl non-SYSTEM, its header warnings would hit coredcc's /WX.
foreach(_lt util platform math)
  if(TARGET ${_lt})
    set_property(TARGET ${_lt} PROPERTY SYSTEM ON)
  endif()
endforeach()

add_subdirectory(source/coredcc)

if(COREDCC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
