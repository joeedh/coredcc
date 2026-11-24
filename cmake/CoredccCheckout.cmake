# The main checkout of the repository this file is in, or "" if there is none (not a git
# checkout, or coredcc as another project's submodule). Same as mainCheckout() in
# tools/checkout.mjs.
function(coredcc_main_checkout out)
  set(${out} "" PARENT_SCOPE)
  execute_process(
    COMMAND git rev-parse --path-format=absolute --git-common-dir
    WORKING_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
    OUTPUT_VARIABLE common RESULT_VARIABLE result
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT result STREQUAL "0")
    return()
  endif()
  get_filename_component(name "${common}" NAME)
  if(name STREQUAL ".git")
    get_filename_component(main "${common}" DIRECTORY)
    set(${out} "${main}" PARENT_SCOPE)
  endif()
endfunction()
