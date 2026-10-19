#!/usr/bin/env python3
"""Browser acceptance of the shared native emulator, with no attached device or agent."""
import importlib.util
import json
from pathlib import Path
import threading
import time
from unittest.mock import patch
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("kubik_emulator", ROOT / "tools/emulator.py")
emulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emulator)


def native_frame(worker, steps):
    worker.send(f"frame {steps}")
    return worker.read_frame()


def check_batches(worker, executable):
    # The reference emits every frame; the batch must still update AND render
    # each intermediate step, with byte-identical RGB and state at every sample.
    started = time.monotonic()
    reference = emulator.Worker(executable)
    samples = 0
    try:
        assert worker.screens == reference.screens
        for character in (0, 1):
            for index, name in enumerate(worker.screens):
                for process in (reference, worker):
                    process.event({"command": "character", "args": [character]})
                    process.event({"command": "select", "args": [index]})
                for steps in (7, 28, 15, 60):
                    for _ in range(steps):
                        expected = native_frame(reference, 1)
                    actual = native_frame(worker, steps)
                    assert actual == expected, (character, name, steps)
                    samples += 1
                if name.startswith("Play:"):
                    for process in (reference, worker):
                        process.event({"command": "event", "args": [2, 0, 0]})
                    for _ in range(7):
                        expected = native_frame(reference, 1)
                    assert native_frame(worker, 7) == expected, (character, name, "KEY")
                    samples += 1
        for steps in (0, 61, -1, 1.5, True):
            try:
                worker.frame(steps)
            except ValueError:
                pass
            else:
                raise AssertionError(f"Invalid step count accepted: {steps}")
        assert native_frame(worker, 1) == native_frame(reference, 1), "Invalid steps changed native state"
    finally:
        reference.close()
    worker.event({"command": "character", "args": [1]})
    print(f"emulator: {samples} exact stepped/batched RGB and state comparisons, both characters, "
          f"{time.monotonic() - started:.1f}s", flush=True)


def read_state(page):
    return page.evaluate("""async () => {
      await queue;
      const response = await fetch('/frame', {signal: AbortSignal.timeout(5000)});
      if (!response.ok) throw new Error(`State frame failed (HTTP ${response.status})`);
      return JSON.parse(response.headers.get('X-Kubik-State'));
    }""")


def forgotten_contacts_expire(worker):
    worker.event({"command": "select", "args": [worker.screens.index("Play: skies")]})
    worker.frame(60)
    worker.event({"command": "touches", "args": [1, 150, 250, 15, 1, 350, 300, 2]})
    assert 0 < worker.touch_deadline - time.monotonic() <= .35
    assert worker.frame()[0]["contacts"] == 2
    worker.touch_deadline = time.monotonic() - 1
    state, _ = worker.frame()
    assert state["contacts"] == state["sky_contacts"] == 0
    assert worker.touch_deadline == 0


def click(page, x, y):
    box = page.locator("canvas").bounding_box()
    page.mouse.click(box["x"] + x * box["width"] / 480, box["y"] + y * box["height"] / 480)


def choose(page, index, name):
    page.select_option("#screen", str(index))
    page.wait_for_function("name => document.querySelector('#state').textContent.startsWith(name + ' ·')", arg=name)


def advance_frames(page, count):
    page.evaluate("""async count => {
      running = true;
      await frame(count); clearTimeout(frameTimer);
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
            if name.startswith("Play:"):
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
        if name.startswith("Play:"):
            assert len(set(sequence)) > 1, f"Game preview must animate: {name}"


def multitouch(page, screens):
    choose(page, screens.index("Play: skies"), "Play: skies")
    page.evaluate("running = false; clearTimeout(frameTimer)")
    page.wait_for_function("!inFlight")
    advance_frames(page, 60)
    box = page.locator("canvas").bounding_box()
    def point(id, x, y):
        return {"id": id, "x": box["x"] + x * box["width"] / 480,
                "y": box["y"] + y * box["height"] / 480}
    first, second, third = point(11, 150, 250), point(29, 350, 300), point(43, 240, 200)
    session = page.context.new_cdp_session(page)
    def touch(kind, points):
        session.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": points})
        # The paused timer must not bypass the real frame's touch heartbeat. A slow CDP event can outlast
        # the server's lease even though the two accepted browser contacts are still held.
        with page.expect_response(lambda response: response.url.endswith("/frame?steps=1")) as response:
            advance_frames(page, 1)
        return json.loads(response.value.header_value("X-Kubik-State"))
    try:
        state = touch("touchStart", [first])
        assert state["contacts"] == state["sky_contacts"] == 1
        state = touch("touchStart", [second])
        assert state["contacts"] == state["sky_contacts"] == 2
        assert touch("touchStart", [third])["contacts"] == 2
        # Chromium CreateWebTouchEvents uses changed points: touchEnd names the lifted IDs.
        # See content/browser/devtools/protocol/input_handler.cc (the WebTouch path).
        page.wait_for_timeout(450)  # Longer than the 350 ms lease: ignored fingers do not send a new snapshot.
        state = touch("touchEnd", [third])
        assert state["contacts"] == state["sky_contacts"] == 2, state
        state = touch("touchEnd", [first])
        assert state["contacts"] == state["sky_contacts"] == 1
        second = point(29, 320, 330)
        assert touch("touchMove", [second])["sky_contacts"] == 1
        state = touch("touchCancel", [])
        assert state["contacts"] == state["sky_contacts"] == 0
        touch("touchStart", [first, second])
        page.evaluate("window.dispatchEvent(new Event('blur'))")
        assert read_state(page)["contacts"] == 0
        touch("touchCancel", [])
        touch("touchStart", [first, second])
        page.select_option("#screen", str(screens.index("Home")))
        assert read_state(page)["contacts"] == 0
    finally:
        session.send("Input.dispatchTouchEvent", {"type": "touchCancel", "touchPoints": []})
        session.detach()
        page.evaluate("running = true; frame()")


def exercise(page, screens):
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.wait_for_function("document.querySelector('#screen').options.length > 1")
    assert len(page.locator("#screen option").all()) == len(screens) + 1
    started = time.monotonic()
    preview_frames(page, screens)
    multitouch(page, screens)
    print(f"emulator: preview sequences {time.monotonic() - started:.1f}s", flush=True)
    started = time.monotonic()
    choose(page, screens.index("Settings"), "Settings")
    click(page, 127, 428)
    state = read_state(page)
    assert state["status_open"] and state["status_page"] == 0
    click(page, 240, 452)
    assert read_state(page)["status_page"] == 1
    click(page, 389, 452)
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
    click(page, 356, 144)
    click(page, 240, 168)
    page.locator('[data-key="1"]').click()
    page.locator('[data-key="1"]').click()
    click(page, 356, 308)
    assert read_state(page)["view"] == 2
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
    print(f"emulator: gestures and menus {time.monotonic() - started:.1f}s", flush=True)


def main():
    started = time.monotonic()
    executable = emulator.build()
    worker = emulator.Worker(executable)
    print(f"emulator: native build/start {time.monotonic() - started:.1f}s", flush=True)
    started = time.monotonic()
    with patch("socket.getfqdn", side_effect=AssertionError("Loopback startup must not resolve DNS")):
        server = emulator.LocalHTTPServer(("127.0.0.1", 0), emulator.Handler)
    assert server.server_name == "127.0.0.1" and server.server_port > 0
    print(f"emulator: HTTP bind/start {time.monotonic() - started:.3f}s", flush=True)
    server.worker = worker
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}"
    try:
        check_batches(worker, executable)
        forgotten_contacts_expire(worker)
        with sync_playwright() as p:
            browser = p.chromium.launch(channel="chrome", headless=True)
            page = browser.new_page(viewport={"width":1100,"height":850})
            page.goto(url)
            exercise(page, worker.screens)
            assert page.request.post(url + "/event", data='{"command":"select","args":[999]}').status == 400
            for args in ([1, 40, 50, 1], [1, 480, 50, 1, 0, 0, 0, 0], [1, 40, 50, 1, 1, 90, 100, 1]):
                assert page.request.post(url + "/event", data={"command": "touches", "args": args}).status == 400
            assert page.request.post(url + "/event", data='[]').status == 400
            assert page.request.post(url + "/event", data='{"command":"select","args":[0]}', headers={"Origin":"https://example.com"}).status == 403
            assert page.request.get(url + "/../README.md").status == 404
            page.evaluate("running = false; clearTimeout(frameTimer)")
            page.wait_for_function("!inFlight")
            with patch.object(worker, "frame", wraps=worker.frame) as frames:
                for query in ("steps=0", "steps=61", "steps=-1", "steps=1.5", "steps=", "steps=1&steps=2", "other=1"):
                    assert page.request.get(url + "/frame?" + query).status == 400
                assert page.request.get(url + "/frame?steps=7", headers={"Origin":"https://example.com"}).status == 403
                assert not frames.called, "Rejected HTTP requests advanced native state"
            with patch.object(worker, "frame", side_effect=ValueError("Corrupt native header")):
                assert page.request.get(url + "/frame?steps=7").status == 503
            browser.close()
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2); worker.close()
    assert worker.process.poll() is not None
    print("emulator: all native screens, menus, guide, overlays, gestures, characters, mobile layout and local-only boundary passed")


if __name__ == "__main__": main()
