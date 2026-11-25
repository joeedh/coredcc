coredcc_library(coredcc_bind
  SOURCES
    catalog.cc
    facades.cc
    foreign_command.cc
    graph_facades.cc
    graph_instance_facades.cc
    layout_facades.cc
    library_facades.cc
    manager.cc
    meta.cc
    path_reader.cc
    run_facades.cc
    script_table.cc
  DEPS
    coredcc_graph
    coredcc_layout
)

# Hosts link the manager's export into their module (P1f step 5).
if(EMSCRIPTEN)
  add_library(coredcc_bind_wasm_entry OBJECT wasm_entry.cc)
  target_link_libraries(coredcc_bind_wasm_entry PUBLIC coredcc_bind)
  coredcc_target_flags(coredcc_bind_wasm_entry)
else()
  # The generated TypeScript, type-checked by the spike (P1f step 5).
  add_executable(coredcc_gen_ts gen_ts.cc)
  target_link_libraries(coredcc_gen_ts PRIVATE coredcc_bind binding_generators)
  coredcc_target_flags(coredcc_gen_ts)
endif()
