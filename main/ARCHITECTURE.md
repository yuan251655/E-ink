# PhotoPainter 产品层目录

`main` 是可演进的产品业务层；硬件驱动与官方通用组件仍保留在项目根目录的 `components`。
业务模块不得直接操作 GPIO、TF、电子纸或 Codec，必须通过对应服务和官方 BSP 接口访问。

| 目录 | 职责 |
|---|---|
| `api` | 手机 App 的版本化 REST API、任务与状态响应。 |
| `album` | 本地/AI 媒体库、轮播、当前显示媒体。 |
| `storage` | TF 挂载状态、媒体索引、staging、校验和原子提交。 |
| `display` | 产品 `DisplayService`、刷新队列、图像转换管线。 |
| `xiaozhi` | 官方小智应用、音频、协议、MCP、通用 UI 和资源。 |
| `user_app_bsp` | 新的 `ModeManager`；逐步兼容官方四种模式。 |
| `input` / `led` / `power` | 按键事件、状态灯策略、电源与睡眠策略。 |
| `network` | AP+STA、配网、扫描、mDNS 和连接状态。 |
| `audio` | 产品级音频策略；不直接操作 Codec。 |
| `sensors` | 温湿度等传感器服务。 |
| `system` | NVS、设备状态、时间、日志、诊断和 OTA。 |
| `board` | 产品级板型适配；官方多板型目录仍在 `boards`。 |
| `common` | 共享数据模型、错误码、事件和任务定义。 |

## 官方底层保留边界

```text
components/port_bsp    屏、TF、按键、LED、I2C、字体和提示音
components/app_bsp     官方图像解码/处理及旧应用示例
components/pmicpower   AXP2101 电源管理
components/codec_board 音频 Codec 硬件适配
```

官方 Basic/Network/Xiaozhi/ModeSelection 模式编排已迁移到 `main/user_app_bsp`；原 `components/user_app_bsp` 仅保留迁移标记，不参与构建。

`main/xiaozhi/ui` 是小智通用 UI，不是 7.3 英寸六色电子纸显示服务；新电子纸逻辑只能在 `main/display` 通过 `DisplayService` 调用官方 `display_bsp`。
