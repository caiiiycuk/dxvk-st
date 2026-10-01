# cmake -DSRC_DIR=<dxvk-st> -DDST=<out/version.h> -P dxvk_version.cmake
# Mirrors meson's vcs_tag(): writes version.h only when the tag changes.
execute_process(
  COMMAND git describe --dirty=+
  WORKING_DIRECTORY "${SRC_DIR}"
  OUTPUT_VARIABLE VCS_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE rc
  ERROR_QUIET)
if(NOT rc EQUAL 0 OR VCS_TAG STREQUAL "")
  set(VCS_TAG "unknown")
endif()
configure_file("${SRC_DIR}/version.h.in" "${DST}" @ONLY)
