#!/usr/bin/env python3
"""Offline browser acceptance: setup flow, keyboard, recovery, English and bounded Tess."""
from pathlib import Path
from contextlib import contextmanager
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import tempfile
import threading
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / 'firmware/main'
STATE = {'nets': [{'s': 'Home', 'r': -48, 'l': True}, {'s': 'Work', 'r': -60, 'l': True}],
         'saved_nets': ['Home', 'Work'], 'device': 'kubik-test', 'server': '', 'phase': 'open'}
ORIGIN = ''


@contextmanager
def static_server():
    # route.fulfill bypasses HTTP decoding; a real server exercises browser gzip support.
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            files = {'/': (WEB / 'portal.html', 'text/html'),
                     '/setup.css': (WEB / 'portal.css', 'text/css'),
                     '/tess.js': (WEB / 'portal_tess.js', 'text/javascript'),
                     '/usb': (ROOT / 'tools/usb-bridge/setup.html', 'text/html')}
            if self.path not in files:
                self.send_error(404)
                return
            file, mime = files[self.path]
            body = file.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', mime + '; charset=utf-8')
            if self.path != '/usb':
                body = gzip.compress(body, mtime=0)
                self.send_header('Content-Encoding', 'gzip')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{server.server_port}'
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def mount(page, usb=False, lose_submit=False):
    state = dict(STATE)
    sent, paths, errors = [], [], []
    page.on('pageerror', lambda error: errors.append(str(error)))
    def route(req):
        path = req.request.url.removeprefix(ORIGIN)
        paths.append(path)
        if path == '/api/state':
            req.fulfill(json=state)
        elif path == '/api/connect':
            sent.append(req.request.post_data_json)
            if lose_submit:
                state.update(phase='failed', error='password')
                req.abort('connectionreset')
            else:
                req.fulfill(json={'ok': True})
        elif path == '/config':
            command = req.request.post_data_json
            sent.append(command)
            if command.get('cmd') == 'set': state['server'] = command.get('url', '')
            req.fulfill(json={'ok': True, 'device': 'kubik-test', 'wifi_connected': True,
                             'online': True, 'via': 'wifi', 'url': state['server']})
        else:
            req.continue_()
    page.route(ORIGIN + '/**', route)
    page.goto(ORIGIN + ('/usb' if usb else '/'))
    return state, sent, paths, errors


def check_page(page, width, height, output):
    page.set_viewport_size({'width': width, 'height': height})
    state, sent, paths, errors = mount(page)
    assert page.locator('html').get_attribute('lang') == 'en'
    assert 'openclaw pairing list kubik' in page.locator('#approvalHint').text_content()
    assert 'Only if the displayed code matches' in page.locator('#approvalHint').text_content()
    page.get_by_role('button', name='Work · saved', exact=True).click()
    fits = page.locator('#pass').evaluate('''(el) => {
      const probe = document.createElement('span');
      probe.style.cssText = `position:absolute;visibility:hidden;white-space:nowrap;font:${getComputedStyle(el).font}`;
      probe.textContent = el.placeholder;
      document.body.appendChild(probe);
      const ok = probe.getBoundingClientRect().width <= el.clientWidth;
      probe.remove();
      return ok;
    }''')
    assert fits and 'saved password' in page.locator('#passHint').inner_text()
    page.locator('#pass').fill('new-password')
    page.set_viewport_size({'width': width, 'height': 360})
    page.locator('#pass').evaluate('(e) => e.scrollIntoView({block: "center"})')
    box, field = page.locator('#go').bounding_box(), page.locator('#pass').bounding_box()
    assert field and field['y'] >= 0 and field['y'] + field['height'] <= box['y']
    assert box['y'] >= 0 and box['y'] + box['height'] <= 360
    assert page.locator('#pass').evaluate('(e) => e === document.activeElement')
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.screenshot(path=str(output / f'portal-{width}-keyboard.png'))
    page.locator('#go').click()
    assert not sent, 'The Wi-Fi step must not save before the agent step'
    page.set_viewport_size({'width': width, 'height': height})
    assert page.locator('#agentStep').is_visible() and page.locator('#wifiStep').is_hidden()
    assert page.evaluate('scrollY === 0'), 'A new step must start at its context after closing the keyboard'
    assert page.locator('#agentTitle').evaluate('(e) => e === document.activeElement')
    page.evaluate('new Promise(resolve => requestAnimationFrame(resolve))')
    action = page.locator('#go').bounding_box()
    assert action and action['y'] >= 0 and action['y'] + action['height'] <= height
    page.screenshot(path=str(output / f'portal-{width}-agent-after-keyboard.png'))
    page.locator('summary').click()
    page.wait_for_function('''() => {
      const hint = document.getElementById('urlHint').getBoundingClientRect();
      const bar = document.getElementById('actions').getBoundingClientRect();
      return hint.bottom <= bar.top + 1;
    }''')
    hint, bar = page.locator('#urlHint').bounding_box(), page.locator('#actions').bounding_box()
    assert hint and bar and hint['y'] + hint['height'] <= bar['y'] + 1, (hint, bar)
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.locator('summary').click()
    page.get_by_role('button', name='Hermes', exact=False).click()
    assert 'Hermes Gateway' in page.locator('#agentHint').inner_text()
    assert 'kubik://HOST:18793' in page.locator('#agentHint').inner_text()
    assert 'GitHub guide' in page.locator('#agentHint').inner_text()
    page.locator('#go').click()
    assert sent == []
    assert 'Enter your Hermes address' in page.locator('#err').inner_text()
    page.locator('#url').fill('kubik://192.168.1.12:18793')
    page.locator('#back').click()
    assert page.locator('#pass').input_value() == 'new-password'
    page.locator('#go').click()
    page.screenshot(path=str(output / f'portal-{width}-agent.png'))
    page.locator('#go').click()
    assert sent == [{'ssid': 'Work', 'pass': 'new-password', 'agent': 'Hermes', 'url': 'kubik://192.168.1.12:18793'}]
    page.wait_for_selector('.status.show')
    assert page.locator('#actions').is_hidden()
    state['phase'] = 'failed'; state['error'] = 'password'
    page.get_by_text('Incorrect password.', exact=False).wait_for()
    assert page.locator('#wifiStep').is_visible()
    assert page.locator('#pass').input_value() == 'new-password'
    state['phase'] = 'open'; state['error'] = ''
    page.locator('#go').click(); page.locator('#go').click()
    state['phase'] = 'done'
    page.get_by_text('Wi-Fi connected. Next: pairing.', exact=True).wait_for()
    assert page.locator('#nextSteps').is_visible()
    assert 'python ~/.hermes/plugins/kubik/pair.py CODE' in page.locator('#approvalHint').inner_text()
    assert 'matching code' in page.locator('#approvalHint').inner_text()
    assert 'kubik-agent-host' not in page.locator('#approvalHint').inner_text()
    count = len(paths); page.wait_for_timeout(1800)
    assert len(paths) == count, 'Completed setup must stop polling'
    assert not errors, errors
    print(f'portal {width}: keyboard, navigation, preserved entries, failure and completion passed')


def check_recovery(page):
    state, sent, paths, errors = mount(page, lose_submit=True)
    page.get_by_role('button', name='Work · saved', exact=True).click()
    page.locator('#pass').fill('work-password')
    page.locator('#go').click(); page.locator('#back').click()
    assert page.locator('#pass').input_value() == 'work-password'
    page.get_by_role('button', name='Home · saved', exact=True).click()
    assert page.locator('#pass').input_value() == '', 'Never carry another network password'
    page.locator('#pass').fill('home-password')
    page.locator('#manual').click(); page.locator('#ssid').fill('Hidden')
    assert page.locator('#pass').input_value() == ''
    page.locator('#pass').fill('hidden-password')
    page.locator('#go').click(); page.locator('#go').click()
    page.get_by_text('Incorrect password.', exact=False).wait_for()
    assert page.locator('#ssid').input_value() == 'Hidden'
    assert page.locator('#pass').input_value() == 'hidden-password'
    assert len(sent) == 1, 'A lost submission reply must never trigger another write'
    assert not errors, errors
    print('portal recovery: per-network passwords, lost response, visible failure and no duplicate write passed')


def check_tess(page, output):
    page.add_init_script("""window.tessProbe = {frames: 0, callbacks: 0, costs: []};
      const clear = CanvasRenderingContext2D.prototype.clearRect;
      CanvasRenderingContext2D.prototype.clearRect = function(...args) {
        if (this.canvas.id === 'tess') tessProbe.frames++;
        return clear.apply(this, args);
      };
      const raf = window.requestAnimationFrame;
      window.requestAnimationFrame = callback => raf.call(window, time => {
        tessProbe.callbacks++;
        const started = performance.now(); callback(time);
        tessProbe.costs.push(performance.now() - started);
      });""")
    mount(page)
    page.wait_for_function('tessProbe.frames > 1')
    page.emulate_media(reduced_motion='reduce')
    page.wait_for_timeout(100)
    a = page.locator('#tess').evaluate('(c) => c.toDataURL()')
    counts = page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    page.wait_for_timeout(250)
    assert counts == page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    assert a == page.locator('#tess').evaluate('(c) => c.toDataURL()')
    page.emulate_media(reduced_motion='no-preference')
    page.wait_for_timeout(100)
    counts = page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    page.wait_for_timeout(1000)
    later = page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    assert 8 <= later[0] - counts[0] <= 25, 'Bound painting near 24 fps'
    assert later[1] - counts[1] <= 25, 'Do not wake at display refresh rate'
    assert a != page.locator('#tess').evaluate('(c) => c.toDataURL()')
    page.locator('#tess').screenshot(path=str(output / 'tess-4d-a.png'))
    page.wait_for_timeout(2400)
    page.locator('#tess').screenshot(path=str(output / 'tess-4d-b.png'))
    backing = page.locator('#tess').evaluate('(c) => [c.width, c.height]')
    assert backing == [340, 262], backing  # 170 x 131 CSS at DPR capped to 2, even on DPR 3
    check_tess_pauses(page)
    costs = sorted(page.evaluate('tessProbe.costs'))
    print(f'Tess: 4D motion, <=24 fps, DPR cap, paused offscreen/hidden/reduced; callback p95 {costs[int(len(costs)*.95)]:.2f} ms')


def check_tess_pauses(page):
    page.evaluate('scrollTo(0, document.body.scrollHeight)')
    page.wait_for_timeout(150)
    counts = page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    page.wait_for_timeout(250)
    assert counts == page.evaluate('[tessProbe.frames, tessProbe.callbacks]'), 'Offscreen Tess must stop'
    page.evaluate('scrollTo(0, 0)')
    page.wait_for_function(f'tessProbe.frames > {counts[0]}')
    # Exercise the real visibility handler with a controlled browser visibility signal.
    page.evaluate("Object.defineProperty(document, 'hidden', {value: true, configurable: true}); document.dispatchEvent(new Event('visibilitychange'))")
    counts = page.evaluate('[tessProbe.frames, tessProbe.callbacks]')
    page.wait_for_timeout(250)
    assert counts == page.evaluate('[tessProbe.frames, tessProbe.callbacks]'), 'Hidden Tess must stop'
    page.evaluate("delete document.hidden; document.dispatchEvent(new Event('visibilitychange'))")
    page.wait_for_function(f'tessProbe.frames > {counts[0]}')


def check_usb(page, output):
    state, sent, paths, errors = mount(page, usb=True)
    assert page.locator('html').get_attribute('lang') == 'en'
    page.locator('#url').fill('ws://insecure.example/kubik/v1')
    page.locator('#save').click()
    assert page.get_by_text('Use kubik://host', exact=False).is_visible()
    assert len(sent) == 1
    page.locator('#url').fill('kubik://computer')
    page.locator('#save').click()
    page.get_by_text('Connected. Continue on Kubik’s screen.', exact=True).wait_for()
    assert page.locator('#tess').get_attribute('data-state') == 'done'
    assert page.locator('#save').is_enabled()
    page.wait_for_timeout(150)
    assert page.locator('#tess').get_attribute('data-state') == 'done'
    assert sent[1] == {'cmd': 'set', 'url': 'kubik://computer'}
    assert not errors, errors
    page.screenshot(path=str(output / 'usb-setup.png'))
    before = len(sent)
    page.locator('#url').fill('ws://insecure.example/kubik/v1')
    page.locator('#save').click()
    assert page.locator('#message').get_attribute('data-error') == 'true'
    assert page.locator('#tess').get_attribute('data-state') == 'error'
    assert len(sent) == before, 'Invalid retry must not write after a successful setup'
    print('USB: English, shared Tess/style, secure validation, submit and invalid retry passed')


def check_muse(page, output):
    page.set_viewport_size({'width': 390, 'height': 844})
    state, sent, paths, errors = mount(page)
    page.get_by_role('button', name='Home · saved', exact=True).click()
    page.locator('#go').click()
    page.get_by_role('button', name='Muse Direct Wi-Fi', exact=True).click()
    assert page.locator('#serverDetails').is_hidden()
    assert page.locator('#sdkToken').get_attribute('type') == 'password'
    assert 'No computer or server needed' in page.locator('#sdkHint').text_content()
    page.locator('#sdkToken').fill('mgst_' + 'A' * 43)
    page.locator('#sdkToken').scroll_into_view_if_needed()
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.screenshot(path=str(output / 'portal-muse-key.png'))
    page.locator('#go').click()
    page.wait_for_function("document.getElementById('sdkToken').value === ''")
    assert len(sent) == 1 and sent[0]['agent'] == 'Muse' and 'url' not in sent[0]
    state['phase'] = 'done'
    page.wait_for_function("document.getElementById('nextSteps').hidden === false")
    assert 'MuseGadget' in page.locator('#pairingHint').text_content()
    assert 'KEY' in page.locator('#approvalHint').text_content()
    assert 'Bluetooth off' in page.locator('#guideHint').text_content()
    page.screenshot(path=str(output / 'portal-muse-pair.png'))
    assert not errors, errors
    print('Muse: phone flow, masked key, no host field and next-step instructions passed')


def main():
    global ORIGIN
    with tempfile.TemporaryDirectory(prefix='kubik-portal-') as temporary:
        output = Path(os.environ.get('KUBIK_BROWSER_SHOTS', temporary)); output.mkdir(parents=True, exist_ok=True)
        with static_server() as ORIGIN, sync_playwright() as p:
            browser = p.chromium.launch(channel='chrome', headless=True)
            for width, height in [(390, 844), (320, 568), (1024, 768)]:
                page = browser.new_page(); check_page(page, width, height, output); page.close()
            page = browser.new_page(); check_muse(page, output); page.close()
            page = browser.new_page(); check_recovery(page); page.close()
            page = browser.new_page(viewport={'width': 320, 'height': 568}, device_scale_factor=3); check_tess(page, output); page.close()
            page = browser.new_page(); check_usb(page, output); page.close()
            browser.close()


if __name__ == '__main__':
    main()
