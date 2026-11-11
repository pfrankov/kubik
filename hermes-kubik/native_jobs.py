"""Accepted Hermes work outlives voice delivery; its output remains connection-bound."""
import asyncio
from contextvars import ContextVar

from gateway.platforms.base import SendResult
from .runtime import profile_scope

CURRENT_JOB = ContextVar('kubik_native_job', default=None)


def active_job(adapter, identity, message_id=None):
    job = CURRENT_JOB.get()
    if job and job.owner.adapter is adapter and job.peer.identity == identity:
        return job
    candidate = adapter.native_jobs.jobs.get(message_id)
    return candidate if candidate and candidate.peer.identity == identity else None


class NativeJobs:
    def __init__(self, adapter):
        self.adapter = adapter
        self.jobs = {}

    def busy(self, identity):
        return any(job.peer.identity == identity for job in self.jobs.values())

    def submit(self, voice, text, delegation_id=None):
        if not isinstance(text, str) or not text.strip() or len(text) > 8192:
            raise ValueError('Invalid native transcript')
        pending = [job for job in self.jobs.values() if job.peer.identity == voice.peer.identity]
        if len(pending) >= 2 or len(self.jobs) >= 16:
            raise ValueError('Native agent queue full')
        event = self.adapter._authorized_voice_event(voice.peer, text)
        if event is None or not voice.current():
            raise ValueError('Native voice authorization changed')
        job = NativeJob(self, voice, event, delegation_id, pending[-1] if pending else None)
        self.jobs[event.message_id] = job
        job.task = asyncio.create_task(job.run())
        return job

    def complete(self, event):
        job = self.jobs.get(event.message_id)
        if job and job.peer.identity == event.source.chat_id:
            job.complete.set()
            if job.task.done():
                self.jobs.pop(event.message_id, None)


class NativeJob:
    def __init__(self, owner, voice, event, delegation_id, previous):
        self.owner, self.voice, self.event = owner, voice, event
        self.peer = voice.peer
        self.delegation_id, self.previous = delegation_id, previous
        self.complete = asyncio.Event()
        self.accepted = False
        self.text_size = self.chunks = 0
        self.task = None

    async def run(self):
        token = CURRENT_JOB.set(self)
        try:
            async with asyncio.timeout(240):
                if self.previous:
                    await self.previous.complete.wait()
                if not self.voice.current():
                    return
                with profile_scope(self.owner.adapter.home):
                    await self.owner.adapter._handle_bound_message(self.peer, self.event)
                    self.accepted = bool(getattr(self.event, '_gateway_accepted', False))
                    if not self.accepted:
                        raise ValueError('Hermes rejected native voice input')
                    await self.complete.wait()
                if self.voice.current():
                    await self.voice.reply('', self.delegation_id, final=True)
        except Exception:
            await self.voice.fail('agent_failed')
        finally:
            CURRENT_JOB.reset(token)
            # Never cancel native Gateway tasks or forget an accepted in-flight job.
            if not self.accepted or self.complete.is_set():
                self.complete.set()
                self.owner.jobs.pop(self.event.message_id, None)

    async def deliver(self, content):
        if not self.voice.current():
            return SendResult(success=True, message_id='voice-detached')
        text = str(content)
        self.text_size += len(text)
        self.chunks += 1
        if self.text_size > 8192 or self.chunks > 128:
            await self.voice.fail('voice_reply_limit')
            return SendResult(success=True, message_id='voice-detached')
        try:
            await self.voice.reply(text, self.delegation_id)
            return SendResult(success=True, message_id=self.event.message_id)
        except Exception:
            await self.voice.fail('voice_failed')
            return SendResult(success=True, message_id='voice-detached')
