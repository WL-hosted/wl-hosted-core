# ADC Service (`0x0007`, Optional)

Method `0x0001 READ`：`AdcReadRequest` / `AdcReadResponse`。只返回校准后的 millivolts；分辨率、参考电压和原始 ADC code 属于 Adapter。无 ADC 能力、非法逻辑 pin 或校准失败必须返回明确错误。

## 消息字段

`AdcReadRequest.pin_id` 是 Profile 公开且 `IoPinCapability.adc_supported=true` 的逻辑 pin。它不是 ADC channel、厂商 GPIO 编号或模拟外设句柄。

`AdcReadResponse.pin_id` 必须回显请求值；`millivolts` 是校准后、非负的引脚电压，单位 mV。超出 uint32/硬件可测范围、未校准或当前复用冲突必须返回错误，不能饱和后仍返回 OK。v1 不表达采样次数、衰减、分辨率和误差；这些由 Profile 固定。
