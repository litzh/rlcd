"""Device checks: installs pixel-cat, exercises playback, preserves unrelated SD files."""
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.error

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'src'))
from rlcd.cli import Device
from rlcd.pet_assets import install_pet, upload

d=Device(sys.argv[1])
def get(): return json.loads(d.request('/pets'))
def post(path,payload,expected=200):
    try:
        result=json.loads(d.request(path,json.dumps(payload).encode(),'POST'))
        assert expected==200,result
        return result
    except urllib.error.HTTPError as error:
        assert error.code==expected,(error.code,error.read())

install_pet(d,ROOT/'pets/pixel-cat')
install_pet(d,ROOT/'pets/pixel-cat')
post('/pets/use',{'id':'pixel-cat'})
assert get()['active_id']=='pixel-cat'
path=get()['active_path']
post('/pets/use',{'id':'not-registered'},404)
assert get()['active_path']==path
# A uniquely named invalid resource must never be registered or selected.
invalid=b'RLP1\xff\xff\xff\xff'+str(time.time_ns()).encode()
invalid_path='/pet-'+hashlib.sha256(invalid).hexdigest()[:24]+'.rlp'
upload(d,invalid_path,invalid)
try:
    post('/pets/register',{'path':invalid_path},400)
    assert get()['active_path']==path
finally:
    d.request('/sd/file?path='+invalid_path,method='DELETE')
print('PASS idempotent install, selection, invalid package isolation',flush=True)
payload=dict(agent_id='material-check',task_id=str(time.time_ns()),seq=1,state='idle',ttl_seconds=60,sound=False)
post('/agent/state',payload)
frames=[]
for _ in range(12):
    time.sleep(.12); frames.append(get()['frame'])
assert len(set(frames))>=3,frames
print('PASS local loop frames:',frames,flush=True)
payload.update(seq=2,state='error',sound=True)
assert post('/agent/state',payload)['sound_http_code']==202
time.sleep(1.2)
assert get()['frame']==3,get()
time.sleep(.3)
assert get()['frame']==3,get()
a=json.loads(d.request('/audio/status'))
assert a['state']=='idle' and a['error'] is None,a
payload.update(seq=3)
assert post('/agent/state',payload)['sound_http_code']==0
print('PASS non-loop final frame, package sound completion, cue deduplication',flush=True)
payload.update(seq=4,state='idle',sound=False,ttl_seconds=3600)
post('/agent/state',payload)
