# 相念 · ESP32-S3 电子墨水相框固件

这是“相念”电子墨水相框的设备端固件。项目基于 Waveshare ESP32-S3-PhotoPainter 硬件和 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 开源项目二次开发，面向 800 × 480 六色电子纸相框。

配套 Android App：[yuan251655/E-LINK-APP](https://github.com/yuan251655/E-LINK-APP)。

## 它负责什么

- 管理 TF 卡上的本地相册、AI 相册和信息看板媒体。
- 串行驱动电子纸刷新，避免并发刷新损坏显示状态。
- 通过局域网 `/api/v1` API 与手机 App 通信。
- 提供 AP 配网与 STA 联网、已保存 Wi-Fi 管理和恢复入口。
- 支持本地/AI 相册轮播、方向筛选、无重复随机播放、暂停与下一张。
- 支持 AXP2101 电源状态、PCF85063 RTC、自动休眠、按键唤醒和定时唤醒。
- 集成小智语音、音量/静音控制，以及受限的相框语音操作。
- 显示信息看板：天气日期、备忘录待办和轻量综合布局。

## App 与设备的边界

AI 生图与照片风格转换由 App 直连 Seedream 完成：

```text
App 保存 Endpoint / Model / API Key
  -> App 调用 Seedream 并下载结果
  -> App 本地预览、转换为六色 BIN
  -> ESP 接收最终 BIN，校验后写入 TF 并显示
```

设备端**不保存 API Key**，也不负责参考图 Base64 上传、HTTPS 模型请求或模型结果下载。它只接收 App 已完成转换的最终图像数据。

## 硬件与运行参数

| 项目 | 说明 |
| --- | --- |
| 主控 | ESP32-S3-WROOM-1-N16R8 |
| 屏幕 | 800 × 480 六色电子纸（黑、白、绿、蓝、红、黄） |
| 显示帧 | 4 bpp，固定 `192000 bytes` |
| 存储 | TF 卡：媒体、索引、看板缓存与运行状态 |
| 电源 | AXP2101；支持主锂电与 USB 外部电源状态读取 |
| 时钟 | PCF85063 RTC；CR2032 仅作为不可充电备用电源 |
| 音频 | ES7210 麦克风、ES8311 Codec、NS4150B 功放 |
| 网络 | 2.4 GHz Wi-Fi，AP + STA |

电子纸为整屏刷新，刷新过程约需数十秒且不可中断。固件将显示请求串行化，并在刷新后设置保护窗口；App 应以 API 返回的忙碌或冷却状态为准。

## 三个显示功能

| 功能 | 用途 |
| --- | --- |
| 本地相册 | 手机导入照片后保存至 TF，并按设置轮播 |
| AI 相册 | 保存 App 生成或风格转换后的最终作品 |
| 信息看板 | 天气日期、备忘录待办、轻量综合信息 |

同一时刻只有当前活动功能可以提交电子纸刷新。媒体写入使用 `.staging` 临时目录、校验和原子提交，未完成文件不会进入轮播或显示。

## 网络使用

- **AP**：始终保留，用于初始配网、App 本地控制和网络故障恢复。
- **STA**：连接路由器后用于小智、NTP、天气等联网能力，仅支持 2.4 GHz Wi-Fi。
- 设备切换 STA 或路由器信道后，AP/STA 地址可能变化；请在 App 的网络配置页重新发现设备。
- 设备 AP 网页配网入口仍可作为恢复方案使用。连接 AP 后访问设备 IP，或使用 App 的网络配置页。

## API 概览

API 根路径为 `/api/v1`，主要接口包括：

| 类别 | 典型接口 |
| --- | --- |
| 健康与能力 | `GET /health`、`GET /device/status`、`GET /device/capabilities` |
| 媒体 | `POST /media/upload`、`GET /media`、媒体显示/删除/预览 |
| 相册轮播 | `GET/POST /local-album/playback`、`GET/POST /ai-album/playback` |
| 模式与看板 | `GET/POST /mode`、`GET/POST /dashboard` |
| 网络 | `GET /network/status`、扫描/保存 STA、配置 AP |
| 电源与时间 | 电源状态、电量显示、休眠配置、RTC 状态与校时 |
| 音频与小智 | 音量、静音、扬声器测试、小智状态与相册语音任务 |
| 存储与诊断 | TF 状态、重挂载、显示状态、日志与异步任务查询 |

完整路由注册见 [`main/api/product_api.cpp`](main/api/product_api.cpp)。API 面向配套 App，不建议把接口视为稳定的公网服务。

## 工程结构

```text
.
├── main/
│   ├── album/       本地/AI 相册轮播与播放状态
│   ├── api/         /api/v1 产品接口
│   ├── dashboard/   信息看板状态与渲染链路
│   ├── display/     唯一电子纸显示服务
│   ├── network/     AP、STA 与网络恢复
│   ├── power/       电源、休眠、按键与 RTC 协作
│   ├── storage/     TF 媒体库、索引和原子入库
│   ├── time/        RTC/NTP 时间服务
│   └── xiaozhi/     小智、音频与语音控制
├── components/      板级与上游复用组件
├── partitions/      16 MB Flash 分区表
├── assets/          固件内置资源
├── scripts/         上游辅助脚本
├── platformio.ini   推荐的构建/烧录前端
└── sdkconfig.*      ESP32-S3 PhotoPainter 配置
```

项目架构和产品约束以根目录 [`../AGENTS.md`](../AGENTS.md) 为准；`资料/` 下的官方工程仅作板级参考，不作为本产品代码入口。

## 构建与烧录

### 前提

- ESP32-S3-PhotoPainter 硬件。
- 16 MB Flash、Octal PSRAM 的目标配置。
- PlatformIO（项目锁定 `espressif32@6.13.0`，使用 ESP-IDF 5 系列兼容 API）。
- 可用的 USB 串口与驱动。

在本目录执行：

```powershell
# 编译
pio run

# 烧录
pio run -t upload

# 串口日志（115200）
pio device monitor -b 115200
```

也可使用已安装的 ESP-IDF 工具链从 `CMakeLists.txt` 构建，但请保留本仓库的 `sdkconfig.photopainter_esp32s3`、分区表和板级配置。不要把通用 DevKitC 的默认分区表用于本产品，否则固件会因应用分区过小而无法写入。

## 安全与维护注意事项

- 不要向固件、TF manifest、日志或设备 API 写入 Seedream API Key。
- 写 TF、上传媒体、电子纸刷新和音频任务均有资源互斥；不要绕过服务层直接调用底层驱动。
- 不要在电子纸刷新期间断电；显示失败时应保留上一张有效画面。
- 使用 CR2032 时，必须先确认 RTC 备用电池充电处于关闭且安装状态安全；CR2032 不可充电。
- 修改 AP/STA 后短暂断连是正常现象。若 STA 配置失败，请连接设备 AP 恢复配置。
- `build-*`、`artifacts/` 等本地验证目录不属于固件源码提交内容。

## 许可证与致谢

本仓库采用 [MIT License](LICENSE)。

感谢 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 及 Waveshare ESP32-S3-PhotoPainter 官方资料提供的上游基础。
