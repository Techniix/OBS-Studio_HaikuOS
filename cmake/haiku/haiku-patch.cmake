# Helpers for building bundled third-party code on Haiku without modifying the submodules.
#
# The sources are copied into the build tree and small, verified text patches are applied to the copies.
# Each patch must match exactly; if an updated submodule no longer contains the expected text, configuring
# fails loudly instead of silently building unpatched code.
#
# The copying happens at configure time, so every source file read is added to CMAKE_CONFIGURE_DEPENDS of the
# calling directory: changing a bundled source (e.g. a submodule update) re-runs configure and refreshes the copy.

include_guard(GLOBAL)

# haiku_copy_tree(<src_dir> <dst_dir> [EXCLUDE <relative paths...>])
# Copies a source tree; files listed in EXCLUDE are left out (they are produced by haiku_patch_file).
function(haiku_copy_tree src dst)
  cmake_parse_arguments(PARSE_ARGV 2 _HCT "" "" "EXCLUDE")
  set(_args)
  foreach(_e IN LISTS _HCT_EXCLUDE)
    list(APPEND _args PATTERN "${_e}" EXCLUDE)
  endforeach()
  file(MAKE_DIRECTORY "${dst}")
  file(COPY "${src}/" DESTINATION "${dst}" PATTERN ".git" EXCLUDE ${_args})

  file(GLOB_RECURSE _files LIST_DIRECTORIES FALSE "${src}/*")
  list(FILTER _files EXCLUDE REGEX "/\\.git(/|$)")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_files})
endfunction()

# haiku_patch_file(<src_file> <dst_file> <old_1> <new_1> [<old_2> <new_2> ...])
# Writes <dst_file> as <src_file> with each <old> replaced by <new>. The file is only rewritten when its
# content changes, so builds stay incremental.
function(haiku_patch_file src dst)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
  file(READ "${src}" _content)
  # Use ARGV<n> rather than ARGN: patch text may contain semicolons, which list handling would split.
  math(EXPR _last "${ARGC} - 1")
  math(EXPR _odd "${ARGC} % 2")
  if(_odd OR ARGC LESS 4)
    message(FATAL_ERROR "haiku_patch_file: replacements must come in <old> <new> pairs")
  endif()
  foreach(_i RANGE 2 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    set(_old "${ARGV${_i}}")
    set(_new "${ARGV${_j}}")
    string(FIND "${_content}" "${_old}" _pos)
    if(_pos EQUAL -1)
      message(FATAL_ERROR "haiku_patch_file: expected text not found in ${src} (submodule updated?):\n${_old}")
    endif()
    string(REPLACE "${_old}" "${_new}" _content "${_content}")
  endforeach()

  set(_current "")
  if(EXISTS "${dst}")
    file(READ "${dst}" _current)
  endif()
  if(NOT _current STREQUAL _content)
    get_filename_component(_dir "${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${dst}" "${_content}")
  endif()
endfunction()
