# Storage 模块

TF 媒体索引、staging、校验、原子提交和空间健康检查在此实现。官方 SDMMC 驱动保留在 `components/port_bsp/sdcard_bsp.*`，不得直接搬入本目录。

`StorageService` 借用已由官方流程挂载的 `CustomSDPort`，不拥有挂载生命周期，也不修改引脚。它为新的产品层提供单写事务：

```text
/sdcard/.staging/<transaction_id>/...  ->  校验完成  ->  rename
                                                  ->  /sdcard/media/<media_id>/
```

它不解析 HTTP、multipart 或媒体 manifest，也不执行显示和媒体列表。首版不会格式化 TF，也不会删除任何已提交的媒体目录。
