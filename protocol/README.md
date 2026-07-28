# WL-hosted Protocol

本目录定义 WL-hosted v1 草案的线上协议，是 Frame、RPC、Service ID、Method ID 与 protobuf schema 的唯一来源。

- `spec/`：线格式、兼容性、错误、安全、服务和传输绑定。
- `proto/`：proto3 payload schema；固定 Frame/RPC Header 不使用 protobuf。
- `proto/nanopb.options`：所有变长字段的静态上限。

当前状态为 Draft 0.1，尚未承诺 Protocol 1.0 兼容性。验证命令：

固定 Frame/RPC codec 是一个不依赖 protobuf 或 nanopb runtime 的静态 C99
库，可完全离线构建和测试：

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake target 名为 `wlh_protocol`，也可通过 alias `wlh::protocol` 链接；公共
入口为 `include/wlh/protocol.h`。codec 提供固定 24-byte Frame、16-byte RPC
Envelope、严格长度/保留位校验、SUM32、CRC-32C 和有界 little-endian helper。

需要 protobuf payload 的 Core 可链接 `wlh_protocol_nanopb`（alias
`wlh::protocol_nanopb`）。该 target 统一编译全部标准 `proto/*.proto` 对应
的 `.pb.c/.pb.h`，公开 generated include 目录，并传递链接
`wlh_nanopb_runtime`（alias `wlh::nanopb_runtime`）。这些 target 与
`wlh_protocol` 分离且 `EXCLUDE_FROM_ALL`，wire-only consumer 不会触发
protobuf codegen/runtime。

如果系统已安装 `protoc` 与 `protoc-gen-nanopb`，CMake 会在构建时从
`proto/*.proto` 实时生成代码；否则自动回退到仓库已提交的预生成源文件
`generated/*.pb.c` / `generated/*.pb.h`，保证无 protoc 环境也能构建
host-core/coproc-core。显式重新生成 target：

```sh
cmake --build build --target generate_standard_nanopb
cmake --build build --target generate_sim_nanopb
```

同时编译、链接并执行标准 schema 和 simulator schema 的 nanopb smoke test
（必须已安装生成工具）：

```sh
cmake -S . -B build-nanopb \
  -DBUILD_TESTING=ON -DWLH_BUILD_NANOPB_SMOKE=ON
cmake --build build-nanopb
ctest --test-dir build-nanopb --output-on-failure
```

固定工具版本见 `proto/TOOLCHAIN.lock`，输入校验值见 `proto/SCHEMA.sha256`。
`generated/` 中的预生成文件与 `proto/*.proto` 保持同步，允许直接离线构建。

`sim/` 是测试专用 IPC contract，不属于标准 WL-hosted wire schema，因而不
加入 `proto/SCHEMA.sha256`。nanopb C runtime 的来源和许可证见
`third_party/nanopb/README.md`；wire codec 不链接该 runtime。
