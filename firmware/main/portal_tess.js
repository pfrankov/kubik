// Tess: 16 four-dimensional vertices and 96 edge samples; no assets or dependencies.
(() => {
  const canvas = document.getElementById('tess');
  const ctx = canvas?.getContext('2d');
  if (!ctx) return;
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  const vertices = Array.from({length: 16}, (_, i) => [0, 1, 2, 3].map(bit => i & (1 << bit) ? 1 : -1));
  const points = vertices.map(v => [...v]);
  const edges = [];
  for (let i = 0; i < 16; i++) {
    for (let axis = 0; axis < 4; axis++) {
      if (i & (1 << axis)) continue;
      edges.push([i, i | (1 << axis)]);
      for (let n = 1; n <= 3; n++) {
        const point = [...vertices[i]];
        point[axis] = -1 + n * .5;
        points.push(point);
      }
    }
  }
  const projected = new Float32Array(points.length * 3);
  const order = Uint16Array.from(points, (_, i) => i);
  const depthOrder = (a, b) => projected[a * 3 + 2] - projected[b * 3 + 2];
  const palette = success => Array.from({length: 32}, (_, i) =>
    `hsl(${success ? 160 : 205 - i * 40 / 31} 75% ${24 + i * 70 / 31}%)`);
  const colors = palette(false), doneColors = palette(true);
  const width = 200, height = 154, interval = 1000 / 24;
  let frame = 0, timer = 0, last = 0, phase = .4, size = 1;
  let visible = false;

  function project() {
    // Independent XW and YZ rotations, then 4D perspective and a fixed 3D camera.
    const a = phase * .32, b = phase * .21 + .3;
    const ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b);
    for (let i = 0; i < points.length; i++) {
      const v = points[i];
      const x = v[0] * ca - v[3] * sa, w = v[0] * sa + v[3] * ca;
      const y = v[1] * cb - v[2] * sb, z = v[1] * sb + v[2] * cb;
      const perspective = 3 / (4 - w);
      const rx = (x * .878 + z * .479) * perspective;
      const rz = (z * .878 - x * .479) * perspective;
      const ry = y * perspective * .929 - rz * .371;
      const depth = rz * .929 + y * perspective * .371;
      projected[i * 3] = width / 2 + rx * 36 * size;
      projected[i * 3 + 1] = height / 2 + ry * 36 * size;
      projected[i * 3 + 2] = depth + w * .4;
    }
    order.sort(depthOrder);
  }

  function draw() {
    project();
    ctx.clearRect(0, 0, width, height);
    // Faint structural links keep the hypercube legible as its inner and outer cells exchange.
    ctx.strokeStyle = '#62f0dc'; ctx.lineWidth = .6; ctx.globalAlpha = .16;
    ctx.beginPath();
    for (const [a, b] of edges) {
      ctx.moveTo(projected[a * 3], projected[a * 3 + 1]);
      ctx.lineTo(projected[b * 3], projected[b * 3 + 1]);
    }
    ctx.stroke(); ctx.globalAlpha = 1;
    const ink = canvas.dataset.state === 'done' ? doneColors : colors;
    for (const i of order) {
      const x = projected[i * 3], y = projected[i * 3 + 1];
      const near = Math.max(0, Math.min(1, (projected[i * 3 + 2] + 2) / 4));
      const r = .85 + near + (i < 16 ? .7 : 0);
      if (i < 16 && near > .65) {
        ctx.globalAlpha = (near - .65) * .35;
        ctx.fillStyle = '#82ffe6';
        ctx.beginPath(); ctx.arc(x, y, r * 2.2, 0, Math.PI * 2); ctx.fill();
        ctx.globalAlpha = 1;
      }
      ctx.fillStyle = ink[Math.round(near * 31)];
      ctx.beginPath(); ctx.arc(x, y, r, 0, Math.PI * 2); ctx.fill();
    }
  }

  function targetSize() {
    return canvas.dataset.state === 'done' ? 1.08 : canvas.dataset.state === 'trying' ? 1.04 : 1;
  }
  function running() { return visible && !document.hidden && !reduced.matches; }
  function schedule() {
    if (!running()) return;
    // One paint callback per frame, not one idle callback per display refresh.
    timer = setTimeout(() => { timer = 0; frame = requestAnimationFrame(tick); },
      Math.max(0, interval - (performance.now() - last)));
  }
  function tick(now) {
    frame = 0;
    if (!running()) return;
    const dt = Math.min((now - last) / 1000, .1);
    last = now;
    phase += dt * (canvas.dataset.state === 'trying' ? 1.7 : 1);
    size += (targetSize() - size) * Math.min(1, dt * 6);
    draw();
    schedule();
  }
  function resume() {
    clearTimeout(timer); cancelAnimationFrame(frame);
    timer = frame = 0;
    if (!visible || document.hidden) return;
    last = performance.now();
    if (reduced.matches) size = targetSize();
    draw();
    schedule();
  }
  function resize() {
    const box = canvas.getBoundingClientRect();
    const ratio = Math.min(devicePixelRatio || 1, 2);
    canvas.width = Math.max(1, Math.round(box.width * ratio));
    canvas.height = Math.max(1, Math.round(box.height * ratio));
    ctx.setTransform(canvas.width / width, 0, 0, canvas.height / height, 0, 0);
    resume();
  }
  document.addEventListener('visibilitychange', resume);
  reduced.addEventListener('change', resume);
  new MutationObserver(resume).observe(canvas, {attributes: true, attributeFilter: ['data-state']});
  new IntersectionObserver(entries => { visible = entries[0].isIntersecting; resume(); }).observe(canvas);
  new ResizeObserver(resize).observe(canvas);
})();
