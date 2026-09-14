import asyncio
import json
import os
import sys
import serial
from bleak import BleakClient, BleakScanner

SERVICE = '98bf0001-7d56-4f36-913a-2f0743a89c11'
WRITE = '98bf0002-7d56-4f36-913a-2f0743a89c11'
STATUS = '98bf0003-7d56-4f36-913a-2f0743a89c11'

async def main():
    ssid = os.environ.get("RLCD_WIFI_SSID", "")
    password = os.environ.get("RLCD_WIFI_PASSWORD", "")
    if not ssid:
        raise SystemExit("Set RLCD_WIFI_SSID before running this device test")
    with serial.Serial(sys.argv[1], 115200, timeout=1) as console:
        console.write(b'provision\n')
        for _ in range(5):
            line=console.readline().decode(errors='replace').strip()
            if line: print(line, flush=True)
    device = await BleakScanner.find_device_by_filter(lambda d, a: SERVICE in a.service_uuids, timeout=12)
    if not device: raise RuntimeError('RLCD BLE device not found')
    print('BLE discovered:', device.name, flush=True)
    async with BleakClient(device) as client:
        async def read():
            return json.loads(bytes(await client.read_gatt_char(STATUS)))
        async def send(obj):
            data=(json.dumps(obj)+'\n').encode()
            await client.write_gatt_char(WRITE,b'\n',response=True)
            for i in range(0,len(data),20):
                await client.write_gatt_char(WRITE,data[i:i+20],response=True)
        initial = await read()
        for _ in range(25):
            if initial['state'] != 'connecting': break
            await asyncio.sleep(1)
            initial = await read()
        print('Initial:', initial, flush=True)
        await send({'ssid':'', 'password':''})
        await asyncio.sleep(1)
        assert (await read())['result'] == 'invalid_credentials'
        print('PASS invalid credentials', flush=True)
        await send({'ssid':'RLCD-Test-Nonexistent-9A71', 'password':'wrongtest123'})
        await asyncio.sleep(23)
        failed=await read()
        print('Failed network:', failed, flush=True)
        assert failed['result']=='connection_failed'
        await send({'ssid':ssid,'password':password})
        for _ in range(25):
            await asyncio.sleep(1)
            state=await read()
            if state['result']=='saved':
                print('PASS BLE saved:',state,flush=True)
                break
        else: raise RuntimeError('Provisioning did not save: '+str(state))
    # Reopen provisioning and fail a new attempt. A subsequent reboot must still
    # connect using the valid credentials saved above.
    with serial.Serial(sys.argv[1], 115200, timeout=1) as console:
        console.write(b'provision\n')
        await asyncio.sleep(2)
    async with BleakClient(device) as client:
        await send({'ssid':'RLCD-Test-Nonexistent-9A71', 'password':'wrongtest123'})
        await asyncio.sleep(23)
        assert (await read())['result']=='connection_failed'
        print('PASS failure after saved credentials; reboot to verify retention', flush=True)

asyncio.run(main())
