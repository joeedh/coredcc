# Shared CMake helpers for coredcc targets and tests.

# Turn off C++ exceptions and RTTI for everything configured after this call (directory
# scope), stripping the defaults CMake puts in CMAKE_CXX_FLAGS first.
macro(coredcc_disable_exceptions_and_rtti)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    string(REGEX REPLACE "/EHsc|/EHs|/GR( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/GR-> $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    add_compile_definitions(_HAS_EXCEPTIONS=0)
  else()
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
  endif()
endmacro()

# Warning and conformance flags for coredcc's own targets (never litestl's).
function(coredcc_target_flags target)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(${target} PRIVATE /W4 /utf-8)
    if(COREDCC_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    # clang-cl rejects /Zc:preprocessor as an unused argument.
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    if(COREDCC_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# coredcc_library(name SOURCES... [DEPS targets...])
function(coredcc_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${ARG_SOURCES})
  target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/source")
  target_link_libraries(${name} PUBLIC ${ARG_DEPS})
  coredcc_target_flags(${name})
endfunction()

# Test support: litestl's harness, plus a stub for the dtl header it includes but
# doesn't use (dtl only exists when litestl's own tests are on).
function(_coredcc_test_support target)
  target_include_directories(${target} SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/extern/litestl/tests")
  # A failed assert or abort() exits instead of opening a dialog (tests/support/no_dialogs.cc).
  if(WIN32)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests/support/no_dialogs.cc")
  endif()
endfunction()

# coredcc_test(name [SERIAL] SOURCES... [DEPS targets...])
# One executable per test, registered with CTest. LITESTL_TEST_NO_BREAK stops a failing
# test_assert from calling __debugbreak (6.5 s per failure under ctest otherwise).
# Tests run at the same time (CMakePresets.json). SERIAL is for those whose time limits are
# relative, or near what they take: they run alone.
function(coredcc_test name)
  cmake_parse_arguments(ARG "SERIAL" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_DEPS})
  coredcc_target_flags(${name})
  _coredcc_test_support(${name})
  _coredcc_wasm_test_options(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "LITESTL_TEST_NO_BREAK=1")
  if(ARG_SERIAL)
    set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE)
  endif()
endfunction()

# Under Emscripten a test runs in node (the toolchain's CROSSCOMPILING_EMULATOR):
# EXIT_RUNTIME so main's return value is the exit code, and a pre-js that copies
# process.env into the C environment (COREDCC_TEST_SCALE, LITESTL_TEST_NO_BREAK).
function(_coredcc_wasm_test_options target)
  if(EMSCRIPTEN)
    target_link_options(${target} PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1
      "--pre-js=${PROJECT_SOURCE_DIR}/tests/support/node_env.js")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
      "${PROJECT_SOURCE_DIR}/tests/support/node_env.js")
  endif()
endfunction()

# coredcc_compile_fail(name SOURCE src REGEX regex [DEPS targets...] [DEFINE macro])
# Passes when building SOURCE fails with output matching REGEX. The same source built
# without DEFINE must compile, which is checked by a companion "compile_ok_" test, so a
# typo elsewhere in the file can't make the failing case pass vacuously.
function(coredcc_compile_fail name)
  cmake_parse_arguments(ARG "" "SOURCE;REGEX;DEFINE" "DEPS" ${ARGN})
  if(NOT ARG_DEFINE)
    set(ARG_DEFINE COREDCC_COMPILE_FAIL)
  endif()
  foreach(kind fail ok)
    set(t cf_${kind}_${name})
    add_library(${t} OBJECT EXCLUDE_FROM_ALL ${ARG_SOURCE})
    target_link_libraries(${t} PRIVATE ${ARG_DEPS})
    coredcc_target_flags(${t})
    _coredcc_test_support(${t})
    if(kind STREQUAL "fail")
      target_compile_definitions(${t} PRIVATE ${ARG_DEFINE})
    endif()
    add_test(NAME compile_${kind}_${name}
      COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --target ${t})
    set_tests_properties(compile_${kind}_${name} PROPERTIES RESOURCE_LOCK coredcc_build_tree)
  endforeach()
  set_tests_properties(compile_fail_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${ARG_REGEX}")
endfunction()
