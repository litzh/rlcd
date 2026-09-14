---
name: rlcd
description: 使用 rlcd CLI 或开发板 HTTP API 展示 Agent 任务状态、中文摘要、进度和等待输入提示，并管理桌面宠物素材。用户要求向 RLCD 开发板上报任务、控制宠物显示或调用设备接口时使用。
---

# RLCD Agent 接入

优先使用已安装的 `rlcd` 命令。它会读取设备配置、渲染中文文字层并处理状态序号。日常状态上报不需要修改固件，也不需要逐帧发送图片。

## 连接与配置

- `rlcd config show` 查看配置；`rlcd status` 查询当前 **Agent 状态**，不是温湿度等硬件状态。
- 默认配置为 `~/.config/rlcd/config.json`，包含 `device` 和可选 `font`。优先级：命令行参数 > `RLCD_DEVICE` / `RLCD_FONT` > 配置文件。
- 临时指定设备用 `rlcd --device http://DEVICE_IP status`，`--device` 必须位于子命令之前。从用户提供的地址、配置或设备屏幕取实际地址，不硬编码历史 IP。
- 缺少设备地址时询问用户地址；不要为了上报状态重新配网或烧录。`rlcd config set device http://DEVICE_IP` 可保存用户指定地址。
- 命令未安装时，已知本项目源码路径可用 `uv tool install /path/to/rlcd` 安装；不要假定 PyPI 上的同名包属于本项目。

## 在任务中上报

用户要求把任务显示到 RLCD 时，在开始、实质阶段变化、需要输入和结束时上报。继续完成用户原任务，不要把设备展示变成主任务。

为一次任务选择稳定且可区分的 `--agent` / `--task`，后续保持一致。ID 为 1–64 位 ASCII 字母、数字、点、下划线或连字符；下面的 `task-42` 是示意值。

```sh
rlcd send working --agent coding-agent --task task-42 \
  --title '正在分析项目' --detail '读取相关代码与文档' --ttl 900

rlcd send working --agent coding-agent --task task-42 \
  --title '正在构建' --detail '已完成实现，开始验证' --progress 70 --ttl 900

rlcd send waiting_input --agent coding-agent --task task-42 \
  --title '需要你的确认' --detail '请回到电脑选择下一步' --ttl 3600

rlcd send success --agent coding-agent --task task-42 \
  --title '任务完成' --detail '构建与验证已通过' --progress 100 --ttl 3600
```

- `working`：正在处理；`waiting_input`：确实需要用户输入；`success`：任务已完成；`error`：失败或无法继续；`idle`：没有活动任务。
- 进度为整数 0–100。有可衡量依据时才传 `--progress`；未知时省略，由设备显示循环进度条。省略进度会变为未知，不是保留旧百分比。
- 标题一行，正文两行；过长会截断。显示简短摘要，不发送凭据或包含敏感信息的整段日志。
- 同一任务不传 `--title` / `--detail` 会保留旧文字图片；传 `--title '' --detail ''` 清空。换任务时更新文字，避免旧内容造成误解。
- CLI 默认读取设备状态并生成序号，通常不传 `--seq`。当前设备只有一个状态位置，新的 Agent/任务会替换当前显示；不要让多个并行工作者竞争刷新同一设备。
- `--ttl` 默认 120 秒，允许 5–86400。超时显示 `stale`。按预期阶段长度选择 TTL，在实际继续工作时更新；不要建立无期限循环或声称已启动后台心跳。
- 音效默认关闭。用户希望声音提醒时可加 `--sound`；对应素材未配置声音则不播放。不要在每次进度更新时主动提醒。
- `waiting_input` 仅是显示提示，实际问题仍需在对话中提出；设备按键当前没有批准/回答 Agent 请求的含义。
- 不用 `showcase` 上报真实任务：它播放固定的示例状态序列，不代表当前工作的事实进展。

## 失败处理

设备上报失败不应阻塞与它无关的主任务。连接异常可重试一次，仍失败就简短告知并继续原任务，不反复轮询、重置设备或改 Wi-Fi。

HTTP 409 可能是旧序号：读取 `rlcd status`，确认仍是当前任务后再发送最新状态，不重放过期事件。HTTP 400/413 应修正输入，不原样重试。

状态响应中的 `sound_http_code` 是音效启动结果，和上报请求的 HTTP 状态码不同：202 表示已排队，409 表示音频忙，0 表示未尝试或没有配置音效。画面更新成功而音效忙时，不重复上报来强行响铃；播放完成情况查询 `/audio/status`。

## 直接调用 HTTP

非 CLI 集成或只需查询设备时使用 HTTP。`DEVICE_URL` 表示从有效配置或用户输入取得的基础 URL；调用局域网接口时避免系统代理转发。

```sh
curl --noproxy '*' --max-time 10 "$DEVICE_URL/agent/state"
curl --noproxy '*' --max-time 10 "$DEVICE_URL/status"
```

- `GET /agent/state`：任务状态、序号、TTL 和音效启动结果。
- `GET /status`：固件、Wi-Fi、传感器、音频、SD 和按键状态。
- `POST /agent/state`：JSON 状态更新。必填 `agent_id`、`task_id`、`seq`、`state`、`ttl_seconds`；`progress` 可省略或 null，`sound` 可省略或为布尔值。

```json
{
  "agent_id": "coding-agent",
  "task_id": "task-42",
  "seq": 18,
  "state": "working",
  "progress": 70,
  "ttl_seconds": 900,
  "sound": false
}
```

以上序号仅示意。发送前读取当前状态，同一任务使用更大的整数序号（1–4294967295），不要固定使用示例值。重复或旧序号返回 409，且不会刷新 TTL。设备重启后不保留任务序号。

HTTP 的 `/agent/state` **不渲染 `title` / `detail` 文本字段**。中文显示使用 CLI；自行实现客户端时才生成固定 384×80 的 `text_hex` 位图。`POST /echo` 只支持 ASCII，不能用来替代中文文字层。

## 宠物与其他设备能力

只在用户要求配置或更换宠物时进行安装/选择；状态上报通常使用当前宠物。

```sh
rlcd pet list
rlcd pet preview deepseek-whale
rlcd pet install deepseek-whale
rlcd pet use deepseek-whale
```

素材按 id 从 `~/.config/rlcd/pets` 读取，也支持显式目录。默认预览在 `~/.config/rlcd/previews`，编译资源在 `~/.config/rlcd/packages`。`install` 登记资源但不切换；`use` 才启用并跨重启保存。

更深入的接口按需查阅 `~/.config/rlcd/docs/` 下的文件（源码仓库对应 `docs/`）：

- `cli.md`：安装、配置、参数优先级和资源目录。
- `agent.md`：状态接口、文字位图格式和大小边界。
- `pet-format.md`：新增宠物的 PNG/WAV 规范、安装与选择 API。
- `media-api.md`：用户要求录音、扬声器、SD 文件或按键操作时读取。单纯状态上报不调用录音或删除文件接口。

若本地参考文档尚未初始化，`rlcd config init` 可补齐内置资源和文档；主流程所需信息已包含在本 SKILL 中。
