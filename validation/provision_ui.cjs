// Run: node validation/provision_ui.cjs
// Executes the real inline UI script against a simulated Web Bluetooth device.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const path=require('node:path');
const html=fs.readFileSync(path.join(__dirname,'../web/provision.html'),'utf8');
const elements={};
for(const id of ['connect','submit','disconnect','form','ssid','password','status'])elements[id]={value:'',disabled:false,textContent:''};
let timer,state={state:'waiting_for_ble',result:'idle',ip:'',provisioning:true},frame=[],packets=[];
const listeners={};
const device={name:'RLCD-Check',addEventListener:(n,f)=>listeners[n]=f,gatt:{connected:false,
  async connect(){this.connected=true;return {getPrimaryService:async()=>({getCharacteristic:async uuid=>uuid.includes('0002-')?writer:reader})};},
  disconnect(){this.connected=false;listeners.gattserverdisconnected();}
}};
const reader={async readValue(){return new TextEncoder().encode(JSON.stringify(state));}};
const writer={async writeValueWithResponse(chunk){
  assert(chunk.length<=20,'GATT packet must fit default MTU');packets.push(chunk);
  for(const b of chunk){if(b===10){if(frame.length){const config=JSON.parse(new TextDecoder().decode(Uint8Array.from(frame)));assert.equal(config.ssid,'FixtureNetwork');assert.equal(config.password,'fixturepass123');state={...state,state:'connecting',result:'connecting'};}frame=[];}else frame.push(b);}
}};
const context={document:{getElementById:id=>elements[id]},window:{isSecureContext:true},navigator:{bluetooth:{requestDevice:async()=>device}},TextEncoder,TextDecoder,setInterval:fn=>{timer=fn;return 1;},clearInterval:()=>{},setTimeout};
vm.createContext(context);vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1],context);
(async()=>{
  await elements.connect.onclick();assert.equal(elements.submit.disabled,false);
  elements.ssid.value='FixtureNetwork';elements.password.value='short';
  await elements.form.onsubmit({preventDefault(){}});assert.equal(packets.length,0);
  elements.password.value='fixturepass123';await elements.form.onsubmit({preventDefault(){}});
  assert.equal(elements.password.value,'');assert(packets.length>2);
  await timer();assert.equal(elements.submit.disabled,true);
  state={state:'connected',result:'saved',ip:'192.0.2.1',provisioning:false};
  await timer();assert(elements.status.textContent.includes('192.0.2.1'));assert(elements.status.textContent.includes('配置已保存'));
  elements.disconnect.onclick();assert.equal(elements.connect.disabled,false);assert.equal(elements.submit.disabled,true);
  console.log('PASS: HTML connect, credential validation, 20-byte framing, result display, disconnect');
})().catch(e=>{console.error(e);process.exitCode=1;});
