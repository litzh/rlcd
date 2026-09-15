/**
 * dsh-rlcd 冒烟测试：用一个最小 ctx 桩加载插件，触发真实生命周期事件，
 * 再查询设备状态确认上报生效。
 *
 *   node integrations/dsh/smoke-test.ts
 *
 * 会真实修改 RLCD 开发板的显示内容；结束时用 idle 清空文字。
 * 需要本机已安装 rlcd CLI 并配置好设备地址。
 */

import { execFile } from 'node:child_process'

import { apply, name } from './index.ts'

type Handler = (...args: unknown[]) => unknown

const handlers = new Map<string, Handler[]>()
const tools: { name?: string; execute?: (args: unknown, exec: unknown) => Promise<unknown> }[] = []
const commands: { name?: string; handler?: (invocation: { rawInput: string }) => unknown }[] = []

const fakeCtx = {
  on(event: string, handler: Handler) {
    const list = handlers.get(event) ?? []
    list.push(handler)
    handlers.set(event, list)
    return () => {}
  },
  inject(_deps: string[], callback: (scope: unknown) => void) {
    callback({
      tools: { register: (definition: never) => { tools.push(definition); return () => {} } },
      commands: { register: (definition: never) => { commands.push(definition); return () => {} } },
    })
  },
  effect(effect: () => unknown) {
    effect()
    return () => {}
  },
  logger: { warn: (...args: unknown[]) => console.log('  [warn]', ...args) },
}

/** 只报告顶层会话的 agent 形状。 */
const primaryAgent = { session: { header: { id: 'smoke-session' } } }
const subAgent = { session: { header: { id: 'child', origin: 'subagent', delegationDepth: 1 } } }

function sleep(ms: number): Promise<void> {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function emit(event: string, ...args: unknown[]): void {
  for (const handler of handlers.get(event) ?? []) handler(...args)
}

function rlcd(args: string[]): Promise<string> {
  return new Promise((resolve) => {
    execFile('rlcd', args, { timeout: 15000 }, (error, stdout) => {
      resolve(error ? `ERROR: ${error.message}` : String(stdout))
    })
  })
}

async function deviceState(): Promise<{ agent_id?: string; state?: string; pet_id?: string }> {
  try {
    return JSON.parse(await rlcd(['status']))
  } catch {
    return {}
  }
}

let failures = 0

function check(label: string, ok: boolean, detail: string): void {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}${detail === '' ? '' : ` — ${detail}`}`)
  if (!ok) failures += 1
}

console.log(`plugin name: ${name}`)
apply(fakeCtx as never)

check('注册 rlcd_report 工具', tools.some(tool => tool.name === 'rlcd_report'),
  tools.map(tool => tool.name).join(', '))
check('注册 /rlcd 命令', commands.some(command => command.name === 'rlcd'),
  commands.map(command => command.name).join(', '))

// 1) agent 开始工作 -> working
emit('agent/status', { agent: primaryAgent, status: 'running' })
await sleep(3000)
let state = await deviceState()
check('running 上报为 working', state.state === 'working', JSON.stringify(state))
check('任务槽为 dsh', state.agent_id === 'dsh', `agent_id=${state.agent_id}`)

// 2) 子 agent 不应抢占显示位
await rlcd(['send', 'working', '--agent', 'sentinel', '--task', 'main', '--ttl', '60'])
emit('agent/status', { agent: subAgent, status: 'idle' })
await sleep(2500)
state = await deviceState()
check('子 agent 被忽略', state.agent_id === 'sentinel', `agent_id=${state.agent_id}`)

// 3) agent 空闲 -> waiting_input，并且此时宠物已探测完成
emit('agent/status', { agent: primaryAgent, status: 'idle' })
await sleep(2500)
state = await deviceState()
check('idle 上报为 waiting_input', state.state === 'waiting_input', JSON.stringify(state))
check('激活 deepseek-whale 宠物', state.pet_id === 'deepseek-whale', `pet_id=${state.pet_id}`)

// 4) 用户提问期间 -> waiting_input，答复后回到 working
// 真实的 next() 会一直挂起到用户答复，这里用可控 Promise 复刻该窗口。
let answer: (value: string) => void = () => {}
const pendingAnswer = new Promise<string>((resolve) => { answer = resolve })
emit('user-questions/request',
  { questions: [{ id: 'q1', question: '要继续吗？' }], agent: primaryAgent },
  () => pendingAnswer)
await sleep(2000)
check('提问期间上报 waiting_input', (await deviceState()).state === 'waiting_input', '')
answer('yes')
await sleep(2500)
check('答复后回到 working', (await deviceState()).state === 'working', '')

// 5) 工具路径
const reportTool = tools.find(tool => tool.name === 'rlcd_report')
const toolResult = await reportTool?.execute({ state: 'success', title: '冒烟测试', progress: 100 }, {})
check('rlcd_report 工具执行成功', String(toolResult).startsWith('已上报'), String(toolResult))
check('工具上报为 success', (await deviceState()).state === 'success', '')

// 6) 命令路径
const rlcdCommand = commands.find(command => command.name === 'rlcd')
const statusResult = await rlcdCommand?.handler?.({ rawInput: 'status' }) as { kind?: string; text?: string }
check('/rlcd status 返回成功', statusResult?.kind === 'success', statusResult?.text ?? '')

// 收尾：清空文字并回到待机
await rlcd(['send', 'idle', '--agent', 'dsh', '--task', 'main', '--title', '', '--detail', '', '--ttl', '60'])
await sleep(500)

console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`)
process.exitCode = failures === 0 ? 0 : 1
