# KV Service (`0x0008`, Optional)

Methods：`0x0001 READ`, `0x0002 WRITE`, `0x0003 ERASE`。key/value 均为 UTF-8。上限和容量来自 Hello。WRITE/ERASE 为单操作原子持久化；删除不存在 key 返回 NOT_FOUND。不提供枚举、事务或批量操作。

## 消息字段

| 消息 | 字段 | 含义与校验 |
|---|---|---|
| `KvReadRequest` | `key` | 非空 UTF-8 key；静态上限 64 bytes，并受协商上限限制。 |
| `KvReadResponse` | `value` | 已持久化 UTF-8 value；静态上限 512 bytes，并受 `kv_max_value_bytes` 限制。不存在返回 NOT_FOUND，不返回空 value 冒充。 |
| `KvWriteRequest` | `key` | 与 READ 相同，选择写入项。 |
| `KvWriteRequest` | `value` | 要原子替换/创建的 UTF-8 value；允许空字符串。成功表示满足 Profile 的持久化语义。 |
| `KvEraseRequest` | `key` | 要删除的 key。 |

key 的比较是 UTF-8 字节精确比较，不做 Unicode normalization、大小写折叠或路径解释。value 若需任意二进制，应用应自行编码或使用 User Passthrough；v1 KV 不接受无效 UTF-8。
