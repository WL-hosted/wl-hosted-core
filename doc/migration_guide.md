# WL-hosted Core 迁移指南

本文档记录 WL-hosted Core 版本间的关键变更。升级 Core 时，应同时升级 Host 与 Coprocessor 两侧的 Core 版本，并检查协议 minor 版本、公共 API 与配置结构体的兼容性。

## 版本约定

- Core 版本与 Protocol Major/Minor 相互独立。
- Frame 中携带 Protocol Major；Hello 中协商共同 Minor。
- Major 不兼容时必须拒绝连接；Minor 选择共同支持的最高值。
- 新增可选 Service/Method/protobuf 字段属于兼容变更；修改固定 Header、端序、已有枚举值属于不兼容变更。

## 迁移检查清单

升级 Core 前，请确认：

- [ ] Host Core 与 Coprocessor Core 升级到相同版本。
- [ ] 检查 `protocol/proto/TOOLCHAIN.lock` 是否需要同步更新。
- [ ] 检查 `wlh_host_config_t` / `wlh_coproc_config_t` 是否有新增或废弃字段。
- [ ] 检查公共 API 签名是否变化。
- [ ] 检查 Hello capability 字段是否变化。
- [ ] 重新运行 Debug/Release/ASan/Portable 构建与测试。
- [ ] 更新 Adapter 中的 gitlink 与 `SUBMODULE.lock`。

## 版本变更记录

> 当前 Core 处于早期草案阶段（0.1.0），以下条目为模板示例。随着版本迭代，将在此补充真实变更。

### 0.1.0（初始草案）

#### 新增

- 引入 `wlh/host.h` 与 `wlh/coproc.h` 公共 API。
- 支持 Link Hello、Heartbeat、Credit Update、Channel Reset。
- 支持 Wi-Fi Initialize、Scan、Connect、Disconnect、Start/Stop AP。
- 支持 Device Info 与 User Passthrough RPC。
- 支持 Ethernet STA 数据面收发。
- 提供 POSIX 与 FreeRTOS OSAL 适配。

#### 不兼容点

- 无（首个版本）。

## 常见 API 变更模式

### 配置结构体新增字段

如果 `wlh_host_config_t` 新增字段，旧 Adapter 若使用 designated initializer 可能会编译失败。推荐：

```c
wlh_host_config_t config = {0};  // 先清零
config.transport = ...;
config.buffers = ...;
// 再填充其余字段
```

### 新增事件类型

`wlh_host_event_kind_t` 新增事件时，Adapter 的 `on_event` 应增加 `default` 分支，避免 switch 未覆盖告警。

### 新增 Service / Method

新增 Service 或 Method 不会破坏旧连接，因为能力协商在 Hello 中完成。旧 Host 不会调用未声明的 Service。

### 状态机调整

如果状态机新增/重命名状态，Adapter 的状态处理代码需要同步更新。

## 协议 Minor 协商注意事项

Hello 过程中，Host 与 Coprocessor 交换各自支持的 min/max minor。Core 选择共同支持的最高 minor。因此：

- 先升级支持更高 minor 的一方不会导致不兼容。
- 如果一方最低 minor 高于另一方最高 minor，连接失败。
- Adapter 不应假设 minor 固定为某个值。

## 锁文件更新

Core 作为父仓库的 submodule 时，升级后需要同步：

1. 更新 `.gitmodules` 中的 commit（如需要）。
2. 更新父仓库 gitlink。
3. 更新 `SUBMODULE.lock` 中的完整 40 字符 SHA。
4. 如果 Core 内部依赖的 protocol/common 版本变化，同步记录 `protocol.transitive.commit` 与 `common.transitive.commit`。

## 回滚建议

如果升级后出现问题：

1. 先确认 Host 与 Coprocessor Core 版本一致。
2. 回退到上一版本的 Core，重新编译两侧。
3. 检查 `protocol/proto/TOOLCHAIN.lock` 是否匹配。
4. 使用 loopback 测试验证基本 Hello 与 RPC。

## 未来不兼容变更预告

以下能力尚未稳定，后续版本可能以不兼容方式调整：

- Bluetooth HCI 数据面细节。
- OTA、Diagnostics、Log Stream 的 Record 格式。
- IO/ADC/KV Service 的 protobuf schema。
- User Passthrough 的流式传输扩展。

建议 Adapter 在使用这些能力前，先确认目标 Core 版本的文档与 protobuf schema。
