coredcc_test(test_base_status SOURCES base/test_status.cc DEPS coredcc_base)
coredcc_test(test_base_id SOURCES base/test_id.cc DEPS coredcc_base)
coredcc_test(test_base_handle_pool SOURCES base/test_handle_pool.cc DEPS coredcc_base)
coredcc_test(test_base_signal SOURCES base/test_signal.cc DEPS coredcc_base)
coredcc_test(test_base_runtime SOURCES base/test_runtime.cc DEPS coredcc_base)

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
