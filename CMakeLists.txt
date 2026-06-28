# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.

cmake_minimum_required(VERSION 3.12)
project(termobulator VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Must be set before targets are defined to take effect
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED lua5.4)
include_directories(${LUA_INCLUDE_DIRS})

add_library(libtermobulator SHARED termobulator.cc termobulator.h app_utils.cc app_utils.h)
set_target_properties(libtermobulator PROPERTIES
    OUTPUT_NAME termobulator
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
target_link_libraries(libtermobulator PRIVATE tsm Threads::Threads nlohmann_json::nlohmann_json)
file(GLOB_RECURSE RECIPE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/recipes/*.md")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/inlined_docs.h
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_headers.py
            ${CMAKE_CURRENT_BINARY_DIR}/inlined_docs.h
            ${CMAKE_CURRENT_SOURCE_DIR}/inlined-doc/mcp_instructions.md
            ${CMAKE_CURRENT_SOURCE_DIR}/inlined-doc/terminal_levels.md
            ${RECIPE_FILES}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/inlined-doc/mcp_instructions.md
            ${CMAKE_CURRENT_SOURCE_DIR}/inlined-doc/terminal_levels.md
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_headers.py
            ${RECIPE_FILES}
    COMMENT "Generating inlined_docs.h from markdown sources"
)

add_executable(termobulator main.cc mcp_server.cc mcp_server.h lua_executor.cc lua_executor.h terminal_levels.h ${CMAKE_CURRENT_BINARY_DIR}/inlined_docs.h)
target_link_libraries(termobulator PRIVATE libtermobulator Threads::Threads nlohmann_json::nlohmann_json ${LUA_LIBRARIES})

# Bundled terminfo entries
include(GNUInstallDirs)
find_program(TIC tic)
if(TIC)
    set(TERMINFO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/terminfo/termobulator.ti)
    set(TERMINFO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/terminfo)
    add_custom_command(
        OUTPUT ${TERMINFO_OUTPUT_DIR}/.stamp
        COMMAND ${TIC} -o ${TERMINFO_OUTPUT_DIR} ${TERMINFO_SOURCE}
        COMMAND ${CMAKE_COMMAND} -E touch ${TERMINFO_OUTPUT_DIR}/.stamp
        DEPENDS ${TERMINFO_SOURCE}
        COMMENT "Compiling bundled terminfo entries"
    )
    add_custom_target(terminfo_compiled ALL DEPENDS ${TERMINFO_OUTPUT_DIR}/.stamp)
    add_dependencies(termobulator terminfo_compiled)
    install(DIRECTORY ${TERMINFO_OUTPUT_DIR}/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/termobulator/terminfo
        PATTERN ".stamp" EXCLUDE
    )
    set(TERMINFO_INSTALL_DIR "${CMAKE_INSTALL_FULL_DATADIR}/termobulator/terminfo")
    set(TERMINFO_BUILD_DIR "${TERMINFO_OUTPUT_DIR}")
else()
    message(WARNING "tic not found: bundled terminfo will not be compiled. Synthetic terminal types and color-suffix symbolic names will be unavailable unless the system terminfo database provides them.")
    set(TERMINFO_INSTALL_DIR "")
    set(TERMINFO_BUILD_DIR "")
endif()

configure_file(termobulator_config.h.in termobulator_config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Testing Support
enable_testing()

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIRS})

add_library(crash_test_dummy_lib STATIC tests/crash_test_dummy.cc tests/crash_test_dummy.h)
target_link_libraries(crash_test_dummy_lib PRIVATE ${CURSES_LIBRARIES})

add_executable(crash_test_dummy tests/crash_test_dummy_main.cc)
target_link_libraries(crash_test_dummy PRIVATE crash_test_dummy_lib ${CURSES_LIBRARIES})

add_executable(test_terminal tests/test_terminal.cc)
target_link_libraries(test_terminal PRIVATE libtermobulator Threads::Threads crash_test_dummy_lib ${CURSES_LIBRARIES})

add_executable(test_recipes tests/test_recipes.cc ${CMAKE_CURRENT_BINARY_DIR}/inlined_docs.h)
target_link_libraries(test_recipes PRIVATE libtermobulator Threads::Threads crash_test_dummy_lib ${CURSES_LIBRARIES})

add_executable(test_lua tests/test_lua.cc lua_executor.cc lua_executor.h)
target_link_libraries(test_lua PRIVATE libtermobulator Threads::Threads nlohmann_json::nlohmann_json ${LUA_LIBRARIES})



add_test(NAME unit_test COMMAND test_terminal)
add_test(NAME recipe_test COMMAND test_recipes)

add_test(NAME lua_test COMMAND test_lua)
add_test(NAME mcp_integration_test COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.py)

# Installation Rules

install(TARGETS termobulator libtermobulator
    EXPORT termobulatorTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS crash_test_dummy RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/termobulator)

install(FILES termobulator.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT termobulatorTargets
    FILE termobulatorTargets.cmake
    NAMESPACE termobulator::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/termobulator
)

# CPack Debian Packaging Configuration
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "termobulator")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Headless terminal emulator driver and testing framework")
set(CPACK_PACKAGE_VENDOR "Termobulator Authors")
set(CPACK_PACKAGE_CONTACT "Termobulator Maintainer <termobulator@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Termobulator Maintainer <termobulator@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/MagnusRingman/termobulator")

include(CPack)

