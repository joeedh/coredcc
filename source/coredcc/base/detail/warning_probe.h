#pragma once

// Used only by tests/compile_fail/warning_probe.cc. With COREDCC_WARNING_PROBE defined,
// this coredcc header contains an unused parameter, which must fail a /W4 /WX build. If
// it compiles, source/ has become a system include directory and coredcc's own warnings
// are silenced.
#ifdef COREDCC_WARNING_PROBE
inline int coredccWarningProbe(int unused)
{
  return 0;
}
#endif
