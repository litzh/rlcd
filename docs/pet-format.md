# 宠物素材规范与接入指南

用户只需提供 PNG、可选 WAV 和 `pet.json`。CLI 负责缩放、黑白转换、音频转换与打包；固件使用统一播放器。安装过 v0.5.0 固件后，新增或更换宠物无需重新烧录。

## 使用随附的像素猫

在项目根目录执行：

```sh
# 本地校验并生成实际黑白效果预览，不需要开发板
rlcd pet preview pixel-cat

# 地址替换成屏幕上的 IP；安装只登记资源，不切换正在显示的宠物
rlcd --device http://DEVICE_IP pet install pixel-cat
rlcd --device http://DEVICE_IP pet use pixel-cat
rlcd --device http://DEVICE_IP pet list

# 上报状态或依次体验各状态
rlcd --device http://DEVICE_IP send working --title '正在处理任务' --progress 35
rlcd --device http://DEVICE_IP showcase --sound
```

也可以设置环境变量 `RLCD_DEVICE=http://DEVICE_IP`，省略后续命令的 `--device`。

`preview` 输出六个状态 GIF 和 `overview.png`。总览从左到右、从上到下为 idle、working、waiting_input、success、error、stale；GIF 按配置播放。CLI 依赖在安装时由 uv 管理。

## 创建自己的宠物

复制 `pets/pixel-cat/`，修改 `id`、`name`，替换 PNG/WAV 并更新帧列表即可。也可以从最小包开始：

```text
my-pet/
├── pet.json
└── idle/
    ├── 001.png
    └── 002.png
```

```json
{
  "format_version": 1,
  "id": "my-pet",
  "name": "我的宠物",
  "canvas": {"width": 160, "height": 128},
  "states": {
    "idle": {
      "frames": ["idle/001.png", "idle/002.png"],
      "frame_ms": 250,
      "loop": true
    }
  }
}
```

- `format_version`：固定整数 1。
- `id`：1–32 位小写字母、数字、下划线、连字符，首位必须为字母或数字；是安装和选择的唯一标识。
- `name`：显示名称，可中文，1–96 个 UTF-8 字节；省略时使用 id。
- `canvas`：v1 固定 160×128。宠物位于横屏 (120,30)，中文文字区域与进度条由系统管理，素材不能覆盖它们。
- `states`：必须有 `idle`；可再提供 `working`、`waiting_input`、`success`、`error`、`stale`。不接受其他状态名。缺失状态使用 idle 的帧、时长与循环规则，不继承 idle 的音效。
- `frames`：按数组顺序播放，不自动按文件名排序。每个状态 1–64 帧；包含回退状态展开后，总计不超过 96 帧。
- `frame_ms`：每帧时长，整数 125–5000，默认 250。当前一整个状态使用同一时长；重复某一帧的路径可以延长停留。
- `loop`：布尔值，默认 true。false 表示播完后停留在最后一帧，直到状态切换；idle/success 超过待机延迟也会离开宠物画面。
- `sound`：可选包内 WAV 相对路径。配置了音效，并且 Agent 请求 `sound=true` 时，在进入该状态时播放一次；不会随动画循环，也不打断录音或其他音频。

同一状态的进度/文字更新不会重启动画或重复音效。切换任务、切换状态、选择新宠物后从首帧开始。TTL 自动过期进入时钟与传感器待机页；过期本身不触发声音。新宠物被选择时也不自动响铃。

## 图片与音频要求

图片必须是 PNG，包内所有图片尺寸一致，每边不超过 2048 像素；建议直接提供 160×128，黑白像素画最容易获得清晰效果。透明区域合成到白底，CLI 保持比例缩放并居中补白，灰度以阈值 160 转成黑白，不做抖动。以 CLI 生成的预览为最终显示效果参考。

图片路径和音效路径必须相对于素材包，不能使用绝对路径、目录外的文件或指向包外的符号链接。所有引用必须存在，文件扩展名必须匹配。

音效输入支持未压缩 PCM WAV：8/16bit、单/双声道、8–96kHz、时长大于 0 且不超过 5 秒。CLI 混合声道并以线性插值转换为 16kHz/16bit 单声道；高质量音效建议提前导出成目标格式。暂不支持 MP3、浮点 WAV 或 24bit WAV。不要依赖透明 PNG 或彩色原图在单色屏上保留原有层次。

## 安装、存储与恢复

1. CLI 在本机完整校验所有素材，生成 RLP1 动画包及转换后的 WAV。
2. 资源以内容 SHA-256 前 24 个十六进制字符命名，例如 `/pet-<hash>.rlp`。已有同名且内容一致的文件直接复用，不覆盖其他内容。
3. 使用现有流式 SD 上传接口写入临时文件，完成后改名；CLI 再下载进行逐字节校验。
4. `/pets/register` 校验包哈希、元数据、帧边界与音效 WAV 后，才把宠物登记到 NVS。
5. `/pets/use` 先加载、校验候选资源，再保存选择并替换内存中的宠物。失败时保持原选择。

最多登记 16 个 id。同 id 重新安装会更新登记的版本，但不会立即替换正在播放的版本；再次 `pet use` 才启用。当前选择跨重启保存，注册表和选择保存在 NVS，素材位于 SD。未选择宠物时显示安装提示；重启后素材缺失/损坏则报告 `saved_pet_unavailable`，联网与其他设备功能仍可使用。

每套动画最多约 250KB，加载到 PSRAM 后逐帧绘制；切换时短暂同时保留新旧两套。不会每帧读取 SD。声音由已有后台任务从 SD 播放。大文件 HTTP 请求期间主循环刷新仍可能延迟，帧率是目标节奏而非硬实时保证。

上传失败不会登记或切换半成品。此前已上传成功的内容文件会保留，重试可复用；目前没有自动清理旧版本或卸载命令。通用 SD 删除接口仍可删除资源，手动清理前应确认注册表和当前 `active_path` 不引用该文件。当前动画已在 RAM 中，删除文件可能到下次选择或重启才暴露问题。

## HTTP 接口

- `GET /pets`：返回 `pets` 注册列表、`active_id`、`active_path`、当前渲染的 `state` / `frame` 及启动加载 `error`。切到其他页面时 frame 保留上次绘制值。
- `POST /pets/register`，JSON `{"path":"/pet-<hash>.rlp"}`：登记完整资源，200；资源无效/缺失 400；超过 16 个 id 返回 409；NVS 保存失败 503。
- `POST /pets/use`，JSON `{"id":"pixel-cat"}`：选择并自动切到宠物页，200；id 无效/资源不可用 400；未登记 404；NVS 保存失败 503。

请求体最大 256 字节。`/agent/state` 协议保持不变，音效来自当前宠物的对应状态；未配置音效时 `sound_http_code=0`。音频忙或文件不可用时画面正常更新，音频结果由该字段及 `/audio/status` 报告。

## 编译格式 RLP1（供工具开发）

文件内容为 4 字节 ASCII `RLP1`、4 字节小端 JSON 长度、UTF-8 JSON（最多 4096 字节）、连续单色帧数据。每帧固定 2560 字节：160×128，逐行，每字节最低位对应左边像素，1 表示黑点。

JSON 包含 `id`、`name` 和固定六项的 `states` 数组，顺序与上文一致。每项包含 `offset`（相对帧数据起点的字节偏移）、`count`、`frame_ms`、`loop`、`sound`（SD 绝对路径，未配置为 ""）。各段必须连续且刚好覆盖文件尾，避免读取越界或多余数据。通常应直接复用 `src/rlcd/pet_assets.py`，不要手工生成此格式。

## 开发验证

```sh
uv run --with pillow validation/pet_assets.py
uv run --with pillow --with fonttools validation/pet_device.py http://DEVICE_IP
```

第一条在本机检查素材格式和像素编码；第二条会安装并选择像素猫、切换状态及播放短音效，只删除本轮唯一生成的无效资源，保留安装好的宠物。

自 v0.6.0 起，Agent 可通过 `pet_id`（CLI `--pet`）临时选择宠物，不改变持久化默认选择；详情见 [Agent 接入](agent.md)。stale 素材仍是兼容格式的一部分，但 TTL 到期不再播放它。

没有活动任务时，`pet use` 只选择默认宠物，宠物页面仍显示待机；发送 Agent 状态后才展示动画。
