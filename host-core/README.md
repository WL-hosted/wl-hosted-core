# WL-hosted Host Core

Platform-independent Host runtime for the WL-hosted v1 protocol. The core owns
link negotiation, session and sequence tracking, bounded RPC matching, credit,
Wi-Fi STA control, Ethernet STA records, timeouts, and recovery. Platform code
provides transport, buffer, clock, and executor operations through `wlh/host.h`.

The Core is driven by an OSAL task and a bounded command/RX queue. Public RPC,
Wi-Fi, Ethernet, and RX ingress APIs enqueue work and never drive the state
machine through an application poll loop. `submit_tx` transfers buffer
ownership to the transport until its completion callback fires. The OSAL
queue blocks until the nearest RPC/peer-health deadline, with no fixed tick.
Transport start/stop are asynchronous submissions and recovery resumes only
after their lifecycle completion callbacks arrive. The OSAL contract exported
by sibling `common/` in `wlh/osal.h` covers task, mutex, semaphore,
event, queue, timer,
monotonic clock, yield, and ISR-safe notification semantics without exposing
native RTOS handles.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The `protocol/` and `common/` directories are sibling sources in
`wl-hosted-core`.
Test-only fault hooks are available only when `WLH_ENABLE_TEST_HOOKS` is
defined by the consumer.
