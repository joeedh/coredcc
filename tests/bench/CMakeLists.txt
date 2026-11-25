# Benchmarks: not in `all`, not tests. tools/bench.mjs builds and runs them, and tools/bench-bvh.mjs
# bench_bvh. Don't run a program by itself: nothing else builds it, so it may be an old one.
foreach(bench bvh graph scene)
  add_executable(bench_${bench} EXCLUDE_FROM_ALL bench_${bench}.cc)
  target_link_libraries(bench_${bench} PRIVATE coredcc_${bench})
  coredcc_target_flags(bench_${bench})
  if(EMSCRIPTEN)
    target_link_options(bench_${bench} PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1)
  endif()
endforeach()

add_executable(bench_scene_commands EXCLUDE_FROM_ALL bench_scene_commands.cc)
target_link_libraries(bench_scene_commands PRIVATE coredcc_scene)
coredcc_target_flags(bench_scene_commands)
if(EMSCRIPTEN)
  target_link_options(bench_scene_commands PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1)
endif()
