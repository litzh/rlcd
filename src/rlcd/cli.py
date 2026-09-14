"""RLCD Agent CLI. Run rlcd --help."""
import argparse
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from . import config

STATES = ("idle", "working", "waiting_input", "success", "error")
WIDTH, HEIGHT = 384, 80


def find_font(explicit):
    candidates = [explicit or os.environ.get("RLCD_FONT"),
                  "/System/Library/Fonts/STHeiti Medium.ttc",
                  "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                  "C:/Windows/Fonts/msyh.ttc"]
    for name in candidates:
        if name and Path(name).is_file():
            return name
    raise ValueError("未找到中文字体，请用 --font 指定 TTF/OTF/TTC 文件")


def render_text(title, detail, font_path):
    if len(title) + len(detail) > 500:
        raise ValueError("文字最多 500 字符")
    with TTFont(font_path, fontNumber=0) as font:
        cmap = font.getBestCmap() or {}
        missing = sorted({c for c in title + detail if c not in "\n " and ord(c) not in cmap})
    if missing:
        raise ValueError("字体缺少字形：" + "".join(missing))
    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(canvas)
    title_font = ImageFont.truetype(font_path, 24)
    body_font = ImageFont.truetype(font_path, 18)

    def fit(text, font):
        while text and draw.textlength(text, font=font) > WIDTH:
            text = text[:-1]
        return text

    title = title.replace("\n", " ")
    if draw.textlength(title, font=title_font) > WIDTH:
        title = fit(title, title_font)
        title = fit(title[:-1] + "…", title_font)
    draw.text((0, 0), title, font=title_font, fill=0, anchor="lt")
    lines, current = [], ""
    for char in detail:
        if char == "\n" or draw.textlength(current + char, font=body_font) > WIDTH:
            lines.append(current)
            current = "" if char == "\n" else char
        else:
            current += char
    lines.append(current)
    if len(lines) > 2:
        lines = lines[:2]
        lines[-1] = fit(lines[-1][:-1] + "…", body_font)
    for index, line in enumerate(lines[:2]):
        draw.text((0, 33 + index * 23), line, font=body_font, fill=0, anchor="lt")
    # XBM: row-major, LSB-first, one means black. No dithering around text strokes.
    data = bytearray(WIDTH * HEIGHT // 8)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if canvas.getpixel((x, y)) < 160:
                data[y * (WIDTH // 8) + x // 8] |= 1 << (x % 8)
    preview = canvas.point(lambda pixel: 0 if pixel < 160 else 255).convert("1")
    return bytes(data), preview


class Device:
    def __init__(self, url):
        self.url = url.rstrip("/")
        if urllib.parse.urlsplit(self.url).scheme != "http":
            raise ValueError("设备地址须为 http://IP")
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(self, path, data=None, method=None, content_type="application/json"):
        request = urllib.request.Request(self.url + path, data=data, method=method,
                                         headers={"Content-Type": content_type})
        with self.opener.open(request, timeout=20) as response:
            return response.read()

    def status(self):
        return json.loads(self.request("/agent/state"))

    def send(self, args, state=None, title=None, detail=None, progress=None):
        current = self.status()
        same = current["agent_id"] == args.agent and current["task_id"] == args.task
        sequence = args.seq if args.seq is not None else (current["seq"] + 1 if same else 1)
        payload = {"agent_id": args.agent, "task_id": args.task, "seq": sequence,
                   "state": state or args.state, "progress": args.progress if state is None else progress,
                   "ttl_seconds": args.ttl, "sound": args.sound}
        title = args.title if title is None else title
        detail = args.detail if detail is None else detail
        if title is not None or detail is not None:
            data, preview = render_text(title or "", detail or "", find_font(args.font))
            payload["text_hex"] = data.hex()
            if args.preview:
                preview.save(args.preview)
        result = json.loads(self.request("/agent/state", json.dumps(payload).encode(), "POST"))
        print(json.dumps(result, ensure_ascii=False))
        return result


def main():
    parser = argparse.ArgumentParser(description="通过 HTTP 展示 Agent 桌宠和中文任务文字")
    parser.add_argument("--device")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    settings = commands.add_parser("config", help="管理 ~/.config/rlcd 配置")
    settings_commands = settings.add_subparsers(dest="config_command", required=True)
    settings_commands.add_parser("show")
    settings_commands.add_parser("init")
    setter = settings_commands.add_parser("set")
    setter.add_argument("key", choices=("device", "font"))
    setter.add_argument("value")
    pet = commands.add_parser("pet", help="管理宠物素材包")
    pet_commands = pet.add_subparsers(dest="pet_command", required=True)
    preview = pet_commands.add_parser("preview")
    preview.add_argument("folder")
    preview.add_argument("--output")
    install = pet_commands.add_parser("install")
    install.add_argument("folder")
    use = pet_commands.add_parser("use")
    use.add_argument("id")
    pet_commands.add_parser("list")
    for command in ("send", "showcase"):
        sub = commands.add_parser(command)
        if command == "send":
            sub.add_argument("state", choices=STATES)
        sub.add_argument("--agent", default="desktop-agent")
        sub.add_argument("--task", default="desktop-pet")
        sub.add_argument("--seq", type=int)
        sub.add_argument("--ttl", type=int, default=120)
        sub.add_argument("--title")
        sub.add_argument("--detail")
        sub.add_argument("--progress", type=int, choices=range(101), metavar="0..100")
        sub.add_argument("--font")
        sub.add_argument("--preview", help="保存实际上传的文字层 PNG")
        sub.add_argument("--sound", action="store_true", help="状态切换时请求播放当前宠物音效")
    args = parser.parse_args()
    try:
        if args.command == "config":
            if args.config_command == "init":
                config.seed_pets()
            elif args.config_command == "set":
                config.set_value(args.key, args.value)
            print(json.dumps({"directory": str(config.home()), "settings": config.read()}, ensure_ascii=False, indent=2))
            return
        values = config.read()
        args.device = args.device or os.environ.get("RLCD_DEVICE") or values.get("device")
        if hasattr(args, "font"):
            args.font = args.font or os.environ.get("RLCD_FONT") or values.get("font")
        if args.command == "pet" and args.pet_command in ("preview", "install"):
            args.folder = config.pet_folder(args.folder)
            if args.pet_command == "preview" and not args.output:
                from .pet_assets import compile_pet
                args.output = config.home()/"previews"/compile_pet(args.folder)[0]["id"]
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + "\n")
    if args.command == "pet" and args.pet_command == "preview":
        from .pet_assets import preview_pet
        try:
            preview_pet(args.folder, args.output)
        except (ValueError, OSError) as error:
            parser.exit(1, str(error) + "\n")
        return
    if not args.device:
        parser.error("请运行 rlcd config set device http://IP，或设置 --device / RLCD_DEVICE")
    try:
        device = Device(args.device)
        if args.command == "pet":
            from .pet_assets import install_pet
            if args.pet_command == "install":
                install_pet(device, args.folder)
            elif args.pet_command == "use":
                print(device.request("/pets/use", json.dumps({"id": args.id}).encode(), "POST").decode())
            else:
                print(device.request("/pets").decode())
        elif args.command == "status":
            print(json.dumps(device.status(), ensure_ascii=False, indent=2))
        elif args.command == "send":
            device.send(args)
        else:
            if args.seq is not None:
                raise ValueError("showcase 自动生成序号，不接受 --seq")
            for state, title, detail, progress in [
                ("idle", "你好，我是你的桌面伙伴", "随时准备接收 Agent 的任务状态。", None),
                ("working", "正在分析项目", "读取代码与文档，整理实现步骤。", 20),
                ("working", "正在构建固件", "中文文字独立更新，小猫在设备上继续活动。", 65),
                ("waiting_input", "需要你的确认", "请回到电脑选择下一步操作。", None),
                ("success", "任务完成", "构建成功，可以开始体验新的功能了。", 100),
            ]:
                device.send(args, state, title, detail, progress)
                if state != "success":
                    time.sleep(4)
    except (ValueError, OSError, urllib.error.URLError) as error:
        if isinstance(error, urllib.error.HTTPError):
            parser.exit(1, f"设备返回 HTTP {error.code}: {error.read().decode(errors='replace')}\n")
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
