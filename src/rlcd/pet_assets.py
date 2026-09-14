"""Compile user PNG/WAV assets into the versioned RLCD pet format."""
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import wave
from PIL import Image, ImageOps

STATES = ('idle', 'working', 'waiting_input', 'success', 'error', 'stale')
FRAME_BYTES = 2560
MAX_FRAMES = 96


def identifier(value):
    return isinstance(value, str) and re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,31}', value)


def asset(root, name, suffix):
    if not isinstance(name, str) or not name or '\\' in name:
        raise ValueError('素材路径必须为包内相对路径')
    candidate = (root / name).resolve()
    if Path(name).is_absolute() or not candidate.is_relative_to(root) or candidate.suffix.lower() != suffix:
        raise ValueError('素材路径越界或格式错误：' + name)
    if not candidate.is_file():
        raise ValueError('素材缺失：' + name)
    return candidate


def image_frame(path):
    with Image.open(path) as source:
        if source.format != 'PNG' or source.width > 2048 or source.height > 2048:
            raise ValueError('图片须为 PNG，边长不超过 2048')
        rgba = source.convert('RGBA')
        background = Image.new('RGBA', rgba.size, 'white')
        background.alpha_composite(rgba)
        fitted = ImageOps.contain(background.convert('L'), (160, 128), Image.Resampling.LANCZOS)
        result = Image.new('L', (160, 128), 255)
        result.paste(fitted, ((160-fitted.width)//2, (128-fitted.height)//2))
        result = result.point(lambda p: 0 if p < 160 else 255).convert('1')
    raw = bytearray(FRAME_BYTES)
    for y in range(128):
        for x in range(160):
            if result.getpixel((x, y)) == 0:
                raw[y*20+x//8] |= 1 << (x%8)
    return bytes(raw), result


def _sound_bytes(path):
    # Convert uncompressed 8/16-bit mono/stereo PCM using linear resampling.
    with wave.open(str(path), 'rb') as source:
        channels, width, rate, count = source.getnchannels(), source.getsampwidth(), source.getframerate(), source.getnframes()
        if source.getcomptype() != 'NONE' or channels not in (1, 2) or width not in (1, 2) or not 8000 <= rate <= 96000 or not 0 < count <= rate*5:
            raise ValueError('音效须为不超过 5 秒的 8/16bit、单/双声道 PCM WAV（8–96kHz）')
        raw = source.readframes(count)
        if len(raw) != count*channels*width:
            raise ValueError('WAV 数据不完整')
    values = []
    for i in range(count):
        samples = []
        for c in range(channels):
            offset = (i*channels+c)*width
            samples.append((raw[offset]-128)*256 if width == 1 else struct.unpack_from('<h', raw, offset)[0])
        values.append(sum(samples)/channels)
    converted = bytearray()
    for i in range(max(1, round(count*16000/rate))):
        position = i*rate/16000
        left = min(int(position), count-1)
        fraction = position-left
        sample = round(values[left]*(1-fraction) + values[min(left+1,count-1)]*fraction)
        converted += struct.pack('<h', max(-32768,min(32767,sample)))
    result = io.BytesIO()
    with wave.open(result, 'wb') as target:
        target.setnchannels(1); target.setsampwidth(2); target.setframerate(16000)
        target.writeframes(converted)
    return result.getvalue()


def sound_bytes(path):
    try:
        return _sound_bytes(path)
    except (wave.Error, EOFError) as error:
        raise ValueError('无法读取 PCM WAV：' + str(path)) from error


def compile_pet(folder):
    root = Path(folder).resolve()
    manifest = json.loads((root/'pet.json').read_text(encoding='utf-8'))
    if not isinstance(manifest, dict) or type(manifest.get('format_version')) is not int or manifest.get('format_version') != 1 or not identifier(manifest.get('id')):
        raise ValueError('format_version 必须为 1；id 须为 1–32 位小写字母/数字/下划线/连字符')
    name = manifest.get('name', manifest['id'])
    if not isinstance(name,str) or not name or len(name.encode())>96 or '\0' in name:
        raise ValueError('name 必须为 1–96 UTF-8 字节')
    canvas = manifest.get('canvas')
    if canvas != {'width':160,'height':128}:
        raise ValueError('format v1 的 canvas 必须为 160×128')
    configs = manifest.get('states')
    if not isinstance(configs,dict) or 'idle' not in configs or set(configs)-set(STATES):
        raise ValueError('states 必须包含 idle，且只能使用规定的六个状态')
    header = {'id':manifest['id'],'name':name,'states':[]}
    frames, previews, sounds, dimensions = bytearray(), {}, {}, set()
    for state in STATES:
        config = configs.get(state,configs['idle'])
        if not isinstance(config,dict): raise ValueError('状态配置必须为对象')
        files = config.get('frames')
        duration, loop = config.get('frame_ms',250), config.get('loop',True)
        if not isinstance(files,list) or not 1 <= len(files) <= 64 or type(duration) is not int or not 125 <= duration <= 5000 or type(loop) is not bool:
            raise ValueError('每状态 1–64 帧；frame_ms 为 125–5000 整数；loop 为布尔值')
        entry = {'offset':len(frames),'count':len(files),'frame_ms':duration,'loop':loop,'sound':''}
        previews[state] = []
        for name in files:
            path = asset(root,name,'.png')
            with Image.open(path) as image: dimensions.add(image.size)
            if len(dimensions)>1: raise ValueError('包内 PNG 尺寸必须一致')
            raw, image = image_frame(path)
            frames.extend(raw); previews[state].append(image)
            if len(frames)>MAX_FRAMES*FRAME_BYTES: raise ValueError('含回退状态在内，总帧数不得超过 96')
        sound = config.get('sound') if state in configs else None
        if sound:
            raw = sound_bytes(asset(root,sound,'.wav'))
            path = '/pet-'+hashlib.sha256(raw).hexdigest()[:24]+'.wav'
            sounds[path] = raw; entry['sound']=path
        header['states'].append(entry)
    metadata = json.dumps(header,ensure_ascii=False,separators=(',',':')).encode()
    if len(metadata)>4096: raise ValueError('素材元数据过大')
    package = b'RLP1'+struct.pack('<I',len(metadata))+metadata+frames
    path = '/pet-'+hashlib.sha256(package).hexdigest()[:24]+'.rlp'
    return header, package, path, sounds, previews


def preview_pet(folder, output):
    header, package, path, sounds, previews = compile_pet(folder)
    output = Path(output); output.mkdir(parents=True,exist_ok=True)
    for state, entry in zip(STATES,header['states']):
        frames = [im.convert('P') for im in previews[state]]
        options = {'loop':0} if entry['loop'] else {}
        frames[0].save(output/(state+'.gif'),save_all=True,append_images=frames[1:],duration=entry['frame_ms'],**options)
    sheet = Image.new('1',(160*3,128*2),1)
    for i,state in enumerate(STATES): sheet.paste(previews[state][0],((i%3)*160,(i//3)*128))
    sheet.save(output/'overview.png')
    print(f"{header['id']}: {len(package)} bytes，{len(sounds)} 个音效；预览：{output.resolve()}")


def upload(device, path, data):
    from urllib.error import HTTPError
    from urllib.parse import quote
    url = '/sd/file?path='+quote(path,safe='')
    try: existing = device.request(url)
    except HTTPError as error:
        if error.code != 404: raise
    else:
        if existing != data: raise ValueError('目标路径已有不同内容，未覆盖：'+path)
        return
    boundary='rlcd-pet-upload'
    body=(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="asset.bin"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()+data+f'\r\n--{boundary}--\r\n'.encode()
    device.request(url,body,'POST','multipart/form-data; boundary='+boundary)
    if device.request(url)!=data: raise ValueError('上传校验失败：'+path)


def install_pet(device, folder):
    header, package, path, sounds, _ = compile_pet(folder)
    from .config import home
    cache = home()/'packages'/header['id']
    cache.mkdir(parents=True, exist_ok=True)
    for target, data in {**sounds, path: package}.items():
        (cache/Path(target).name).write_bytes(data)
    for target,data in sounds.items(): upload(device,target,data)
    upload(device,path,package)
    result = device.request('/pets/register',json.dumps({'path':path}).encode(),'POST')
    print(result.decode())
