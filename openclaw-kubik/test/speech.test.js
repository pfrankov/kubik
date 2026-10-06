import assert from 'node:assert/strict';
import test from 'node:test';
import { EMOTIONS, chunkSpeech, emotionOf, parseReply, sanitizeForDisplay, sanitizeForSpeech, speakableText, splitShown } from '../src/speech.js';

test('emotion list matches the device contract', () => {
  assert.deepEqual([...EMOTIONS], ['neutral', 'happy', 'joy', 'love', 'sad', 'angry', 'surprised', 'confused',
    'sleepy', 'thinking', 'wink', 'shy', 'proud']);
});

test('emotionOf accepts allowed names case-insensitively and with a prefix', () => {
  assert.equal(emotionOf('happy'), 'happy');
  assert.equal(emotionOf(' Happy '), 'happy');
  assert.equal(emotionOf('emotion: sad'), 'sad');
  assert.equal(emotionOf('e=wink'), 'wink');
  assert.equal(emotionOf('furious'), null);
  assert.equal(emotionOf('reply_to_current'), null);
});

test('parseReply splits segments at emotion tags', () => {
  const { segments, pendingEmotion } = parseReply('[[happy]] Привет! [[sad]] Но мне грустно.');
  assert.deepEqual(segments, [{ emotion: 'happy', text: 'Привет!' }, { emotion: 'sad', text: 'Но мне грустно.' }]);
  assert.equal(pendingEmotion, null);
});

test('parseReply: text before the first tag has no emotion; untagged text joins the previous segment', () => {
  assert.deepEqual(parseReply('Смотри. [[wink]] Вот так. И ещё.').segments,
    [{ emotion: null, text: 'Смотри.' }, { emotion: 'wink', text: 'Вот так. И ещё.' }]);
  assert.deepEqual(parseReply('Раз. Два.').segments, [{ emotion: null, text: 'Раз. Два.' }]);
});

test('parseReply merges repeated identical emotions and keeps the last of adjacent tags', () => {
  assert.deepEqual(parseReply('[[happy]] Раз. [[happy]] Два.').segments, [{ emotion: 'happy', text: 'Раз. Два.' }]);
  assert.deepEqual(parseReply('[[happy]][[sad]] Ой.').segments, [{ emotion: 'sad', text: 'Ой.' }]);
});

test('parseReply drops unknown and core directives without creating events', () => {
  const { segments } = parseReply('[[reply_to_current]] [[furious]] Привет [[audio_as_voice]] мир');
  assert.deepEqual(segments, [{ emotion: null, text: 'Привет мир.' }]);
});

test('parseReply carries a trailing tag to the next block', () => {
  const first = parseReply('Первый блок. [[surprised]]');
  assert.equal(first.pendingEmotion, 'surprised');
  assert.deepEqual(first.segments, [{ emotion: null, text: 'Первый блок.' }]);
  const second = parseReply('Второй блок.', { carry: first.pendingEmotion });
  assert.deepEqual(second.segments, [{ emotion: 'surprised', text: 'Второй блок.' }]);
  assert.equal(second.pendingEmotion, null);
});

test('parseReply: tag-only, emoji-only and markdown-only replies are not spoken', () => {
  assert.deepEqual(parseReply('[[happy]]').segments, []);
  assert.deepEqual(parseReply('😊🎉').segments, []);
  assert.deepEqual(parseReply('```js\nconsole.log(1)\n```').segments, []);
  assert.deepEqual(parseReply('---').segments, []);
  assert.deepEqual(parseReply('').segments, []);
  assert.deepEqual(parseReply(null).segments, []);
});

test('tags spanning lines or brackets inside text are not treated as tags', () => {
  assert.equal(speakableText('Массив [[1, 2]] пуст'), 'Массив пуст.');
});

test('sanitizeForSpeech removes markdown emphasis, headings, quotes and list markers', () => {
  assert.equal(sanitizeForSpeech('# Заголовок\n**Жирный** и *курсив* и __ещё__ и ~~зачёркнутый~~'),
    'Заголовок. Жирный и курсив и ещё и зачёркнутый.');
  assert.equal(sanitizeForSpeech('> Цитата'), 'Цитата.');
  assert.equal(sanitizeForSpeech('Список:\n- молоко\n- хлеб\n1. раз\n2) два\n* [x] готово'),
    'Список: молоко. хлеб. раз. два. готово.');
});

test('sanitizeForSpeech removes URLs, e-mails and code but keeps link labels', () => {
  assert.equal(sanitizeForSpeech('Смотри https://example.com/a?b=1 и www.test.ru тоже'), 'Смотри и тоже.');
  assert.equal(sanitizeForSpeech('Читай [документацию](https://docs.openclaw.ai) сегодня'), 'Читай документацию сегодня.');
  assert.equal(sanitizeForSpeech('Картинка ![кот](http://x/cat.png) тут'), 'Картинка тут.');
  assert.equal(sanitizeForSpeech('Пиши на me@example.com, ок?'), 'Пиши на, ок?');
  assert.equal(sanitizeForSpeech('Код:\n```\nrm -rf /\n```\nГотово.'), 'Код: Готово.');
  assert.equal(sanitizeForSpeech('Запусти `npm test` сейчас'), 'Запусти npm test сейчас.');
  assert.equal(sanitizeForSpeech('<b>жирный</b> текст'), 'жирный текст.');
});

test('sanitizeForSpeech removes emoji, ZWJ sequences, flags and shortcodes', () => {
  assert.equal(sanitizeForSpeech('Привет 👋🏽! Семья 👨‍👩‍👧 и флаг 🇷🇺 :smile:'), 'Привет! Семья и флаг.');
  assert.equal(sanitizeForSpeech('Сердце ❤️ тут'), 'Сердце тут.');
});

test('sanitizeForSpeech flattens tables', () => {
  assert.equal(sanitizeForSpeech('| Город | Погода |\n|---|:---:|\n| Москва | +20 |'), 'Город, Погода. Москва, +20.');
});

test('sanitizeForSpeech keeps normal punctuation, numbers and does not double periods', () => {
  assert.equal(sanitizeForSpeech('Сейчас 23 градуса, ясно.'), 'Сейчас 23 градуса, ясно.');
  assert.equal(sanitizeForSpeech('Что? Правда!'), 'Что? Правда!');
  assert.equal(sanitizeForSpeech('Первая строка\nвторая строка.'), 'Первая строка. вторая строка.');
  assert.equal(sanitizeForSpeech('snake_case_name остаётся'), 'snake_case_name остаётся.');
  assert.equal(sanitizeForSpeech('2 * 3 = 6'), '2 3 = 6.');
});

test('parseReply sanitises each segment independently', () => {
  assert.deepEqual(parseReply('[[proud]] **Готово!** 🎉 Ссылка: https://x.y\n[[wink]] - пункт').segments,
    [{ emotion: 'proud', text: 'Готово! Ссылка:' }, { emotion: 'wink', text: 'пункт.' }]);
});

test('splitShown separates [[show]] blocks and carries an unclosed one over', () => {
  const norm = (r) => ({ ...r, speech: r.speech.replace(/\s+/g, ' ').trim() });
  assert.deepEqual(norm(splitShown('Код: [[show]]AB-12[[/show]] готово')), { speech: 'Код: готово', shown: ['AB-12'], inShow: false });
  assert.deepEqual(norm(splitShown('Смотри [[ SHOW ]]начало')), { speech: 'Смотри', shown: ['начало'], inShow: true });
  assert.deepEqual(norm(splitShown('конец[[/show]] и всё', { inShow: true })), { speech: 'и всё', shown: ['конец'], inShow: false });
});

test('sanitizeForDisplay keeps links and numbers, turns lists into bullets and caps the size', () => {
  assert.equal(sanitizeForDisplay('## Итог\n- **раз** 🙂\n- [сайт](https://a.b/c)\n\n\n| a | b |\n|---|---|'),
    'Итог\n• раз\n• сайт (https://a.b/c)\n\na · b');
  assert.equal(sanitizeForDisplay('🙂 ***'), '');
  const long = sanitizeForDisplay('слово '.repeat(400));
  assert.ok(Buffer.byteLength(long) <= 1000 && long.endsWith('…'));
});

test('chunkSpeech: short first request, sentence boundaries, decimals kept', () => {
  assert.deepEqual(chunkSpeech('Да. Будет 23.5 градуса, тепло и сухо. Возьми очки.', { first: true }),
    ['Да. Будет 23.5 градуса, тепло и сухо.', 'Возьми очки.']);
  assert.deepEqual(chunkSpeech('Раз. Два. Три.'), ['Раз. Два. Три.']);
  const [head] = chunkSpeech(`${'очень '.repeat(20)}длинное вступление, а потом ещё продолжение ${'и '.repeat(40)}конец.`, { first: true });
  assert.ok(head.length <= 160 && head.endsWith(','));
});
