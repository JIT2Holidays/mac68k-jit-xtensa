# SDL front-end for the emulator. Built only when SDL2 + SDL2_ttf are
# present; the rest of the project does not depend on it.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
    pkg_check_modules(SDL2TTF QUIET SDL2_ttf)
endif()

if(SDL2_FOUND AND SDL2TTF_FOUND)
    add_executable(mac_gui mac_gui.c)
    target_include_directories(mac_gui PRIVATE
        ${SDL2_INCLUDE_DIRS} ${SDL2TTF_INCLUDE_DIRS})
    target_link_directories(mac_gui PRIVATE
        ${SDL2_LIBRARY_DIRS} ${SDL2TTF_LIBRARY_DIRS})
    target_link_libraries(mac_gui PRIVATE ${SDL2_LIBRARIES} ${SDL2TTF_LIBRARIES})
    # mac68k_host (the --server backend) is expected next to mac_gui, so
    # put both binaries in the build root.
    set_target_properties(mac_gui PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_dependencies(mac_gui mac68k_host)
    message(STATUS "mac_gui: SDL front-end enabled")
else()
    message(STATUS "mac_gui: SDL2 / SDL2_ttf not found — front-end skipped")
endif()
