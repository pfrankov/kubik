#!/usr/bin/env python3
"""Browser acceptance of the shared native emulator, with no attached device or agent."""
import importlib.util
from pathlib import Path
import threading
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("kubik_emulator", ROOT / "tools/emulator.py")
emulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emulator)


def read_state(page):
    return page.evaluate("""async () => {
      await queue;
      const response = await fetch('/frame', {signal: AbortSignal.timeout(5000)});
      if (!response.ok) throw new Error(`State frame failed (HTTP ${response.status})`);
      return JSON.parse(response.headers.get('X-Kubik-State'));
    }""")


def click(page, x, y):
    box = page.locator("canvas").bounding_box()
    page.mouse.click(box["x"] + x * box["width"] / 480, box["y"] + y * box["height"] / 480)


def choose(page, index, name):
    page.select_option("#screen", str(index))
    page.wait_for_function("name => document.querySelector('#state').textContent.startsWith(name + ' ·')", arg=name)


def advance_frames(page, count):
    page.evaluate("""async count => {
      running = true;
      for (let i = 0; i < count; i++) { await frame(); clearTimeout(frameTimer); }
      running = false;
    }""", count)


def preview_frames(page, screens):
    # Native animation advances one fixed step per frame request. Stop the UI
    # timer while sampling so runner speed cannot pick a different game phase.
    page.evaluate("running = false; clearTimeout(frameTimer)")
    page.wait_for_function("!inFlight")
    frames = {}
    try:
        for index, name in enumerate(screens):
            page.select_option("#screen", str(index))
            samples = []
            for count in (7, 28, 15):  # 0.23, 1.17, 1.67 s: sample invitation and waiting poses
                advance_frames(page, count)
                assert page.locator("#state").inner_text().startswith(name + " ·"), name
                samples.append(page.locator("canvas").screenshot())
            frames[name] = tuple(samples)
            if name.startswith(("Duet:", "Chase:")):
                page.get_by_role("button", name="KEY", exact=True).click()
                advance_frames(page, 7)
                assert page.locator("canvas").screenshot() == samples[0], f"KEY must replay {name}"
    finally:
        page.evaluate("running = true; frame()")
    legacy = [frames[name][0] for name in screens[:22]]
    assert len(set(legacy)) == 22, "Existing screens must not be duplicate placeholders"
    # Full growth must retain the old adult renderer. The game tiers instead
    # differ through their native movement/rhythm, without a title or game HUD.
    assert frames["Home"] == frames["Growth: tesseract"], "Full growth must preserve Home"
    distinct = {name: sequence for name, sequence in frames.items() if name != "Growth: tesseract"}
    assert len(set(distinct.values())) == len(distinct), "Preview sequences must not be duplicate placeholders"
    for name, sequence in frames.items():
        if name.startswith(("Duet:", "Chase:")):
            assert len(set(sequence)) > 1, f"Game preview must animate: {name}"


def exercise(page, screens):
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.wait_for_function("document.querySelector('#screen').options.length > 1")
    assert len(page.locator("#screen option").all()) == len(screens) + 1
    preview_frames(page, screens)
    choose(page, screens.index("Settings"), "Settings")
    click(page, 356, 336)
    state = read_state(page)
    assert state["status_open"] and state["status_page"] == 0
    click(page, 240, 452)
    assert read_state(page)["status_page"] == 1
    click(page, 240, 452)
    assert read_state(page)["status_page"] == 2
    page.locator('[data-key="1"]').click()
    assert not read_state(page)["status_open"]
    click(page, 272, 36)
    state = read_state(page)
    assert not state["journal_open"]
    for _ in range(5): click(page, 420, 36)
    click(page, 272, 36)
    state = read_state(page)
    assert state["journal_open"]
    page.locator('[data-key="1"]').click()
    click(page, 356, 116)
    click(page, 240, 168)
    page.locator('[data-key="1"]').click()
    page.locator('[data-key="1"]').click()
    click(page, 356, 226)
    for _ in range(4): click(page, 240, 428)
    choose(page, screens.index("Event log"), "Event log")
    def journal():
        return read_state(page)
    assert journal()["journal_open"] and journal()["journal_count"] == 16
    box = page.locator("canvas").bounding_box()
    def point(x, y): return (box["x"] + x * box["width"] / 480, box["y"] + y * box["height"] / 480)
    page.mouse.move(*point(240, 370)); page.mouse.down()
    page.mouse.move(*point(240, 140), steps=12); page.mouse.up(); page.wait_for_timeout(200)
    assert journal()["journal_offset"] >= 2 and not journal()["journal_detail"]
    click(page, 240, 174); assert journal()["journal_detail"]
    page.locator('[data-key="1"]').click(); assert not journal()["journal_detail"]
    click(page, 240, 92); assert journal()["journal_overlay"]
    choose(page, screens.index("Agent events"), "Agent events")
    click(page, 240, 92); assert journal()["journal_count"] == 2
    choose(page, screens.index("Speaking"), "Speaking")
    click(page, 80, 428)
    state = read_state(page)
    assert not state["signal"]
    click(page, 400, 428)
    page.wait_for_function("document.querySelector('#screen').value === '-1'")
    page.locator('[data-key="1"]').click()
    page.wait_for_function("document.querySelector('#state').textContent.startsWith('Settings ·')")
    choose(page, screens.index("Home"), "Home")
    box = page.locator("canvas").bounding_box()
    page.mouse.move(box["x"] + 240, box["y"] + 180)
    page.mouse.down(); page.mouse.move(box["x"] + 300, box["y"] + 320, steps=12); page.mouse.up()
    page.wait_for_timeout(300)
    page.select_option("#character", "0")
    page.wait_for_function("document.querySelector('#screen').value === '-1'")
    choose(page, screens.index("Settings"), "Settings")
    page.set_viewport_size({"width":390,"height":844})
    page.wait_for_timeout(100)
    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
    assert not errors and not page.locator("#error").inner_text(), errors


def main():
    worker = emulator.Worker(emulator.build())
    server = emulator.HTTPServer(("127.0.0.1", 0), emulator.Handler)
    server.worker = worker
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}"
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(channel="chrome", headless=True)
            page = browser.new_page(viewport={"width":1100,"height":850})
            page.goto(url)
            exercise(page, worker.screens)
            assert page.request.post(url + "/event", data='{"command":"select","args":[999]}').status == 400
            assert page.request.post(url + "/event", data='[]').status == 400
            assert page.request.post(url + "/event", data='{"command":"select","args":[0]}', headers={"Origin":"https://example.com"}).status == 403
            assert page.request.get(url + "/../README.md").status == 404
            browser.close()
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2); worker.close()
    assert worker.process.poll() is not None
    print("emulator: all native screens, menus, guide, overlays, gestures, characters, mobile layout and local-only boundary passed")


if __name__ == "__main__": main()
