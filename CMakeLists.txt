# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The Termobulator Authors.

cmake_minimum_required(VERSION 3.12)
project(termobulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Must be set before targets are defined to take effect
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(libtermobulator SHARED termobulator.cc termobulator.h)
set_target_properties(libtermobulator PROPERTIES
    OUTPUT_NAME termobulator
    VERSION 1.1.0
    SOVERSION 1
)
target_link_libraries(libtermobulator PRIVATE tsm Threads::Threads)

add_executable(termobulator main.cc app_utils.cc app_utils.h interactive_cli.cc interactive_cli.h mcp_server.cc mcp_server.h dsl_interpreter.cc dsl_interpreter.h)
target_link_libraries(termobulator PRIVATE libtermobulator Threads::Threads nlohmann_json::nlohmann_json)

# Testing Support
enable_testing()

add_executable(test_terminal tests/test_terminal.cc)
target_link_libraries(test_terminal PRIVATE libtermobulator Threads::Threads)

add_executable(test_dsl tests/test_dsl.cc dsl_interpreter.cc dsl_interpreter.h app_utils.cc app_utils.h)
target_link_libraries(test_dsl PRIVATE libtermobulator Threads::Threads nlohmann_json::nlohmann_json)

add_test(NAME unit_test COMMAND test_terminal)
add_test(NAME dsl_test COMMAND test_dsl)
add_test(NAME integration_test COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.py)
add_test(NAME mcp_integration_test COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp.py)

# Installation Rules
include(GNUInstallDirs)

install(TARGETS termobulator libtermobulator
    EXPORT termobulatorTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES termobulator.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT termobulatorTargets
    FILE termobulatorTargets.cmake
    NAMESPACE termobulator::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/termobulator
)

# CPack Debian Packaging Configuration
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "termobulator")
set(CPACK_PACKAGE_VERSION "1.1.0")
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

