coredcc_library(coredcc_bvh
  SOURCES
    build.cc
    kernels.cc
  DEPS
    coredcc_base
)

# The triangle test needs unfused products; kernels.cc says why and has the pragmas too.
if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(_kernel_flags -fno-fast-math -ffp-contract=off)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_kernel_flags /clang:-ffp-contract=off)
else()
  set(_kernel_flags /fp:precise)
endif()
set_source_files_properties(kernels.cc PROPERTIES COMPILE_OPTIONS "${_kernel_flags}")
