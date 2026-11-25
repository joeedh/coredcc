coredcc_test(test_base_assert SOURCES base/test_assert.cc DEPS coredcc_base)
coredcc_test(test_base_status SOURCES base/test_status.cc DEPS coredcc_base)
coredcc_test(test_base_id SOURCES base/test_id.cc DEPS coredcc_base)
coredcc_test(test_base_handle_pool SOURCES base/test_handle_pool.cc DEPS coredcc_base)
coredcc_test(test_base_signal SOURCES base/test_signal.cc DEPS coredcc_base)
coredcc_test(test_base_runtime SOURCES base/test_runtime.cc DEPS coredcc_base)
coredcc_test(test_base_bytes SOURCES base/test_bytes.cc DEPS coredcc_base)
coredcc_test(test_base_text SOURCES base/test_text.cc DEPS coredcc_base)
coredcc_test(test_base_iface SOURCES base/test_iface.cc DEPS coredcc_base)
coredcc_test(test_base_iface_rules SOURCES base/test_iface_rules.cc DEPS coredcc_base)
coredcc_test(test_bvh_kernels SOURCES bvh/test_bvh_kernels.cc DEPS coredcc_bvh)
coredcc_test(test_bvh_build SOURCES bvh/test_bvh_build.cc DEPS coredcc_bvh)
coredcc_test(test_bvh_any_source SOURCES bvh/test_bvh_any_source.cc DEPS coredcc_bind coredcc_bvh)
coredcc_test(test_bvh_queries SOURCES bvh/test_bvh_queries.cc DEPS coredcc_bvh)
coredcc_test(test_props_value SOURCES props/test_value.cc DEPS coredcc_props)
coredcc_test(test_props_def SOURCES props/test_def.cc DEPS coredcc_props)
coredcc_test(test_props_units SOURCES props/test_units.cc DEPS coredcc_props)
coredcc_test(test_props_value_io SOURCES props/test_value_io.cc DEPS coredcc_props)
coredcc_test(test_parse_path SOURCES parse/test_path.cc DEPS coredcc_parse)
coredcc_test(test_parse_invocation SOURCES parse/test_invocation.cc DEPS coredcc_parse)
coredcc_test(test_parse_fuzz SOURCES parse/test_fuzz.cc DEPS coredcc_parse)
coredcc_test(test_lib_library SOURCES lib/test_library.cc DEPS coredcc_lib)
coredcc_test(test_lib_lifetime SOURCES lib/test_lifetime.cc DEPS coredcc_lib)
coredcc_test(test_lib_block_io SOURCES lib/test_block_io.cc DEPS coredcc_lib)
coredcc_test(test_lib_placeholder SOURCES lib/test_placeholder.cc DEPS coredcc_lib)
coredcc_test(test_lib_attachment SOURCES lib/test_attachment.cc DEPS coredcc_lib)
coredcc_test(test_api_resolve SOURCES api/test_resolve.cc DEPS coredcc_api)
coredcc_test(test_api_change_bus SOURCES api/test_change_bus.cc DEPS coredcc_api)
coredcc_test(test_command_history SOURCES command/test_command_history.cc DEPS coredcc_command)
coredcc_test(test_command_values SOURCES command/test_command_values.cc DEPS coredcc_command)
coredcc_test(test_command_blocks SOURCES command/test_command_blocks.cc DEPS coredcc_command)
coredcc_test(test_command_block_edits SOURCES command/test_command_block_edits.cc DEPS coredcc_command)
coredcc_test(test_command_review SOURCES command/test_command_review.cc DEPS coredcc_command)
coredcc_test(test_command_evaluating SOURCES command/test_command_evaluating.cc DEPS coredcc_command)
coredcc_test(test_bind_facades SOURCES bind/test_bind_facades.cc DEPS coredcc_bind)
coredcc_test(test_bind_foreign SOURCES bind/test_bind_foreign.cc DEPS coredcc_bind)
coredcc_test(test_bind_review SOURCES bind/test_bind_review.cc DEPS coredcc_bind)
if(WIN32)
  coredcc_test(test_compile_embed SOURCES compile_embed.cc DEPS coredcc_bvh coredcc_command)
endif()

# Compile-fail tests. Each also builds the same file without the define, which must succeed.
if(COREDCC_WERROR)
  # Catches any change that turns source/ into a system include directory (P0 pressure
  # test: adding litestl SYSTEM from inside source/ silenced coredcc's own warnings).
  coredcc_compile_fail(warning_probe
    SOURCE compile_fail/warning_probe.cc
    REGEX "C4100|unused-parameter|unused parameter"
    DEFINE COREDCC_WARNING_PROBE
    DEPS coredcc_base)
  coredcc_compile_fail(status_nodiscard
    SOURCE compile_fail/status_nodiscard.cc
    REGEX "C4834|unused-result|nodiscard"
    DEPS coredcc_base)
endif()
coredcc_compile_fail(result_of_status
  SOURCE compile_fail/result_of_status.cc
  REGEX "use Status directly"
  DEPS coredcc_base)
coredcc_compile_fail(iface_misspelled_slot
  SOURCE compile_fail/iface_misspelled_slot.cc
  REGEX "C2039|no member named"
  DEPS coredcc_base)
coredcc_compile_fail(iface_wrong_args
  SOURCE compile_fail/iface_wrong_args.cc
  REGEX "C2660|C2664|too few arguments|no matching function"
  DEPS coredcc_base)
coredcc_compile_fail(iface_wrong_arg_type
  SOURCE compile_fail/iface_wrong_arg_type.cc
  REGEX "C2664|could not bind|cannot initialize|no matching function|no viable conversion"
  DEPS coredcc_base)

# The P1f TS spike, against the wasm-st module (tests/wasm/).
if(EMSCRIPTEN)
  add_subdirectory(wasm)
endif()
