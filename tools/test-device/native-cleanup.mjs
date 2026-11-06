// An offline fixture has no primary host to restore its runtime voice capabilities.
import { restoreFixture } from './fixture-baseline.mjs';

export async function restoreNativeFixture({ offlineFixture, restoreMode, stopServer, waitForStatus,
  cancelCapture, restoreRoute, close }) {
  let closed = false;
  const closeOnce = async () => { if (!closed) { closed = true; await close(); } };
  try {
    try {
      if (offlineFixture) { await cancelCapture(); await restoreMode(); }
    } finally { await stopServer(); }
    // Keep the complete-status guard before any endpoint restoration writes.
    await waitForStatus();
    try { await cancelCapture(); }
    finally { await restoreFixture({ offlineFixture, close: closeOnce, restore: restoreRoute }); }
  } finally { await closeOnce(); }
}

/** Menu input is ignored during explicit PWR sleep; restore it before returning to darkness. */
export async function restoreNativeScreen(original, { info, sim, until }) {
  let state = await info();
  if (state.menu !== original.menu) {
    if (state.screen_dark) {
      await sim('pwr');
      await until(value => !value.screen_dark, 'awake before restoring menu');
    }
    await sim('menu');
    await until(value => value.menu === original.menu, 'original menu restored');
  }
  state = await info();
  if (state.screen_dark !== original.screen_dark) await sim('pwr');
  return until(value => value.screen_dark === original.screen_dark && value.menu === original.menu,
    'original screen and menu restored');
}

export async function cleanupPreservingFailure(primary, cleanup, report = error => console.error(`CLEANUP_FAILED ${error.message}`)) {
  try { await cleanup(); }
  catch (error) {
    if (!primary) throw error;
    report(error);
  }
}
