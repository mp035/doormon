cmake_minimum_required(VERSION 3.16.0)
# Local mDNS component (esp-protocols) when present – avoids Component Manager
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/components/esp-protocols/components)
  list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components/esp-protocols/components)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doormon)
