# /// script
# dependencies = ["pillow>=11,<13"]
# ///
"""Recreate the original pixel-cat assets shipped with RLCD."""
import json
import math
from pathlib import Path
import struct
import wave
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'pets/pixel-cat'
root.mkdir(parents=True,exist_ok=True)
manifest = {'format_version':1,'id':'pixel-cat','name':'像素猫','canvas':{'width':160,'height':128},'states':{}}
for state in ('idle','working','waiting_input','success','error','stale'):
    folder=root/state; folder.mkdir(exist_ok=True)
    frames=[]
    for frame in range(4):
        image=Image.new('RGBA',(80,64),(255,255,255,0))
        d=ImageDraw.Draw(image)
        dy = -2 if state in ('working','success') and frame%2 else 0
        # Draw at half resolution for sharp, consistent two-pixel strokes.
        d.polygon([(24,22+dy),(24,8+dy),(35,17+dy),(45,17+dy),(56,8+dy),(56,22+dy)],fill='white',outline='black',width=2)
        d.rounded_rectangle((21,17+dy,59,44+dy),radius=9,fill='white',outline='black',width=2)
        d.polygon([(27,16+dy),(27,12+dy),(32,17+dy)],fill='black')
        d.polygon([(48,17+dy),(53,12+dy),(53,16+dy)],fill='black')
        for x in (31,49):
            if state=='error':
                d.line((x-2,27+dy,x+2,31+dy),fill='black',width=2)
                d.line((x+2,27+dy,x-2,31+dy),fill='black',width=2)
            elif state=='stale' or state=='idle' and frame==3:
                d.line((x-3,29+dy,x+3,29+dy),fill='black',width=2)
            elif state=='success':
                d.line([(x-3,30+dy),(x,27+dy),(x+3,30+dy)],fill='black',width=2)
            else:
                d.rectangle((x-1,27+dy,x+1,31+dy),fill='black')
        d.polygon([(38,33+dy),(42,33+dy),(40,35+dy)],fill='black')
        d.line([(36,36+dy),(40,38+dy),(44,36+dy)],fill='black')
        d.line((15,33+dy,26,35+dy),fill='black',width=1)
        d.line((54,35+dy,65,33+dy),fill='black',width=1)
        d.rounded_rectangle((28,44+dy,52,57+dy),radius=5,fill='white',outline='black',width=2)
        d.line([(52,51+dy),(62,50+dy),(65,44+dy+(frame%2)*4)],fill='black',width=3)
        if state=='working':
            for n in range(3):
                d.rectangle((33+n*6,4,35+n*6,6),fill='black' if n<=frame%3 else 'white')
        if state=='waiting_input':
            d.text((65,8),'?' if frame%2 else '!',fill='black')
            d.line((25,48+dy,18,40+dy-(frame%2)*4),fill='black',width=2)
        if state=='success':
            d.line([(9,20),(12,23),(18,14)],fill='black',width=2)
        if state=='stale': d.text((63,10),'z',fill='black')
        name=f'{frame+1:03d}.png'; image.resize((160,128),Image.Resampling.NEAREST).save(folder/name)
        frames.append(state+'/'+name)
    manifest['states'][state]={'frames':frames,'frame_ms':250,'loop':state not in ('error','stale')}
sounds=root/'sounds'; sounds.mkdir(exist_ok=True)
for name,frequencies in [('attention',(660,880)),('success',(784,1047)),('error',(440,330))]:
    with wave.open(str(sounds/(name+'.wav')),'wb') as target:
        target.setnchannels(1); target.setsampwidth(2); target.setframerate(16000)
        samples=[]
        for i in range(6400):
            p=i%3200; env=min(1,p/160,(3199-p)/320)
            samples.append(int(4500*env*math.sin(2*math.pi*frequencies[i//3200]*i/16000)))
        target.writeframes(struct.pack('<'+'h'*len(samples),*samples))
for state,sound in [('waiting_input','attention'),('success','success'),('error','error')]:
    manifest['states'][state]['sound']='sounds/'+sound+'.wav'
(root/'pet.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
print(root)
