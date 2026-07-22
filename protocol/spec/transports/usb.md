# USB Binding

USB 使用 Vendor-specific interface：Bulk OUT 为 Host→Coprocessor，Bulk IN 为 Coprocessor→Host，可选 Interrupt IN 只做 wake/doorbell。Bulk byte stream 按 Frame Header 的固定 magic/header/payload length 切帧；USB packet boundary 不是协议边界。

Profile 声明 VID/PID/interface、endpoint、speed、max transfer 和 reset/re-enumeration 策略。短 packet 只结束 USB transfer，不代表 Frame 结束。Disconnect、stall clear 或 device reset 后必须重新 Hello，旧 Session Frame 全部丢弃。
