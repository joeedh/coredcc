# Benchmarks: not in `all`, not tests. tools/bench-bvh.mjs builds and runs bench_bvh;
# bench_graph.cc says how to run that one.
foreach(bench bvh graph scene)
  add_executable(bench_${bench} EXCLUDE_FROM_ALL bench_${bench}.cc)
  target_link_libraries(bench_${bench} PRIVATE coredcc_${bench})
  coredcc_target_flags(bench_${bench})
  if(EMSCRIPTEN)
    target_link_options(bench_${bench} PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1)
  endif()
endforeach()
