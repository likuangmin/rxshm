cmake_minimum_required(VERSION 3.16)
project(rxshm VERSION 1.0.0 LANGUAGES C)
include(GNUInstallDirs)

# The header-only library: rxshm::rxshm. Its include directory is SYSTEM, so
# that a C++ consumer's -Wold-style-cast or -Wshadow stay quiet about it, and
# it needs POSIX.1-2008 (clock_gettime), which strict -std=c11 lacks
add_library(rxshm INTERFACE)
add_library(rxshm::rxshm ALIAS rxshm)
target_include_directories(rxshm SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/c/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(rxshm INTERFACE _POSIX_C_SOURCE=200809L)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RXSHM_TOP_LEVEL ON)
else()
    set(RXSHM_TOP_LEVEL OFF)
endif()
option(RXSHM_BUILD_TOOLS "Build sdr_shm_cat and sdr_shm_put" ${RXSHM_TOP_LEVEL})
option(RXSHM_BUILD_TESTS "Build the tests (needs Python 3.10 or later)" ${RXSHM_TOP_LEVEL})
option(RXSHM_INSTALL "Install the header, the tools and the CMake package" ${RXSHM_TOP_LEVEL})
option(RXSHM_WERROR "Warnings are errors" OFF)

set(RXSHM_WARNINGS -Wall -Wextra)
if(RXSHM_WERROR)
    list(APPEND RXSHM_WARNINGS -Werror)
endif()

function(rxshm_program name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE rxshm::rxshm)
    target_compile_options(${name} PRIVATE ${RXSHM_WARNINGS})
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                          C_EXTENSIONS OFF CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF)
endfunction()

if(RXSHM_BUILD_TOOLS OR RXSHM_BUILD_TESTS)
    rxshm_program(sdr_shm_cat c/tools/sdr_shm_cat.c)
    rxshm_program(sdr_shm_put c/tools/sdr_shm_put.c)
endif()

if(RXSHM_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    rxshm_program(writer_test tests/c/writer_test.c)
    rxshm_program(layout tests/c/layout.c)
    # As RealSuite builds: through the SYSTEM include, these stay quiet
    rxshm_program(cxx_test tests/c/cxx.cpp)
    target_compile_options(cxx_test PRIVATE -Wold-style-cast -Wshadow)
    add_test(NAME rxshm
             COMMAND ${Python3_EXECUTABLE} -m unittest -v
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(rxshm PROPERTIES ENVIRONMENT "RXSHM_BIN=${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(RXSHM_INSTALL)
    include(CMakePackageConfigHelpers)
    install(DIRECTORY c/include/rxshm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS rxshm EXPORT rxshmTargets)
    if(RXSHM_BUILD_TOOLS)
        install(TARGETS sdr_shm_cat sdr_shm_put)
    endif()
    install(EXPORT rxshmTargets NAMESPACE rxshm::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rxshm)
    configure_package_config_file(cmake/rxshmConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/rxshmConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rxshm)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rxshmConfigVersion.cmake
        COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rxshmConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/rxshmConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rxshm)
endif()
