# WL-hosted Coprocessor Core

Portable C99 server-side core for WL-hosted v1. The Core uses the OS-neutral
contract from sibling `common/` and has no POSIX dependency; a
platform supplies frame transport, monotonic time, Ethernet, and Wi-Fi backend
callbacks through `wlh_coproc_config_t`.

The state machine runs only on its OSAL Core task. Transport RX and backend
events are copied into a bounded queue; `submit_tx` is ownership-transferring
and completes asynchronously. When idle, the queue blocks until the next
Heartbeat deadline instead of waking on a fixed tick. Wi-Fi `scan`, `connect`, and `disconnect`
callbacks only accept work. Final scan/link state is injected later through
the public event ingress APIs, so vendor callbacks may arrive from another
RTOS task without re-entering the protocol dispatcher.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The OSAL contract comes from sibling `common/`. The standard protocol,
generated nanopb messages, and nanopb runtime come from sibling `protocol/`.
