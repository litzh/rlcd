"""Validate configuration and resource locations using an isolated temporary directory."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from rlcd import config
from rlcd.cli import main

class Configuration(unittest.TestCase):
    def test_settings_and_resource_preservation(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(config,'home',return_value=Path(directory)):
            self.assertEqual(config.read(),{})
            config.set_value('device','http://192.0.2.10/')
            self.assertEqual(config.read()['device'],'http://192.0.2.10')
            self.assertEqual((Path(directory)/'config.json').stat().st_mode & 0o777,0o600)
            for value in ('https://example.com','http://','http://user:pass@example.com','http://example.com/path'):
                with self.assertRaises(ValueError): config.set_value('device',value)
            self.assertEqual(config.read()['device'],'http://192.0.2.10')
            resources=config.seed_pets()
            self.assertTrue((resources/'deepseek-whale/pet.json').is_file())
            note=resources/'deepseek-whale/README.md'
            note.write_text('user changes')
            config.seed_pets()
            self.assertEqual(note.read_text(),'user changes')
            self.assertEqual(config.pet_folder('deepseek-whale'),resources/'deepseek-whale')
            self.assertTrue((Path(directory)/'docs/pet-format.md').is_file())

    def test_default_preview_without_device(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(config,'home',return_value=Path(directory)):
            with patch.object(sys,'argv',['rlcd','pet','preview','deepseek-whale']): main()
            self.assertTrue((Path(directory)/'previews/deepseek-whale/overview.png').is_file())

if __name__=='__main__': unittest.main()
