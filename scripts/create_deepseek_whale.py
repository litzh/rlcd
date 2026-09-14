# /// script
# requires-python = ">=3.10"
# ///
"""Build vector-derived whale frames. Requires the rsvg-convert executable."""
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import wave
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1] / 'pets/deepseek-whale'
SVG = ROOT / 'source/deepseek.svg'
renderer = shutil.which('rsvg-convert')
if not renderer:
    raise SystemExit('需要安装 librsvg，确保 rsvg-convert 可用')
paths = [e.attrib['d'] for e in ET.parse(SVG).getroot().iter() if e.tag.endswith('}path')]
if not paths:
    raise SystemExit('参考 SVG 没有 path')
body = ''.join(f'<path d="{escape(d)}"/>' for d in paths)
manifest = {'format_version': 1, 'id': 'deepseek-whale', 'name': 'DeepSeek 鲸鱼',
            'canvas': {'width': 160, 'height': 128}, 'states': {}}


def ring(x, y, radius):
    return f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius}" fill="white" stroke="black" stroke-width="1.6"/>'


def star(x, y, radius=5):
    return f'<path d="M{x},{y-radius} L{x+1.5},{y-1.5} L{x+radius},{y} L{x+1.5},{y+1.5} L{x},{y+radius} L{x-1.5},{y+1.5} L{x-radius},{y} L{x-1.5},{y-1.5} Z"/>'


for state in ('idle', 'working', 'waiting_input', 'success', 'error', 'stale'):
    folder = ROOT / state
    folder.mkdir(parents=True, exist_ok=True)
    frames = []
    for i in range(8):
        phase = 2 * math.pi * i / 8
        dx, dy, angle = 0, 2 * math.sin(phase), 2 * math.sin(phase)
        extras = ''
        if state == 'idle':
            extras += ring(23, 48 - (i * 3) % 24, 2.5)
            extras += ring(15, 64 - (i * 2) % 20, 1.8)
        elif state == 'working':
            dx, dy, angle = 2 * math.sin(phase), 1.5 * math.cos(phase), 3 * math.sin(phase)
            for n in range(3):
                extras += f'<circle cx="{67+n*12}" cy="15" r="{3 if n==i%3 else 1.5}"/>'
            extras += f'<path d="M12,{71+i%3} h10 M8,{79+i%3} h12" fill="none" stroke="black" stroke-width="2" stroke-linecap="round"/>'
        elif state == 'waiting_input':
            angle = 3 * math.sin(phase)
            extras += '<path d="M130,30 C130,22 143,22 143,29 C143,35 136,34 136,40" fill="none" stroke="black" stroke-width="3" stroke-linecap="round"/><circle cx="136" cy="46" r="1.8"/>'
            if i % 4 < 2:
                extras += '<path d="M20,48 l-6,-4 M19,55 h-8 M21,62 l-6,4" stroke="black" stroke-width="2" stroke-linecap="round"/>'
        elif state == 'success':
            dy, angle = -4 - 4 * math.sin(phase), -5 + 4 * math.sin(phase)
            extras += star(22, 35, 5 if i % 2 else 3) + star(139, 55, 4 if i % 2 else 6)
            extras += '<path d="M67,13 l5,5 l11,-11" fill="none" stroke="black" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>'
        elif state == 'error':
            dx = (3 if i % 2 else -3) if i < 6 else 0
            dy, angle = 0, 0
            extras += '<path d="M137,25 v13" stroke="black" stroke-width="3" stroke-linecap="round"/><circle cx="137" cy="45" r="2"/>'
            extras += '<path d="M20,44 l-5,7 l6,2 l-5,8" fill="none" stroke="black" stroke-width="2"/>'
        elif state == 'stale':
            dy, angle = 2 + i * 0.5, -2 - i * 0.3
            extras += '<path d="M120,26 h7 l-7,8 h7 M132,14 h10 l-10,11 h10" fill="none" stroke="black" stroke-width="1.8" stroke-linejoin="round"/>'
        # Place the original 24x24 outline in the fixed 160x128 pet canvas.
        art = f'<g transform="translate({dx:.2f},{dy:.2f}) rotate({angle:.2f},80,68)"><g transform="translate(28,13) scale(4.3)" fill="black" fill-rule="evenodd">{body}</g></g>'
        # Water gives a stable baseline while the whale floats above it.
        water = '<path d="M33,114 q8,-4 16,0 t16,0 t16,0 t16,0 t16,0" fill="none" stroke="black" stroke-width="1.4" stroke-linecap="round"/>'
        svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="160" height="128" viewBox="0 0 160 128">{art}<g fill="black">{extras}</g>{water}</svg>'
        name = f'{i+1:03d}.png'
        subprocess.run([renderer, '-w', '160', '-h', '128', '-o', str(folder/name)], input=svg.encode(), check=True)
        frames.append(f'{state}/{name}')
    manifest['states'][state] = {'frames': frames, 'frame_ms': 200 if state != 'stale' else 350,
                                 'loop': state not in ('error', 'stale')}

sound_folder = ROOT/'sounds'
sound_folder.mkdir(exist_ok=True)
for name, frequencies in [('attention', (523.25, 783.99)), ('success', (659.25, 987.77)), ('error', (392, 261.63))]:
    samples = []
    for i in range(6400):
        p = i % 3200
        envelope = min(1, p/240, (3199-p)/480)
        frequency = frequencies[i//3200]
        samples.append(round(3800*envelope*math.sin(2*math.pi*frequency*i/16000)))
    with wave.open(str(sound_folder/(name+'.wav')), 'wb') as output:
        output.setnchannels(1); output.setsampwidth(2); output.setframerate(16000)
        output.writeframes(struct.pack('<'+'h'*len(samples), *samples))
for state, sound in [('waiting_input', 'attention'), ('success', 'success'), ('error', 'error')]:
    manifest['states'][state]['sound'] = f'sounds/{sound}.wav'
(ROOT/'pet.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(ROOT)
