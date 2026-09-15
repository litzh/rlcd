/**
 * RLCD Status Reporter Extension
 *
 * 向 RLCD 开发板自动上报 pi 的任务状态（通过 rlcd CLI，支持中文文字层）：
 * - agent_start      -> working（标题 + 用户 prompt 摘要）
 * - turn_end         -> working keepalive（节流，仅刷新 TTL，保留文字）
 * - ui_prompt_start  -> waiting_input（confirm/select 等弹出时，带音效）
 * - ui_prompt_end    -> 恢复 working
 * - agent_settled    -> waiting_input（等待用户输入，带音效）
 * - session_shutdown -> idle
 *
 * 同时注册 rlcd_report 工具，让 LLM 在实质阶段变化时推送有意义的中文
 * 标题 / 摘要 / 进度；/rlcd 命令支持 on|off|status|test。
 * 上报时携带 --pet pi-bot 激活 pi 自己的宠物（未安装则自动省略并提示）。
 *
 * 上报失败不阻塞主任务：连续失败后自动禁用并提示一次。
 */

import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { StringEnum } from "@earendil-works/pi-ai";
import { execFile, spawn } from "node:child_process";
import { Type } from "typebox";

type RlcdState = "working" | "waiting_input" | "success" | "error" | "idle";

const AGENT_ID = "pi";
const PET_ID = "pi-bot";
const WORKING_TTL = 600;
const WAITING_TTL = 3600;
const IDLE_TTL = 300;
const KEEPALIVE_MS = WORKING_TTL * 0.6 * 1000;
const MAX_FAILURES = 2;
const SEND_TIMEOUT_MS = 15000;

interface Report {
	state: RlcdState;
	title?: string;
	detail?: string;
	progress?: number;
	sound?: boolean;
	ttl?: number;
}

/** 折叠为单行并按字符数截断（设备标题一行、正文两行）。 */
function oneLine(text: string, max: number): string {
	const t = text.replace(/\s+/g, " ").trim();
	return t.length > max ? `${t.slice(0, max - 1)}…` : t;
}

export default function (pi: ExtensionAPI) {
	let enabled = true;
	let checked = false;
	let taskId = "session";
	let lastPrompt = "";
	let lastSentAt = 0;
	let failures = 0;
	let unavailable = false;
	let petAvailable = false;
	let pendingNotice: string | null = null;

	/** 异步回调中不能用捕获的 ctx（会过期），通知统一在下个事件里补发。 */
	function flushNotice(ctx: ExtensionContext): void {
		if (!pendingNotice) return;
		const msg = pendingNotice;
		pendingNotice = null;
		try {
			ctx.ui.notify(msg, "warning");
		} catch {
			// ctx 已过期则丢弃
		}
	}

	function buildArgs(r: Report): string[] {
		const args = [
			"send",
			r.state,
			"--agent",
			AGENT_ID,
			"--task",
			taskId,
			"--ttl",
			String(r.ttl ?? WORKING_TTL),
		];
		if (r.title !== undefined) args.push("--title", r.title);
		if (r.detail !== undefined) args.push("--detail", r.detail);
		if (petAvailable) args.push("--pet", PET_ID);
		if (r.progress !== undefined) {
			args.push("--progress", String(Math.max(0, Math.min(100, Math.round(r.progress)))));
		}
		if (r.sound) args.push("--sound");
		return args;
	}

	/** fire-and-forget 上报；409 旧序号时延迟重读序号重试一次，其他失败计数并自动禁用。 */
	function send(r: Report, retried = false): void {
		if (!enabled) return;
		lastSentAt = Date.now();
		execFile("rlcd", buildArgs(r), { timeout: SEND_TIMEOUT_MS }, (err) => {
			if (err && !retried && /409|stale_sequence/.test(err.message)) {
				setTimeout(() => send(r, true), 1000).unref();
				return;
			}
			if (err) {
				failures++;
				if (enabled && failures >= MAX_FAILURES) {
					enabled = false;
					pendingNotice = `RLCD 上报连续失败，本会话暂停上报（/rlcd on 恢复）：${err.message}`;
				}
			} else {
				failures = 0;
			}
		});
	}

	function sendAwait(r: Report): Promise<string> {
		lastSentAt = Date.now();
		return new Promise((resolve) => {
			execFile("rlcd", buildArgs(r), { timeout: SEND_TIMEOUT_MS }, (err, stdout) => {
				if (err) {
					failures++;
					resolve(`RLCD 上报失败：${err.message}`);
				} else {
					failures = 0;
					resolve(`已上报到 RLCD：${oneLine(stdout.trim() || "ok", 200)}`);
				}
			});
		});
	}

	/** 进程退出场景使用：detached 子进程，不持有事件循环。 */
	function sendDetached(r: Report): void {
		if (!enabled) return;
		const child = spawn("rlcd", buildArgs(r), { detached: true, stdio: "ignore" });
		child.on("error", () => {});
		child.unref();
	}

	/** 惰性检查 rlcd CLI 是否可用（config show 同时验证配置存在）。
	 *  CLI 不存在或未配置时静默禁用，不打扰用户；/rlcd status 可查看。
	 *  再检查 pi-bot 宠物是否已安装，未安装则不带 --pet 上报（不存在的宠物会让整条请求失败）。 */
	function ensureChecked(): void {
		if (checked) return;
		checked = true;
		execFile("rlcd", ["config", "show"], { timeout: 5000 }, (err) => {
			if (err) {
				enabled = false;
				unavailable = true;
				return;
			}
			execFile("rlcd", ["pet", "list"], { timeout: 8000 }, (petErr, stdout) => {
				if (petErr) return;
				try {
					const pets = (JSON.parse(stdout) as { pets?: { id?: string }[] }).pets ?? [];
					petAvailable = pets.some((p) => p.id === PET_ID);
					if (!petAvailable) {
						pendingNotice = `RLCD 未安装 ${PET_ID} 宠物，上报将沿用设备当前宠物（rlcd pet install ${PET_ID} 可安装）`;
					}
				} catch {
					// 输出无法解析时保守起见不启用 --pet
				}
			});
		});
	}

	pi.on("session_start", (_event, ctx) => {
		const sessionId = ctx.sessionManager.getSessionId() ?? "session";
		taskId = sessionId.toLowerCase().replace(/[^a-z0-9._-]/g, "-").slice(0, 16) || "session";
		lastPrompt = "";
		lastSentAt = 0;
		failures = 0;
		enabled = true;
		checked = false;
		unavailable = false;
		petAvailable = false;
		pendingNotice = null;
		ensureChecked();
	});

	pi.on("before_agent_start", (event) => {
		lastPrompt = oneLine(event.prompt ?? "", 60);
	});

	pi.on("agent_start", (_event, ctx) => {
		flushNotice(ctx);
		send({ state: "working", title: "正在处理任务", detail: lastPrompt, ttl: WORKING_TTL });
	});

	pi.on("turn_end", (_event, ctx) => {
		flushNotice(ctx);
		if (Date.now() - lastSentAt > KEEPALIVE_MS) {
			send({ state: "working", ttl: WORKING_TTL });
		}
	});

	pi.on("ui_prompt_start", (event, ctx) => {
		flushNotice(ctx);
		send({
			state: "waiting_input",
			title: "需要你的确认",
			detail: event.title ? oneLine(event.title, 40) : undefined,
			sound: true,
			ttl: WAITING_TTL,
		});
	});

	pi.on("ui_prompt_end", (_event, ctx) => {
		flushNotice(ctx);
		if (!ctx.isIdle()) {
			send({ state: "working", ttl: WORKING_TTL });
		}
	});

	pi.on("agent_settled", (_event, ctx) => {
		flushNotice(ctx);
		send({
			state: "waiting_input",
			title: "等待你的指令",
			detail: lastPrompt,
			sound: true,
			ttl: WAITING_TTL,
		});
	});

	pi.on("session_shutdown", () => {
		sendDetached({ state: "idle", title: "", detail: "", ttl: IDLE_TTL });
	});

	pi.registerCommand("rlcd", {
		description: "RLCD 上报控制：/rlcd [on|off|status|test]",
		handler: async (args, ctx) => {
			const sub = (args ?? "").trim() || "status";
			if (sub === "on") {
				enabled = true;
				failures = 0;
				unavailable = false;
				petAvailable = false;
				checked = false;
				ensureChecked();
				ctx.ui.notify("RLCD 上报已启用", "info");
			} else if (sub === "off") {
				enabled = false;
				ctx.ui.notify("RLCD 上报已禁用", "info");
			} else if (sub === "test") {
				const result = await sendAwait({
					state: "success",
					title: "RLCD 测试",
					detail: "来自 pi 扩展的测试消息",
					progress: 100,
					sound: true,
					ttl: 300,
				});
				ctx.ui.notify(result, result.startsWith("已上报") ? "info" : "error");
			} else {
				const state = unavailable ? "rlcd CLI 不可用，已自动禁用" : enabled ? "启用" : "禁用";
				const pet = petAvailable ? `pet=${PET_ID}` : "pet=未安装（沿用设备当前宠物）";
				ctx.ui.notify(`RLCD 上报：${state}，task=${taskId}，${pet}`, "info");
			}
		},
	});

	pi.registerTool({
		name: "rlcd_report",
		label: "RLCD 状态上报",
		description:
			"向 RLCD 开发板上报当前任务状态。在实质阶段变化时调用：更新中文标题、摘要或进度。" +
			"state 默认 working；progress 为 0-100 整数，有可衡量依据时才传，未知时省略。" +
			"标题一行（约 16 个汉字内），摘要最多两行。不要发送凭据或敏感日志。",
		promptSnippet: "向 RLCD 开发板上报任务状态、中文摘要和进度",
		promptGuidelines: [
			"在任务的实质阶段变化（开始、进入新阶段、完成、出错）时调用 rlcd_report 更新 RLCD 开发板上的标题/摘要/进度；rlcd_report 的 progress 只在有可衡量依据时传，未知时省略。",
		],
		parameters: Type.Object({
			state: Type.Optional(
				StringEnum(["working", "waiting_input", "success", "error", "idle"] as const, {
					description: "任务状态，默认 working",
				}),
			),
			title: Type.Optional(Type.String({ description: "一行中文标题" })),
			detail: Type.Optional(Type.String({ description: "最多两行的中文摘要" })),
			progress: Type.Optional(Type.Number({ description: "0-100 整数进度，未知时省略" })),
		}),
		async execute(_toolCallId, params) {
			if (!enabled) {
				return {
					content: [{ type: "text" as const, text: "RLCD 上报当前已禁用（设备不可达或被用户关闭），跳过。" }],
					details: {},
				};
			}
			const state = params.state ?? "working";
			const report: Report = {
				state,
				ttl: state === "working" ? WORKING_TTL : WAITING_TTL,
			};
			if (params.title !== undefined) report.title = oneLine(params.title, 24);
			if (params.detail !== undefined) report.detail = oneLine(params.detail, 60);
			if (params.progress !== undefined) report.progress = params.progress;
			const result = await sendAwait(report);
			return { content: [{ type: "text" as const, text: result }], details: {} };
		},
	});
}
