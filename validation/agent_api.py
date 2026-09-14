"""Device integration checks: changes displayed Agent state; leaves no SD files."""
import json
import sys
import time
import urllib.error
import urllib.request

base = sys.argv[1].rstrip("/")
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def request(value=None, code=200):
    data = json.dumps(value).encode() if value is not None else None
    req = urllib.request.Request(base + "/agent/state", data=data, headers={"Content-Type": "application/json"})
    try:
        with opener.open(req, timeout=10) as response:
            actual, body = response.status, response.read()
    except urllib.error.HTTPError as error:
        actual, body = error.code, error.read()
    assert actual == code, (actual, body)
    return json.loads(body)

payload = dict(agent_id="validation", task_id=str(time.time_ns()), seq=1, state="working", progress=42,
               ttl_seconds=5, text_hex="00" * 3840, sound=False)
assert request(payload)["progress"] == 42
request(payload, 409)
for key, value in [("progress", 101), ("progress", 1.5), ("seq", 0), ("state", "bad"),
                   ("ttl_seconds", 0), ("text_hex", "00"), ("text_hex", "gg" * 3840),
                   ("agent_id", "a\0b"), ("sound", "true")]:
    bad = dict(payload, seq=2)
    bad[key] = value
    request(bad, 400)
assert request()["seq"] == 1
for number, state in enumerate(("idle", "waiting_input", "success", "error", "working"), 2):
    payload.update(seq=number, state=state)
    assert request(payload)["state"] == state
print("PASS transitions, atomic validation, duplicate/old sequence rejection", flush=True)
time.sleep(5.2)
assert request()["state"] == "stale"
print("PASS state expiration", flush=True)
payload.update(seq=7, state="idle", ttl_seconds=120)
assert request(payload)["state"] == "idle"
print("PASS recovery", flush=True)
