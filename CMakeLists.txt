cmake_minimum_required(VERSION 3.20)

project(wlh_core VERSION 0.1.0 LANGUAGES C)

include(CTest)

add_subdirectory(protocol)
add_subdirectory(common)
add_subdirectory(host-core)
add_subdirectory(coproc-core)
