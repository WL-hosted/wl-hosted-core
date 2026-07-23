# Simulator IPC v1 (test-only)

This contract is only for the POSIX/macOS simulators and Sim Manager. It is not
part of the standard WL-hosted v1 wire protocol, is never sent to real firmware,
and is deliberately excluded from `proto/SCHEMA.sha256`.

All integers in the fixed IPC structures are little-endian. A receiver must
reject a connection or record before allocating from an untrusted length.

## Connection hello

Each Unix-socket peer sends exactly one 16-byte Hello before any record:

| Offset | Size | Field | Required value |
|---:|---:|---|---|
| 0 | 8 | `magic` | bytes `57 4c 48 53 49 4d 00 00` (`WLHSIM\0\0`) |
| 8 | 2 | `version` | `1` |
| 10 | 1 | `role` | `1` Host Sim, `2` Coprocessor Sim, `3` Manager |
| 11 | 1 | `flags` | bit 0 `SIDEBAND`; bits 1..7 must be zero |
| 12 | 4 | `max_record_size` | inclusive maximum encoded IPC Record size |

`max_record_size` must be in `8..1048584` (8-byte record header plus at most a
1 MiB payload). The effective limit is the smaller value advertised by the two
peers. An invalid magic, version, role, flags value, or size closes the
connection. Sideband records (kinds 2..7) are enabled only when one peer is the
Manager and both peers set `SIDEBAND`. Direct Host/Coprocessor connections send
only kind 1 even if both advertise the flag.

## Record framing

Every record has an 8-byte header followed by `payload_len` bytes:

| Offset | Size | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | `record_len` | bytes from `kind` through payload; exactly `4 + payload_len` |
| 4 | 1 | `kind` | one of 1..7 below |
| 5 | 1 | `flags` | zero in v1 |
| 6 | 2 | `reserved` | zero |
| 8 | N | `payload` | exactly `record_len - 4` bytes |

The total encoded size is `4 + record_len`. `record_len` must be at least 4,
must not exceed `effective_max_record_size - 4`, and must be available in full
before parsing the payload. Unknown kinds, nonzero flags/reserved fields,
integer overflow, trailing bytes inside protobuf payloads, and over-limit
records are protocol errors.

| Kind | Name | Payload |
|---:|---|---|
| 1 | `WIRE_FRAME` | One complete standard WL-hosted Frame |
| 2 | `RUNTIME_INFO` | `wlh.sim.v1.SimRuntimeInfo` protobuf |
| 3 | `FAULT_REQUEST` | `wlh.sim.v1.SimFaultRequest` protobuf |
| 4 | `FAULT_RESPONSE` | `wlh.sim.v1.SimFaultResponse` protobuf |
| 5 | `WIFI_COMMAND` | `wlh.sim.v1.SimWifiCommand` protobuf |
| 6 | `PING_COMMAND` | `wlh.sim.v1.SimPingCommand` protobuf |
| 7 | `PING_RESULT` | `wlh.sim.v1.SimPingResult` protobuf |

Without fault injection, a Manager must forward the bytes of `WIRE_FRAME`
unchanged. Sideband records never consume standard channel credit and never
change standard Session or Sequence values.

`WIFI_COMMAND` flows Manager to Host Simulator only. The Host Simulator turns
it into the corresponding standard Wi-Fi RPC over the wire. Command results
and Wi-Fi events are never reported on the sideband: they travel as ordinary
standard Frames, which the Manager observes on the `WIRE_FRAME` stream it
relays. When the peer on the other side of the relay is a real transport
(e.g. a USB link to firmware instead of a Coprocessor Simulator socket), the
Manager still relays `WIRE_FRAME` payloads byte-unchanged to and from that
transport; only the encapsulation differs.

`PING_COMMAND` flows Manager to Host Simulator only and requests a diagnostic
ICMP Echo operation through the Host Simulator's network stack. Exactly one
matching `PING_RESULT` flows back to the Manager. These records are test-only
and never cross the standard wire transport.

## Protobuf semantics and bounds

The test-only schema is `sim/proto/sim_sideband.proto`; its nanopb allocation
bounds are in `sim/proto/nanopb.options`. Receivers must additionally enforce:

- `SimRuntimeInfo.role` is 1 or 2 when reported to a Manager. `channels` has at
  most 64 entries, each `channel` is 0..255, `implementation` is at most 48
  UTF-8 bytes, and `implementation_version` is at most 32 UTF-8 bytes.
- `SimFaultRequest.request_id` is nonzero and unique among outstanding requests
  in that direction. `fault` is one of 1..12, `channel` is 0..255, `count` is
  0..1024, `duration_ms` is 0..60000, and `parameters` is at most 256 bytes.
  Zero `count` means the fault-specific default; zero `channel` denotes channel
  0, not a wildcard. A fault that does not use a field requires it to be zero.
- `SimFaultResponse.request_id` is nonzero and exactly matches one outstanding
  request. `detail` is at most 128 UTF-8 bytes. `accepted=true` requires
  `status_code=0`; rejection requires a nonzero status. Duplicate, zero, or
  unknown response IDs are protocol errors.
- `SimWifiCommand.command_id` is nonzero and unique among outstanding commands
  on the connection. `ssid` is at most 32 bytes and `credential` at most 64
  bytes, matching the standard Wi-Fi schema bounds. `security` carries a
  `wlh.protocol.v1.WifiSecurity` value; zero selects the receiver's default.
  Exactly one `command` alternative must be set.
- `SimPingCommand.request_id` is nonzero and unique among outstanding ping
  requests. `hostname` is 1..253 UTF-8 bytes, `count` is 1..10, and
  `timeout_ms` is 1..60000.
- `SimPingResult.request_id` exactly matches one outstanding request.
  `hostname` is at most 253 UTF-8 bytes, `address` is at most 46 bytes, and
  `detail` is at most 128 UTF-8 bytes. `success=true` requires `received > 0`
  and `received <= transmitted`.

Protobuf unknown fields may be skipped for forward compatibility, but the
decoded known-field bounds and all semantics above remain mandatory.

## Fault ownership

The Manager implements forwarding-path faults such as drop, duplicate,
reorder, delay, checksum corruption, and truncation locally. Kinds 3 and 4 are
for faults inside Host/Coprocessor simulator processes, including resets,
credit/channel changes, resource exhaustion, queue starvation, mock Wi-Fi
failures, RPC timeouts, and OTA interruption.
