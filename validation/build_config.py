"""Host checks for generated credentials; no device or network required."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GENERATOR = Path(__file__).resolve().parents[1] / "scripts/generate_wifi_config.py"
spec = importlib.util.spec_from_file_location("wifi_config", GENERATOR)
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


class BuildConfigTest(unittest.TestCase):
    def test_validation(self):
        self.assertEqual(config.credentials({}), ("", ""))
        self.assertEqual(config.credentials({"RLCD_WIFI_SSID": "fixture"}), ("fixture", ""))
        config.credentials({"RLCD_WIFI_SSID": "网络", "RLCD_WIFI_PASSWORD": "a" * 64})
        for env in (
            {"RLCD_WIFI_PASSWORD": "abcdefgh"},
            {"RLCD_WIFI_SSID": "网" * 11},
            {"RLCD_WIFI_SSID": "fixture", "RLCD_WIFI_PASSWORD": "short"},
            {"RLCD_WIFI_SSID": "fixture", "RLCD_WIFI_PASSWORD": "g" * 64},
            {"RLCD_WIFI_SSID": "nul\0"},
        ):
            with self.assertRaises(ValueError):
                config.credentials(env)

    def test_generated_cpp_and_stale_removal(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("RLCD_WIFI_")}
        value = '网络"\\$()`'
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "defaults.h"
            command = [sys.executable, str(GENERATOR), str(header)]
            subprocess.run(command, env=dict(env, RLCD_WIFI_SSID=value), check=True)
            source = Path(directory) / "check.cpp"
            expected = ",".join(str(b) for b in value.encode()) + ",0"
            source.write_text('#include "defaults.h"\n#include <cstring>\n'
                              f'int main() {{ unsigned char expected[] = {{{expected}}}; '
                              'return std::strcmp(DEFAULT_SSID, (char*)expected); }\n')
            binary = Path(directory) / "fixture"
            subprocess.run(["c++", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            subprocess.run(command, env=env, check=True)
            self.assertIn('DEFAULT_SSID[] = "";', header.read_text())
            result = subprocess.run(command, env=dict(env, RLCD_WIFI_PASSWORD="short"),
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(header.exists())


if __name__ == "__main__":
    unittest.main()
