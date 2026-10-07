#!/usr/bin/env python3
"""Screen-power and talk-turn tables (app_state.c) and Tess's mood machine (mood.c) on the host: every state x event pair."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
board = (ROOT / "firmware/main/board.c").read_text()
assert "axp_write(0x27, 0x23);" in board, "off state must power on only after a 2 s PWR hold"
with tempfile.TemporaryDirectory(prefix="kubik-state-") as tmp:
    exe = str(Path(tmp) / "state-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined",
        "-Ifirmware/main", "firmware/sim/state_test.c", "firmware/main/app_state.c", "-o", exe,
    ], cwd=ROOT, check=True)
    subprocess.run([exe], cwd=ROOT, check=True)
    # Tess's mood machine (mood.c): its table, priorities, decay back to CALM, gates, blend
    mood_exe = str(Path(tmp) / "mood-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-fsanitize=address,undefined",
        "-Ifirmware/main", "firmware/sim/mood_test.c", "firmware/main/mood.c", "-lm", "-o", mood_exe,
    ], cwd=ROOT, check=True)
    subprocess.run([mood_exe], cwd=ROOT, check=True)

    mailbox_exe = str(Path(tmp) / "mailbox-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ifirmware/main", "firmware/sim/app_mailbox_test.c",
        "firmware/main/app_mailbox.c", "-o", mailbox_exe,
    ], cwd=ROOT, check=True)
    subprocess.run([mailbox_exe], cwd=ROOT, check=True)

    # Run the production idle policy as well as the transition table.
    runtime = (ROOT / "firmware/main/app_runtime.c").read_text()
    policy = runtime[runtime.index("static bool activity_pending("):runtime.index("static power_network_inputs_t network_sleep_inputs(")]
    policy += runtime[runtime.index("static void tick_sleep("):runtime.index("// The app task's tick:")]
    events = (ROOT / "firmware/main/app_events.c").read_text()
    policy += events[events.index("static void handle_cron_event("):events.index("static void handle_welcome_event(")]
    fixture = (ROOT / "firmware/sim/app_idle_test.c").read_text().replace("/* PRODUCTION_POLICY */", policy)
    idle_c = Path(tmp) / "idle.c"
    idle_c.write_text(fixture)
    idle_exe = str(Path(tmp) / "idle-test")
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Ifirmware/main", str(idle_c), "firmware/main/app_state.c", "-o", idle_exe,
    ], cwd=ROOT, check=True)
    subprocess.run([idle_exe], cwd=ROOT, check=True)

    welcome = events[events.index("static void handle_welcome_event("):events.index("static void handle_state_event(")]
    welcome_c = Path(tmp) / "welcome.c"
    welcome_c.write_text((ROOT / "firmware/sim/app_welcome_test.c").read_text().replace("/* PRODUCTION_WELCOME */", welcome))
    welcome_exe = str(Path(tmp) / "welcome-test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(welcome_c), "-o", welcome_exe], cwd=ROOT, check=True)
    subprocess.run([welcome_exe], cwd=ROOT, check=True)

    app = (ROOT / "firmware/main/app.c").read_text()
    header = (ROOT / "firmware/main/app.h").read_text()
    event_enum = header[header.index("typedef enum {"):header.index("} app_ev_type_t;") + len("} app_ev_type_t;")]
    post = app[app.index("static int mailbox_slot("):app.index("static bool take_critical_event(")]
    take = app[app.index("static bool take_critical_event("):app.index("// ------------------------------------------------------------------ state")]
    task = app[app.index("static void app_task("):app.index("void app_start(")]
    task_drain = task[task.index("for (int n = 0; n < 8 && xQueueReceive(s_q, &e, 0);"):task.index("        app_tick();")]
    link_event = events[events.index("static bool handle_link_event("):events.index("static void handle_cron_event(")]
    welcome = events[events.index("static void handle_welcome_event("):events.index("static void handle_state_event(")]
    remote = events[events.index("static bool remote_current("):events.index("static void handle_speak_cancel_event(")]
    server = events[events.index("static bool handle_server_event("):events.index("static void handle_agent_capabilities(")]
    dispatch = events[events.index("void app_handle("):]
    post_fixture = (ROOT / "firmware/sim/app_post_queue_test.c").read_text()
    replacements = {
        "/* PRODUCTION_EVENTS */": event_enum,
        "/* PRODUCTION_POST */": post,
        "/* PRODUCTION_TAKE */": take,
        "/* PRODUCTION_LINK_HANDLER */": link_event,
        "/* PRODUCTION_WELCOME_HANDLER */": welcome,
        "/* PRODUCTION_REMOTE_CHECK */": remote,
        "/* PRODUCTION_SERVER_HANDLER */": server,
        "/* PRODUCTION_DISPATCH */": dispatch,
        "/* PRODUCTION_TASK_DRAIN */": task_drain.strip(),
    }
    for marker, source in replacements.items():
        assert marker in post_fixture, f"missing fixture marker: {marker}"
        post_fixture = post_fixture.replace(marker, source)
    post_c = Path(tmp) / "post-queue.c"
    post_c.write_text(post_fixture)
    post_exe = str(Path(tmp) / "post-queue-test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(post_c),
                    "firmware/main/app_mailbox.c", "-o", post_exe], cwd=ROOT, check=True)
    subprocess.run([post_exe], cwd=ROOT, check=True)

    dispatch = events[events.index("static bool dark_input_ignored("):]
    dark_c = Path(tmp) / "dark.c"
    dark_c.write_text((ROOT / "firmware/sim/app_dark_input_test.c").read_text().replace("/* PRODUCTION_EVENTS */", event_enum).replace("/* PRODUCTION_DISPATCH */", dispatch).replace("/* PRODUCTION_TOUCH */", events[events.index("static bool is_touch_event("):events.index("static void handle_link_down(")]))
    dark_exe = str(Path(tmp) / "dark-test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(dark_c), "firmware/main/app_state.c", "-o", dark_exe], cwd=ROOT, check=True)
    subprocess.run([dark_exe], cwd=ROOT, check=True)

    replies = events[events.index("static void handle_state_event("):events.index("static void handle_speak_end_event(")]
    replies_c = Path(tmp) / "replies.c"
    replies_c.write_text((ROOT / "firmware/sim/app_reply_wake_test.c").read_text().replace("/* PRODUCTION_REPLIES */", replies))
    replies_exe = str(Path(tmp) / "replies-test")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Ifirmware/main", str(replies_c), "firmware/main/app_state.c", "-o", replies_exe], cwd=ROOT, check=True)
    subprocess.run([replies_exe], cwd=ROOT, check=True)
