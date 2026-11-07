# OBS CMake Haiku helper functions module

include_guard(GLOBAL)

# Haiku's runtime loader understands RPATH/$ORIGIN and plain ELF shared objects, so the Linux target helpers
# (rundir layout, install rules, RPATH handling) are used unchanged.
include("${CMAKE_SOURCE_DIR}/cmake/linux/helpers.cmake")
