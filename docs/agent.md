# Agent 中文桌宠（v0.4.0）

此版本在设备上原生绘制猫的表情和动画、中文状态标签、进度条；CLI 使用电脑字体把动态中文排版为独立位图，经 HTTP 一次提交。文字不变时可只提交状态/进度。动画目标更新间隔为 125ms；实际帧率受显示传输及 HTTP 操作影响，尚未测量。

## 快速体验

电脑安装 uv，设备运行 v0.4.0 并联网。以下地址替换为屏幕上的 IP：

```sh
uv run cli/rlcd.py --device http://DEVICE_IP showcase --sound
```

依次显示待机、分析项目、构建固件、等待输入、任务完成，每个阶段约四秒。`--sound` 会先安装并校验 `/agent-chime-v1.wav`，等待输入和完成时播放一次短提示音。已有同名且内容不同的文件会报错，不覆盖；省略 `--sound` 不上传、不播放声音。

CLI 声明 Pillow 和 fontTools 依赖，uv 首次运行自动安装。默认查找 macOS STHeiti、Linux Noto Sans CJK、Windows 微软雅黑，也可以传 `--font /path/to/font.ttf` 或设置 `RLCD_FONT`。不会把电脑字体上传或加入仓库；字形缺失会明确报错。

```sh
export RLCD_DEVICE=http://DEVICE_IP
uv run cli/rlcd.py send working --title '正在实现新功能' --detail '读取代码，准备构建和验证。' --progress 35
uv run cli/rlcd.py send working --progress 75
uv run cli/rlcd.py send waiting_input --title '需要你的确认' --detail '请回到电脑选择下一步操作。' --sound
uv run cli/rlcd.py send success --title '任务完成' --progress 100 --sound
uv run cli/rlcd.py status
```

只上报进度时，同一 Agent/任务保留之前的文字图片。传 `--title '' --detail ''` 清空文字。标题最多一行，正文最多两行，超出截断显示。标题 24px、正文 18px，按像素宽度换行。`--preview build/text.png` 保存实际上传的文字层。

不同任务可传 `--agent coding-agent --task task-42`。CLI 默认读取设备现有序号，再生成递增序号；多个并发发送者可能冲突，自动化调用可自行用 `--seq` 管理顺序，遇到 HTTP 409 时读取状态后再决定是否重试。

## HTTP 协议

`GET /agent/state` 查询当前 Agent 状态。`POST /agent/state` 原子校验并更新状态；验证失败不会修改旧状态或播放声音。

```json
{
  "agent_id": "coding-agent",
  "task_id": "task-42",
  "seq": 1,
  "state": "working",
  "progress": 65,
  "ttl_seconds": 120,
  "sound": false
}
```

- `agent_id`、`task_id`：必填，1–64 个 ASCII 字母、数字、点、下划线或连字符。
- `seq`：必填，整数 1–4294967295；同一 Agent/任务只接受大于当前值的序号，否则 409。重复请求不会刷新 TTL 或再次播放声音。
- `state`：必填，`idle`、`working`、`waiting_input`、`success`、`error`。
- `progress`：可省略或 null，表示未知；否则整数 0–100（不是 0–1）。working 的未知进度显示循环进度条。
- `ttl_seconds`：必填，整数 5–86400。超时后返回/显示 `stale`，停止进度动画并保留任务文字；更高序号可恢复。调用方应在 TTL 内继续上报。重启不保留状态或序号。
- `text_hex`：可选，固定 384×80 单色位图的十六进制字符串，长度 7680。逐行排列，每字节从最低位到最高位，1 为黑点；位置为屏幕 (8,159)。不传时同一任务保留旧图，新任务清空。CLI 负责中文渲染，直接 JSON 不支持 title/detail 文本排版。
- `sound`：可选布尔值，默认 false。仅进入 waiting_input/success/error 或从过期恢复到这些状态时尝试播放；同一状态的新进度不会重复响。需要提前用 `prepare` 或 `showcase --sound` 安装提示音。

成功返回 200 和当前状态；无效字段 400，超过 10000 字节的 JSON 请求体返回 413，旧序号 409。与现有 WebServer 相同，请求体先由服务器接收再校验大小，不是流式上传端点。

`sound_http_code` 表示本次状态更新的提示音启动结果：0 未尝试、202 已提交后台播放、409 音频忙、404 文件不存在等。声音启动失败不阻止画面更新，也不打断录音/其他播放；完成结果查 `/audio/status`。提示音不随动画循环。

## 页面与当前边界

收到有效状态后自动切到宠物页。BOOT 单击按网络、传感器、音频、宠物四页循环；KEY 继续使用录音/停止、双击回放，BOOT 长按继续配网。宠物页显示 IP 和 BLE 配网提示。过期不会自动切回网络页。

当前版本只维护一个 Agent 状态：新 Agent/任务替换当前内容，不保留多任务历史，也不进行优先级调度。成功画面保留到下一条更新或 TTL 到期。没有使用按键批准 Agent 操作。

猫的图形目前内置于固件，不包含任意宠物素材包上传或逐帧资源播放器；文字图片在 RAM 中，不写 SD。后续可在该接口基础上增加素材库。中文固定标签使用 U8g2 已有 GB2312 字体；动态文字使用 CLI 指定字体，字形可用性取决于该字体。

设备使用当前可信局域网 HTTP 服务，没有新增鉴权。较长的 SD HTTP 传输会延迟主循环动画；音频仍由独立任务处理。

## 验证

```sh
uv run --with pillow --with fonttools validation/agent_cli.py
uv run validation/agent_api.py http://DEVICE_IP
```

CLI 验证验证中文渲染、缺字报错、换行与逐像素位序；实机验证改变当前显示，检查所有状态、错误输入、序号去重、TTL 及恢复，不操作 SD 文件。
