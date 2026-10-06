// ---- mock LLM -------------------------------------------------------------------------------------------
const REPLIES = [
  { match: /погод/i, text: '[[happy]] Завтра обещают солнечно, около двадцати градусов тепла. [[wink]] Отличный день для прогулки!' },
  { match: /шутк|анекдот|пошути/i, text: '[[joy]] Почему программисты путают Хэллоуин и Рождество? Потому что тридцать первое октября равно двадцать пятому декабря! [[wink]] В восьмеричной системе, конечно.' },
  { match: /грустн|печаль|плохо|тоскл/i, text: '[[sad]] Ой, мне очень жаль, что тебе грустно. [[love]] Я рядом. Хочешь, расскажу что-нибудь приятное, или просто посидим вместе?' },
  { match: /напомн/i, text: '[[proud]] Договорились! Через пять минут напомню тебе выпить воды.' },
  { match: /умеешь|можешь|что ты такое/i, text: [
    '[[happy]] Ой, я умею довольно много всего! Я живу у тебя на столе, слушаю, когда ты держишь кнопку, и отвечаю голосом.',
    '[[thinking]] Могу подсказать погоду, поставить напоминание, посчитать что-нибудь в уме или просто поболтать, когда скучно. Ещё я знаю много шуток, правда не все из них смешные.',
    '[[proud]] А ещё у меня есть лицо, и оно показывает, что я чувствую: радость, удивление, даже лёгкое смущение. **Вот** так:\n- улыбаюсь\n- подмигиваю\n- краснею 😊',
    '[[wink]] Попробуй спросить меня о чём-нибудь прямо сейчас, например, какая завтра погода или сколько будет семью восемь.',
  ].join('\n\n') },
  { match: /привет|здравствуй|как дела/i, text: '[[happy]] Привет-привет! У меня всё отлично, я тут на столе тебя ждал. [[wink]] А у тебя как дела?' },
];
const DEFAULT_REPLY = '[[thinking]] Хм, интересный вопрос. [[neutral]] Давай я немного подумаю и отвечу чуть позже.';

function messageText(message) {
  if (!message) return '';
  if (typeof message.content === 'string') return message.content;
  if (Array.isArray(message.content)) return message.content.map((part) => (typeof part === 'string' ? part : part?.text ?? '')).join('\n');
  return '';
}

const INTERNAL_RE = /<<<BEGIN_OPENCLAW_INTERNAL_CONTEXT>>>[\s\S]*?(?:<<<END_OPENCLAW_INTERNAL_CONTEXT>>>|$)/g;
/** The latest thing the human said: skips OpenClaw's internal-context user blocks and the envelope timestamp. */
export function lastUserUtterance(messages = []) {
  for (const message of [...messages].reverse()) {
    if (message?.role !== 'user') continue;
    const text = messageText(message).replace(INTERNAL_RE, ' ').trim();
    if (!text) continue;
    return text.replace(/^\[[^\]\n]{0,80}\]\s*/, '').trim();
  }
  return '';
}

export function mockLlmReply(messages = []) {
  const text = lastUserUtterance(messages);
  if (/HEARTBEAT/i.test(text)) return 'HEARTBEAT_OK';
  return REPLIES.find((r) => r.match.test(text))?.text ?? DEFAULT_REPLY;
}


