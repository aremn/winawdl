# SPDX-License-Identifier: GPL-3.0-or-later
# AWDL daemon: OWL core on Windows, with explicit RX metadata defaults.
# OWL is GNU C, hence a MinGW build apart from the MSVC radio side.
cmake_minimum_required(VERSION 3.15)
project(winawdl_awdl C)

set(WINAWDL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(OWL "${WINAWDL_ROOT}/upstream/owl")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# compat/ comes first so OWL's Unix includes resolve to the shims.
# -mno-ms-bitfields lays packed structs out as GCC does on Linux.
add_library(owl_build INTERFACE)
target_include_directories(owl_build INTERFACE
    compat "${OWL}/src" "${OWL}/radiotap" "${OWL}/daemon" win
    "${WINAWDL_ROOT}/radio")
target_compile_options(owl_build INTERFACE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/owl_win.h" -mno-ms-bitfields)

add_library(radiotap STATIC "${OWL}/radiotap/radiotap.c")
target_link_libraries(radiotap PRIVATE owl_build)
# Its bounds checks subtract pointers through unsigned long, which is 32 bits
# on Windows. The differences are small, so the truncation cancels out.
target_compile_options(radiotap PRIVATE -Wno-pointer-to-int-cast)

set(OWL_SOURCES log state election sync channel schedule tx frame crc32 wire
    peers version hashmap siphash24 circular_buffer)
list(TRANSFORM OWL_SOURCES PREPEND "${OWL}/src/")
list(TRANSFORM OWL_SOURCES APPEND ".c")
# OWL's RX parser leaves RSSI/FLAGS uninitialized when radiotap omits them.
# Keep the pinned source checkout intact and compile a reviewed copy.
file(READ "${OWL}/src/rx.c" OWL_RX_SOURCE)
foreach(FIELD "signed char rssi;" "uint8_t flags;")
    string(FIND "${OWL_RX_SOURCE}" "${FIELD}" FIELD_POSITION)
    if(FIELD_POSITION EQUAL -1)
        message(FATAL_ERROR "Review OWL RX defaults after updating its source")
    endif()
endforeach()
string(REPLACE "signed char rssi;" "signed char rssi = 0;" OWL_RX_SOURCE "${OWL_RX_SOURCE}")
string(REPLACE "uint8_t flags;" "uint8_t flags = 0;" OWL_RX_SOURCE "${OWL_RX_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/owl_rx.c" "${OWL_RX_SOURCE}")
list(APPEND OWL_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/owl_rx.c")
add_library(awdl STATIC ${OWL_SOURCES})
target_link_libraries(awdl PUBLIC owl_build radiotap)

# The host adapter is kept clear of owl_build: -mno-ms-bitfields must not
# reach the Windows networking structures it fills in.
add_library(host_if STATIC win/host_if.c win/host_frame.c)
target_include_directories(host_if PUBLIC win)
target_compile_options(host_if PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_if PRIVATE iphlpapi)

set(WIN_SOURCES
    win/main.c win/ev_win.c win/io_win.c win/netutils_win.c win/radio_client.c
    win/redact.c win/frame_fixup.c win/tcp_merge.c win/service_bridge.c
    compat/ether.c)
add_executable(awdld "${OWL}/daemon/core.c" ${WIN_SOURCES})
set_source_files_properties(${WIN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(awdld PRIVATE awdl host_if bcrypt)
# Static, so the daemon does not need the MinGW runtime DLLs beside it.
# The wrap routes OWL's log calls through main.c, which can redact them.
target_link_options(awdld PRIVATE -static -Wl,--wrap=log_log)

add_executable(host_frame_test tests/host_frame_test.c)
target_compile_options(host_frame_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_frame_test PRIVATE host_if)
target_link_options(host_frame_test PRIVATE -static)

# Explicit Administrator integration check; never part of unit test runs.
# Creates only the owned temporary Wintun, with no radio or phone traffic.
add_executable(host_neighbor_test tests/host_neighbor_test.c)
target_compile_options(host_neighbor_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_neighbor_test PRIVATE host_if iphlpapi)
target_link_options(host_neighbor_test PRIVATE -static)

add_executable(tcp_merge_test tests/tcp_merge_test.c win/tcp_merge.c)
target_include_directories(tcp_merge_test PRIVATE win)
target_compile_options(tcp_merge_test PRIVATE -Wall -Wextra -Werror)
target_link_options(tcp_merge_test PRIVATE -static)

add_executable(frame_fixup_test tests/frame_fixup_test.c win/frame_fixup.c)
target_include_directories(frame_fixup_test PRIVATE win)
target_compile_options(frame_fixup_test PRIVATE -Wall -Wextra -Werror)
target_link_options(frame_fixup_test PRIVATE -static)

add_executable(service_bridge_test tests/service_bridge_test.c win/service_bridge.c
    win/frame_fixup.c)
target_include_directories(service_bridge_test PRIVATE win)
target_compile_options(service_bridge_test PRIVATE -Wall -Wextra -Werror)
target_link_options(service_bridge_test PRIVATE -static)

add_executable(redact_test tests/redact_test.c win/redact.c)
target_include_directories(redact_test PRIVATE win)
target_compile_options(redact_test PRIVATE -Wall -Wextra -Werror)
target_link_options(redact_test PRIVATE -static)
