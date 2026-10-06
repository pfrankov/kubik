#!/usr/bin/env python3
"""Build and verify the A6 guide; --check verifies the saved package inputs/PDFs."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import tempfile
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'docs/buyer'
OUTPUTS = ('kubik-getting-started-A6.pdf', 'kubik-booklet-A5-duplex.pdf')
PAGE_COUNT = 8


def inputs():
    paths = [*SOURCE.glob('*.html'), *SOURCE.glob('*.css'), *SOURCE.glob('*.js'), *SOURCE.glob('*.txt'),
             *SOURCE.glob('assets/*.png'), ROOT / 'tools/build-buyer-guide.py',
             *[ROOT / 'tools/font/sources' / f'PT_Sans-Web-{style}.ttf' for style in ('Regular', 'Bold')]]
    return {str(p.relative_to(ROOT)): sha256(p.read_bytes()).hexdigest() for p in sorted(paths)}


def check_links():
    for path in SOURCE.glob('*.html'):
        text = path.read_text()
        assert not re.search('[А-Яа-яЁё]', text), f'Non-English buyer copy: {path}'
        for target in re.findall(r'(?:href|src)="([^"]+)"', text):
            url = urlsplit(target)
            if url.scheme: continue
            destination = path.parent / unquote(url.path) if url.path else path
            assert destination.exists(), f'Missing buyer link: {target}'
            if url.fragment:
                assert f'id="{url.fragment}"' in destination.read_text(), f'Missing anchor: {target}'


def render(path):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel='chrome', headless=True)
        page = browser.new_page()
        page.goto((SOURCE / 'booklet.html').as_uri())
        page.emulate_media(media='print')
        page.evaluate('document.fonts.ready')
        assert page.locator('.page').count() == PAGE_COUNT
        assert page.locator('img').evaluate_all('(imgs) => imgs.every(i => i.complete && i.naturalWidth > 0)')
        overflow = page.locator('.page').evaluate_all('''pages => pages.flatMap((page, i) => {
          const content = page.querySelector('.content').getBoundingClientRect();
          return [...page.querySelector('.content').children].filter(el => {
            const r = el.getBoundingClientRect();
            return r.bottom > content.bottom + 1 || r.top < content.top - 1 ||
              (el.tagName !== 'IMG' && (r.left < content.left - 1 || r.right > content.right + 1));
          }).map(el => ({page: i + 1, element: el.tagName, text: el.textContent.slice(0, 80)}));
        })''')
        assert not overflow, f'Print overflow: {overflow}'
        art_sizes = page.locator('.art').evaluate_all('imgs => imgs.map(i => i.getBoundingClientRect().height)')
        assert min(art_sizes) >= 85, f'Illustration shrank below 22.5 mm: {art_sizes}'
        page.pdf(path=str(path), prefer_css_page_size=True, print_background=True, tagged=True,
                 display_header_footer=False)
        browser.close()


def normalize_pdf(path):
    import fitz
    with fitz.open(path) as pdf:
        # Chromium rounds print sizes to pixels. Trim only the fractional blank edge.
        for page in pdf:
            page.set_mediabox(fitz.Rect(0, 0, 105 * 72 / 25.4, 148 * 72 / 25.4))
        pdf.set_metadata({'title': 'Kubik · Getting started', 'author': 'Kubik',
                          'subject': 'English A6 guide · Tess · software 0.6.2'})
        data = pdf.tobytes(garbage=4, deflate=True, no_new_id=True)
    path.write_bytes(data)


def impose(source, target):
    import fitz
    with fitz.open(source) as guide, fitz.open() as sheets:
        width, height = guide[0].rect.width, guide[0].rect.height
        for sheet in range(len(guide) // 4):
            pairs = ((len(guide) - 1 - sheet * 2, sheet * 2),
                     (sheet * 2 + 1, len(guide) - 2 - sheet * 2))
            for left, right in pairs:
                page = sheets.new_page(width=width * 2, height=height)
                page.show_pdf_page(fitz.Rect(0, 0, width, height), guide, left)
                page.show_pdf_page(fitz.Rect(width, 0, width * 2, height), guide, right)
        sheets.set_metadata({'title': 'Kubik · A5 duplex booklet sheets'})
        sheets.save(target, garbage=4, deflate=True, no_new_id=True)


def check_pdf(path, pages, width_mm, height_mm):
    import fitz
    with fitz.open(path) as pdf:
        assert len(pdf) == pages, (path, len(pdf))
        fonts = set()
        for page in pdf:
            assert abs(page.rect.width * 25.4 / 72 - width_mm) < .3
            assert abs(page.rect.height * 25.4 / 72 - height_mm) < .3
            assert page.get_text().strip(), f'Empty page {page.number + 1}'
            for word in page.get_text('words'):
                assert page.rect.contains(fitz.Rect(*word[:4])), f'Text outside page: {word[4]}'
            fonts.update(font[0] for font in page.get_fonts())
        assert fonts and all(pdf.extract_font(xref)[3] for xref in fonts), 'Unembedded font'


def verify_saved():
    manifest = json.loads((SOURCE / 'print-manifest.json').read_text())
    assert manifest['inputs'] == inputs(), 'Buyer sources changed; rebuild with tools/build-buyer-guide.py'
    for name, digest in manifest['outputs'].items():
        assert sha256((SOURCE / name).read_bytes()).hexdigest() == digest, f'Changed PDF: {name}'


def check_imposition():
    import fitz
    expected = [(left, right) for sheet in range(PAGE_COUNT // 4)
                for left, right in ((PAGE_COUNT - sheet * 2, sheet * 2 + 1),
                                    (sheet * 2 + 2, PAGE_COUNT - sheet * 2 - 1))]
    with fitz.open(SOURCE / OUTPUTS[1]) as pdf:
        for page, numbers in zip(pdf, expected, strict=True):
            for side, number in enumerate(numbers):
                box = fitz.Rect(side * page.rect.width / 2, 0, (side + 1) * page.rect.width / 2, page.rect.height)
                assert page.get_text(clip=box).strip().splitlines()[-1] == f'{number:02}', 'Wrong folded page order'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    check_links()
    if args.check:
        verify_saved()
    else:
        with tempfile.TemporaryDirectory(prefix='kubik-booklet-') as directory:
            source = Path(directory) / OUTPUTS[0]
            render(source)
            normalize_pdf(source)
            (SOURCE / OUTPUTS[0]).write_bytes(source.read_bytes())
        impose(SOURCE / OUTPUTS[0], SOURCE / OUTPUTS[1])
        manifest = {'inputs': inputs(), 'outputs': {
            name: sha256((SOURCE / name).read_bytes()).hexdigest() for name in OUTPUTS}}
        (SOURCE / 'print-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    check_pdf(SOURCE / OUTPUTS[0], PAGE_COUNT, 105, 148)
    check_pdf(SOURCE / OUTPUTS[1], PAGE_COUNT // 2, 210, 148)
    check_imposition()
    print(f'buyer guide: {PAGE_COUNT} A6 pages, {PAGE_COUNT // 2} imposed A5 sides, links, fonts and source hashes verified')


if __name__ == '__main__':
    main()
