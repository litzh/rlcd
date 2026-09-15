# /// script
# requires-python = ">=3.10"
# ///
"""Create a geometric Pi robot from the supplied SVG logo. Requires rsvg-convert."""
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import wave
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1] / 'pets/pi-bot'
renderer = shutil.which('rsvg-convert')
if not renderer:
    raise SystemExit('请安装 librsvg，确保 rsvg-convert 可用')
paths = [e.attrib['d'] for e in ET.parse(ROOT/'source/favicon.svg').getroot().iter() if e.tag.endswith('}path')]
if len(paths) != 2:
    raise SystemExit('参考 SVG 的 path 结构与预期不一致')
logo = ''.join(f'<path fill="white" fill-rule="evenodd" d="{escape(d)}"/>' for d in paths)
manifest = {'format_version': 1, 'id': 'pi-bot', 'name': 'Pi 方块机器人',
            'canvas': {'width': 160, 'height': 128}, 'states': {}}


def line(path, width=3):
    return f'<path d="{path}" fill="none" stroke="black" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>'


def spark(x,y,r=4):
    return f'<path d="M{x},{y-r} l1.5,{r-1.5} l{r-1.5},1.5 l-{r-1.5},1.5 l-1.5,{r-1.5} l-1.5,-{r-1.5} l-{r-1.5},-1.5 l{r-1.5},-1.5 Z"/>'


for state in ('idle','working','waiting_input','success','error','stale'):
    folder = ROOT/state; folder.mkdir(parents=True,exist_ok=True)
    frames = []
    for i in range(8):
        phase = i*2*math.pi/8
        dy = math.sin(phase)*1.5
        dx, angle = 0, 0
        decorations = ''
        antenna = '<path d="M80,21 V13" stroke="black" stroke-width="3"/><rect x="77" y="7" width="6" height="6" rx="1"/>'
        arms = line('M44,61 L33,68 L29,63')+line('M116,61 L127,68 L131,63')
        legs = line('M61,94 V104 H53',4)+line('M99,94 V104 H107',4)
        # The square void in the original mark becomes a single movable eye.
        pupil = '<rect x="73" y="50" width="4" height="5" rx="1" fill="white"/>'
        if state == 'idle':
            if i in (5,6): pupil='<path d="M71,53 H78" stroke="white" stroke-width="2"/>'
            else: pupil=f'<rect x="{73+(1 if i in (2,3) else 0)}" y="50" width="4" height="5" rx="1" fill="white"/>'
        elif state == 'working':
            dy=1 if i%2 else 0
            arms=line(f'M44,65 L34,82 L48,{103+(i%2)*3}')+line(f'M116,65 L126,82 L112,{106-(i%2)*3}')
            decorations='<rect x="44" y="109" width="72" height="10" rx="2" fill="white" stroke="black" stroke-width="2"/>'
            for x in range(50,113,8): decorations+=line(f'M{x},112 V115',1.5)
            for n in range(3): decorations+=f'<rect x="{132+n*6}" y="{33+(n==i%3)*-3}" width="3" height="3"/>'
        elif state == 'waiting_input':
            angle=math.sin(phase)*2
            arms=line('M44,61 L32,68 L26,64')+line(f'M116,61 L130,48 L{132+i%2*4},32')
            decorations=line('M15,28 C15,19 28,19 28,27 C28,33 21,33 21,39',2.5)+'<circle cx="21" cy="45" r="1.8"/>'
            if i%4<2: decorations+=line('M145,28 L149,24 M146,34 H151',1.5)
        elif state == 'success':
            dy=-2-3*math.sin(phase)
            arms=line('M44,60 L30,45 L26,34')+line('M116,60 L130,45 L134,34')
            legs=line('M61,94 L56,102 L48,101',4)+line('M99,94 L104,102 L112,101',4)
            pupil='<path d="M71,54 L74,50 L78,54" fill="none" stroke="white" stroke-width="2"/>'
            decorations=spark(18,66,4 if i%2 else 6)+spark(143,74,6 if i%2 else 4)+line('M124,15 L128,19 L136,9',2)
        elif state == 'error':
            dx=(-2 if i%2 else 2) if i<6 else 0
            angle=(-2 if i%2 else 2) if i<6 else 0
            pupil='<path d="M71,49 L78,56 M78,49 L71,56" stroke="white" stroke-width="2"/>'
            arms=line('M44,61 L32,77 L29,82')+line('M116,61 L128,77 L131,82')
            decorations=line('M137,24 V37',3)+'<circle cx="137" cy="44" r="2"/>'+line('M20,62 L15,69 L21,72 L16,79',2)
        else:
            dy=i*.5
            pupil='<path d="M71,54 H78" stroke="white" stroke-width="2"/>'
            antenna=line('M80,21 V13',3)+'<rect x="77" y="7" width="6" height="6" rx="1" fill="white" stroke="black" stroke-width="1.5"/>'
            arms=line('M44,61 L36,78 L40,85')+line('M116,61 L124,78 L120,85')
            decorations=line('M131,28 H137 L131,35 H137 M140,15 H149 L140,25 H149',1.5)
        casing='<rect x="42" y="20" width="76" height="76" rx="11" fill="black"/>'
        core=f'<g transform="translate(42,20) scale(.095)">{logo}</g>'
        robot=f'<g transform="translate({dx},{dy:.2f}) rotate({angle:.2f},80,60)">{antenna}{arms}{legs}{casing}{core}{pupil}</g>'
        shadow='<path d="M52,123 H108" stroke="black" stroke-width="1.2" stroke-linecap="round"/>'
        svg=f'<svg xmlns="http://www.w3.org/2000/svg" width="160" height="128" viewBox="0 0 160 128">{robot}<g fill="black">{decorations}</g>{shadow}</svg>'
        name=f'{i+1:03d}.png'
        subprocess.run([renderer,'-w','160','-h','128','-o',str(folder/name)],input=svg.encode(),check=True)
        frames.append(f'{state}/{name}')
    manifest['states'][state]={'frames':frames,'frame_ms':200 if state!='stale' else 350,'loop':state not in ('error','stale')}
sounds=ROOT/'sounds'; sounds.mkdir(exist_ok=True)
for name,frequencies in [('attention',(740,988)),('success',(659,880,1175)),('error',(440,294))]:
    samples=[]
    for frequency in frequencies:
        for i in range(2400):
            envelope=min(1,i/160,(2399-i)/320)
            tone=math.sin(2*math.pi*frequency*i/16000)+.2*math.sin(4*math.pi*frequency*i/16000)
            samples.append(round(3000*envelope*tone))
    with wave.open(str(sounds/(name+'.wav')),'wb') as target:
        target.setnchannels(1); target.setsampwidth(2); target.setframerate(16000)
        target.writeframes(struct.pack('<'+'h'*len(samples),*samples))
for state,name in [('waiting_input','attention'),('success','success'),('error','error')]:
    manifest['states'][state]['sound']=f'sounds/{name}.wav'
(ROOT/'pet.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(ROOT)
