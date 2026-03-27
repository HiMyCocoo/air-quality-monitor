cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

# Keep PROJECT_VER as the release baseline, but prefer an explicit release
# version or an exact git tag when embedding firmware metadata.
if(DEFINED ENV{AIRMON_RELEASE_VERSION} AND NOT "$ENV{AIRMON_RELEASE_VERSION}" STREQUAL "")
  set(PROJECT_VER "$ENV{AIRMON_RELEASE_VERSION}")
else()
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
      WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
      RESULT_VARIABLE AIRMON_GIT_TAG_RESULT
      OUTPUT_VARIABLE AIRMON_GIT_TAG
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(AIRMON_GIT_TAG_RESULT EQUAL 0 AND AIRMON_GIT_TAG MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
      set(PROJECT_VER "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(air_quality_monitor)
