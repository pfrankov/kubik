import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const portalPath = fileURLToPath(new URL('../firmware/main/portal.html', import.meta.url));
const portalHtml = await readFile(portalPath, 'utf8');
const scriptMatch = portalHtml.match(/<script>\n([\s\S]*?)\n<\/script>/);
assert.ok(scriptMatch, 'В portal.html должен быть встроенный JavaScript');
const script = scriptMatch[1];
const elements = new Map();
const requests = [];
const state = {
  nets: [{ s: '<Гостевая & домашняя>', r: -60, l: true }],
  saved_nets: ['<Гостевая & домашняя>', 'Work'],
  scanning: false,
  device: 'K-1',
  server: 'wss://old.example/kubik/v1',
  phase: 'idle'
};

const getElement = (id) => {
  if (!elements.has(id)) {
    elements.set(id, {
      id,
      style: {}, dataset: {},
      setAttribute() {}, removeAttribute() {},
      value: '',
      textContent: '',
      innerHTML: '',
      className: '',
      disabled: false,
      type: id === 'pass' ? 'password' : 'text',
      placeholder: '',
      focus() {},
      getBoundingClientRect() { return {top: 0, bottom: 0, left: 0, right: 0, width: 0, height: 0}; },
      contains() { return false; },
      querySelectorAll() { return []; }
    });
  }
  return elements.get(id);
};

const context = {
  document: { getElementById: getElement, activeElement: null, addEventListener() {}, hidden: false },
  window: {
    innerHeight: 844,
    scrollTo(x, y) { assert.equal(x, 0); assert.equal(y, 0); },
    visualViewport: { height: 500, offsetTop: 0, addEventListener() {} }
  },
  Math, innerHeight: 844, AbortController, clearTimeout() {},
  fetch(path, options) {
    requests.push({ path, options });
    const response = path === '/api/state' ? state : { ok: true };
    return Promise.resolve({ ok: true, json: () => Promise.resolve(response) });
  },
  setTimeout() {},
  JSON,
  String,
  Array
};
vm.createContext(context);
vm.runInContext(script, context);
await new Promise((resolve) => setImmediate(resolve));

assert.equal(getElement('url').value, 'old.example', 'Сохранённый WSS адрес показывается без служебного пути');
assert.match(getElement('nets').innerHTML, /&lt;Гостевая &amp; домашняя&gt;/, 'Имя сети экранируется перед вставкой в HTML');
assert.equal(context.normalizeServerAddress('new.example'), 'wss://new.example/kubik/v1');
assert.equal(context.normalizeServerAddress('http://new.example'), 'http://new.example');
assert.equal(context.normalizeServerAddress('wss://new.example/custom'), 'wss://new.example/custom');
assert.equal(context.friendlyServerAddress('wss://new.example/kubik/v1'), 'new.example');
assert.equal(context.normalizeServerAddress('   '), '', 'Пустое поле означает поиск OpenClaw в этой сети');
assert.equal(context.normalizeServerAddress('kubik://mac-mini.local/'), 'kubik://mac-mini.local');
assert.equal(context.normalizeServerAddress('KUBIK://192.168.1.20:18790'), 'kubik://192.168.1.20:18790');
assert.equal(context.friendlyServerAddress('kubik://mac-mini.local'), 'kubik://mac-mini.local');
assert.equal(context.friendlyServerAddress(''), '');
assert.match(portalHtml, /id="urlHint">On your trusted home network, leave blank to find the agent/, 'Подсказка объясняет поиск в доверенной сети');
assert.match(portalHtml, /id="url"[^>]*aria-describedby="urlHint"/, 'Подсказка связана с полем адреса');
assert.doesNotMatch(portalHtml, /server_required/, 'Адрес сервера больше не обязателен');

assert.equal(getElement('actions').style.bottom, '344px', 'Кнопка перемещается над экранной клавиатурой');
context.window.visualViewport.offsetTop = 50;
context.positionActions();
assert.equal(getElement('actions').style.bottom, '294px', 'Прокрутка visual viewport учитывается');
context.window.visualViewport.height = 844;
context.window.visualViewport.offsetTop = 0;
context.positionActions();
assert.equal(getElement('actions').style.bottom, '0px', 'После закрытия клавиатуры кнопка возвращается вниз');
vm.runInContext("selectedNetworkName = 'Work'", context);
context.updatePasswordHint();
assert.match(getElement('passHint').textContent, /saved password/, 'Пароль сохраняется для каждой известной сети');
assert.equal(getElement('pass').placeholder, 'Optional', 'Короткая подсказка помещается в поле');
vm.runInContext("selectedNetworkName = 'New'", context);
context.updatePasswordHint();
assert.equal(getElement('pass').placeholder, '', 'Новая сеть требует свой пароль');
vm.runInContext("selectedNetworkName = '<Гостевая & домашняя>'", context);
getElement('pass').value = 'test-password';
getElement('url').value = 'new.example';
await context.document.getElementById('form').onsubmit({ preventDefault() {} });
await new Promise((resolve) => setImmediate(resolve));
assert.ok(!requests.some(r => r.path === '/api/connect'), 'Wi-Fi step does not submit prematurely');
await context.document.getElementById('form').onsubmit({ preventDefault() {} });
const connectRequest = requests.find((request) => request.path === '/api/connect');
assert.ok(connectRequest, 'Форма отправляет запрос /api/connect');
assert.deepEqual(JSON.parse(connectRequest.options.body), {
  ssid: '<Гостевая & домашняя>',
  pass: 'test-password',
  agent: 'OpenClaw',
  url: 'wss://new.example/kubik/v1'
});
assert.equal(getElement('status').className, 'status show', 'После успешной отправки показывается статус подключения');
assert.equal(getElement('go').disabled, false, 'Кнопка формы разблокируется после ответа');

// An emptied field is sent as "" (LAN discovery), not dropped.
vm.runInContext('connectionSubmitted = false', context);
getElement('url').value = '';
context.document.getElementById('form').onsubmit({ preventDefault() {} });
await new Promise((resolve) => setImmediate(resolve));
const lanRequest = requests.filter((request) => request.path === '/api/connect').at(-1);
assert.equal(JSON.parse(lanRequest.options.body).url, '', 'Пустой адрес отправляется как поиск в сети');

await getElement('rescan').onclick();
assert.ok(requests.some((request) => request.path === '/api/scan' && request.options.method === 'POST'));
assert.match(getElement('nets').innerHTML, /Looking for networks/, 'При обновлении списка показывается состояние поиска');
assert.match(portalHtml, /<html lang="en">/);
assert.doesNotMatch(portalHtml, /[А-Яа-яЁё]/);
console.log('Портал: сохранённая сеть, экранирование, WSS и kubik:// адреса, пустой адрес (поиск в сети), подключение, статус и поиск сетей проверены.');

// Muse uses the phone/app flow, without a server URL; switching back retains it.
vm.runInContext('connectionSubmitted = false', context);
context.selectAgent('Muse');
assert.equal(getElement('serverDetails').hidden,true);
assert.equal(getElement('museDetails').hidden,false);
assert.match(getElement('agentHint').textContent,/Bluetooth is used only for pairing/);
assert.match(getElement('pairingHint').textContent,/MuseGadget/);
assert.match(getElement('approvalHint').textContent,/press KEY/);
getElement('sdkToken').value = 'mgst_' + 'A'.repeat(43);
await getElement('form').onsubmit({preventDefault(){}});
const museRequest=JSON.parse(requests.filter(r=>r.path==='/api/connect').at(-1).options.body);
assert.equal(museRequest.agent,'Muse');assert.equal(museRequest.sdk_token.length,48);
assert.equal('url' in museRequest,false);assert.equal(getElement('sdkToken').value,'');
context.selectAgent('Hermes');assert.equal(getElement('serverDetails').hidden,false);
assert.equal(getElement('url').value,'');
console.log('Muse: phone-only instructions, masked key submission, no server address and host switch passed.');
