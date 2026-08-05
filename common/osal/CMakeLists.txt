add_library(wlh_osal INTERFACE)
add_library(wlh::osal ALIAS wlh_osal)

target_include_directories(wlh_osal INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(wlh_osal INTERFACE c_std_99)

function(wlh_common_enable_posix_osal)
    set(one_value_args BUILD_TESTING)
    cmake_parse_arguments(WLH_POSIX "" "${one_value_args}" "" ${ARGN})

    if(NOT TARGET wlh_posix_osal)
        find_package(Threads REQUIRED)

        add_library(wlh_posix_osal STATIC
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/posix_osal.c"
        )
        add_library(wlh::posix_osal ALIAS wlh_posix_osal)
        target_compile_features(wlh_posix_osal PUBLIC c_std_11)
        target_link_libraries(wlh_posix_osal PUBLIC wlh::osal Threads::Threads)
        set_target_properties(wlh_posix_osal PROPERTIES
            C_EXTENSIONS OFF
            POSITION_INDEPENDENT_CODE ON
        )
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(wlh_posix_osal PRIVATE
                -Wall -Werror
            )
        endif()
    endif()

    if(WLH_POSIX_BUILD_TESTING AND NOT TARGET wlh_posix_osal_tests)
        add_executable(wlh_posix_osal_tests
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests/test_posix_osal.c"
        )
        target_compile_features(wlh_posix_osal_tests PRIVATE c_std_11)
        target_link_libraries(wlh_posix_osal_tests PRIVATE wlh::posix_osal)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(wlh_posix_osal_tests PRIVATE
                -Wall -Werror -UNDEBUG
            )
        elseif(MSVC)
            target_compile_options(wlh_posix_osal_tests PRIVATE /UNDEBUG)
        endif()
        add_test(NAME wlh_posix_osal COMMAND wlh_posix_osal_tests)
    endif()
endfunction()

function(wlh_common_enable_freertos_osal)
    if(TARGET wlh_freertos_osal)
        return()
    endif()

    add_library(wlh_freertos_osal STATIC
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/freertos_osal.c"
    )
    add_library(wlh::freertos_osal ALIAS wlh_freertos_osal)
    target_compile_features(wlh_freertos_osal PUBLIC c_std_99)
    target_link_libraries(wlh_freertos_osal PUBLIC wlh::osal)
    set_target_properties(wlh_freertos_osal PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        # Use only -Wall -Werror to stay compatible with vendor SDK headers
        # (e.g. ESP-IDF newlib) that rely on GNU extensions such as #include_next.
        target_compile_options(wlh_freertos_osal PRIVATE
            -Wall -Werror
        )
    endif()
endfunction()

function(wlh_common_enable_rtt_osal)
    set(multi_value_args RT_THREAD_INCLUDE_DIRS)
    cmake_parse_arguments(WLH_RTT "" "" "${multi_value_args}" ${ARGN})

    if(TARGET wlh_rtt_osal)
        return()
    endif()

    add_library(wlh_rtt_osal STATIC
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/rtt_osal.c"
    )
    add_library(wlh::rtt_osal ALIAS wlh_rtt_osal)
    target_compile_features(wlh_rtt_osal PUBLIC c_std_99)
    target_link_libraries(wlh_rtt_osal PUBLIC wlh::osal)
    set_target_properties(wlh_rtt_osal PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    if(WLH_RTT_RT_THREAD_INCLUDE_DIRS)
        target_include_directories(wlh_rtt_osal PRIVATE
            ${WLH_RTT_RT_THREAD_INCLUDE_DIRS}
        )
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        # Use only -Wall -Werror to stay compatible with vendor SDK headers
        # that rely on GNU extensions such as #include_next.
        target_compile_options(wlh_rtt_osal PRIVATE
            -Wall -Werror
        )
    endif()
endfunction()
