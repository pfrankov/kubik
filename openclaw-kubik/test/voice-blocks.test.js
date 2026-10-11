import assert from 'node:assert/strict';
import test from 'node:test';
import { VoiceBlocks, joinVoiceBlock } from '../src/voice-blocks.js';
import { parseReply, splitShown } from '../src/speech.js';

test('voice blocks keep split fenced code silent, then speak the following prose', () => {
  for (const fence of ['```', '~~~']) {
    const blocks = new VoiceBlocks();
    assert.deepEqual(blocks.push(`${fence}py\nprint(`), []);
    assert.deepEqual(blocks.push('secret()'), []);
    assert.deepEqual(blocks.push(`\n${fence}\nDone.`), [{ emotion:null, text:'Done.' }]);
    assert.deepEqual(blocks.push('', true), []);
  }
});

test('voice blocks stream plain text immediately and preserve screen-only and emotion state', () => {
  const blocks = new VoiceBlocks();
  assert.deepEqual(blocks.push('[[happy]]Hello. [[show]]Private'), [{ emotion:'happy', text:'Hello.' }]);
  assert.deepEqual(blocks.push('details[[/show]] [[sad]]'), []);
  assert.deepEqual(blocks.push('Goodbye.'), [{ emotion:'sad', text:'Goodbye.' }]);
});

test('unfinished code is suppressed at dispatch completion and inline markup waits for closure', () => {
  const code = new VoiceBlocks();
  assert.deepEqual(code.push('```py\nsecret()'), []);
  assert.deepEqual(code.push('', true), []);
  const inline = new VoiceBlocks();
  assert.deepEqual(inline.push('Use `the'), []);
  assert.deepEqual(inline.push('command`.'), parseReply('Use `the\ncommand`.').segments);
});

test('every split of screen-only delimiters keeps private text out of speech and preserves the final card', () => {
  for (const tag of ['[[show]]', '[[/show]]', '[[ SHOW ]]', '[[ / show ]]']) {
    for (let cut = 1; cut < tag.length; cut++) {
      const closing = tag.includes('/');
      const parts = closing ? ['[[show]]Private' + tag.slice(0, cut), tag.slice(cut) + ' Visible.']
        : ['Hello. ' + tag.slice(0, cut), tag.slice(cut) + 'Private[[/show]] Visible.'];
      const blocks = new VoiceBlocks();
      const spoken = [...blocks.push(parts[0]), ...blocks.push(parts[1]), ...blocks.push('', true)];
      assert.doesNotMatch(spoken.map(s => s.text).join(' '), /Private|show/i, `${tag} at ${cut}`);
      assert.match(spoken.map(s => s.text).join(' '), /Visible\./);
      const answer = parts.reduce(joinVoiceBlock, '');
      assert.deepEqual(splitShown(answer).shown, ['Private']);
    }
  }
});

test('comparison prose streams immediately while split HTML tags wait for closure', () => {
  const prose = new VoiceBlocks();
  assert.deepEqual(prose.push('2 < 3.'), parseReply('2 < 3.').segments);
  assert.deepEqual(prose.push('Next sentence.'), parseReply('Next sentence.').segments);
  const markup = new VoiceBlocks();
  assert.deepEqual(markup.push('<span title="'), []);
  assert.deepEqual(markup.push('label">Hello.</span>'), parseReply('<span title="\nlabel">Hello.</span>').segments);
});
