# CLI 安装与配置

## 安装

在包含 `pyproject.toml` 的项目根目录执行：

```sh
uv tool install .
rlcd config init
rlcd config set device http://DEVICE_IP
```

uv 安装命令是 `uv tool install`，不是 `uv install`。CLI 安装到 uv 管理的独立环境，不需要手动执行 Python 文件。安装后可以从任意目录使用 `rlcd`；若命令不在 PATH，按 uv 的提示运行 `uv tool update-shell` 并重新打开终端。官方说明：[uv tools](https://docs.astral.sh/uv/guides/tools/)。

也可从其他目录指定源码绝对路径，例如 `uv tool install /path/to/rlcd`。项目尚未发布到 PyPI，不应使用不带来源的 `uv tool install rlcd`。更新本地安装：`uv tool install --reinstall /path/to/rlcd`。

开发时使用 `uv run rlcd ...`，或 `uv tool install --editable .`。开发目录中的 `.venv` 和 uv 自身环境、缓存由 uv 管理，应用配置和资源仍使用下述目录。

## 目录

```text
~/.config/rlcd/
├── config.json          设备地址及可选字体路径
├── pets/
│   ├── pixel-cat/       可编辑的内置像素猫
│   └── deepseek-whale/  可编辑的内置鲸鱼
├── previews/<pet-id>/   默认 GIF 和 PNG 预览
├── packages/<pet-id>/   安装时编译的 RLP/WAV 资源
└── docs/                使用指南副本
```

`config init` 初始化素材与文档，无需连接设备；首次使用 `pet preview` / `pet install` 也会自动初始化。`config.json` 在首次 `config set` 时创建。升级或重复初始化不会覆盖已有宠物目录和文档；若希望采用新版内置素材，请先备份并移走对应目录，再运行 `config init`。

配置以 UTF-8 JSON 原子写入，文件权限为 0600。仅保存 `device` 和 `font`，不读取项目 `.env`，不保存 Wi-Fi 凭据。设备固件构建参数仍独立使用 `RLCD_WIFI_SSID` / `RLCD_WIFI_PASSWORD`，此项没有变化。

## 配置与优先级

```sh
rlcd config show
rlcd config set device http://DEVICE_IP
rlcd config set font '/path/to/chinese-font.ttc'
```

设备和字体优先级均为：命令行参数 > `RLCD_DEVICE` / `RLCD_FONT` 环境变量 > config.json。未配置字体时尝试已有系统中文字体。字体文件保持在原位置，配置只保存路径，不复制系统字体。

临时指定其他设备：`rlcd --device http://OTHER_IP status`。

## 素材与 Agent

```sh
rlcd pet preview deepseek-whale
rlcd pet install deepseek-whale
rlcd pet use deepseek-whale
rlcd pet list
rlcd send working --title '正在处理任务' --progress 35
rlcd showcase --sound
```

内置素材可直接按 id 使用，默认解析到 `~/.config/rlcd/pets/<id>`。也支持显式的外部素材目录，例如 `rlcd pet install ./my-pet`；源码素材仍保留在用户指定目录，编译资源保存在 `~/.config/rlcd/packages/<id>`。

`pet preview` 默认输出到 `~/.config/rlcd/previews/<id>`；可通过 `--output` 明确指定其他位置。`send --preview` 的输出文件同样使用用户显式指定的位置。默认工作流不会把生成资源写进当前工作目录。

素材格式见 [宠物素材规范](pet-format.md)，状态接口见 [Agent 接入](agent.md)。

## 打包与验证

```sh
uv build
uv run validation/cli_config.py
uv run validation/agent_cli.py
uv run validation/pet_assets.py
```

wheel 只包含 CLI、宠物素材和文档，不包含固件、构建目录、Wi-Fi 生成头文件或 `.env`。CLI 的版本独立于设备固件；本次 CLI 打包不需要重新烧录设备。
