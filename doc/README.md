# WL-hosted Core 文档

本目录面向 WL-hosted Core 的使用者、平台适配开发者和维护者，说明 Core 的架构、API、移植方法、生命周期、测试与排障。Core 的线上协议规范请继续参考 `protocol/spec/`；本文档不再重复 wire format 细节。

## 文档索引

| 文档 | 类型 | 说明 |
|---|---|---|
| [overview.md](overview.md) | 架构概览 | Core 定位、目录职责、依赖边界、快速构建 |
| [architecture.md](architecture.md) | 架构设计 | 分层模型、运行时结构、状态机、核心不变量 |
| [porting_guide.md](porting_guide.md) | 开发指南 | 为新 MCU/RTOS 平台编写 Adapter 的步骤与检查清单 |
| [osal.md](osal.md) | 接口规范 | `wlh/osal.h` 语义、ISR 安全、POSIX/FreeRTOS 适配 |
| [host_core_integration.md](host_core_integration.md) | 开发指南/API | Host Core 公共 API、config、事件与生命周期 |
| [coproc_core_integration.md](coproc_core_integration.md) | 开发指南/API | Coprocessor Core 公共 API、后端回调与事件注入 |
| [lifecycle.md](lifecycle.md) | 运行时说明 | Host/Coproc 初始化、启动、恢复、停止状态机 |
| [api_sync_async.md](api_sync_async.md) | 接口规范 | Host/Coproc 公共 API 与 Adapter 回调的同步、异步和非阻塞 ingress 语义 |
| [wire_protocol_usage.md](wire_protocol_usage.md) | 协议使用指南 | 在 Core 中使用 `wlh_protocol` 编解码与路由 |
| [testing.md](testing.md) | 测试指南 | 构建矩阵、mock OSAL、fake transport、测试钩子 |
| [troubleshooting.md](troubleshooting.md) | 运维指南 | 常见集成问题与排查思路 |
| [migration_guide.md](migration_guide.md) | 维护指南 | 版本迁移、API 变更、兼容性注意事项 |

## 快速开始

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```
