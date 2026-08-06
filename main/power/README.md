# Power 模块

电池、充电、深睡、唤醒和高耗电任务互锁在此实现。AXP2101 的官方驱动保留在 `components/pmicpower`。

## 阶段 P0：安全观测（当前）

- `PowerService` 只读取 AXP2101 的 USB、系统和主电池状态，通过 `GET /api/v1/power/status` 暴露。
- 没有检测到主电池时，接口返回 `battery.present=false`、`percent=null`；不把无电池误报为 0%。
- 启动时不再写入主锂电的预充、恒流或终止充电参数；接口会读取 PMIC 当前的充电电流设置和目标电压。它们可能来自 PMIC 默认/既有状态，电池型号、容量和充电策略确认并实测前，产品层不会将其标记为已批准策略。
- RTC 备用电池的充电位被显式关闭；不得插入一次性 CR 系列纽扣电池后再改变该策略。
- 本阶段保持 `always_on`，不启用深睡眠、RTC 唤醒或自动断电，也不占用 DisplayService、StorageService、网络或音频的控制权。
