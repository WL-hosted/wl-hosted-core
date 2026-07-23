cmake_minimum_required(VERSION 3.20)

project(wlh_protocol VERSION 0.1.0 LANGUAGES C)

include(CTest)

option(WLH_BUILD_NANOPB_SMOKE
    "Build and test generated standard/simulator nanopb code"
    OFF
)

add_library(wlh_protocol STATIC
    src/wire.c
)
add_library(wlh::protocol ALIAS wlh_protocol)

target_include_directories(wlh_protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(wlh_protocol PUBLIC c_std_99)
set_target_properties(wlh_protocol PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wlh_protocol PRIVATE -Wall -Werror)
endif()

add_library(wlh_nanopb_runtime STATIC EXCLUDE_FROM_ALL
    third_party/nanopb/pb_common.c
    third_party/nanopb/pb_encode.c
    third_party/nanopb/pb_decode.c
)
add_library(wlh::nanopb_runtime ALIAS wlh_nanopb_runtime)
target_include_directories(wlh_nanopb_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanopb>
)
target_compile_features(wlh_nanopb_runtime PUBLIC c_std_99)
set_target_properties(wlh_nanopb_runtime PROPERTIES
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

find_program(WLH_PROTOC_EXECUTABLE NAMES protoc)
find_program(WLH_NANOPB_PLUGIN_EXECUTABLE NAMES protoc-gen-nanopb)

if(WLH_PROTOC_EXECUTABLE AND WLH_NANOPB_PLUGIN_EXECUTABLE)
    file(GLOB WLH_STANDARD_PROTO_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto")
    set(WLH_STANDARD_PROTO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/standard")
    set(WLH_SIM_PROTO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sim")

    set(WLH_STANDARD_PROTO_SOURCES)
    set(WLH_STANDARD_PROTO_HEADERS)
    foreach(WLH_PROTO_FILE IN LISTS WLH_STANDARD_PROTO_FILES)
        get_filename_component(WLH_PROTO_NAME "${WLH_PROTO_FILE}" NAME_WE)
        list(APPEND WLH_STANDARD_PROTO_SOURCES
            "${WLH_STANDARD_PROTO_OUTPUT_DIR}/${WLH_PROTO_NAME}.pb.c")
        list(APPEND WLH_STANDARD_PROTO_HEADERS
            "${WLH_STANDARD_PROTO_OUTPUT_DIR}/${WLH_PROTO_NAME}.pb.h")
    endforeach()

    add_custom_command(
        OUTPUT ${WLH_STANDARD_PROTO_SOURCES}
        BYPRODUCTS ${WLH_STANDARD_PROTO_HEADERS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WLH_STANDARD_PROTO_OUTPUT_DIR}"
        COMMAND ${WLH_PROTOC_EXECUTABLE}
            -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
            --plugin=protoc-gen-nanopb=${WLH_NANOPB_PLUGIN_EXECUTABLE}
            --nanopb_out=-q,-f${CMAKE_CURRENT_SOURCE_DIR}/proto/nanopb.options:${WLH_STANDARD_PROTO_OUTPUT_DIR}
            ${WLH_STANDARD_PROTO_FILES}
        DEPENDS ${WLH_STANDARD_PROTO_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/proto/nanopb.options"
        COMMENT "Generating bounded standard nanopb messages"
        VERBATIM
    )
    add_custom_target(generate_standard_nanopb DEPENDS ${WLH_STANDARD_PROTO_SOURCES})

    add_library(wlh_protocol_nanopb STATIC EXCLUDE_FROM_ALL
        ${WLH_STANDARD_PROTO_SOURCES}
    )
    add_library(wlh::protocol_nanopb ALIAS wlh_protocol_nanopb)
    target_include_directories(wlh_protocol_nanopb
        PUBLIC
            $<BUILD_INTERFACE:${WLH_STANDARD_PROTO_OUTPUT_DIR}>
    )
    target_link_libraries(wlh_protocol_nanopb PUBLIC wlh_nanopb_runtime)
    target_compile_features(wlh_protocol_nanopb PUBLIC c_std_99)
    set_target_properties(wlh_protocol_nanopb PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )

    set(WLH_SIM_PROTO_SOURCE "${WLH_SIM_PROTO_OUTPUT_DIR}/sim_sideband.pb.c")
    set(WLH_SIM_PROTO_HEADER "${WLH_SIM_PROTO_OUTPUT_DIR}/sim_sideband.pb.h")
    add_custom_command(
        OUTPUT "${WLH_SIM_PROTO_SOURCE}"
        BYPRODUCTS "${WLH_SIM_PROTO_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WLH_SIM_PROTO_OUTPUT_DIR}"
        COMMAND ${WLH_PROTOC_EXECUTABLE}
            -I "${CMAKE_CURRENT_SOURCE_DIR}/sim/proto"
            --plugin=protoc-gen-nanopb=${WLH_NANOPB_PLUGIN_EXECUTABLE}
            --nanopb_out=-q,-f${CMAKE_CURRENT_SOURCE_DIR}/sim/proto/nanopb.options:${WLH_SIM_PROTO_OUTPUT_DIR}
            "${CMAKE_CURRENT_SOURCE_DIR}/sim/proto/sim_sideband.proto"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/sim/proto/sim_sideband.proto"
            "${CMAKE_CURRENT_SOURCE_DIR}/sim/proto/nanopb.options"
        COMMENT "Generating bounded simulator-only nanopb messages"
        VERBATIM
    )
    add_custom_target(generate_sim_nanopb DEPENDS "${WLH_SIM_PROTO_SOURCE}")

    add_library(wlh_sim_nanopb STATIC EXCLUDE_FROM_ALL
        "${WLH_SIM_PROTO_SOURCE}"
    )
    target_include_directories(wlh_sim_nanopb
        PUBLIC
            $<BUILD_INTERFACE:${WLH_SIM_PROTO_OUTPUT_DIR}>
    )
    target_link_libraries(wlh_sim_nanopb PUBLIC wlh_nanopb_runtime)
    target_compile_features(wlh_sim_nanopb PUBLIC c_std_99)
    set_target_properties(wlh_sim_nanopb PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
else()
    message(STATUS "protoc and protoc-gen-nanopb were not both found; nanopb generation targets are unavailable")
endif()

if(BUILD_TESTING)
    add_executable(wlh_protocol_tests tests/test_wire.c)
    target_link_libraries(wlh_protocol_tests PRIVATE wlh_protocol)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_protocol_tests PRIVATE -Wall -Werror)
    endif()
    add_test(NAME wlh_protocol_wire COMMAND wlh_protocol_tests)
endif()

if(WLH_BUILD_NANOPB_SMOKE)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "WLH_BUILD_NANOPB_SMOKE requires BUILD_TESTING=ON")
    endif()
    if(NOT TARGET wlh_protocol_nanopb OR NOT TARGET wlh_sim_nanopb)
        message(FATAL_ERROR
            "WLH_BUILD_NANOPB_SMOKE requires protoc and protoc-gen-nanopb")
    endif()
    add_executable(wlh_nanopb_generated_smoke tests/test_nanopb_generated.c)
    target_link_libraries(wlh_nanopb_generated_smoke
        PRIVATE
            wlh_protocol_nanopb
            wlh_sim_nanopb
    )
    target_compile_features(wlh_nanopb_generated_smoke PRIVATE c_std_99)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_nanopb_generated_smoke
            PRIVATE -Wall -Werror)
    endif()
    add_test(NAME wlh_nanopb_generated COMMAND wlh_nanopb_generated_smoke)
endif()
