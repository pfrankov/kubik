import test from 'node:test';
import assert from 'node:assert/strict';
import { askUserQuestionId, isVoiceCommand, speakablePayloadText, spokenQuestion } from '../src/inbound.js';

test('voice commands: only session settings passed on verbatim are authorised', () => {
  for (const ok of ['/model', '/model openai/gpt-5.6-sol', '/think high', '/reasoning on', '/stop', '/new', ' /fast off']) assert.ok(isVoiceCommand(ok), ok);
  for (const no of ['поставь модель сол', '/exec rm -rf', '/config set', 'model /think', '/modelx', '/approve']) assert.ok(!isVoiceCommand(no), no);
});

test('an agent question is spoken without its English framing, even as a tool payload', () => {
  const payload = { text: 'Question for you:\n\nМодель\nКакую модель поставить?\n1. gpt-5.6-sol\n2. gpt-5.6-luna\nOther: reply with your own answer.\n\nReply with the number, the option text, or your own answer.',
    channelData: { askUser: { questionId: 'q_1' } } };
  assert.equal(askUserQuestionId(payload), 'q_1');
  const spoken = speakablePayloadText(payload, 'tool');
  assert.equal(spoken, 'Вопрос: Модель\nКакую модель поставить?\n1. gpt-5.6-sol\n2. gpt-5.6-luna\nИли скажи свой вариант.');
  assert.equal(spokenQuestion('Question for you:\n\nReply with your answer.'), '');
  assert.equal(speakablePayloadText({ text: 'tool output' }, 'tool'), null, 'other tool payloads stay silent');
});
