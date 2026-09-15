# 全屏黑白反转（0.6.1）

长按 **KEY 约 1 秒**，切换所有页面的黑白显示。长按不会额外触发录音单击；单击录音/停止、双击回放、BOOT 切页和配网功能保留。

反转在 ST7305 输出层完成，宠物帧、中文文字层、进度条、待机时钟、传感器及网络/音频页面统一交换黑白，不修改 SD 素材或 CLI 预览。设置保存在开发板 NVS，重启恢复；默认关闭。

## CLI

```sh
rlcd display status
rlcd display invert toggle
rlcd display invert on
rlcd display invert off
```

`on` 和 `off` 可安全重复调用；`toggle` 每次切换一次。如果请求超时，先查询状态，再用 on/off 设置期望结果，不要盲目重试 toggle。

## HTTP

`GET /display` 获取显示设置：

```json
{"inverted":false,"storage_ready":true,"error":null}
```

`PUT /display/invert`，Content-Type 为 application/json：

```json
{"mode":"toggle"}
```

mode 必须为 on、off 或 toggle。成功返回 200 和显示设置；无效输入返回 400，超过 128 字节的请求体返回 413。保存失败返回 503，保留之前的显示方向，error 为 display_save_failed。重复设置当前值不会重复写入 NVS。

返回成功表示设置已接受，主循环在随后刷新时应用。现有同步 HTTP 文件传输仍可能延迟页面刷新。实体 KEY 长按使用同一保存与切换逻辑，结果可由 GET /display 查询。

这是设备级配置，与某个 Agent 或宠物无关。API 与 CLI 需要固件/客户端 0.6.1 或更新兼容版本。本版本没有在每个页面额外绘制反转按钮。

## 开发验证

`uv run validation/display_device.py http://DEVICE_IP` 验证实机接口，结束时恢复原设置；`uv run validation/display_cli.py` 验证 CLI 路由。实体 KEY 和所有页面的实际显示效果需在开发板上观察。
