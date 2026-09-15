# Agent 中文桌宠（v0.6.0）

如果要让 AI 在工作中自动上报状态，优先使用仓库中的 `skills/rlcd/SKILL.md`；本文作为接口与渲染协议参考。SKILL 可独立提供给 AI，必要时再读取本文。

此版本在设备上播放宠物素材动画、原生绘制中文状态标签和进度条；CLI 使用电脑字体把动态中文排版为独立位图，经 HTTP 一次提交。文字不变时可只提交状态/进度。动画目标更新间隔为 125ms；实际帧率受显示传输及 HTTP 操作影响，尚未测量。

## 快速体验

电脑安装 uv，设备运行 v0.5.0 并联网。先按照 [宠物素材指南](pet-format.md) 安装和选择宠物。以下地址替换为屏幕上的 IP：

```sh
rlcd --device http://DEVICE_IP showcase --sound
```

依次显示待机、分析项目、构建固件、等待输入、任务完成，每个阶段约四秒。`--sound` 请求播放所选宠物的状态音效；未配置时不播放。

CLI 的 Pillow 和 fontTools 依赖由 uv tool install 安装到隔离环境。默认查找 macOS STHeiti、Linux Noto Sans CJK、Windows 微软雅黑，也可以传 `--font /path/to/font.ttf` 或设置 `RLCD_FONT`。不会把电脑字体上传或加入仓库；字形缺失会明确报错。

```sh
export RLCD_DEVICE=http://DEVICE_IP
rlcd send working --title '正在实现新功能' --detail '读取代码，准备构建和验证。' --progress 35
rlcd send working --progress 75
rlcd send waiting_input --title '需要你的确认' --detail '请回到电脑选择下一步操作。' --sound
rlcd send success --title '任务完成' --progress 100 --sound
rlcd status
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
- `seq`：必填，整数 1–4294967295；按 Agent/任务独立记录，只接受大于该任务已接收值的序号，否则 409。重复请求不会刷新 TTL 或再次播放声音。
- `state`：必填，`idle`、`working`、`waiting_input`、`success`、`error`。
- `progress`：可省略或 null，表示未知；否则整数 0–100（不是 0–1）。working 的未知进度显示循环进度条。
- `ttl_seconds`：必填，整数 5–86400。超时后 API 返回 `stale`，屏幕进入时钟与传感器待机页，不显示“状态已过期”提示；更高序号可恢复。调用方应在 TTL 内继续上报。重启不保留状态或序号。
- `text_hex`：可选，固定 384×80 单色位图的十六进制字符串，长度 7680。逐行排列，每字节从最低位到最高位，1 为黑点；位置为屏幕 (8,159)。不传时同一任务保留旧图，新任务清空。CLI 负责中文渲染，直接 JSON 不支持 title/detail 文本排版。
- `sound`：可选布尔值，默认 false。进入新状态/任务或从过期恢复时，尝试播放该状态配置的音效；同一状态的新进度不会重复响。需要提前安装并选择宠物素材包。

成功返回 200 和当前状态；无效字段 400，超过 10000 字节的 JSON 请求体返回 413，旧序号 409。与现有 WebServer 相同，请求体先由服务器接收再校验大小，不是流式上传端点。

`sound_http_code` 表示本次状态更新的提示音启动结果：0 未尝试、202 已提交后台播放、409 音频忙、404 文件不存在等。声音启动失败不阻止画面更新，也不打断录音/其他播放；完成结果查 `/audio/status`。提示音不随动画循环。

## 页面与当前边界

收到有效状态后自动切到宠物页。BOOT 单击按网络、传感器、音频、宠物四页循环；KEY 继续使用录音/停止、双击回放，BOOT 长按继续配网。宠物页显示 IP 和 BLE 配网提示。过期进入待机页，新的有效上报立即唤醒宠物页。

当前版本只维护一个 Agent 状态：新 Agent/任务替换当前内容，只保留各任务序号，不保留多任务画面，也不进行优先级调度。idle/success 画面最多停留 30 秒；working/waiting_input/error 保留到更新或 TTL 到期。没有使用按键批准 Agent 操作。

宠物图形和音效由素材包提供，新增形象不再需要修改或烧录固件，详见 [宠物素材规范](pet-format.md)。文字图片仍在 RAM 中；固定中文标签使用 U8g2 GB2312 字体，动态文字使用 CLI 指定字体。

设备使用当前可信局域网 HTTP 服务，没有新增鉴权。较长的 SD HTTP 传输会延迟主循环动画；音频仍由独立任务处理。

## 验证

```sh
uv run --with pillow --with fonttools validation/agent_cli.py
uv run validation/agent_api.py http://DEVICE_IP
```

CLI 验证验证中文渲染、缺字报错、换行与逐像素位序；实机验证改变当前显示，检查所有状态、错误输入、序号去重、TTL 及恢复，不操作 SD 文件。

## 待机与多客户端（v0.6.0）

开机默认进入待机页，显示 RTC 时钟、日期、温湿度、电池电压、网络状态和配网提示。时钟显示到分钟，沿用五秒传感器采样；RTC 无效时显示不可用，不自动校准时间。

- 没有上报或 TTL 到期：待机。
- `idle` / `success`：最后一次有效上报后 30 秒待机（TTL 更短时以 TTL 为准）。
- `working` / `waiting_input` / `error`：保留到下一条上报或 TTL 到期。
- API 新增 `standby`（宠物页是否处于待机模式）、`expired`、`pet_id`。手动切到其他硬件页时，standby 仍表示宠物页模式。

```sh
rlcd send working --agent deepseek --task task-42 --pet deepseek-whale --title '正在思考'
rlcd send working --agent coding-agent --task task-43 --pet pixel-cat --title '正在构建'
```

HTTP 使用可选 `pet_id` 字段（已登记的宠物 id，不是显示名称）。未指定保持当前宠物；不存在返回 404，无效 id 或素材不可用返回 400，整条请求不更新任务、序号、画面。自动切换只改变 RAM 中的选择，不写 NVS；重启仍加载 `pet use` 保存的默认宠物。`GET /pets` 的 `runtime_path` 是当前素材，`active_path` 仍是持久化默认素材。

最后一条有效上报占据屏幕。每个客户端应在需要激活自己宠物的上报中携带 pet_id；切换回某个任务时需重新发送文字层，设备不保存各任务的旧画面。

CLI 按任务查询序号：`GET /agent/state?agent_id=...&task_id=...` 返回该任务的 id 和 seq（未出现为 0），不是历史画面或任务内容。设备 RAM 最多保留 64 个不同 Agent/任务组合的序号，不随 TTL 清除或自动淘汰，避免旧请求抢回画面。达到上限后新任务返回 503 `task_history_full`，已有任务仍可更新；重启清空序号表。长期运行可为每个客户端采用固定任务槽并持续递增序号。

本次 CLI 与固件需要同时升级到 0.6.0。30 秒为当前固定待机延迟，尚无可调配置项。
