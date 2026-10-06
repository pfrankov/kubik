import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { createSetupHandler } from './setup-page.mjs';

async function fixture(t, config = async c => ({ok:true, ...c})) {
  const server=http.createServer(); await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const port=server.address().port;
  server.on('request',createSetupHandler({port,config,route:async c=>({ok:true,enabled:c.enabled})}));
  t.after(()=>new Promise(r=>server.close(r)));
  return (path='/',options={})=> options.headers?.Host ? new Promise(resolve=>{http.get({hostname:'127.0.0.1',port,path,headers:options.headers},res=>{res.resume();resolve({status:res.statusCode})})}) : fetch(`http://127.0.0.1:${port}${path}`,options);
}
const post = body => ({method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
test('serves local setup with key pairing and no token form',async t=>{
  const get=await fixture(t);const res=await get();assert.equal(res.status,200);
  const html=await res.text();assert.match(html,/<html lang="en">/);assert.doesNotMatch(html,/[А-Яа-яЁё]/);assert.match(html,/cmd: 'set', url/);assert.doesNotMatch(html,/id="token"|command\.token|ESP BLE Provisioning/);
  assert.match(html,/Date.now\(\) \+ 30000/);assert.match(res.headers.get('content-security-policy'),/frame-ancestors 'none'/);
  assert.equal((await get('/config',post({cmd:'info'}))).status,200);
  assert.match(await (await get('/tess.js')).text(), /getContext/);
  assert.match(await (await get('/setup.css')).text(), /--mint/);
});
test('rejects hostile origin, DNS rebinding host and non-JSON',async t=>{
  const get=await fixture(t);
  assert.equal((await get('/config',{...post({cmd:'set'}),headers:{'Content-Type':'application/json',Origin:'https://evil.example'}})).status,403);
  assert.equal((await get('/',{headers:{Host:'evil.example'}})).status,403);
  assert.equal((await get('/config',{method:'POST',body:'{}'})).status,415);
  assert.equal((await get('/config',post([]))).status,400);
  assert.equal((await get('/config',{...post({}),body:'bad'})).status,400);
  assert.equal((await get('/config',post({x:'a'.repeat(4097)}))).status,413);
});
test('serializes configuration and bounds device failure',async t=>{
  let finish,entered;const started=new Promise(r=>entered=r);
  const get=await fixture(t,()=>{entered();return new Promise(r=>finish=r)});
  const first=get('/config',post({cmd:'info'}));await started;
  assert.equal((await get('/config',post({cmd:'info'}))).status,429);
  finish({ok:true});assert.equal((await first).status,200);
  assert.equal((await get('/usb-route',post({enabled:'yes'}))).status,400);
  const result=await get('/usb-route',post({enabled:false}));assert.deepEqual(await result.json(),{ok:true,enabled:false});
});

test('client requests safe info and does not expose legacy secrets',async()=>{
  const {readFileSync}=await import('node:fs');const {runInNewContext}=await import('node:vm');
  const html=readFileSync(new URL('./setup.html',import.meta.url),'utf8');
  const elements=new Map();const requests=[];
  const document={getElementById(id){if(!elements.has(id))elements.set(id,{textContent:'',value:'',dataset:{}});return elements.get(id)}};
  const context={document,URL,AbortController,setTimeout,clearTimeout,fetch:async(path,opts)=>{
    requests.push(JSON.parse(opts.body));return {ok:true,json:async()=>({ok:true,device:'kubik-test',url:'wss://example.test/kubik/v1',token:'must-never-appear',online:false})};
  }};
  runInNewContext(html.match(/<script>([\s\S]*)<\/script>/)[1],context);
  await new Promise(r=>setTimeout(r,0));
  assert.deepEqual(requests,[{cmd:'info'}]);
  for(const element of elements.values()){assert.ok(!element.textContent.includes('must-never-appear'));assert.ok(!element.value.includes('must-never-appear'));}
  assert.match(html,/maxlength="127"/);assert.doesNotMatch(html,/maxlength="79"/);
  assert.match(html,/info.online && info.url === url/);
});
