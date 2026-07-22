# SDIO Binding

SDIO transport 交换完整 WL-hosted Frame，不改变 Header。Profile 必须声明 function/block size、bus width、最大 transaction、alignment、interrupt/doorbell 语义和 reset 流程。实现应优先多块传输并允许聚合，但不得让 Ethernet burst 饿死 LINK_CONTROL。

接收长度来自 transport descriptor 与 Frame `payload_size` 的双重校验；两者不一致为 TRANSPORT/DATA_LOSS。DMA buffer 的 cache clean/invalidate 和所有权属于 Adapter，不进入 wire。恢复顺序为 queue reset、function reset、重新枚举、重新 Hello；Session 不得沿用。
