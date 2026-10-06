const canvas = document.querySelector('canvas'), context = canvas.getContext('2d');
const screen = document.querySelector('#screen');
fetch("/catalog").then(response => response.json()).then(names => names.forEach((name,index) => screen.add(new Option(name,index))));
let queue = Promise.resolve(), running = true, down = false, start, last, hold, moved;
let pendingMove = null, frameTimer = 0, inFlight = false;
function flushMove() { if (pendingMove) { send("pointer",pendingMove); pendingMove = null; } }
function send(command,args) {
  queue = queue.then(async () => {
    const response = await fetch('/event',{signal:AbortSignal.timeout(5000),method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command,args})});
    if (!response.ok) throw new Error('Preview input failed');
  }).catch(error => { document.querySelector('#error').textContent = error.message; });
  return queue;
}
screen.onchange = () => send('select',[Number(screen.value)]);
document.querySelector('#character').onchange = event => send('character',[Number(event.target.value)]);
for (const id of ['tilt-x','tilt-y']) document.querySelector(`#${id}`).oninput = () => send('tilt',[Number(document.querySelector('#tilt-x').value),Number(document.querySelector('#tilt-y').value)]);
function point(event) { const rect = canvas.getBoundingClientRect(); return [Math.round(Math.max(0,Math.min(480,(event.clientX-rect.left)*480/rect.width))),Math.round(Math.max(0,Math.min(480,(event.clientY-rect.top)*480/rect.height)))]; }
canvas.onpointerdown = event => {
  canvas.setPointerCapture(event.pointerId); down = true; moved = false; start = last = point(event);
  send('pointer',[1,...last]);
  hold = setTimeout(() => { if (down && !moved) { send('event',[5,...start]); moved = true; } },800);
};
canvas.onpointermove = event => { if (!down) return; last = point(event); if (Math.hypot(last[0]-start[0],last[1]-start[1]) > 8) { moved = true; clearTimeout(hold); } pendingMove = [1,...last]; };
function release(event,cancel) { if (!down) return; down = false; clearTimeout(hold); flushMove(); last = point(event); send('pointer',[0,...last]); if (!moved && !cancel) send('event',[0,...last]); }
canvas.onpointerup = event => release(event,false);
canvas.onpointercancel = event => release(event,true);
document.querySelectorAll('[data-key]').forEach(button => {
  let timer, held = false;
  button.onpointerdown = event => { button.setPointerCapture(event.pointerId); held = false; if (button.dataset.key === '1') timer = setTimeout(() => { held = true; send('event',[3,0,0]); },800); };
  button.onpointerup = () => { clearTimeout(timer); if (!held) send('event',[Number(button.dataset.key),0,0]); };
  button.onpointercancel = () => clearTimeout(timer);
});
async function frame() {
  if (!running || document.hidden || inFlight) return;
  inFlight = true;
  flushMove();
  const began = performance.now();
  try {
    await queue;
    const response = await fetch('/frame',{signal:AbortSignal.timeout(5000)});
    if (!response.ok) throw new Error('Native renderer unavailable');
    const state = JSON.parse(response.headers.get('X-Kubik-State'));
    const image = await createImageBitmap(await response.blob()); context.drawImage(image,0,0); image.close();
    document.querySelector('#state').textContent = `${state.name} · preview only`;
    screen.value = String(state.screen);
  } catch(error) { document.querySelector('#error').textContent = error.message; running = false; }
  inFlight = false;
  if (running && !document.hidden) frameTimer = setTimeout(frame,Math.max(0,1000/30-(performance.now()-began)));
}
document.addEventListener('visibilitychange',() => { clearTimeout(frameTimer); if (!document.hidden && running) frame(); });
window.addEventListener('pagehide',() => { running = false; clearTimeout(frameTimer); });
frame();
