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
