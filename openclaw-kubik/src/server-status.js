import { out } from './protocol.js';

export function sendSessionCron(session, cron, always = false) {
  const summary = cron?.summary();
  if (summary && (always || summary.running || summary.next >= 0)) session.send(out.cron(summary.running, summary.next));
}

export function refreshSessionActivity(session, activity, typing) {
  let own = '', other = '';
  if (activity) {
    let key;
    try { key = activity.sessionKeyFor(session.device.id); } catch { key = undefined; }
    ({ own, other } = activity.summary(key));
  }
  const until = typing.get(session.device.id);
  if (until && until < Date.now()) typing.delete(session.device.id);
  else if (until && !own) { own = other || 'thinking'; other = ''; }
  session.setActivity({ own, other });
}
