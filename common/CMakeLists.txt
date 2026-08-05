cmake_minimum_required(VERSION 3.20)
project(wlh_common VERSION 0.1.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()

add_subdirectory(osal)
add_subdirectory(log)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR BUILD_TESTING)
    wlh_common_enable_posix_osal(BUILD_TESTING "${BUILD_TESTING}")
endif()
