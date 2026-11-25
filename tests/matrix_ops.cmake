# litestl's Matrix::inverse() is NaN for singular matrices, its operator*= returns a dangling
# reference, and Matrix * Vec multiplies by the transpose. scene/ and draw/ use math/transform.h
# instead. This finds the first two (any `*=` counts); reviews have to find the third.
set(bad "")
foreach(dir draw scene)
  file(GLOB_RECURSE files ${ROOT}/source/coredcc/${dir}/*.h ${ROOT}/source/coredcc/${dir}/*.cc)
  foreach(f ${files})
    file(STRINGS ${f} lines REGEX "[.]inverse[(]|[.]invert[(]|[*]=")
    foreach(line ${lines})
      string(APPEND bad "\n${f}: ${line}")
    endforeach()
  endforeach()
endforeach()
if(bad)
  message(FATAL_ERROR "use litestl/math/transform.h:${bad}")
endif()
