# Benchmarks: not in `all`, not tests. tools/bench-bvh.mjs builds and runs this one.
add_executable(bench_bvh EXCLUDE_FROM_ALL bench_bvh.cc)
target_link_libraries(bench_bvh PRIVATE coredcc_bvh)
coredcc_target_flags(bench_bvh)
if(EMSCRIPTEN)
  target_link_options(bench_bvh PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1)
endif()
