# The test app's WASM module (P1f step 6): one host, linked twice.
# - coredcc_testapp_node: node; the TS spike (tests/wasm) and the TS generator load it.
# - coredcc_testapp: web; the NW.js test app (apps/testapp) loads it. A module that also
#   allows node takes emscripten's node path in NW.js and fails.
# Only coredcc's own wasm-st build: not a host project's, and not wasm-mt yet.
if(NOT (EMSCRIPTEN AND COREDCC_BUILD_TESTS AND NOT COREDCC_WASM_THREADS))
  return()
endif()

add_library(coredcc_testapp_host OBJECT host.cc)
target_link_libraries(coredcc_testapp_host PUBLIC coredcc_bind binding_generators)
coredcc_target_flags(coredcc_testapp_host)

# litestl's runtime entry points (LT_WASM_SYMBOLS), plus the host's manager.
get_property(_ta_syms GLOBAL PROPERTY LT_WASM_SYMBOLS)
string(REGEX REPLACE "[ \t\r\n]+" ";" _ta_syms "${_ta_syms}")
list(APPEND _ta_syms TESTAPP_GetManager malloc free)
list(REMOVE_DUPLICATES _ta_syms)
set(_ta_exports "")
foreach(sym IN LISTS _ta_syms)
  if(sym)
    list(APPEND _ta_exports "_${sym}")
  endif()
endforeach()
list(JOIN _ta_exports "," _ta_exports)

foreach(_variant IN ITEMS node web)
  if(_variant STREQUAL "node")
    set(_target coredcc_testapp_node)
  else()
    set(_target coredcc_testapp)
  endif()
  add_executable(${_target} $<TARGET_OBJECTS:coredcc_testapp_host>)
  # The objects come in as sources: linking the OBJECT library too would add them twice.
  target_link_libraries(${_target} PRIVATE coredcc_bind binding_generators)
  set_target_properties(${_target} PROPERTIES SUFFIX ".mjs")
  target_link_options(${_target} PRIVATE
    --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=${_variant}
    "-sEXPORTED_FUNCTIONS=${_ta_exports}")
endforeach()
