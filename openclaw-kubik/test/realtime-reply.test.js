import assert from 'node:assert/strict';
import test from 'node:test';
import { RealtimeReply } from '../src/realtime-reply.js';

test('Realtime keeps the first block immediate and batches the waiting plain text without crossing an emotion', async () => {
  const messages = [], emotions = []; let done = 0;
  const reply = new RealtimeReply({ send:text => messages.push(text), emotion:e => emotions.push(e),
    finish:() => done++, fail:() => assert.fail('Unexpected provider failure') });
  reply.enqueue([{text:'First.', emotion:null}]);
  reply.enqueue([{text:'Second.', emotion:null}]); reply.enqueue([{text:'Third.', emotion:null}]);
  reply.enqueue([{text:'Fourth.', emotion:'happy'}]); reply.end();
  assert.equal(messages.length, 1); assert.match(messages[0], /First\./);
  reply.completed(); await Promise.resolve();
  assert.equal(messages.length, 2); assert.match(messages[1], /Second\. Third\./);
  assert.doesNotMatch(messages[1], /Fourth/); assert.deepEqual(emotions, []);
  reply.completed(); await Promise.resolve(); assert.deepEqual(emotions, ['happy']);
  reply.completed(); await Promise.resolve(); assert.equal(done, 1);
});

test('Realtime batches remain bounded when the agent produces a long answer', async () => {
  const messages = [];
  const reply = new RealtimeReply({ send:text => messages.push(text), finish:() => {}, fail:() => assert.fail() });
  reply.enqueue([{text:'First.', emotion:null}]);
  for (let i = 0; i < 5; i++) reply.enqueue([{text:String(i).repeat(300), emotion:null}]);
  reply.end();
  reply.completed(); await Promise.resolve();
  assert.match(messages[1], /0{300} 1{300}/); assert.doesNotMatch(messages[1], /2{300}/);
});
