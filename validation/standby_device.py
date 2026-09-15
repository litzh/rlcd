"""Device integration: changes Agent display, requires pixel-cat and deepseek-whale installed."""
import json
import sys
import time
import urllib.request
import urllib.error

base=sys.argv[1].rstrip('/')
client=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def call(path, data=None, expected=200):
    req=urllib.request.Request(base+path,data=json.dumps(data).encode() if data is not None else None,headers={'Content-Type':'application/json'})
    try:
        with client.open(req,timeout=15) as response:
            assert expected==response.status
            return json.load(response)
    except urllib.error.HTTPError as error:
        assert error.code==expected,(error.code,error.read())

saved=call('/pets')['active_path']
task='standby-'+str(time.time_ns())
a=dict(agent_id='client-a',task_id=task,seq=1,state='working',ttl_seconds=120,pet_id='pixel-cat')
b=dict(a,agent_id='client-b',pet_id='deepseek-whale')
assert call('/agent/state',a)['pet_id']=='pixel-cat'
assert call('/agent/state',b)['pet_id']=='deepseek-whale'
call('/agent/state',a,409)
assert call('/agent/state')['agent_id']=='client-b'
assert call('/agent/state?agent_id=client-a&task_id='+task)['seq']==1
a.update(seq=2,pet_id='not-installed')
call('/agent/state',a,404)
assert call('/agent/state')['agent_id']=='client-b'
assert call('/agent/state?agent_id=client-a&task_id='+task)['seq']==1
a['pet_id']='pixel-cat'
assert call('/agent/state',a)['pet_id']=='pixel-cat'
assert call('/pets')['active_path']==saved
print('PASS per-task sequences, automatic pet switching, atomic rejection, default selection unchanged',flush=True)
a.update(seq=3,state='working',ttl_seconds=5)
assert not call('/agent/state',a)['standby']
time.sleep(5.3)
s=call('/agent/state'); assert s['expired'] and s['standby'] and s['state']=='stale',s
print('PASS TTL enters standby',flush=True)
a.update(seq=4,state='success',ttl_seconds=120)
assert not call('/agent/state',a)['standby']
time.sleep(30.4)
s=call('/agent/state'); assert s['standby'] and not s['expired'],s
print('PASS success enters standby after 30 seconds before TTL',flush=True)
a.update(seq=5,state='waiting_input',ttl_seconds=120)
assert not call('/agent/state',a)['standby']
time.sleep(30.4)
assert not call('/agent/state')['standby']
print('PASS waiting_input remains visible beyond idle timeout',flush=True)
a.update(seq=6,state='idle')
assert not call('/agent/state',a)['standby']
time.sleep(30.4)
assert call('/agent/state')['standby']
print('PASS idle enters standby after 30 seconds',flush=True)
