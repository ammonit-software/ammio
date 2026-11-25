cmake_minimum_required(VERSION 3.16)
project(ammio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(AMMIO_TESTS "Build the test suite (ctest)" OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/std:c11 /experimental:c11atomics)
endif()

find_package(Threads REQUIRED)

# open62541 appends its own flags (-std=c99, warnings, ...) to the global
# CMAKE_C_FLAGS cache entry. Remember the original flags so they can be
# restored for everything outside open62541 (see below).
if(NOT DEFINED AMMIO_BASE_C_FLAGS)
    set(AMMIO_BASE_C_FLAGS "${CMAKE_C_FLAGS}" CACHE INTERNAL "")
endif()
set(CMAKE_C_FLAGS "${AMMIO_BASE_C_FLAGS}")

# Version: from `git describe` ("v1.2.0", "v1.2.0-3-gabc1234"), else "dev"
set(AMMIO_VERSION "dev")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(GIT_DESCRIBE)
        set(AMMIO_VERSION ${GIT_DESCRIBE})
    endif()
endif()

# ==============================================================================
# External dependencies (git submodules, built from source, never modified)
# ==============================================================================

set(EXT ${CMAKE_SOURCE_DIR}/external)

# cJSON: JSON parser
add_library(cjson STATIC ${EXT}/cJSON/cJSON.c)
target_include_directories(cjson SYSTEM PUBLIC ${EXT}/cJSON)
if(NOT MSVC)
    target_compile_options(cjson PRIVATE -w)
endif()

# uthash: header-only hash table
add_library(uthash INTERFACE)
target_include_directories(uthash SYSTEM INTERFACE ${EXT}/uthash/src)

# nng: messaging library for the client API
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/nng EXCLUDE_FROM_ALL)

# TRDP (TCNopen): Train Real-time Data Protocol
set(TRDP_DIR ${EXT}/TCNopen/trdp)
add_library(trdp STATIC
    ${TRDP_DIR}/src/common/trdp_pdcom.c
    ${TRDP_DIR}/src/common/trdp_mdcom.c
    ${TRDP_DIR}/src/common/trdp_utils.c
    ${TRDP_DIR}/src/common/trdp_stats.c
    ${TRDP_DIR}/src/common/tlp_if.c
    ${TRDP_DIR}/src/common/tlc_if.c
    ${TRDP_DIR}/src/common/tlm_if.c
    ${TRDP_DIR}/src/vos/common/vos_utils.c
    ${TRDP_DIR}/src/vos/common/vos_mem.c
)
target_include_directories(trdp SYSTEM PUBLIC
    ${TRDP_DIR}/src/api
    ${TRDP_DIR}/src/vos/api
    ${TRDP_DIR}/src/common
)
if(WIN32)
    target_sources(trdp PRIVATE
        ${TRDP_DIR}/src/vos/windows/vos_sock.c
        ${TRDP_DIR}/src/vos/windows/vos_thread.c
        ${TRDP_DIR}/src/vos/windows/vos_shared_mem.c
    )
    target_include_directories(trdp PRIVATE ${TRDP_DIR}/src/vos/windows)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(trdp PUBLIC WIN64 L_ENDIAN MD_SUPPORT=1)
    else()
        target_compile_definitions(trdp PUBLIC WIN32 L_ENDIAN MD_SUPPORT=1)
    endif()
    target_compile_definitions(trdp PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    target_link_libraries(trdp PUBLIC ws2_32 iphlpapi)
else()
    target_sources(trdp PRIVATE
        ${TRDP_DIR}/src/vos/posix/vos_sock.c
        ${TRDP_DIR}/src/vos/posix/vos_thread.c
        ${TRDP_DIR}/src/vos/posix/vos_shared_mem.c
    )
    target_include_directories(trdp PRIVATE ${TRDP_DIR}/src/vos/posix)
    target_compile_definitions(trdp PUBLIC POSIX LINUX _GNU_SOURCE L_ENDIAN MD_SUPPORT=1)
    target_compile_options(trdp PRIVATE -w)
    target_link_libraries(trdp PUBLIC Threads::Threads rt)
endif()

# libmodbus: Modbus TCP/RTU. Its generated headers go to the build directory.
set(LIBMODBUS_DIR ${EXT}/libmodbus/src)
set(LIBMODBUS_GEN ${CMAKE_BINARY_DIR}/libmodbus_gen)
set(LIBMODBUS_VERSION_MAJOR 3)
set(LIBMODBUS_VERSION_MINOR 1)
set(LIBMODBUS_VERSION_MICRO 12)
set(LIBMODBUS_VERSION "3.1.12")
configure_file(${LIBMODBUS_DIR}/modbus-version.h.in ${LIBMODBUS_GEN}/modbus-version.h @ONLY)
if(WIN32)
    configure_file(${LIBMODBUS_DIR}/win32/config.h.win32 ${LIBMODBUS_GEN}/config.h @ONLY)
else()
    file(WRITE ${LIBMODBUS_GEN}/config.h "#pragma once\n#define HAVE_GAI_STRERROR 1\n")
endif()
add_library(modbus_lib STATIC
    ${LIBMODBUS_DIR}/modbus.c
    ${LIBMODBUS_DIR}/modbus-data.c
    ${LIBMODBUS_DIR}/modbus-tcp.c
    ${LIBMODBUS_DIR}/modbus-rtu.c
)
target_include_directories(modbus_lib SYSTEM PUBLIC ${LIBMODBUS_DIR} ${LIBMODBUS_GEN})
target_compile_definitions(modbus_lib PUBLIC DLLBUILD)
if(WIN32)
    target_compile_definitions(modbus_lib PRIVATE FD_SETSIZE=1024)
    target_link_libraries(modbus_lib PUBLIC ws2_32)
else()
    # modbus-tcp.c tests these before it includes config.h
    target_compile_definitions(modbus_lib PRIVATE _GNU_SOURCE HAVE_NETINET_IN_H=1 HAVE_NETINET_IP_H=1)
    target_compile_options(modbus_lib PRIVATE -w)
endif()

# open62541: OPC UA
set(UA_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(UA_LOGLEVEL         600 CACHE STRING "" FORCE)
unset(CMAKE_C_FLAGS) # let open62541 see (and extend) the cache entry
add_subdirectory(${EXT}/open62541 EXCLUDE_FROM_ALL)
set(CMAKE_C_FLAGS "${AMMIO_BASE_C_FLAGS}") # its flags stay in its own directory

# ==============================================================================
# ammio
# ==============================================================================

add_library(ammio_core STATIC
    src/conn.c
    src/json.c
    src/log.c
    src/server.c
    src/vars.c
    src/protocols/modbus.c
    src/protocols/opcua.c
    src/protocols/trdp.c
)
target_include_directories(ammio_core PUBLIC src)
# open62541's (generated) headers as system headers: their warnings are not ours
target_include_directories(ammio_core SYSTEM PRIVATE
    $<TARGET_PROPERTY:open62541,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(ammio_core PUBLIC
    cjson uthash nng trdp modbus_lib open62541 Threads::Threads)
if(NOT WIN32)
    target_link_libraries(ammio_core PUBLIC m)
endif()

if(MSVC)
    target_compile_options(ammio_core PRIVATE /W4)
    target_compile_definitions(ammio_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ammio_core PRIVATE -Wall -Wextra -Wshadow -Wstrict-prototypes)
endif()

add_executable(ammio src/main.c)
target_compile_definitions(ammio PRIVATE AMMIO_VERSION="${AMMIO_VERSION}")
target_link_libraries(ammio PRIVATE ammio_core)

if(AMMIO_TESTS)
    if(WIN32)
        message(FATAL_ERROR "the test suite needs a POSIX system (Linux, macOS)")
    endif()
    enable_testing()
    add_executable(ammio_tests tests/tests.c)
    target_link_libraries(ammio_tests PRIVATE ammio_core)
    add_test(NAME ammio_tests COMMAND ammio_tests)
    add_test(NAME example_configs
        COMMAND ${CMAKE_COMMAND} -DAMMIO=$<TARGET_FILE:ammio> -DDIR=${CMAKE_SOURCE_DIR}/config
                -P ${CMAKE_SOURCE_DIR}/tests/check_configs.cmake)
endif()
