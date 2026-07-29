# Common 模块

跨模块的数据模型、错误码、事件和任务状态定义在此实现，供 API、相册、存储、显示和网络模块共享。
`product_types.h` 是产品层的跨模块领域模型：媒体、显示、存储、模式和异步任务均使用其中的类型。

它不依赖 GPIO、TF 路径、HTTP、JSON 或 ESP-IDF 具体类型；硬件适配与 REST 编解码必须位于各自服务目录。首个真实闭环只允许 `UploadMode::kSourcePlusBin`。
