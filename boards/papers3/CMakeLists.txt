cmake_minimum_required(VERSION 3.16)

# Repo root = this file's great-grandparent (boards/papers3/ -> repo).
get_filename_component(MAC68K_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
# FastEPD e-paper driver (submodule). Point at the component dir itself, not the
# third_party/ parent (which also holds the non-IDF minivmac submodule).
list(APPEND EXTRA_COMPONENT_DIRS "${MAC68K_ROOT}/third_party/FastEPD")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mac68k_papers3)

# FastEPD is a vendored third-party component; this project builds with -Werror,
# which FastEPD's source does not satisfy (e.g. -Wmissing-field-initializers).
# Relax it for that component only, from here, so the submodule stays pristine.
idf_component_get_property(fastepd_lib FastEPD COMPONENT_LIB)
target_compile_options(${fastepd_lib} PRIVATE -Wno-error)
