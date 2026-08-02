cmake_minimum_required(VERSION 3.20)
project(wlh_host_core VERSION 0.1.0 LANGUAGES C)

include(CTest)

set(WLH_HOST_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
if(NOT TARGET wlh_protocol)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol
        ${CMAKE_CURRENT_BINARY_DIR}/protocol-build EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET wlh_osal)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common
        ${CMAKE_CURRENT_BINARY_DIR}/common-build EXCLUDE_FROM_ALL)
endif()
set(BUILD_TESTING ${WLH_HOST_BUILD_TESTING})

add_library(wlh_host_core STATIC
    src/host.c
    src/host_adc.c
    src/host_bluetooth.c
    src/host_device_info.c
    src/host_eth.c
    src/host_ethernet.c
    src/host_frame.c
    src/host_io.c
    src/host_kv.c
    src/host_link.c
    src/host_ota.c
    src/host_rpc.c
    src/host_user_passthrough.c
    src/host_wire.c
    src/host_wifi.c
)
add_library(wlh::host_core ALIAS wlh_host_core)
target_include_directories(wlh_host_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(wlh_host_core PUBLIC c_std_99)
target_link_libraries(wlh_host_core PUBLIC
    wlh::osal
    wlh::protocol
    wlh::protocol_nanopb
    wlh::log
)
set_target_properties(wlh_host_core PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(wlh_host_core PRIVATE -Wall -Werror)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize")
    target_compile_options(wlh_host_core PRIVATE -Wframe-larger-than=640)
  endif()
endif()

if(WLH_HOST_BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(wlh_host_core_tests
        tests/host_test_support.c
        tests/test_host.c
        tests/test_host_allocation.c
        tests/test_host_bluetooth.c
        tests/test_host_device_user.c
        tests/test_host_eth.c
        tests/test_host_ethernet.c
        tests/test_host_link.c
        tests/test_host_peripherals.c
        tests/test_host_wifi.c
    )
    target_link_libraries(wlh_host_core_tests PRIVATE wlh_host_core Threads::Threads)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_host_core_tests PRIVATE
            -Wall -Werror -UNDEBUG)
    elseif(MSVC)
        target_compile_options(wlh_host_core_tests PRIVATE /UNDEBUG)
    endif()
    add_test(NAME wlh_host_core COMMAND wlh_host_core_tests)
endif()
