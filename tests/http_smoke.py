"""运行: uv run tests/http_smoke.py http://设备IP"""
import json
import sys
import urllib.error
import urllib.request

base = sys.argv[1].rstrip('/')
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def request(path, payload=None, expected=200):
    data = None if payload is None else payload.encode()
    req = urllib.request.Request(base + path, data=data, headers={'Content-Type': 'application/json'})
    try:
        response = opener.open(req, timeout=10)
    except urllib.error.HTTPError as e:
        response = e
    assert response.status == expected, (path, response.status, expected)
    return json.load(response)

status = request('/status')
assert status['wifi']['state'] == 'connected'
assert status['wifi']['ip']
assert {'shtc3', 'battery', 'rtc'} <= status['sensors'].keys()
assert 'password' not in json.dumps(status).lower()
for message in ['Hello from HTTP', 'Quotes: "hello" \\ backslash\nSecond line', '', 'x' * 240]:
    assert request('/echo', json.dumps({'message': message})) == {'message': message}
request('/echo', '{invalid', 400)
request('/echo', '{}', 400)
request('/echo', '{"message":123}', 400)
request('/echo', json.dumps({'message': '中文'}), 400)
request('/echo', json.dumps({'message': 'before\x00after'}), 400)
request('/echo', json.dumps({'message': 'x' * 241}), 400)
request('/echo', json.dumps({'message': 'x' * 2100}), 413)
request('/missing', expected=404)
request('/echo', json.dumps({'message': 'HTTP echo OK\nWi-Fi + sensors ready'}))
print(json.dumps(status, indent=2))
print('PASS: status, echo roundtrip, ASCII/size validation, malformed JSON, 404')
