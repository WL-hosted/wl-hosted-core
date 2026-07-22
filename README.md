# WL-hosted Core

`wl-hosted-core` 是 WL-hosted v1 协议的平台无关实现仓库，包含协议编解码、共享 OSAL 契约、Host 运行时和 Coprocessor 运行时四个子目录。仓库以 C99 编写，可在 MCU、RTOS 与 POSIX 桌面环境之间保持可移植。

## 目录结构

```text
protocol/     Wire/RPC codec、protobuf schema、nanopb runtime 与协议规范
common/       平台无关 OSAL 契约及 POSIX/FreeRTOS 适配
host-core/    平台无关 Host 运行时
coproc-core/  平台无关 Coprocessor 运行时
```

这四个目录是本仓库的源码目录，不是嵌套的 git submodule。平台 Adapter 应把本仓库作为单个 `core/` submodule 引入，并引用所需的子目录，例如 `core/host-core`、`core/coproc-core` 或 `core/protocol`。

## 快速构建

完整构建 Core 及全部测试：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

验证可移植性（不链接 pthread/测试框架）：

```sh
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-portable --parallel
```

## 文档

详细文档位于 `doc/` 目录：

- [doc/overview.md](doc/overview.md)：Core 定位、目录职责、依赖边界
- [doc/architecture.md](doc/architecture.md)：分层模型、运行时结构、状态机
- [doc/porting_guide.md](doc/porting_guide.md)：为新平台编写 Adapter
- [doc/osal.md](doc/osal.md)：OSAL 接口规范
- [doc/host_core_integration.md](doc/host_core_integration.md)：Host Core 集成
- [doc/coproc_core_integration.md](doc/coproc_core_integration.md)：Coprocessor Core 集成
- [doc/lifecycle.md](doc/lifecycle.md)：生命周期与恢复
- [doc/wire_protocol_usage.md](doc/wire_protocol_usage.md)：Wire/RPC 使用
- [doc/testing.md](doc/testing.md)：测试指南
- [doc/troubleshooting.md](doc/troubleshooting.md)：故障排查
- [doc/migration_guide.md](doc/migration_guide.md)：版本迁移

线上协议规范请继续参考 `protocol/spec/`。
