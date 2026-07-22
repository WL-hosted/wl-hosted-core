# UART Binding

UART 是字节流，必须使用 COBS 对“24 字节 Header + payload”整体编码，以 `0x00` 分隔；在 COBS 前计算 Header/payload checksum，`CRC32C_PRESENT` 置位时使用 CRC32C，否则使用 SUM32，接收后验证。空 delimiter 忽略。Profile 声明 baud、flow control、最大编码帧和 RX ring size。

连续解码失败、超长无 delimiter 或 checksum 错误触发丢弃到下一 delimiter 的 resync。UART 必须启用 Per-Channel Credit；没有硬件流控时也必须保留 LINK_CONTROL buffer，禁止 Ethernet/HCI 占满 RX ring。
