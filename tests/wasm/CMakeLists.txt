# The P1f TS spike: coredcc_spike.mjs (an ES6 Emscripten module for node) and the
# node:test suite that drives it (run.mjs; needs `npm ci` in this directory).

add_executable(coredcc_spike spike_host.cc)
target_link_libraries(coredcc_spike PRIVATE coredcc_bind binding_generators)
coredcc_target_flags(coredcc_spike)
set_target_properties(coredcc_spike PROPERTIES SUFFIX ".mjs")

# litestl's runtime entry points (LT_WASM_SYMBOLS), plus the spike's manager.
get_property(_spike_syms GLOBAL PROPERTY LT_WASM_SYMBOLS)
string(REGEX REPLACE "[ \t\r\n]+" ";" _spike_syms "${_spike_syms}")
list(APPEND _spike_syms SPIKE_GetManager malloc free)
list(REMOVE_DUPLICATES _spike_syms)
set(_spike_exports "")
foreach(sym IN LISTS _spike_syms)
  if(sym)
    list(APPEND _spike_exports "_${sym}")
  endif()
endforeach()
list(JOIN _spike_exports "," _spike_exports)
target_link_options(coredcc_spike PRIVATE
  --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node
  "-sEXPORTED_FUNCTIONS=${_spike_exports}")

find_program(COREDCC_NODE node)
if(COREDCC_NODE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/node_modules")
  add_test(NAME spike_ts
    COMMAND "${COREDCC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.mjs"
      --module $<TARGET_FILE:coredcc_spike>
      --out "${CMAKE_CURRENT_BINARY_DIR}/spike")
else()
  message(STATUS "coredcc: spike_ts needs node and `npm ci` in tests/wasm")
endif()
