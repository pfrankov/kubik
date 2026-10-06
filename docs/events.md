# Recent agent events

In Settings, tap the battery indicator five times within four seconds to reveal
**Events**. It is hidden again when Settings closes. Open **Events**. The journal holds the last **16 applied events** since
restart, newest first. It records connection, state, activity, emotion, reply and
notification excerpts, voice lifecycle, errors and requested settings. Identical
consecutive entries are combined with `xN`; unchanged scheduler snapshots are omitted. The time is minutes/seconds since
startup, not wall-clock time. Swipe to browse, or tap Older/Newer; tap an entry to
read its excerpt and reaction. If a message being read leaves the bounded history,
the detail view says it has expired instead of showing a different message.
BOOT returns to the list, then Settings.

Enable **Overlay on home** to see the latest event and its reaction description
on a translucent panel over Tess or Plush. Live status, clock and charging remain
visible. Text reply cards, menus, QR/setup and sleep hide the panel. The overlay is
visual only: it does not change gestures, recording, sounds or notification wake.
Its setting survives restart; the RAM history does not. The journal allocates no
memory per event, sends no requests and writes no event history to flash.

This is the device's view of delivered events, not the agent's complete server log
or tool arguments. Messages are excerpts of up to 159 UTF-8 bytes. Queue coalescing
may omit intermediate states; events from an obsolete connection are discarded.
Raw frames, pairing codes, API credentials and binary audio are not journaled;
message excerpts stay on the device and are not exposed in USB diagnostics.

To preview events without an agent, hold BOOT for Settings; tap its title five
times within four seconds, then hold the title for 0.8 seconds. In Screen Lab,
choose Background work, Reminder, Agent events or Event log. Next or KEY advances
Agent events through local examples. Demo history and its overlay setting never
replace the real journal or saved preferences.
