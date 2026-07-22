# WL-hosted Core

`wl-hosted-core` contains the portable WL-hosted v1 implementation and its
shared protocol and OSAL contracts in one repository.

```text
protocol/     Wire/RPC codec, protobuf schemas, nanopb runtime, and specifications
common/       Platform-neutral OSAL contract and POSIX/FreeRTOS adapters
host-core/    Platform-independent Host runtime
coproc-core/  Platform-independent Coprocessor runtime
```

The four directories are source directories in this repository, not nested git
submodules. Platform adapters should add this repository as a single `core/`
submodule and reference the required child directory, such as
`core/host-core`, `core/coproc-core`, or `core/protocol`.

To build the complete portable stack and its tests:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

Each child directory remains usable with `add_subdirectory` by an adapter. The
Host and Coprocessor CMake targets load sibling Protocol and Common sources
only when the parent project has not already done so.
