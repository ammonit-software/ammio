cmake_minimum_required(VERSION 3.13)
project(ammio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/std:c11 /experimental:c11atomics)
endif()

find_package(Threads REQUIRED)

# ==============================================================================
# External dependencies
# ==============================================================================

# cJSON: single-file JSON parser
add_library(cjson STATIC external/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_SOURCE_DIR}/external/cJSON)

# uthash: header-only hash table
add_library(uthash INTERFACE)
target_include_directories(uthash INTERFACE ${CMAKE_SOURCE_DIR}/external/uthash/src)

# nng: messaging library
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/nng)

# TRDP: Train Real-time Data Protocol (built from source)
set(TRDP_DIR ${CMAKE_SOURCE_DIR}/external/TCNopen/trdp)

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

target_include_directories(trdp PUBLIC
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
    target_compile_definitions(trdp PUBLIC WIN32 L_ENDIAN)
    target_link_libraries(trdp PUBLIC ws2_32 iphlpapi)
else()
    target_sources(trdp PRIVATE
        ${TRDP_DIR}/src/vos/posix/vos_sock.c
        ${TRDP_DIR}/src/vos/posix/vos_thread.c
        ${TRDP_DIR}/src/vos/posix/vos_shared_mem.c
    )
    target_include_directories(trdp PRIVATE ${TRDP_DIR}/src/vos/posix)
    target_compile_definitions(trdp PUBLIC POSIX LINUX _GNU_SOURCE L_ENDIAN)
    target_link_libraries(trdp PUBLIC Threads::Threads rt)
endif()

# link libraries
set(EXTERNAL_LINK_LIBRARIES
    cjson
    uthash
    nng
    trdp
    Threads::Threads
)

# ==============================================================================
# ammio
# ==============================================================================

add_executable(ammio
    src/main.c
    src/config.c
    src/log.c
    src/var_table.c
    src/var_server.c
    src/protocols/protocol.c
    src/protocols/trdp.c
)

target_link_libraries(ammio
    ${EXTERNAL_LINK_LIBRARIES}
)
