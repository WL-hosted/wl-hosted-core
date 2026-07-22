# SPI Binding

SPI Master/Device 角色由 profile 指定，系统角色仍称 Host/Coprocessor。Binding 必须定义 ready/interrupt GPIO 的逻辑、transaction prefix、最大 transfer、full/half duplex、padding/alignment 和 reset 时序。

一次 transaction 可承载完整 Frame 或由 binding 分段；分段不得改变通用 Frame。Host 只有在 ready/credit 允许时发起大数据 transaction。短读、超时和 prefix 长度不一致不得把部分数据交给 Frame parser；应丢弃该 transaction、计数并按恢复层级重试。
