# Toolchain file of the wasm presets: finds the pinned emsdk (tools/install-emsdk.mjs) and
# includes its toolchain file. Worktrees use the main checkout's. Also runs in try_compile.
include("${CMAKE_CURRENT_LIST_DIR}/CoredccCheckout.cmake")

if(NOT COREDCC_EMSDK_DIR)
  if(DEFINED ENV{COREDCC_EMSDK_DIR})
    file(TO_CMAKE_PATH "$ENV{COREDCC_EMSDK_DIR}" COREDCC_EMSDK_DIR)
  else()
    get_filename_component(_coredcc_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
    set(COREDCC_EMSDK_DIR "${_coredcc_root}/build/deps/emsdk")
    if(NOT EXISTS "${COREDCC_EMSDK_DIR}")
      coredcc_main_checkout(_coredcc_main)
      set(COREDCC_EMSDK_DIR "${_coredcc_main}/build/deps/emsdk")
    endif()
  endif()
endif()
set(COREDCC_EMSDK_DIR "${COREDCC_EMSDK_DIR}" CACHE PATH "The emsdk checkout")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES COREDCC_EMSDK_DIR)

set(_coredcc_toolchain
  "${COREDCC_EMSDK_DIR}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
if(NOT EXISTS "${_coredcc_toolchain}")
  message(FATAL_ERROR
    "No emsdk in '${COREDCC_EMSDK_DIR}'. Run `node tools/install-emsdk.mjs`, then configure "
    "with a fresh build folder or -DCOREDCC_EMSDK_DIR=<folder>.")
endif()
include("${_coredcc_toolchain}")
