/**
 * StackChan notification relay (Google Apps Script)
 *
 * Collects new Gmail messages and upcoming Google Calendar events and hands them to StackChan,
 * which polls this web app every few minutes and reads them aloud with VOICEVOX.
 *
 * Setup:
 *   1. Paste this file into a new project on https://script.google.com/
 *   2. Run setup() once (authorize Gmail / Calendar access). The log shows the token.
 *   3. Deploy > New deployment > Web app, Execute as: Me, Who has access: Anyone
 *   4. Put "<web app URL>?token=<token>" into the StackChan settings page (Notifications)
 *
 * Response: {"ok":true,"items":[{"type":"gmail"|"calendar","text":"<to speak>","bubble":"<short text>"}]}
 */

// ------------------------------- Settings -------------------------------

// What to read out: 'subject' = sender + subject, 'count' = only "you have N new mails"
const READ_MODE = 'subject';
// Gmail messages to watch (Gmail search syntax)
const GMAIL_QUERY = 'is:unread in:inbox newer_than:1d';
// Announce each calendar event this many minutes before it starts
const CALENDAR_LEAD_MINUTES = 10;
// Calendars to watch ('primary' = your main calendar; add calendar IDs to watch more)
const CALENDAR_IDS = ['primary'];
// Read out at most this many mails per poll; the rest are summarized as "N more"
const MAX_MAILS_PER_POLL = 3;

// ------------------------------------------------------------------------

const PROP_TOKEN = 'TOKEN';
const PROP_SEEN_MAILS = 'SEEN_MAILS';
const PROP_SEEN_EVENTS = 'SEEN_EVENTS';
const PROP_STARTED = 'STARTED_AT';

function setup() {
  const props = PropertiesService.getScriptProperties();
  let token = props.getProperty(PROP_TOKEN);
  if (!token) {
    token = Utilities.getUuid().replace(/-/g, '');
    props.setProperty(PROP_TOKEN, token);
  }
  // Mails that arrived before setup are not announced
  props.setProperty(PROP_STARTED, String(Date.now()));
  props.deleteProperty(PROP_SEEN_MAILS);
  props.deleteProperty(PROP_SEEN_EVENTS);
  // Touch the services so the authorization prompt covers them
  GmailApp.getInboxUnreadCount();
  CalendarApp.getDefaultCalendar();
  Logger.log('token: ' + token);
  Logger.log('StackChan URL: <web app URL>?token=' + token);
}

function doGet(e) {
  const props = PropertiesService.getScriptProperties();
  const token = props.getProperty(PROP_TOKEN);
  if (!token || !e || !e.parameter || e.parameter.token !== token) {
    return json({ ok: false, error: 'unauthorized' });
  }

  // One poll at a time so two requests do not announce the same item twice
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(20000)) {
    return json({ ok: true, items: [] });
  }
  try {
    const items = collectMails(props).concat(collectEvents(props));
    return json({ ok: true, items: items });
  } catch (err) {
    return json({ ok: false, error: String(err) });
  } finally {
    lock.releaseLock();
  }
}

// ---------------------------------- Gmail ---------------------------------

function collectMails(props) {
  const startedAt = Number(props.getProperty(PROP_STARTED) || 0);
  const seen = loadList(props, PROP_SEEN_MAILS);
  const fresh = [];

  GmailApp.search(GMAIL_QUERY, 0, 30).forEach(thread => {
    thread.getMessages().forEach(message => {
      if (!message.isUnread() || message.getDate().getTime() < startedAt) return;
      const id = message.getId();
      if (seen.indexOf(id) >= 0) return;
      seen.push(id);
      fresh.push(message);
    });
  });
  saveList(props, PROP_SEEN_MAILS, seen, 300);
  if (fresh.length === 0) return [];

  fresh.sort((a, b) => a.getDate() - b.getDate());

  if (READ_MODE === 'count') {
    return [{ type: 'gmail', text: '新しいメールが' + fresh.length + '件届いたよ。', bubble: 'メール ' + fresh.length + '件' }];
  }

  const items = fresh.slice(0, MAX_MAILS_PER_POLL).map(message => {
    const sender = senderName(message.getFrom());
    const subject = clip(message.getSubject() || '件名なし', 40);
    return {
      type: 'gmail',
      text: sender + 'さんからメール。' + subject,
      bubble: sender + '：' + subject,
    };
  });
  const rest = fresh.length - items.length;
  if (rest > 0) {
    items.push({ type: 'gmail', text: 'ほかにも' + rest + '件メールが届いているよ。', bubble: 'ほか ' + rest + '件' });
  }
  return items;
}

// "Taro Tanaka <taro@example.com>" -> "Taro Tanaka", "taro@example.com" -> "taro"
function senderName(from) {
  const match = from.match(/^\s*"?([^"<]*?)"?\s*<([^>]+)>/);
  let name = match ? match[1].trim() : '';
  if (!name) {
    const address = match ? match[2] : from;
    name = address.split('@')[0];
  }
  return clip(name, 20);
}

// --------------------------------- Calendar --------------------------------

function collectEvents(props) {
  const now = new Date();
  const until = new Date(now.getTime() + CALENDAR_LEAD_MINUTES * 60 * 1000);
  const seen = loadList(props, PROP_SEEN_EVENTS);
  const items = [];

  CALENDAR_IDS.forEach(calendarId => {
    const calendar = calendarId === 'primary' ? CalendarApp.getDefaultCalendar() : CalendarApp.getCalendarById(calendarId);
    if (!calendar) return;
    calendar.getEvents(now, until).forEach(event => {
      if (event.isAllDayEvent()) return;
      const start = event.getStartTime();
      if (start < now) return;  // already started
      if (event.getMyStatus && event.getMyStatus() === CalendarApp.GuestStatus.NO) return;  // declined
      const key = event.getId() + '@' + start.getTime();
      if (seen.indexOf(key) >= 0) return;
      seen.push(key);

      const minutes = Math.max(1, Math.round((start - now) / 60000));
      const title = clip(event.getTitle() || '予定', 30);
      const time = Utilities.formatDate(start, Session.getScriptTimeZone(), 'H:mm');
      items.push({
        type: 'calendar',
        text: READ_MODE === 'count' ? minutes + '分後に予定があるよ。' : minutes + '分後に、' + title + 'が始まるよ。',
        bubble: time + ' ' + (READ_MODE === 'count' ? '予定' : title),
      });
    });
  });
  saveList(props, PROP_SEEN_EVENTS, seen, 100);
  return items;
}

// ---------------------------------- Utils ---------------------------------

function loadList(props, key) {
  try {
    return JSON.parse(props.getProperty(key) || '[]');
  } catch (e) {
    return [];
  }
}

// Keep only the newest entries so the property stays under the 9KB limit
function saveList(props, key, list, max) {
  props.setProperty(key, JSON.stringify(list.slice(-max)));
}

function clip(text, max) {
  text = String(text).replace(/\s+/g, ' ').trim();
  return text.length > max ? text.slice(0, max) + '…' : text;
}

function json(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj)).setMimeType(ContentService.MimeType.JSON);
}
