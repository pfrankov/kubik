#!/usr/bin/env python3
"""Buyer handoff: one setup page, draft GitHub addresses, responsive copy and saved A6 print."""
import importlib.util
import os
from pathlib import Path
import re
import tempfile
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'docs/buyer'
SETUP_URL = 'https://OWNER.github.io/kubik/'


def check_print():
    spec = importlib.util.spec_from_file_location('buyer_print', ROOT / 'tools/build-buyer-guide.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.check_links()
    module.verify_saved()
    module.check_pdf(SOURCE / module.OUTPUTS[0], module.PAGE_COUNT, 105, 148)
    module.check_pdf(SOURCE / module.OUTPUTS[1], module.PAGE_COUNT // 2, 210, 148)
    module.check_imposition()
    booklet = (SOURCE / 'booklet.html').read_text()
    assert SETUP_URL in booklet and 'Temporary address' in booklet
    assert booklet.index('Know the buttons.') < booklet.index('Press <kbd>PWR</kbd>')
    assert 'Talk to Tess.' in booklet and '“Hi Tessa”' in booklet
    commands = re.findall(r'<pre[^>]*>(.*?)</pre>', booklet, re.S)
    # A buyer must be able to run listing without also pasting approval.
    for command in ('openclaw pairing list kubik', 'openclaw pairing approve kubik CODE',
                    'python ~/.hermes/plugins/kubik/pair.py CODE'):
        assert command in [block.strip() for block in commands], f'Pairing command needs its own block: {command}'
    for stale in ('order email', 'order page', 'START.html', 'VOICE.html', 'CONNECT.html', 'ASK-AGENT.txt', 'Say hello.', 'Match. Then approve.'):
        assert stale not in booklet, stale
    with tempfile.TemporaryDirectory(prefix='kubik-print-check-') as directory:
        module.render(Path(directory) / 'guide.pdf')


def open_details(page):
    # DOM order opens parents before nested details; do not accidentally close an open parent.
    for disclosure in page.locator('details').all():
        if disclosure.get_attribute('open') is None:
            disclosure.locator(':scope > summary').click()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'), disclosure.inner_text()[:100]


def check_copy(page):
    button = page.locator('.copy').first
    page.evaluate("Object.defineProperty(navigator, 'clipboard', {value: {writeText: () => Promise.reject(Error('denied'))}, configurable: true})")
    button.click()
    assert 'Selected' in button.inner_text()
    assert page.evaluate('getSelection().toString()') == 'openclaw gateway status'
    page.evaluate("navigator.clipboard.writeText = async text => { window.copiedCommand = text; }")
    button.click()
    assert button.inner_text() == 'Copied'
    assert page.evaluate('window.copiedCommand') == 'openclaw gateway status'
    # Preserve quotes/newlines in both speech credentials and remote-listener configuration.
    for target in ('voice-openclaw', 'address-openclaw', 'assistant'):
        detail = page.locator(f'#{target}')
        expected = detail.locator('pre[data-copy]').first.inner_text().strip()
        detail.locator('.copy').first.click()
        assert page.evaluate('window.copiedCommand') == expected


def check_browser(browser, output, width):
    page = browser.new_page(viewport={'width': width, 'height': 900})
    requests, errors = [], []
    page.on('request', lambda request: requests.append(request.url))
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto((SOURCE / 'START.html').as_uri())
    page.evaluate('document.fonts.ready')
    assert page.locator('html').get_attribute('lang') == 'en'
    assert page.locator('[data-setup-url]').get_attribute('href') == SETUP_URL
    assert page.locator('[data-download-url]').get_attribute('href') == 'https://github.com/OWNER/kubik/releases'
    assert 'Temporary address' in page.locator('.draft').inner_text()
    assert page.locator('img').evaluate_all('imgs => imgs.every(i => i.complete && i.naturalWidth > 0)')
    for link in page.locator('nav a').all():
        link.click()
        assert page.url.endswith(link.get_attribute('href'))
    # A direct recipe link opens that recipe, without asking for another click.
    page.goto((SOURCE / 'START.html').as_uri() + '#voice-hermes')
    assert page.locator('#voice-hermes').get_attribute('open') is not None
    open_details(page)
    assert page.locator('.button-actions dt, .button-actions dd').evaluate_all('items => items.every(item => item.scrollWidth <= item.clientWidth)'), 'Field text overflows its column'
    check_copy(page)
    assert not [url for url in requests if url.startswith(('http:', 'https:'))], requests
    page.locator('details').evaluate_all('items => items.forEach(item => { item.open = false; })')
    page.evaluate('scrollTo(0, 0)')
    page.screenshot(path=str(output / f'start-{width}.png'), full_page=True)
    assert not errors, errors
    page.close()


def main():
    check_print()
    with tempfile.TemporaryDirectory(prefix='kubik-buyer-browser-') as directory:
        output = Path(os.environ.get('KUBIK_BUYER_SHOTS', directory))
        output.mkdir(parents=True, exist_ok=True)
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(channel='chrome', headless=True)
            for width in (320, 390, 1024):
                check_browser(browser, output, width)
            browser.close()
    print('buyer guide: single setup page, explicit GitHub placeholders, recipe navigation, copy/select and 8-page A6 print passed')


if __name__ == '__main__':
    main()
