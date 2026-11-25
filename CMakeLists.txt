cmake_minimum_required(VERSION 3.25)
project(coredcc C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COREDCC_WERROR "Treat warnings in coredcc's own targets as errors" ${PROJECT_IS_TOP_LEVEL})
option(COREDCC_BUILD_TESTS "Build coredcc's tests" ${PROJECT_IS_TOP_LEVEL})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CoredccHelpers)
if(PROJECT_IS_TOP_LEVEL AND WIN32 AND NOT EMSCRIPTEN)
  include(CoredccCache)
endif()

# No exceptions and no RTTI, for litestl too, so _HAS_EXCEPTIONS agrees across every
# translation unit (Unreal builds this way). Only when we own the build: a host project
# such as ubertopo decides its own flags.
if(PROJECT_IS_TOP_LEVEL)
  coredcc_disable_exceptions_and_rtti()
endif()

# link.exe's incremental linking fails at random with LNK1235 ("corrupt or invalid COFF symbol
# table") after a rebase changes many files (docs/debugging.md).
if(PROJECT_IS_TOP_LEVEL AND MSVC)
  foreach(_kind EXE SHARED MODULE)
    foreach(_config DEBUG RELWITHDEBINFO)
      string(APPEND CMAKE_${_kind}_LINKER_FLAGS_${_config} " /INCREMENTAL:NO")
    endforeach()
  endforeach()
endif()

# WASM (Emscripten): litestl needs BUILD_WASM and -DWASM in every TU, or its Vector
# layout and MemHead disagree between TUs and platform/wasm.cc isn't built.
if(EMSCRIPTEN)
  set(BUILD_WASM ON CACHE BOOL "" FORCE)
  add_compile_definitions(WASM)
  option(COREDCC_WASM_THREADS "Build with pthreads and shared memory (wasm-mt)" OFF)
  if(COREDCC_WASM_THREADS)
    add_compile_options(-pthread)
    add_link_options(-pthread -sSHARED_MEMORY=1)
  endif()
  # emcc's default stack is 64 KB; litestl's TS runtime patches memory.grow through
  # wasmMemory and HEAPU8.
  add_link_options(-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608
                   "-sEXPORTED_RUNTIME_METHODS=wasmMemory,HEAPU8")
endif()

# litestl lives in extern/, not source/: it adds its parent directory as an include root,
# and under source/ adding it SYSTEM would silence warnings in coredcc's own headers.
if(NOT TARGET util)
  set(LITESTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(extern/litestl EXCLUDE_FROM_ALL SYSTEM)
  # binding.cc's offsetof on descriptor classes is by design (litestl sets this for
  # WASM), and it uses POSIX strdup.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(binding PRIVATE -Wno-invalid-offsetof -Wno-deprecated-declarations)
  endif()
endif()
# When a host supplied litestl non-SYSTEM, its header warnings would hit coredcc's /WX.
foreach(_lt util platform math binding io lz4)
  if(TARGET ${_lt})
    set_property(TARGET ${_lt} PROPERTY SYSTEM ON)
  endif()
endforeach()

add_subdirectory(source/coredcc)

if(COREDCC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
