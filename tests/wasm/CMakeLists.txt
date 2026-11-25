# The TS suites against the test app's wasm-st module (source/coredcc/testapp):
# - spike_ts: the P1f TS spike under node (run.mjs; needs `npm ci` in this directory);
# - testapp: the NW.js test app's suite (apps/testapp/ctest.mjs; needs `pnpm install`
#   there).
# Registered here, not under source/: source/coredcc is added before enable_testing(),
# and an add_test there is silently dropped. Without their dependencies both exit 77,
# which ctest reports as a skip rather than a silent pass.
if(NOT TARGET coredcc_testapp)
  return()
endif()

find_program(COREDCC_NODE node)
if(NOT COREDCC_NODE)
  message(STATUS "coredcc: spike_ts and testapp need node")
  return()
endif()

add_test(NAME spike_ts
  COMMAND "${COREDCC_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.mjs"
    --module $<TARGET_FILE:coredcc_testapp_node>
    --out "${CMAKE_CURRENT_BINARY_DIR}/spike")
set_tests_properties(spike_ts PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME testapp
  COMMAND "${COREDCC_NODE}" "${PROJECT_SOURCE_DIR}/apps/testapp/ctest.mjs"
    --node-module $<TARGET_FILE:coredcc_testapp_node>
    --module $<TARGET_FILE:coredcc_testapp>)
set_tests_properties(testapp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 RUN_SERIAL TRUE)
