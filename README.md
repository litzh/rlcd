# RLCD 联网固件

面向 Waveshare ESP32-S3-RLCD-4.2，Arduino C++ 开发。该目录可独立复制和构建，全部图形库源码及显示驱动已内置。

## 安装 CLI

在本项目目录运行：

```sh
uv tool install .
rlcd config init
rlcd config set device http://DEVICE_IP
rlcd pet preview deepseek-whale
rlcd pet install deepseek-whale
rlcd pet use deepseek-whale
```

安装后可在任意目录运行 `rlcd`。配置、内置宠物、预览与编译资源保存在 `~/.config/rlcd`，详见 [CLI 安装与配置](docs/cli.md)。本地代码更新后使用 `uv tool install --reinstall .` 更新安装。

## AI 调用说明

让 AI 读取 [RLCD SKILL](skills/rlcd/SKILL.md)，即可按任务阶段使用 CLI 或 HTTP 上报状态。该文档包含触发场景、命令、进度与 TTL、中文渲染和失败处理规则；可复制给支持技能或自定义指令的 AI 使用。

[Agent 接口文档](docs/agent.md) 保留作为协议参考。普通接入优先提供 SKILL，需要实现新客户端时再补充接口文档。

DeepSeek Harness（dsh）用户可以直接安装 [dsh 插件](integrations/dsh/README.md)：它监听 dsh 的 agent 生命周期，自动上报任务状态并激活 `deepseek-whale`，无需模型参与。

## 功能

- 开机优先读取 NVS 保存的 Wi-Fi；没有有效保存配置则连接构建时指定的默认网络；未指定默认网络时直接等待 BLE 配网。
- 每次连接最多等待 20 秒，窗口内对断连事件有限重试；失败后等待 BLE 配网，不自动回退到默认网络。
- 蓝牙提交的 Wi-Fi **连接成功才保存**；失败不会覆盖原配置。重启后使用保存配置。
- 运行中断网会尝试重连当前网络，20 秒失败后开放配网。
- 屏幕显示英文联网状态、SSID、IP、HTTP 端口、温湿度、电池电压和最新 echo 消息。
- HTTP 80 端口提供 `POST /echo` 和 `GET /status`。
- v0.3.0 增加录音、WAV 播放、音量、SD 文件管理和按键事件 API，见 [音频/SD/按键文档](docs/media-api.md)。
- KEY 单击录音/停止、双击播放最近录音；BOOT 单击切换网络/传感器/音频/宠物页面，长按三秒仍用于配网。

## 黑白反转

长按 KEY 一秒或运行 `rlcd display invert toggle` 即可全屏交换黑白，设置跨重启保存。详见 [显示控制](docs/display.md)。

## 待机与客户端宠物

v0.6.0 默认显示时钟/传感器待机页，任务上报唤醒宠物；idle/success 30 秒后或 TTL 到期时回到待机。上报可加 `--pet deepseek-whale` 临时切换宠物，多客户端序号独立校验，详见 [Agent 接入](docs/agent.md)。

## 宠物素材包

v0.5.0 支持用户提供 PNG/WAV 和 `pet.json`，通过 CLI 安装、切换宠物，无需修改固件。随附 `pets/pixel-cat`，见 [素材规范与接入指南](docs/pet-format.md)。先执行 `pet install`、`pet use`，再上报 Agent 状态。

## Agent 中文桌宠

支持宠物动画、中文状态标签、动态中文文字层、进度条和事件音效。电脑端使用 `rlcd --device http://DEVICE_IP showcase --sound` 体验，详见 [Agent 功能与 API](docs/agent.md)。

## 构建环境

已验证 Arduino CLI 1.5.1、ESP32 Arduino Core `3.3.2-cn`。官方要求 ESP32 Core ≥ 3.3.0，本项目建议使用已验证版本。新机器需先安装 Arduino CLI、ESP32 Core 及其工具链，以及 uv（运行构建参数生成脚本）。

项目内置 U8g2 2.36.18 和官方例子中的 esp_codec_dev 源码；WiFi、WebServer、Preferences、Wire、BLE、SD_MMC 和 cJSON 来自 ESP32 Core，无需另外安装第三方库。

```sh
arduino-cli version
arduino-cli core list
bash scripts/build.sh
```

`build/` 内生成应用、引导程序和分区表。构建配置为 ESP32S3 Dev Module、16MB Flash、OPI PSRAM、USB CDC 开启，以及 16MB Flash 的 3MB 应用分区。

默认 Wi-Fi 通过构建进程环境变量传入，不保存在源码中。以下值均为占位符：

```sh
RLCD_WIFI_SSID='<your-ssid>' RLCD_WIFI_PASSWORD='<your-password>' bash scripts/build.sh
```

`RLCD_WIFI_SSID` 为 1–32 个 UTF-8 字节；开放网络可省略密码，其他网络使用 8–63 字节密码或 64 位十六进制 PSK。两个变量都不设置时，不内置默认网络。有 NVS 保存配置时仍优先使用保存配置。

每次构建都会重新生成 `firmware/rlcd/wifi_defaults.h`，不会沿用上次构建的凭据。该文件和 `build/` 已加入 Git 忽略规则；生成头文件及固件产物包含传入的凭据。上传脚本会重新构建，因此上传时也要传入相同变量：

```sh
RLCD_WIFI_SSID='<your-ssid>' RLCD_WIFI_PASSWORD='<your-password>' bash scripts/upload.sh /dev/cu.usbmodem31201
```

## 烧录与日志

```sh
arduino-cli board list
bash scripts/upload.sh /dev/cu.usbmodem31201
arduino-cli monitor --port /dev/cu.usbmodem31201 --config baudrate=115200
```

端口以实际检测结果为准。烧录会替换当前应用；普通上传不会清除 NVS 保存的 Wi-Fi。串口不输出 Wi-Fi 密码。

## Web Bluetooth 配网

1. 用 **Mac Chrome** 打开 `web/provision.html`。页面没有外部资源依赖。
2. 设备联网失败时自动开放配网；已联网时，**在固件运行中长按 BOOT 三秒**，屏幕底部显示 `BLE setup available`。不要在上电时按住 BOOT，那会进入下载模式。
3. 点击「选择蓝牙设备」，选择 `RLCD-xxxx`，输入 2.4 GHz Wi-Fi 名称和密码。
4. 点击「连接并保存」，等待页面显示保存结果及 IP。
5. 成功后设备停止广播并关闭配置写入，现有蓝牙连接仍可读取结果。再次配网需重新长按 BOOT。

也可以在 USB 串口中发送 `provision` 加换行，执行与 BOOT 长按相同的操作。

串口还支持 `status`（输出状态 JSON）和 `scan`（空闲时扫描并仅报告当前目标 Wi-Fi 的信道、RSSI、认证类型）。断连原因会输出到串口，并保留在 `wifi.last_disconnect_reason` 中；该值是历史事件，不代表当前连接一定异常。

支持开放 Wi-Fi（空密码）、8–63 字节密码或 64 位十六进制 PSK；SSID 为 1–32 字节。页面不记住密码。新配置保存失败时会明确显示 `save_failed`，不应视为重启后可用。

若 `file://` 受浏览器限制，可以在本项目目录启动本地静态服务：

```sh
uv run python -m http.server 8080 --bind 127.0.0.1 --directory web
```

然后用 Chrome 打开 `http://localhost:8080/provision.html`。Web Bluetooth 需要支持该 API 的浏览器、安全上下文以及用户点击授权。Safari 不适合作为本项目的配网浏览器。

## HTTP API

### POST /echo

请求 `Content-Type: application/json`：

```sh
curl -X POST http://DEVICE_IP/echo \
  -H 'Content-Type: application/json' \
  -d '{"message":"Hello RLCD!"}'
```

返回 `200`：

```json
{"message":"Hello RLCD!"}
```

消息支持可打印 ASCII 及换行，最长 240 字节；屏幕自动按行折行，超出可见区域的部分不显示，返回内容保持完整。新消息覆盖上一条。允许空字符串清空消息。不支持中文显示，非 ASCII 返回 `400`。无效 JSON、缺失字段及超长消息返回 `400`；请求体超过 2048 字节返回 `413`。屏幕在下一次刷新时更新（通常约一秒）。

### GET /status

```sh
curl http://DEVICE_IP/status
```

字段：

| 字段 | 含义 |
| --- | --- |
| `firmware` | 固件版本 |
| `uptime_seconds` / `free_heap_bytes` | 运行秒数、空闲堆 |
| `wifi.state` | `connecting`、`connected`、`waiting_for_ble` |
| `wifi.ssid` / `wifi.ip` / `wifi.rssi_dbm` | 当前网络、IP、RSSI |
| `wifi.source` | `default`、`saved`、`provisioned`、`none`（尚未配置网络） |
| `wifi.provisioning` / `provisioning_result` | 配网开放状态及结果 |
| `sensors.sample_age_ms` | 上次传感器采样距今毫秒数 |
| `sensors.shtc3` | `status`、`temperature_c`、`humidity_percent` |
| `sensors.battery` | `status`、`voltage_v`（ADC 测量值，不推断电量或电池是否安装） |
| `sensors.rtc` | `status`、`time`（本地 RTC 时间，无时区标记） |
| `audio` / `sd` / `buttons` | 音频任务、SD 容量与按键状态，详见媒体 API 文档 |

传感器每五秒采样。SHTC3 校验 CRC，读取失败返回 `read_failed` 和 `null` 测量值。RTC 读取失败返回 `read_failed`，振荡器失效标志或时间字段异常返回 `invalid_time` / `null`。本版本不校准 RTC，不保证已有 RTC 时间准确。电池电压按官方分压系数 3 计算，并做八次采样平均。

未知路由及不支持的方法返回 `404`。接口不返回密码。

## 验证

构建参数验证（不连接设备）：`uv run validation/build_config.py`。检查参数边界、特殊字符生成的 C++ 字符串，以及重新构建时清除旧凭据。

设备联网后：

```sh
uv run validation/http_api.py http://DEVICE_IP
```

检查 status 结构、echo 回显、JSON 转义、输入边界、中文拒绝和 404。蓝牙协议及人工验证步骤见 `docs/provisioning.md`。

配网页面逻辑验证：`node validation/provision_ui.cjs`。它使用模拟 GATT 设备运行真实页面脚本，检查输入校验、20 字节分包和状态显示；不替代浏览器蓝牙权限及实际配网操作验证。

实机 BLE 验证（Mac 需允许执行环境使用蓝牙）：

```sh
uv run --with bleak --with pyserial validation/ble_provisioning.py /dev/cu.usbmodem31201
```

该脚本会临时切换网络，验证无效配置和失败连接，然后保存环境变量 `RLCD_WIFI_SSID` / `RLCD_WIFI_PASSWORD` 指定的指定网络（运行前必须设置 SSID），并再次尝试错误网络。验证结束后重启设备，确认自动恢复保存的指定网络。仅在允许覆盖目标设备 Wi-Fi 配置时运行。

## 硬件

| 信号 | GPIO / 地址 |
| --- | --- |
| RLCD SCK / MOSI / DC / CS / RST | 11 / 12 / 5 / 40 / 41 |
| I2C SDA / SCL | 13 / 14 |
| SHTC3 / PCF85063 | 0x70 / 0x51 |
| 电池 ADC | GPIO 4（ADC1 channel 3） |
| BOOT | GPIO 0，低电平按下 |

屏幕原生 300×400，通过 `U8G2_R1` 横屏显示为 400×300，无背光。Wi-Fi SSID 中的非 ASCII 字符在屏幕上替换为 `?`，连接时保留原始 SSID。

## 目录

```text
firmware/rlcd/  应用、硬件驱动与 BLE 配置
libraries/U8g2/     图形库源码和许可证
scripts/           构建、烧录与素材生成脚本
src/rlcd/          可安装的 CLI 包、配置与素材管理
pets/              可直接安装、复制修改的宠物素材包
web/provision.html 独立配网页面
validation/             HTTP 和配网页面验证脚本
docs/              蓝牙协议与验证说明
licenses/          上游许可证
build/             自动生成，忽略版本控制
```

本版本面向可信局域网：HTTP 没有鉴权或 TLS，BLE 配网窗口没有配对口令，NVS 未启用加密；默认凭据仅在构建时传入后编入固件。配网仅在联网失败或本机按键/USB 操作后开放。

ST7305 驱动来自 Waveshare `10_U8G2_Test`，许可证保存在 `licenses/Waveshare-LICENSE`。U8g2 为官方资料包中的 2.36.18，许可证位于 `libraries/U8g2/LICENSE`。参考：[Waveshare 文档](https://docs.waveshare.net/ESP32-S3-RLCD-4.2/)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)。
