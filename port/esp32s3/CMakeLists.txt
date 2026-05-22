cmake_minimum_required(VERSION 3.16)

# Repo root = this file's grandparent.
get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mac68k_s3)
