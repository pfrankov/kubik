const audio = new Audio();
const volume = document.querySelector('#volume');
const status = document.querySelector('#status');
const error = document.querySelector('#error');
let favourites;
try { favourites = new Set(JSON.parse(localStorage.getItem('kubik-sparkle-favourites') || '[]')); }
catch { favourites = new Set(); }
let rows = [];
let playbackId = 0;
function stop() {
  playbackId++;
  audio.pause(); audio.currentTime = 0;
  document.querySelectorAll('.playing').forEach(row => row.classList.remove('playing'));
  status.textContent = 'Playback stopped.';
}
function update() {
  document.querySelector('#selection').textContent = favourites.size
    ? `Favourites: ${[...favourites].sort((a,b) => a-b).join(', ')}` : 'No favourites yet';
  rows.forEach(({ row, item, favourite }) => {
    favourite.setAttribute('aria-pressed', String(favourites.has(item.id)));
    row.hidden = document.querySelector('#filter').checked && !favourites.has(item.id);
  });
}
async function play(item, kind, row) {
  stop(); error.textContent = '';
  const current = playbackId;
  audio.src = `${String(item.id).padStart(2,'0')}-${kind}.wav`;
  audio.volume = Number(volume.value) / 100;
  row.classList.add('playing');
  status.textContent = `Playing ${String(item.id).padStart(2,'0')} · ${item.name} · ${kind === 'single' ? 'one particle' : 'rolling cluster'}`;
  try { await audio.play(); } catch {
    if (current !== playbackId) return;
    stop(); error.textContent = 'Could not play the sound. Try again.';
  }
}
document.querySelector('#stop').addEventListener('click', stop);
volume.addEventListener('input', () => { audio.volume = Number(volume.value) / 100; });
document.querySelector('#filter').addEventListener('change', update);
audio.addEventListener('ended', () => {
  document.querySelectorAll('.playing').forEach(row => row.classList.remove('playing'));
  status.textContent = 'Finished. Try another sound or mark a favourite.';
});
window.addEventListener('pagehide', stop);
fetch('variants.json').then(response => { if (!response.ok) throw Error(); return response.json(); }).then(items => {
  for (const item of items) {
    const row = document.createElement('article'); row.className = 'row'; row.id = `sound-${String(item.id).padStart(2,'0')}`;
    const number = document.createElement('span'); number.className = 'number'; number.textContent = String(item.id).padStart(2,'0');
    const title = document.createElement('div');
    const heading = document.createElement('h2'); heading.textContent = item.name;
    const family = document.createElement('span'); family.className = 'family'; family.textContent = item.family + (item.scale ? ` · ${item.scale}` : '');
    title.append(heading, family);
    const actions = document.createElement('div'); actions.className = 'actions';
    for (const [kind, label] of [['single','One particle'], ['roll','Rolling cluster']]) {
      const button = document.createElement('button'); button.textContent = label; button.className = kind;
      button.setAttribute('aria-label', `${label}: ${item.id}, ${item.name}`);
      button.addEventListener('click', () => play(item, kind, row)); actions.append(button);
    }
    const favourite = document.createElement('button'); favourite.textContent = 'Favourite';
    favourite.setAttribute('aria-label', `Favourite ${item.id}, ${item.name}`);
    favourite.addEventListener('click', () => {
      favourites.has(item.id) ? favourites.delete(item.id) : favourites.add(item.id);
      try { localStorage.setItem('kubik-sparkle-favourites', JSON.stringify([...favourites])); } catch {}
      update();
    });
    actions.append(favourite); row.append(number, title, actions);
    document.querySelector('#variants').append(row); rows.push({ row, item, favourite });
  }
  update();
  if (location.hash) document.getElementById(location.hash.slice(1))?.scrollIntoView();
}).catch(() => { error.textContent = 'Sound catalogue unavailable. Reload the page.'; });
