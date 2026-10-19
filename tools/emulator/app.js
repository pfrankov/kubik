const canvas = document.querySelector('canvas'), context = canvas.getContext('2d');
const screen = document.querySelector('#screen');
fetch('/catalog').then(response => response.json()).then(names => names.forEach((name,index) => screen.add(new Option(name,index))));
let queue = Promise.resolve(), running = true, hold, moved = false;
let pendingMove = null, frameTimer = 0, inFlight = false, lastTouchSent = 0;
let primaryPointer = null, start, last, nextId = 0;
const contacts = new Map(), touchCapacity = 2;
function snapshot() {
  const points = Array.from({length:touchCapacity},() => [0,0,0,0]);
  for (const p of contacts.values()) points[p.slot] = [1,p.x,p.y,p.id];
  return points.flat();
}
function flushMove() {
  if (!pendingMove) return;
  send('touches',pendingMove); pendingMove = null; lastTouchSent = performance.now();
}
function send(command,args) {
  queue = queue.then(async () => {
    const response = await fetch('/event',{signal:AbortSignal.timeout(5000),method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command,args})});
    if (!response.ok) throw new Error('Preview input failed');
  }).catch(error => { document.querySelector('#error').textContent = error.message; });
  return queue;
}
function cancelContacts() {
  clearTimeout(hold); pendingMove = null;
  const ids = [...contacts.keys()];
  contacts.clear(); primaryPointer = null; moved = true;
  send('touches',snapshot());
  for (const id of ids) if (canvas.hasPointerCapture(id)) canvas.releasePointerCapture(id);
}
screen.onchange = () => { cancelContacts(); send('select',[Number(screen.value)]); };
document.querySelector('#character').onchange = event => { cancelContacts(); send('character',[Number(event.target.value)]); };
for (const id of ['tilt-x','tilt-y']) document.querySelector(`#${id}`).oninput = () => send('tilt',[Number(document.querySelector('#tilt-x').value),Number(document.querySelector('#tilt-y').value)]);
function point(event) {
  const rect = canvas.getBoundingClientRect();
  return [Math.round(Math.max(0,Math.min(479,(event.clientX-rect.left)*480/rect.width))),Math.round(Math.max(0,Math.min(479,(event.clientY-rect.top)*480/rect.height)))];
}
function contactId() {
  const used = new Set([...contacts.values()].map(p => p.id));
  while (used.has(nextId)) nextId = (nextId + 1) % 16;
  const id = nextId; nextId = (nextId + 1) % 16; return id;
}
canvas.onpointerdown = event => {
  if (contacts.has(event.pointerId) || contacts.size >= touchCapacity) return;
  const [x,y] = point(event), used = new Set([...contacts.values()].map(p => p.slot));
  const slot = used.has(0) ? 1 : 0;
  if (!contacts.size) {
    primaryPointer = event.pointerId; moved = false; start = last = [x,y];
    hold = setTimeout(() => { if (contacts.has(primaryPointer) && !moved) { send('event',[5,...start]); moved = true; } },800);
  } else { moved = true; clearTimeout(hold); }
  contacts.set(event.pointerId,{slot,id:contactId(),x,y});
  canvas.setPointerCapture(event.pointerId);
  pendingMove = snapshot(); flushMove();
};
canvas.onpointermove = event => {
  const contact = contacts.get(event.pointerId); if (!contact) return;
  [contact.x,contact.y] = point(event);
  if (event.pointerId === primaryPointer) {
    last = [contact.x,contact.y];
    if (Math.hypot(last[0]-start[0],last[1]-start[1]) > 8) { moved = true; clearTimeout(hold); }
  }
  pendingMove = snapshot();
};
function release(event,cancel) {
  const contact = contacts.get(event.pointerId); if (!contact) return;
  clearTimeout(hold); flushMove();
  if (event.pointerId === primaryPointer) last = cancel ? [contact.x,contact.y] : point(event);
  contacts.delete(event.pointerId);
  const tap = event.pointerId === primaryPointer && !contacts.size && !moved && !cancel;
  pendingMove = snapshot(); flushMove();
  if (tap) send('event',[0,...last]);
  if (!contacts.size) primaryPointer = null;
}
canvas.onpointerup = event => release(event,false);
canvas.onpointercancel = event => release(event,true);
canvas.onlostpointercapture = event => release(event,true);
document.querySelectorAll('[data-key]').forEach(button => {
  let timer, held = false;
  button.onpointerdown = event => {
    cancelContacts(); button.setPointerCapture(event.pointerId); held = false;
    if (button.dataset.key === '1') timer = setTimeout(() => { held = true; send('event',[3,0,0]); },800);
  };
  button.onpointerup = () => { clearTimeout(timer); if (!held) send('event',[Number(button.dataset.key),0,0]); };
  button.onpointercancel = () => clearTimeout(timer);
});
async function frame(steps = 1) {
  if (!running || document.hidden || inFlight) return;
  inFlight = true;
  if (contacts.size && performance.now() - lastTouchSent > 120) pendingMove = snapshot();
  flushMove();
  const began = performance.now();
  try {
    await queue;
    const response = await fetch(`/frame?steps=${steps}`,{signal:AbortSignal.timeout(5000)});
    if (!response.ok) throw new Error('Native renderer unavailable');
    const state = JSON.parse(response.headers.get('X-Kubik-State'));
    const image = await createImageBitmap(await response.blob()); context.drawImage(image,0,0); image.close();
    document.querySelector('#state').textContent = `${state.name} · preview only`;
    screen.value = String(state.screen);
  } catch(error) { document.querySelector('#error').textContent = error.message; running = false; }
  inFlight = false;
  if (running && !document.hidden) frameTimer = setTimeout(frame,Math.max(0,1000/30-(performance.now()-began)));
}
document.addEventListener('visibilitychange',() => {
  clearTimeout(frameTimer);
  if (document.hidden) cancelContacts(); else if (running) frame();
});
window.addEventListener('blur',cancelContacts);
window.addEventListener('pagehide',() => { cancelContacts(); running = false; clearTimeout(frameTimer); });
frame();
