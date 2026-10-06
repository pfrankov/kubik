// Optional convenience; all instructions and commands work without JavaScript.
function revealTarget() {
  const target = document.getElementById(location.hash.slice(1));
  if (!target) return;
  for (let node = target; node; node = node.parentElement)
    if (node.tagName === 'DETAILS') node.open = true;
  target.scrollIntoView({ block: 'start' });
}
addEventListener('hashchange', revealTarget);
revealTarget();
for (const code of document.querySelectorAll('pre[data-copy]')) {
  const button = document.createElement('button');
  button.type = 'button'; button.className = 'copy'; button.textContent = code.dataset.copyLabel || 'Copy command';
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(code.textContent.trim());
      button.textContent = 'Copied';
    } catch {
      const range = document.createRange(); range.selectNodeContents(code);
      const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range);
      button.textContent = 'Selected — press Ctrl+C or ⌘C';
    }
  });
  code.after(button);
}
