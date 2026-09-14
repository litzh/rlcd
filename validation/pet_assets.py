"""Validate the public material format and compiled frame layout without a device."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0,str(ROOT/'src'))
from rlcd import pet_assets as assets


class Materials(unittest.TestCase):
    def test_package_pixels_and_sound(self):
        header,package,path,sounds,previews=assets.compile_pet(ROOT/'pets/pixel-cat')
        self.assertEqual(package[:4],b'RLP1')
        offset=8+struct.unpack_from('<I',package,4)[0]
        self.assertEqual(json.loads(package[8:offset]),header)
        self.assertEqual(len(sounds),3)
        for index,state in enumerate(assets.STATES):
            clip=header['states'][index]
            for frame,image in enumerate(previews[state]):
                start=offset+clip['offset']+frame*2560
                raw=package[start:start+2560]
                for y in range(128):
                    for x in range(160):
                        self.assertEqual(bool(raw[y*20+x//8] & (1<<(x%8))),image.getpixel((x,y))==0)
        self.assertEqual(package,assets.compile_pet(ROOT/'pets/pixel-cat')[1])

    def test_invalid_and_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)/'pet'; shutil.copytree(ROOT/'pets/pixel-cat',root)
            path=root/'pet.json'; original=json.loads(path.read_text())
            for edit in (
                lambda m:m.update(id='../escape'),
                lambda m:m['states']['idle'].update(frames=['../../escape.png']),
                lambda m:m['states']['idle'].update(frame_ms=0),
                lambda m:m['states']['idle'].update(loop='true'),
                lambda m:m['states']['idle'].update(frames=['missing.png']),
                lambda m:m.update(canvas={'width':400,'height':300}),
                lambda m:m['states']['idle'].update(frames=['idle/001.png']*65),
            ):
                m=json.loads(json.dumps(original)); edit(m); path.write_text(json.dumps(m))
                with self.assertRaises(ValueError): assets.compile_pet(root)
            m=original; m['states']={'idle':m['states']['idle']}; path.write_text(json.dumps(m))
            header,*_=assets.compile_pet(root)
            self.assertEqual(len(header['states']),6)
            self.assertTrue(all(s['count']==4 and not s['sound'] for s in header['states']))

if __name__=='__main__': unittest.main()
