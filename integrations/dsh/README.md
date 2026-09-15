# dsh-rlcd

把 DeepSeek Harness（dsh）的任务状态自动上报到 RLCD 开发板。相当于 [pi 扩展](../../../skills/rlcd/SKILL.md) 的 dsh 版本：不需要模型参与，dsh 一有状态变化就会更新屏幕上的宠物与中文摘要。

## 能力

| 时机 | 上报状态 | 显示 |
| --- | --- | --- |
| agent 开始工作（`agent/status` → running） | `working` | 「正在处理任务」+ 最近一次用户请求摘要 |
| agent 空闲（`agent/status` → idle） | `waiting_input` | 「等待你的指令」+ 摘要，带音效 |
| 用户提问（`user-questions/request`） | `waiting_input` | 「需要你的回答」+ 问题，带音效；答复后回到 working |
| 授权请求（`approval/request`） | `waiting_input` | 「需要你的授权」+ 原因/工具名，带音效；决定后回到 working |
| 会话结束（`agent/disposed`） | `idle` | 清空文字，设备回到待机页 |

另外注册：

- **`rlcd_report` 工具**：模型在实质阶段变化时推送有意义的中文标题、摘要与进度。
- **`/rlcd` 命令**：`on` / `off` / `status` / `test` / `sound on|off`。

## 安装

把插件文件装到 `$DSH_HOME/plugins/dsh-rlcd/`（默认 `~/.dsh/plugins/dsh-rlcd/`），再用 **home 级 patch** 注册。home patch 覆盖在每个 profile 自己的层之上，所以对所有 profile（web / headless / sdk / acp）都生效：

```sh
./install.sh          # 复制到 $DSH_HOME/plugins/dsh-rlcd/，并提示缺失的 patch 内容
```

等价于手动复制：

```sh
mkdir -p ~/.dsh/plugins/dsh-rlcd
cp index.ts smoke-test.ts README.md ~/.dsh/plugins/dsh-rlcd/
```

```yaml
# ~/.dsh/cordis.patch.yml —— home 级用户 patch 层
- insert:
    - id: dsh-rlcd
      name: '/Users/<you>/.dsh/plugins/dsh-rlcd/index.ts'
```

home patch 与 profile patch 都是 **config-only live HMR**，保存后立即生效，不必重启（改插件源码本身才需要重启 `dsh web`）。

用 dump 确认解析结果：

```sh
dsh --profile web --dump-config | grep -A1 -B1 dsh-rlcd
# == /Users/<you>/.dsh/cordis.patch.yml
# - id: dsh-rlcd
#   name: file:///Users/<you>/.dsh/plugins/dsh-rlcd/index.ts
```

前提：本机已安装 `rlcd` CLI 并配置好设备地址（`rlcd config set device http://DEVICE_IP`），且已安装宠物素材：

```sh
rlcd pet install deepseek-whale
```

## 验证

```sh
node ~/.dsh/plugins/dsh-rlcd/smoke-test.ts
```

用一个最小 ctx 桩加载插件、触发真实生命周期事件，再查询设备状态逐项断言（工具/命令注册、状态映射、宠物激活、子 agent 过滤、用户提问往返）。会真实改动开发板显示，结束时清空。

也可以直接看设备：

```sh
rlcd status          # agent_id=dsh、task_id=main 即本插件在生效
```

## 行为说明

- **固定任务槽**：上报固定用 `--agent dsh --task main`。设备每次启动最多保留 64 个 agent/task 序号，固定槽位让序号持续递增，不会因为会话切换耗尽序号表。
- **宠物**：每条上报都带 `--pet deepseek-whale`，未安装时自动省略并只在日志里提醒一次；不会改动 `rlcd pet use` 保存的默认选择。
- **子 agent 被过滤**：`session.header.origin === 'subagent'` 或 `delegationDepth > 0` 的会话不上报，避免 subagent 抢占设备唯一的显示位。
- **TTL 与保活**：working 600s、waiting_input 3600s、idle 300s。会话记录里的 `step/end` / `turn/end` 会按 TTL 的 60% 做一次保活，长轮次不会提前回到待机页。
- **失败不阻塞主任务**：连续失败 3 次后暂停上报并记一条日志；`/rlcd on` 可恢复。409（旧序号）会重新读序号重试一次。
- **文字保留**：同一状态重复上报时省略 `--title/--detail`，设备保留已有文字；切换状态时才重写。

## 开发约束

本文件是**零运行时依赖**的：只 import `node:child_process` / `node:os` / `node:path`，所有 `@deepseek-ai/*` 都是 `import type`，会被 Node 的类型擦除移除。因此插件可以直接躺在 `~/.dsh/plugins/` 下、由 home patch 用绝对路径加载，不需要它所在目录能解析到 dsh 的包。

新增 `@deepseek-ai/*` 导入时**必须用 `import type`**。用普通 `import` 会在加载时报 `ERR_MODULE_NOT_FOUND`。

类型检查需要在能解析 dsh 包的目录里跑（仓库本身没有 node_modules）：

```sh
mkdir -p /tmp/rlcd-check/node_modules/@types
ln -sfn "$DSH_CHECKOUT/node_modules/@types/node" /tmp/rlcd-check/node_modules/@types/node
ln -sfn ~/.dsh/profiles/node_modules/@deepseek-ai /tmp/rlcd-check/node_modules/@deepseek-ai
cp ~/.dsh/plugins/dsh-rlcd/index.ts /tmp/rlcd-check/plugin.ts
cd "$DSH_CHECKOUT" && pnpm exec tsc --ignoreConfig --noEmit --strict --skipLibCheck \
  --module nodenext --moduleResolution nodenext --target es2022 --types node /tmp/rlcd-check/plugin.ts
```

注意：home/profile patch 的 HMR 是 **config-only**，改 `cordis.patch.yml` 会立即生效，但修改本插件源码本身需要重启 `dsh web` 才会重新加载。

改完仓库里的源码后，重新 `cp` 到 `~/.dsh/plugins/dsh-rlcd/` 并重启 `dsh web`。
