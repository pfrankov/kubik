"""Native acceptance fails for both live and garbage-collected orphan tasks."""
import asyncio
import contextlib
import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'hermes_native/lifecycle.py'
spec = importlib.util.spec_from_file_location('hermes_voice_lifecycle', SOURCE)
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)


class NativeOutput:
    async def run(self):
        await asyncio.Event().wait()


class VoiceCleanupContract(unittest.IsolatedAsyncioTestCase):
    async def test_live_orphan_fails_and_joined_task_passes(self):
        audit = lifecycle.VoiceTaskAudit()
        self.addCleanup(audit.restore)
        task = asyncio.create_task(NativeOutput().run())
        try:
            with self.assertRaisesRegex(AssertionError, 'remain after Gateway stop'):
                await audit.check(timeout=.01)
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        await audit.check(timeout=.01)

    async def test_destroyed_orphan_cannot_hide_behind_empty_task_inventory(self):
        audit = lifecycle.VoiceTaskAudit()
        self.addCleanup(audit.restore)
        asyncio.create_task(NativeOutput().run())
        await asyncio.sleep(0)
        with self.assertRaisesRegex(AssertionError, 'was destroyed'):
            await audit.check(timeout=.01)
