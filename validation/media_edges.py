"""uv run validation/media_edges.py http://DEVICE_IP -- checks only unique temporary files."""
import json,socket,time,urllib.request,urllib.error,urllib.parse,uuid,sys,struct
base=sys.argv[1].rstrip('/')
url=urllib.parse.urlsplit(base)
opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
name='/edge-'+uuid.uuid4().hex[:10]
def call(path,method='GET',data=None,expected=200,ct='application/json'):
    req=urllib.request.Request(base+path,data=data,method=method,headers={'Content-Type':ct})
    try:r=opener.open(req,timeout=15)
    except urllib.error.HTTPError as e:r=e
    payload=r.read();assert r.status==expected,(r.status,expected,payload)
    return json.loads(payload) if 'json' in r.headers.get('Content-Type','') else payload

def part(data,boundary,tail=True):
    return (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="sample.bin"\r\n\r\n').encode()+data+(f'\r\n--{boundary}--\r\n'.encode() if tail else b'')
def upload(path,data,expected=200):
    boundary='check-'+uuid.uuid4().hex
    return call('/sd/file?path='+path,'POST',part(data,boundary),expected,'multipart/form-data; boundary='+boundary)

# Disconnect in the middle of a multipart file, then check that no final file was published.
boundary='abort-check'
payload=part(b'x'*4096,boundary,False)
request=(f'POST /sd/file?path={name}.bin HTTP/1.1\r\nHost: {url.hostname}\r\nContent-Type: multipart/form-data; boundary={boundary}\r\nContent-Length: 99999\r\n\r\n').encode()+payload
with socket.create_connection((url.hostname,url.port or 80),timeout=5) as s:s.sendall(request)
time.sleep(2)
call('/sd/file?path='+name+'.bin',expected=404)
assert not call('/sd/status')['transfer_active']
# Same target is usable after the abandoned upload.
blob=bytes(range(256))*2048
upload(name+'.bin',blob)
assert call('/sd/file?path='+name+'.bin')==blob
call('/sd/file?path='+name+'.bin','DELETE')
# Multiple uploaded files must not commit either one.
b='multi-file'
first=part(b'one',b,False)+b'\r\n'
call('/sd/file?path='+name+'.bin','POST',first+part(b'two',b),400,'multipart/form-data; boundary='+b)
call('/sd/file?path='+name+'.bin',expected=404)
# Embedded NUL must not silently truncate a path.
call('/audio/play','POST',json.dumps({'path':'/bad\x00.wav'}).encode(),400)
# RIFF claims content beyond actual EOF.
bad=b'RIFF'+struct.pack('<I',1000000)+b'WAVE'+b'fmt '+struct.pack('<I',16)+b'\x00'*16
upload(name+'.wav',bad)
call('/audio/play','POST',json.dumps({'path':name+'.wav'}).encode(),415)
call('/sd/file?path='+name+'.wav','DELETE')
assert not call('/sd/status')['transfer_active']
print('PASS interrupted upload cleanup, target reuse, 512KiB streaming/hash, multipart rejection, NUL and malformed WAV')
