# 音频、SD 卡和按键 API（v0.3.0）

基础地址：`http://设备IP`，端口 80。JSON 请求使用 `Content-Type: application/json`。本版支持 **16kHz、16bit、小端 PCM、单声道 WAV**；播放会将单声道复制到硬件左右声道，录音保存 ES7210 MIC1 数据，不包含回声消除或双麦降噪。

## 音频

### 开始录音

```sh
curl -X POST http://DEVICE_IP/audio/record/start \
  -H 'Content-Type: application/json' \
  -d '{"path":"/recordings/hello.wav","max_seconds":60}'
```

`path` 可省略，自动生成不重复的文件名；`max_seconds` 默认 60，允许整数 1–600。自定义录音文件必须位于 `/recordings/` 下，扩展名 `.wav`，不含更深子目录。不覆盖已有文件，按最大时长预检查剩余空间。

返回 `202` 和音频状态，表示后台任务已接受。后续轮询 `/audio/status`，`state=idle` 且 `error=null` 表示完成。录音过程中数据写入保留临时文件，停止或达到时长后补全 WAV 头并改名；中途错误不会发布为完整录音。

### 停止录音

```sh
curl -X POST http://DEVICE_IP/audio/record/stop
```

返回 `200 {"result":"stop_requested"}`；请求异步停止并完成文件，需轮询状态到 `idle` 后下载。空闲时可重复调用；正在播放时返回 `409`。

### 播放与停止

```sh
curl -X POST http://DEVICE_IP/audio/play \
  -H 'Content-Type: application/json' -d '{"path":"/recordings/hello.wav"}'
curl -X POST http://DEVICE_IP/audio/stop
```

播放返回 `202`，后台完成后回到 `idle`。校验 RIFF/WAVE、PCM 格式、采样率、位深、通道数、块长度和文件边界，不支持的 WAV 返回 `415`。支持 fmt/data 之间的附加块。`/audio/stop` 停止播放，录音中调用返回 `409`。

录音和播放互斥，已有任务时新任务返回 `409`。`202` 不代表硬件任务一定完成，务必检查最终 `error`。出错后可以发起新的操作。

### 音量

```sh
curl -X PUT http://DEVICE_IP/audio/volume \
  -H 'Content-Type: application/json' -d '{"volume":35}'
```

允许整数 0–100，默认 35，0 为静音。播放中在下一音频块应用；空闲时在下次播放应用。本版音量不跨重启保存。输入增益固定为 24dB。

### 状态

`GET /audio/status`：

```json
{
  "ready": true,
  "state": "recording",
  "path": "/recordings/hello.wav",
  "last_recording": "/recordings/previous.wav",
  "processed_bytes": 32000,
  "total_bytes": 1920000,
  "position_seconds": 1,
  "volume": 35,
  "stopping": false,
  "error": null
}
```

`state` 为 `idle` / `recording` / `playing`；`processed_bytes` 不含 WAV 头，录音的 `total_bytes` 为最大时长对应的字节数。空闲状态保留上次任务路径和进度。`last_recording` 仅在录音成功完成后更新，并保存到 NVS，重启后可回放；文件若被外部删除则播放返回 `404`。

## SD 卡

仅挂载现有文件系统，**不格式化、不清空已有数据**。建议 FAT32；本版不支持运行中拔插后自动重新挂载，更换卡后重启。没有 SD 卡时联网和其他接口仍可用，文件及音频操作返回 `503`。

### 状态和文件列表

```sh
curl http://DEVICE_IP/sd/status
curl 'http://DEVICE_IP/sd/files?path=/recordings&offset=0&limit=100'
```

状态包括 `mounted`、`capacity_bytes`（卡容量）、`total_bytes`、`used_bytes`、`free_bytes`（文件系统）、`transfer_active`。`mounted` 表示本次启动挂载成功；不表示卡被拔出后仍然物理存在。

列表返回 `files` 数组，每项包含 `name`、`directory`、`size`；最多 100 项。`next_offset=null` 表示结束，否则传入下一页的 offset。临时文件不显示。

### 上传

```sh
curl -X POST 'http://DEVICE_IP/sd/file?path=/hello.wav' \
  -F 'file=@hello.wav'
```

必须使用 multipart/form-data，且只有一个文件部分。路径在 URL query 中提供；文件名不决定目标路径。最大文件 64MiB，流式处理，不整文件载入 RAM；上传到临时文件，收到完整请求后改名。已存在目标返回 `409`，不覆盖。上传中断或格式错误清理本次临时文件。目标父目录须已存在，本版没有创建目录接口。

### 下载与删除

```sh
curl 'http://DEVICE_IP/sd/file?path=/recordings/hello.wav' -o hello.wav
curl -X DELETE 'http://DEVICE_IP/sd/file?path=/recordings/hello.wav'
```

活动录音、播放、传输中的文件受保护，冲突返回 `409`。仅删除文件，不删除目录。文件路径最多 120 字节，必须以 `/` 开头，使用可打印 ASCII；拒绝父目录跳转、反斜杠、重复分隔符、尾随点/空格，以及 `.rlcd-` 保留名称。非 ASCII 文件仍可能出现在现有卡的目录列表中，但本版 API 不操作这些路径。

文件传输与音频使用独立文件句柄；大文件 HTTP 传输期间其他 HTTP 请求、屏幕刷新和主循环动作可能延迟。音频和按键扫描任务仍独立运行，不支持并发多个 HTTP 客户端的实时流媒体服务。

## 按键

| 按键 | 单击 | 双击 | 长按 |
| --- | --- | --- | --- |
| KEY（GPIO18） | 空闲时录音；录音/播放时停止 | 播放最近成功录音 | 1 秒生成事件，不绑定动作 |
| BOOT（GPIO0） | 切换网络、传感器、音频页面 | 仅生成事件 | 3 秒开放 BLE 配网 |

只在固件运行中使用 BOOT 长按，上电时按住 BOOT 会进入下载模式。单击等待双击判定窗口后执行，因此约有 350ms 延迟；消抖 30ms。长按不会额外触发单击。

```sh
curl http://DEVICE_IP/buttons/status
curl 'http://DEVICE_IP/buttons/events?after=0'
```

状态包含 `ready`、`latest_sequence` 和 KEY/BOOT 的 `pressed`、`held_ms`。事件包含 `sequence`、`timestamp_ms`、`button`、`type`（`single_click`、`double_click`、`long_press`）。保留最近 64 个事件；以 `latest_sequence` 作为下次 after 参数。`lost_events=true` 表示游标过旧，`cursor_reset=true` 表示游标大于当前序号（例如设备重启），客户端应重置游标。没有新事件时返回空数组。

现有 `GET /status` 增加 `audio`、`sd`、`buttons` 三个对象，结构与各独立状态 API 一致。

## 常用状态码

| 状态码 | 含义 |
| --- | --- |
| 200 | 成功、停止已请求 |
| 202 | 音频任务已接受 |
| 400 | 参数、路径或请求格式错误 |
| 404 | 文件/目录/路由不存在 |
| 409 | 任务或文件冲突、目标已存在 |
| 413 | 上传超过限制 |
| 415 | 不支持或损坏的 WAV |
| 500 | 文件系统操作失败 |
| 503 | SD 或音频硬件不可用 |
| 507 | 空间不足或写入失败 |

## 测试

```sh
uv run tests/media_smoke.py http://DEVICE_IP
c++ -std=c++11 tests/button_logic.cpp -o /tmp/rlcd-buttons && /tmp/rlcd-buttons
```

实机脚本只使用唯一命名的测试文件，会播放低音量提示音、录制短音频并清理本轮文件；会更新“最近录音”选择，建议在用户正式录音前运行。按键逻辑测试覆盖消抖、单击/双击、长按抑制单击和毫秒计时回绕。

## 驱动来源

`firmware/rlcd_demo/src/esp_codec_dev` 从官方 Arduino `07_Audio_Test` 随附驱动复制，保留其 Apache-2.0 许可证与头部声明。本项目在 I2S 读写返回处增加完整字节数校验。仅复用编解码器驱动，未引入官方 LVGL 界面或音乐素材。

硬件音频引脚：MCLK16、BCLK9、WS45、DOUT8、DIN10、PA46；I2C 复用 SDA13/SCL14，ES8311 地址 0x18、ES7210 地址 0x40。SDMMC 为 CLK38、CMD21、D0 39，1-bit 模式。
