add_library(wlh_log INTERFACE)
add_library(wlh::log ALIAS wlh_log)

target_include_directories(wlh_log INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

function(wlh_common_enable_log)
    set(one_value_args BACKEND)
    cmake_parse_arguments(WLH_LOG "" "${one_value_args}" "" ${ARGN})

    if(NOT WLH_LOG_BACKEND)
        set(WLH_LOG_BACKEND "PRINTF")
    endif()

    if(TARGET wlh_log_backend)
        return()
    endif()

    if(WLH_LOG_BACKEND STREQUAL "PRINTF")
        add_library(wlh_log_backend STATIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/log_backend_printf.c"
        )
        target_compile_definitions(wlh_log INTERFACE WLH_LOG_BACKEND_PRINTF)
    elseif(WLH_LOG_BACKEND STREQUAL "EASYLOGGER")
        add_library(wlh_log_backend STATIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/log_backend_easylogger.c"
        )
        target_compile_definitions(wlh_log INTERFACE WLH_LOG_BACKEND_EASYLOGGER)
    elseif(WLH_LOG_BACKEND STREQUAL "ESP")
        add_library(wlh_log_backend STATIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/log_backend_esp.c"
        )
        target_compile_definitions(wlh_log INTERFACE WLH_LOG_BACKEND_ESP)
    else()
        message(FATAL_ERROR "Unknown WLH_LOG_BACKEND: ${WLH_LOG_BACKEND}")
    endif()

    target_link_libraries(wlh_log INTERFACE wlh_log_backend)
    target_include_directories(wlh_log_backend PUBLIC
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/include"
    )
    target_compile_features(wlh_log_backend PUBLIC c_std_99)
    set_target_properties(wlh_log_backend PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wlh_log_backend PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()
