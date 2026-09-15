/**
 * dsh-rlcd — 把 DeepSeek Harness 的任务状态上报到 RLCD 开发板。
 *
 * 自动上报（无需模型参与）：
 * - `agent/status` → running：working（标题 + 最近一次用户请求摘要）
 * - `agent/status` → idle：waiting_input（等待用户指令，带音效）
 * - `user-questions/request` / `approval/request`：waiting_input（带音效），
 *   用户答复后回到 working
 * - `agent/disposed`：idle（清空文字，设备回到待机页）
 * - 会话记录里的 step/turn 边界按 TTL 做 keepalive，长任务不会提前超时
 *
 * 另外注册 `rlcd_report` 工具（模型可推送有意义的中文标题/摘要/进度）与
 * `/rlcd` 命令（on | off | status | test | sound on|off）。
 *
 * 上报使用固定任务槽 `--agent dsh --task main`：设备每次启动最多保留 64 个
 * agent/task 序号，固定槽位可以持续递增序号，不会因会话切换而耗尽序号表。
 * 每次上报都带 `--pet deepseek-whale` 激活本项目的宠物；未安装时自动省略。
 *
 * 子 agent（subagent）不参与上报，避免它抢占设备唯一的显示位。
 * 上报失败不阻塞主任务：连接异常重试一次，连续失败后暂停上报并记录一次日志。
 *
 * 实现约束：本文件只依赖 node 内置模块（`node:child_process` / `node:os` /
 * `node:path`），`@deepseek-ai/*` 全部是 `import type`，会被 Node 的类型擦除
 * 移除。因此它可以从任意路径（例如 profile patch 里的绝对路径）直接加载，
 * 不需要 node_modules 里能解析到 dsh 的包。
 */

import { execFile, spawn } from 'node:child_process'
import { homedir } from 'node:os'
import { join } from 'node:path'

import type { Context } from '@deepseek-ai/cordis'
import type { Agent } from '@deepseek-ai/dsh-agent'
// 下面的空导入只为加载各包的事件类型声明（供编辑器与类型检查使用）。
import type {} from '@deepseek-ai/dsh-commands'
import type {} from '@deepseek-ai/dsh-session'
import type {} from '@deepseek-ai/dsh-tools'
import type {} from '@deepseek-ai/dsh-user-approval'
import type {} from '@deepseek-ai/dsh-user-questions'

export const name = 'dsh-rlcd'

/** 固定任务槽：设备每次启动最多保留 64 个 agent/task 序号。 */
const AGENT_ID = 'dsh'
const TASK_ID = 'main'
const PET_ID = 'deepseek-whale'

const WORKING_TTL = 600
const WAITING_TTL = 3600
const IDLE_TTL = 300
/** 工作中按 TTL 的 60% 刷新，避免长轮次提前回到待机页。 */
const KEEPALIVE_MS = WORKING_TTL * 0.6 * 1000
const SEND_TIMEOUT_MS = 15_000
const MAX_FAILURES = 3

/** 设备文字层：标题一行，正文两行。 */
const TITLE_MAX = 20
const DETAIL_MAX = 60

type RlcdState = 'working' | 'waiting_input' | 'success' | 'error' | 'idle'

const STATES: readonly RlcdState[] = ['working', 'waiting_input', 'success', 'error', 'idle']

interface Report {
  state: RlcdState
  title?: string
  detail?: string
  progress?: number
  sound?: boolean
  ttl?: number
}

/** 折叠为单行并按字符数截断。 */
function oneLine(text: string, max: number): string {
  const t = text.replace(/\s+/g, ' ').trim()
  return t.length > max ? `${t.slice(0, max - 1)}…` : t
}

/** 从消息 content 块里取纯文本。 */
function textOf(content: unknown): string {
  if (!Array.isArray(content)) return ''
  const parts: string[] = []
  for (const block of content) {
    if (block !== null && typeof block === 'object') {
      const candidate = block as { type?: unknown; text?: unknown }
      if (candidate.type === 'text' && typeof candidate.text === 'string') parts.push(candidate.text)
    }
  }
  return parts.join(' ')
}

/** rlcd 候选可执行文件：先按 PATH，再查常见安装目录。 */
function cliCandidates(): string[] {
  const candidates = ['rlcd', join(homedir(), '.local', 'bin', 'rlcd')]
  for (const dir of (process.env.PATH ?? '').split(':')) {
    if (dir !== '') candidates.push(join(dir, 'rlcd'))
  }
  return [...new Set(candidates)]
}

export function apply(ctx: Context): void {
  let enabled = true
  let checked = false
  let cli = 'rlcd'
  let deviceUrl = ''
  let soundEnabled = true
  let petAvailable = false
  let unavailable = false
  let failures = 0
  let disposed = false

  /** 最近一次用户请求的摘要，用作 working / waiting_input 的正文。 */
  let lastPrompt = ''
  /** 最近一次成功上报的状态；null 表示尚未上报或已被清空。 */
  let lastState: RlcdState | null = null
  let lastSentAt = 0

  const warn = (message: string, ...args: unknown[]): void => {
    try {
      ctx.logger.warn(`[dsh-rlcd] ${message}`, ...args)
    } catch {
      // 日志失败无关紧要
    }
  }

  /** 顶层会话才算主任务；子 agent 不占用设备唯一的显示位。 */
  interface SessionHeaderLike {
    origin?: unknown
    delegationDepth?: unknown
  }

  function isPrimaryHeader(header: SessionHeaderLike | undefined): boolean {
    if (header === undefined) return true
    if (header.origin === 'subagent') return false
    const depth = header.delegationDepth
    return typeof depth !== 'number' || depth === 0
  }

  function isPrimary(agent: Agent): boolean {
    try {
      return isPrimaryHeader(
        (agent as { session?: { header?: SessionHeaderLike } }).session?.header,
      )
    } catch {
      return true
    }
  }

  function buildArgs(report: Report): string[] {
    const args = [
      'send',
      report.state,
      '--agent',
      AGENT_ID,
      '--task',
      TASK_ID,
      '--ttl',
      String(report.ttl ?? WORKING_TTL),
    ]
    if (report.title !== undefined) args.push('--title', report.title)
    if (report.detail !== undefined) args.push('--detail', report.detail)
    if (petAvailable) args.push('--pet', PET_ID)
    if (report.progress !== undefined) {
      const clamped = Math.max(0, Math.min(100, Math.round(report.progress)))
      args.push('--progress', String(clamped))
    }
    if (report.sound === true && soundEnabled) args.push('--sound')
    return args
  }

  /**
   * 惰性探测 rlcd CLI：不存在或不带配置时静默停用，不打扰用户。
   * 同时缓存设备地址与 deepseek-whale 宠物是否可用。
   */
  function ensureChecked(): void {
    if (checked) return
    checked = true
    const candidates = cliCandidates()

    const tryCandidate = (index: number): void => {
      if (disposed) return
      if (index >= candidates.length) {
        enabled = false
        unavailable = true
        warn('未找到可用的 rlcd CLI，已停用上报（安装：uv tool install <rlcd 源码路径>）')
        return
      }
      const candidate = candidates[index] as string
      execFile(candidate, ['config', 'show'], { timeout: 5000 }, (error, stdout) => {
        if (disposed) return
        if (error) {
          tryCandidate(index + 1)
          return
        }
        cli = candidate
        try {
          const parsed = JSON.parse(String(stdout)) as { settings?: { device?: unknown } }
          if (typeof parsed.settings?.device === 'string') deviceUrl = parsed.settings.device
        } catch {
          // 配置输出无法解析时不影响上报
        }
        execFile(cli, ['pet', 'list'], { timeout: 8000 }, (petError, petStdout) => {
          if (disposed || petError) return
          try {
            const parsed = JSON.parse(String(petStdout)) as { pets?: { id?: unknown }[] }
            petAvailable = (parsed.pets ?? []).some(pet => pet.id === PET_ID)
            if (!petAvailable) {
              warn('未安装 %s 宠物，上报将沿用设备当前宠物（rlcd pet install %s 可安装）', PET_ID, PET_ID)
            }
          } catch {
            // 输出无法解析时保守不带 --pet
          }
        })
      })
    }

    tryCandidate(0)
  }

  /** fire-and-forget 上报；409 旧序号时重新读序号重试一次。 */
  function send(report: Report, retried = false): void {
    if (!enabled || disposed) return
    lastSentAt = Date.now()
    try {
      execFile(cli, buildArgs(report), { timeout: SEND_TIMEOUT_MS }, (error) => {
        if (disposed) return
        if (error && !retried && /409|stale/i.test(String(error.message))) {
          // CLI 会重新读取设备序号，重试一次即可
          setTimeout(() => send(report, true), 1000).unref()
          return
        }
        if (error) {
          failures += 1
          if (failures >= MAX_FAILURES) {
            enabled = false
            warn('连续上报失败，已暂停上报（/rlcd on 可恢复）：%s', error.message)
          }
          return
        }
        failures = 0
        lastState = report.state
      })
    } catch (error) {
      warn('上报调用失败：%o', error)
    }
  }

  /** 等待结果的路径（工具与命令用），永远不会抛异常。 */
  function sendAwait(report: Report): Promise<string> {
    if (disposed) return Promise.resolve('RLCD 上报已停止。')
    lastSentAt = Date.now()
    return new Promise((resolve) => {
      try {
        execFile(cli, buildArgs(report), { timeout: SEND_TIMEOUT_MS }, (error, stdout) => {
          if (error) {
            failures += 1
            resolve(`RLCD 上报失败：${error.message}`)
            return
          }
          failures = 0
          lastState = report.state
          resolve(`已上报到 RLCD：${oneLine(String(stdout).trim() || 'ok', 200)}`)
        })
      } catch (error) {
        resolve(`RLCD 上报失败：${String(error)}`)
      }
    })
  }

  /** 进程退出路径：detached 子进程，不持有事件循环。 */
  function sendDetached(report: Report): void {
    if (!enabled) return
    try {
      const child = spawn(cli, buildArgs(report), { detached: true, stdio: 'ignore' })
      child.on('error', () => {})
      child.unref()
    } catch {
      // 退出路径尽力而为
    }
  }

  ctx.on('agent/status', ({ agent, status }) => {
    if (!enabled || !isPrimary(agent)) return
    ensureChecked()
    if (status === 'running') {
      if (lastState === 'working') {
        // 只刷新 TTL，保留已有文字
        send({ state: 'working', ttl: WORKING_TTL })
        return
      }
      send({
        state: 'working',
        title: '正在处理任务',
        detail: lastPrompt === '' ? undefined : lastPrompt,
        ttl: WORKING_TTL,
      })
      return
    }
    send({
      state: 'waiting_input',
      title: '等待你的指令',
      detail: lastPrompt === '' ? undefined : lastPrompt,
      sound: true,
      ttl: WAITING_TTL,
    })
  })

  ctx.on('session/event', (session, event) => {
    try {
      if (!isPrimaryHeader((session as { header?: SessionHeaderLike }).header)) return
    } catch {
      // 无法判定时按主会话处理
    }

    if (event.type === 'user/message') {
      const text = textOf((event.data as { content?: unknown }).content)
      if (text === '') return
      lastPrompt = oneLine(text, DETAIL_MAX)
      if (lastState === 'working') {
        send({ state: 'working', title: '正在处理任务', detail: lastPrompt, ttl: WORKING_TTL })
      }
      return
    }

    if ((event.type === 'step/end' || event.type === 'turn/end') && lastState === 'working') {
      if (Date.now() - lastSentAt > KEEPALIVE_MS) send({ state: 'working', ttl: WORKING_TTL })
    }
  })

  ctx.on('user-questions/request', async (request, next) => {
    const agent = (request as { agent?: Agent }).agent
    const track = enabled && (agent === undefined || isPrimary(agent))
    if (track) {
      ensureChecked()
      try {
        const questions = (request as { questions?: { question?: unknown }[] }).questions ?? []
        const question = questions[0]?.question
        send({
          state: 'waiting_input',
          title: '需要你的回答',
          detail: typeof question === 'string' ? oneLine(question, DETAIL_MAX) : undefined,
          sound: true,
          ttl: WAITING_TTL,
        })
      } catch (error) {
        warn('用户提问上报失败：%o', error)
      }
    }
    try {
      return await next()
    } finally {
      if (track) send({ state: 'working', ttl: WORKING_TTL })
    }
  })

  ctx.on('approval/request', async (request, next) => {
    const agent = (request as { agent?: Agent }).agent
    const track = enabled && (agent === undefined || isPrimary(agent))
    if (track) {
      ensureChecked()
      try {
        const reason = (request as { reason?: unknown; toolName?: unknown }).reason
        const toolName = (request as { toolName?: unknown }).toolName
        const detail = typeof reason === 'string' && reason !== ''
          ? reason
          : typeof toolName === 'string' ? `工具 ${toolName}` : undefined
        send({
          state: 'waiting_input',
          title: '需要你的授权',
          detail: detail === undefined ? undefined : oneLine(detail, DETAIL_MAX),
          sound: true,
          ttl: WAITING_TTL,
        })
      } catch (error) {
        warn('授权请求上报失败：%o', error)
      }
    }
    try {
      return await next()
    } finally {
      if (track) send({ state: 'working', ttl: WORKING_TTL })
    }
  })

  ctx.on('agent/disposed', ({ agent }) => {
    if (!enabled || !isPrimary(agent)) return
    sendDetached({ state: 'idle', title: '', detail: '', ttl: IDLE_TTL })
  })

  // 可选能力：命令注册表存在时才挂 /rlcd。
  if (typeof ctx.inject === 'function') {
    ctx.inject(['commands'], (commandCtx) => {
      commandCtx.commands.register({
        name: 'rlcd',
        description: 'RLCD 开发板上报控制：on | off | status | test | sound on|off',
        input: { hint: '[on|off|status|test|sound on|sound off]' },
        handler: ({ rawInput }) => {
          const sub = rawInput.trim().toLowerCase()
          if (sub === 'on') {
            enabled = true
            failures = 0
            unavailable = false
            petAvailable = false
            checked = false
            ensureChecked()
            return { kind: 'success' as const, text: 'RLCD 上报已启用。' }
          }
          if (sub === 'off') {
            enabled = false
            return { kind: 'success' as const, text: 'RLCD 上报已禁用。' }
          }
          if (sub === 'sound on' || sub === 'sound off') {
            soundEnabled = sub === 'sound on'
            return { kind: 'success' as const, text: `RLCD 音效提醒已${soundEnabled ? '开启' : '关闭'}。` }
          }
          if (sub === 'test') {
            return sendAwait({
              state: 'success',
              title: 'RLCD 测试',
              detail: '来自 dsh-rlcd 插件的测试消息',
              progress: 100,
              sound: true,
              ttl: 300,
            }).then(text => text.startsWith('已上报')
              ? { kind: 'success' as const, text }
              : { kind: 'error' as const, text })
          }
          const state = unavailable ? 'rlcd CLI 不可用，已自动停用' : enabled ? '启用' : '禁用'
          const pet = petAvailable ? `pet=${PET_ID}` : 'pet=未安装（沿用设备当前宠物）'
          const device = deviceUrl === '' ? '设备地址未知' : deviceUrl
          return {
            kind: 'success' as const,
            text: `RLCD 上报：${state}；任务槽 ${AGENT_ID}/${TASK_ID}；${device}；${pet}；音效${soundEnabled ? '开' : '关'}；CLI=${cli}`,
          }
        },
      })
    })

    ctx.inject(['tools'], (toolCtx) => {
      toolCtx.tools.register({
        name: 'rlcd_report',
        description:
          '向 RLCD 开发板上报当前任务状态。在实质阶段变化时调用：更新中文标题、摘要或进度。'
          + 'state 默认 working；progress 为 0-100 整数，有可衡量依据时才传，未知时省略。'
          + '标题一行（约 20 字以内），摘要最多两行（约 60 字以内）。不要发送凭据或敏感日志。',
        parameters: {
          type: 'object',
          additionalProperties: false,
          properties: {
            state: {
              type: 'string',
              enum: ['working', 'waiting_input', 'success', 'error', 'idle'],
              description: '任务状态，默认 working',
            },
            title: { type: 'string', description: '一行中文标题' },
            detail: { type: 'string', description: '最多两行的中文摘要' },
            progress: { type: 'number', description: '0-100 整数进度，未知时省略' },
          },
        },
        output: {
          schema: { type: 'string' },
          render: (_args: unknown, value: unknown) => [{ type: 'text' as const, text: String(value) }],
        },
        execute: async (args: unknown) => {
          if (!enabled) return 'RLCD 上报当前已停用（设备不可达或被 /rlcd off 关闭），已跳过。'
          const input = (args ?? {}) as {
            state?: unknown
            title?: unknown
            detail?: unknown
            progress?: unknown
          }
          const requested = typeof input.state === 'string' ? input.state : 'working'
          const state = (STATES as readonly string[]).includes(requested)
            ? requested as RlcdState
            : 'working'
          const report: Report = { state, ttl: state === 'working' ? WORKING_TTL : WAITING_TTL }
          if (typeof input.title === 'string') report.title = oneLine(input.title, TITLE_MAX)
          if (typeof input.detail === 'string') report.detail = oneLine(input.detail, DETAIL_MAX)
          if (typeof input.progress === 'number' && Number.isFinite(input.progress)) {
            report.progress = input.progress
          }
          return await sendAwait(report)
        },
      })
    })
  }

  ctx.effect(() => () => {
    disposed = true
  }, 'dsh-rlcd: 停止上报')

  // 主动探测一次，使首次上报就带有设备地址与宠物信息；
  // 探测失败只会停用上报，不影响 dsh 本体。
  ensureChecked()
}
