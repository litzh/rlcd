"""Exercise display controls on a device, restoring its initial setting."""
import json
import sys
import urllib.error
import urllib.request

base=sys.argv[1].rstrip('/')
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
def call(payload=None,expected=200,raw=None):
    data=raw if raw is not None else json.dumps(payload).encode() if payload is not None else None
    request=urllib.request.Request(base+('/display' if data is None else '/display/invert'),data=data,method='GET' if data is None else 'PUT',headers={'Content-Type':'application/json'})
    try:
        with opener.open(request,timeout=10) as response:
            assert response.status==expected
            return json.load(response)
    except urllib.error.HTTPError as error:
        assert error.code==expected,(error.code,error.read())
initial=call()
assert initial['storage_ready'],initial
original=initial['inverted']
try:
    assert call({'mode':'on'})['inverted']
    assert call({'mode':'on'})['inverted']
    assert not call({'mode':'toggle'})['inverted']
    assert not call({'mode':'off'})['inverted']
    for value in ({},{'mode':True},{'mode':'invalid'},{'mode':'on\0off'}):
        call(value,400)
        assert not call()['inverted']
    call(expected=400,raw=b'{invalid')
    call(expected=413,raw=b' '*129)
    assert not call()['inverted']
    print('PASS on/off/toggle, repeated setting, invalid input isolation')
finally:
    call({'mode':'on' if original else 'off'})
    print('Original display setting restored')
