# WL-hosted Common

Shared, platform-independent contracts and platform adapters used by
WL-hosted Host and Coprocessor Core.

## OSAL

`wlh::osal` exports the portable `wlh/osal.h` contract without selecting an
operating system or requiring a thread library. Core and MCU builds link only
this interface target.

POSIX applications opt in after adding Common:

```cmake
wlh_common_enable_posix_osal(BUILD_TESTING "${BUILD_TESTING}")
target_link_libraries(my_posix_app PRIVATE wlh::posix_osal)
```

The POSIX adapter uses pthread tasks, condition-variable waits, bounded queues,
and monotonic timers. It is not configured or compiled unless explicitly
enabled.

FreeRTOS applications (for example ESP-IDF firmware) opt in similarly:

```cmake
wlh_common_enable_freertos_osal()
target_link_libraries(my_freertos_app PRIVATE wlh::freertos_osal)
```

The FreeRTOS adapter uses native FreeRTOS tasks, semaphores, event groups,
queues, and software timers. It requires FreeRTOS headers to be available in the
build environment and is not compiled unless explicitly enabled.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
