"""uv run tests/media_smoke.py http://DEVICE_IP
Writes uniquely named test files only. Plays a quiet two-second test tone.
"""
import array
import hashlib
import io
import json
import math
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import wave
import sys

base = sys.argv[1].rstrip('/')
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def call(path, method='GET', obj=None, expected=200, raw=None, content_type='application/json'):
    data = json.dumps(obj).encode() if obj is not None else raw
    req = urllib.request.Request(base + path, data=data, method=method, headers={'Content-Type': content_type})
    try:
        r = opener.open(req, timeout=15)
    except urllib.error.HTTPError as e:
        r = e
    result = r.read()
    assert r.status == expected, (method, path, r.status, result)
    return json.loads(result) if 'json' in r.headers.get('Content-Type','') else result

def path_arg(path): return urllib.parse.quote(path, safe='')
def upload(path, data, expected=200):
    boundary = 'rlcd-test-' + uuid.uuid4().hex
    payload = (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="test.wav"\r\nContent-Type: application/octet-stream\r\n\r\n').encode() + data + f'\r\n--{boundary}--\r\n'.encode()
    return call('/sd/file?path=' + path_arg(path), 'POST', raw=payload, expected=expected, content_type='multipart/form-data; boundary=' + boundary)

def idle(timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        state = call('/audio/status')
        if state['state'] == 'idle':
            assert state['error'] is None, state
            return state
        time.sleep(.2)
    raise AssertionError('audio did not finish')

status = call('/status')
assert status['sd']['mounted'], status
assert status['audio']['ready'], status
assert status['buttons']['ready'], status
print('Hardware ready', flush=True)
name = '/api-test-' + uuid.uuid4().hex[:10]
p = name + '.wav'
rpath = '/recordings' + name + '.wav'
stop_path = '/recordings' + name + '-stop.wav'
created=[]
try:
    samples = array.array('h', (int(3500 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(32000)))
    if sys.byteorder != 'little': samples.byteswap()
    buffer=io.BytesIO()
    with wave.open(buffer,'wb') as w:
        w.setnchannels(1);w.setsampwidth(2);w.setframerate(16000);w.writeframes(samples.tobytes())
    wav=buffer.getvalue()
    upload(p,wav);created.append(p)
    assert call('/sd/file?path='+path_arg(p)) == wav
    upload(p,wav,409)
    assert any(f['name'].endswith(p[1:]) for f in call('/sd/files?path=/')['files'])
    call('/audio/volume','PUT',{'volume':35})
    call('/audio/volume','PUT',{'volume':101},400)
    call('/audio/volume','PUT',{'volume':1.5},400)
    call('/audio/play','POST',{'path':p},202)
    call('/sd/file?path='+path_arg(p),'DELETE',expected=409)
    idle()
    print('PASS upload/download/hash, duplicate protection, playback, volume validation',flush=True)
    call('/audio/record/start','POST',{'path':rpath,'max_seconds':2},202);created.append(rpath)
    started=time.monotonic();assert call('/status')['audio']['state']=='recording';assert time.monotonic()-started<2
    call('/audio/play','POST',{'path':p},409)
    call('/sd/file?path='+path_arg(rpath),'DELETE',expected=409)
    upload(rpath,wav,409)
    done=idle();assert done['processed_bytes']==64000,done
    recorded=call('/sd/file?path='+path_arg(rpath))
    with wave.open(io.BytesIO(recorded),'rb') as w:
        assert (w.getnchannels(),w.getsampwidth(),w.getframerate(),w.getnframes())==(1,2,16000,32000)
        pcm=array.array('h',w.readframes(w.getnframes()))
        if sys.byteorder!='little':pcm.byteswap()
    print('Recording RMS:',round(math.sqrt(sum(x*x for x in pcm)/len(pcm)),2),'peak:',max(abs(x) for x in pcm),flush=True)
    call('/audio/record/start','POST',{'path':stop_path,'max_seconds':10},202);created.append(stop_path)
    time.sleep(.6);call('/audio/record/stop','POST',{})
    done=idle();assert 0<done['processed_bytes']<320000,done
    call('/audio/play','POST',{'path':stop_path},202);time.sleep(.15);call('/audio/stop','POST',{});idle()
    bad=name+'.bin';upload(bad,b'not a wav');created.append(bad)
    call('/audio/play','POST',{'path':bad},415)
    for path in ['/../bad','/recordings/../bad','/bad.','/bad ']:
        call('/sd/file?path='+path_arg(path),'DELETE',expected=400)
    call('/buttons/status');call('/buttons/events?after=0')
    call('/buttons/events?after=-1',expected=400)
    print('PASS record/finalize/replay/stop, activity conflicts, WAV validation, paths, button APIs',flush=True)
finally:
    # Only clean up this run's test files; never enumerate/delete user files.
    for path in created:
        try: call('/sd/file?path='+path_arg(path),'DELETE')
        except Exception as e: print('Cleanup pending:',path,e)
