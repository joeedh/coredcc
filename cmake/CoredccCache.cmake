# The build cache that every checkout of coredcc shares (tools/cache.mjs). Include it after
# project() and before any target, in a native Windows build that coredcc is the top of.
include("${CMAKE_CURRENT_LIST_DIR}/CoredccCheckout.cmake")

option(COREDCC_SCCACHE "Use the shared build cache, if tools/cache.mjs has set it up" ON)

# Debug info in the objects: sccache can't cache a PDB that several compiles write to.
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

# Sets `out` to the launcher, or to "" and `why` to the reason.
function(_coredcc_cache_launcher out why)
  set(${out} "" PARENT_SCOPE)
  if(NOT COREDCC_SCCACHE)
    set(${why} "COREDCC_SCCACHE is OFF" PARENT_SCOPE)
    return()
  endif()
  if(DEFINED ENV{COREDCC_CACHE_DIR})
    file(TO_CMAKE_PATH "$ENV{COREDCC_CACHE_DIR}" folder)
    get_filename_component(folder "${folder}" ABSOLUTE)
  else()
    coredcc_main_checkout(main)
    if(NOT main)
      set(${why} "this isn't a repository of its own" PARENT_SCOPE)
      return()
    endif()
    get_filename_component(parent "${main}" DIRECTORY)
    get_filename_component(name "${main}" NAME)
    set(folder "${parent}/.${name}-cache")
  endif()

  set(file "${folder}/launcher.txt")
  if(NOT EXISTS "${file}")
    set(${why} "not set up (node tools/cache.mjs setup)" PARENT_SCOPE)
    return()
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  file(STRINGS "${file}" arguments ENCODING UTF-8)
  list(GET arguments -1 sccache)
  # One that doesn't know the pipe would start a TCP server.
  set(pipe "")
  if(EXISTS "${sccache}")
    file(STRINGS "${sccache}" pipe REGEX "SCCACHE_SERVER_PIPE" LIMIT_COUNT 1)
  endif()
  if(NOT pipe)
    set(${why} "${sccache} is missing or has no named-pipe support" PARENT_SCOPE)
    return()
  endif()
  set(${out} "${CMAKE_COMMAND}" -E env ${arguments} PARENT_SCOPE)
endfunction()

# Normal variables, so that the option takes effect on the next configure. The user's own
# launcher (-D, or the environment variable CMake copies into its cache) wins.
_coredcc_cache_launcher(_coredcc_launcher _coredcc_why)
if(_coredcc_launcher)
  message(STATUS "coredcc: using the build cache")
else()
  message(STATUS "coredcc: not using the build cache: ${_coredcc_why}")
endif()
foreach(_lang C CXX)
  if(DEFINED CACHE{CMAKE_${_lang}_COMPILER_LAUNCHER})
    set(_own "$CACHE{CMAKE_${_lang}_COMPILER_LAUNCHER}")
    message(STATUS "coredcc: CMAKE_${_lang}_COMPILER_LAUNCHER is given: '${_own}'")
    if(_own MATCHES "sccache" AND NOT _own MATCHES "SCCACHE_SERVER_PIPE="
       AND NOT DEFINED ENV{SCCACHE_SERVER_PIPE})
      message(WARNING "This sccache launcher sets no SCCACHE_SERVER_PIPE, so it starts a "
        "TCP server. Clear CMAKE_${_lang}_COMPILER_LAUNCHER to use coredcc's build cache.")
    endif()
  elseif(_coredcc_launcher)
    set(CMAKE_${_lang}_COMPILER_LAUNCHER "${_coredcc_launcher}")
  endif()
endforeach()
