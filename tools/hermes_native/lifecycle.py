"""Make orphaned Kubik voice tasks fail native Gateway acceptance."""
import asyncio
import gc


VOICE_TASKS = ('NativeVoice.', 'NativeOutput.', 'NativePlayback.',
               'VoiceProvider.', 'LiveProvider.', 'RealtimeProvider.')


def voice_task_name(task):
    coroutine = getattr(task, 'get_coro', None)
    name = getattr(coroutine(), '__qualname__', '') if coroutine else ''
    return name if name.startswith(VOICE_TASKS) else ''


class VoiceTaskAudit:
    def __init__(self):
        self.loop = asyncio.get_running_loop()
        self.previous = self.loop.get_exception_handler()
        self.failures = []
        self.loop.set_exception_handler(self.handle)

    def handle(self, loop, context):
        name = voice_task_name(context.get('task') or context.get('future'))
        if name:
            self.failures.append(name)
        elif self.previous:
            self.previous(loop, context)
        else:
            loop.default_exception_handler(context)

    async def check(self, timeout=2):
        deadline = self.loop.time() + timeout
        while True:
            gc.collect()
            pending = [voice_task_name(task) for task in asyncio.all_tasks()
                       if voice_task_name(task) and not task.done()]
            if not pending or self.loop.time() >= deadline:
                break
            await asyncio.sleep(.01)
        assert not self.failures, f'Kubik voice task failed or was destroyed: {self.failures}'
        assert not pending, f'Kubik voice tasks remain after Gateway stop: {pending}'

    def restore(self):
        self.loop.set_exception_handler(self.previous)
