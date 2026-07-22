# IO Service (`0x0006`, Optional)

Methods：`0x0001 CONFIGURE`, `0x0002 READ`, `0x0003 WRITE`。wire 只使用 profile 定义的逻辑 `pin_id`。WRITE 仅允许 OUTPUT/OPEN_DRAIN；v1 不定义 GPIO interrupt event。能力中的 mode flags：bit0 INPUT、bit1 OUTPUT、bit2 OPEN_DRAIN、bit3 PULL_UP、bit4 PULL_DOWN。

## 枚举

`IoMode.INPUT` 为数字输入；`OUTPUT` 为推挽输出；`OPEN_DRAIN` 只能主动拉低、释放后由上拉产生高电平；`UNSPECIFIED` 在 CONFIGURE 中非法。

`IoPull.NONE` 关闭内部上下拉；`UP` 开内部上拉；`DOWN` 开内部下拉；`UNSPECIFIED` 在 CONFIGURE 中非法。模式/上下拉组合还必须出现在 Hello 的 pin capability 中。

## 消息字段

### `IoConfigureRequest`

| 字段 | 含义 |
|---|---|
| `pin_id` | Profile 公开的逻辑 pin。未出现在 `ResourceLimits.io_pins` 中返回 NOT_FOUND/NOT_SUPPORTED。 |
| `mode` | 目标方向/驱动模式。 |
| `pull` | 目标内部上下拉。OPEN_DRAIN 通常配 UP，但协议不强制 Profile 不支持的组合。 |
| `initial_level` | OUTPUT/OPEN_DRAIN 切换方向前应先锁存的初始电平，避免毛刺；INPUT 时忽略且建议为 false。 |

### `IoReadRequest` / `IoReadResponse`

请求的 `pin_id` 指定逻辑 pin。响应 `pin_id` 必须回显；`level` 是采样数字电平；`mode` 和 `pull` 是实际生效配置，而非缓存的请求值。读取 OUTPUT 返回引脚/锁存器的实际语义由 Profile 声明。

### `IoWriteRequest`

`pin_id` 指定已配置为 OUTPUT/OPEN_DRAIN 的 pin；`level` 是目标逻辑电平。OPEN_DRAIN 的 true 表示释放、false 表示拉低。写 INPUT 返回 INVALID_STATE。
