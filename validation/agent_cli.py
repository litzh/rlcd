import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("rlcd_cli", Path(__file__).resolve().parents[1] / "cli/rlcd.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class TextRenderingTest(unittest.TestCase):
    def test_chinese_and_bit_order(self):
        data, preview = cli.render_text("正在构建固件", "中文、English 与数字 65%\n等待用户确认", cli.find_font(None))
        self.assertEqual(len(data), 3840)
        self.assertTrue(any(data))
        for y in range(80):
            for x in range(384):
                self.assertEqual(bool(data[y * 48 + x // 8] & (1 << (x % 8))), preview.getpixel((x, y)) == 0)
        output = Path(__file__).resolve().parents[1] / "build/agent-text-preview.png"
        preview.resize((768, 160)).save(output)

    def test_empty_and_wrapping(self):
        data, _ = cli.render_text("", "", cli.find_font(None))
        self.assertEqual(data, bytes(3840))
        data, _ = cli.render_text("标题" * 30, "换行验证" * 60, cli.find_font(None))
        self.assertEqual(len(data), 3840)
        with self.assertRaises(ValueError):
            cli.render_text("\U0010ffff", "", cli.find_font(None))


if __name__ == "__main__":
    unittest.main()
