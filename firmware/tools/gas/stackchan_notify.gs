/**
 * StackChan notification relay (Google Apps Script)
 *
 * Collects new Gmail messages, upcoming Google Calendar events and new Google Chat messages and hands
 * them to StackChan, which polls this web app every few minutes and reads them aloud with VOICEVOX.
 *
 * Setup:
 *   1. Paste this file into a new project on https://script.google.com/
 *   2. Run setup() once (authorize Gmail / Calendar access). The log shows the token.
 *   3. Deploy > New deployment > Web app, Execute as: Me, Who has access: Anyone
 *   4. Put "<web app URL>?token=<token>" into the StackChan settings page (Notifications)
 *
 * Google Chat (optional) needs a standard Cloud project with the Chat API enabled and the Chat app
 * configured, linked in Project Settings, plus the "Google Chat API" service (ID: Chat). Add the
 * "People API" service (ID: People, also enabled in the Cloud project) to read out sender names.
 *
 * Response: {"ok":true,"items":[{"type":"gmail"|"calendar"|"chat","text":"<to speak>","bubble":"<short text>"}]}
 *           chat items add "notice": the space name (space posts) or the sender (DMs), no body, spoken when the device is set not to read chat bodies
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
// Google Chat: 'dm_and_mentions' = direct / group messages and @mentions in spaces, 'all' = every message
const CHAT_MODE = 'dm_and_mentions';
// Read out at most this many chat messages per poll
const MAX_CHATS_PER_POLL = 3;

// ------------------------------------------------------------------------

// Returned in every response so you can tell which code the deployment runs (open the URL in a browser)
const VERSION = '2026-10-20a';

const PROP_TOKEN = 'TOKEN';
const PROP_SEEN_MAILS = 'SEEN_MAILS';
const PROP_SEEN_EVENTS = 'SEEN_EVENTS';
const PROP_STARTED = 'STARTED_AT';
const PROP_SEEN_CHATS = 'SEEN_CHATS';
const PROP_CHAT_CHECKED = 'CHAT_CHECKED_AT';
const PROP_CHAT_SELF = 'CHAT_SELF';
const PROP_CHAT_NAMES = 'CHAT_NAMES';

function setup() {
  const props = PropertiesService.getScriptProperties();
  let token = props.getProperty(PROP_TOKEN);
  if (!token) {
    token = Utilities.getUuid().replace(/-/g, '');
    props.setProperty(PROP_TOKEN, token);
  }
  // Items from before setup are not announced
  props.setProperty(PROP_STARTED, String(Date.now()));
  [PROP_SEEN_MAILS, PROP_SEEN_EVENTS, PROP_SEEN_CHATS, PROP_CHAT_CHECKED, PROP_CHAT_SELF].forEach(key => props.deleteProperty(key));
  // Touch the services so the authorization prompt covers them
  GmailApp.getInboxUnreadCount();
  CalendarApp.getDefaultCalendar();
  if (chatAvailable()) {
    Chat.Spaces.list({ pageSize: 1 });
    Logger.log('Google Chat: enabled (you are ' + (chatSelf(props) || 'unknown') + ')');
    Logger.log('Sender names: ' + (typeof People !== 'undefined' ? 'People API' : 'not available (add the People API service)'));
  } else {
    Logger.log('Google Chat: not enabled (add the "Google Chat API" service to read chats)');
  }
  Logger.log('version: ' + VERSION);
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
    const items = collectMails(props).concat(collectEvents(props), collectChatsSafely(props));
    // Shows up under Executions in the editor: what the device was actually handed
    if (items.length > 0) console.log('items: ' + JSON.stringify(items));
    return json({ ok: true, version: VERSION, items: items });
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

// ------------------------------- Google Chat -------------------------------

function chatAvailable() {
  return typeof Chat !== 'undefined';
}

// A Chat failure (service missing, quota, ...) must not hide the mails and events
function collectChatsSafely(props) {
  if (!chatAvailable()) return [];
  try {
    return collectChats(props);
  } catch (err) {
    console.error('chat: ' + err);
    return [];
  }
}

function isDirectSpace(space) {
  return space.spaceType === 'DIRECT_MESSAGE' || space.spaceType === 'GROUP_CHAT';
}

function collectChats(props) {
  const startedAt = Number(props.getProperty(PROP_STARTED) || 0);
  // Re-read the last minute too (messages can land slightly out of order); seen ids drop duplicates
  const since = new Date(Math.max(startedAt, Number(props.getProperty(PROP_CHAT_CHECKED) || 0) - 60 * 1000));
  const checkedAt = Date.now();
  const self = chatSelf(props);
  const seen = loadList(props, PROP_SEEN_CHATS);
  const fresh = [];

  activeSpaces(since).forEach(space => {
    // One unreadable space must not hide the others
    try {
      listMessagesSince(space, since).forEach(message => {
        if (seen.indexOf(message.name) >= 0) return;
        seen.push(message.name);
        const sender = message.sender || {};
        if ((self && sender.name === self) || sender.type === 'BOT') return;
        if (CHAT_MODE !== 'all' && !isDirectSpace(space) && !mentionsMe(message, self)) return;
        fresh.push({ space: space, message: message });
      });
    } catch (err) {
      console.error('chat: ' + (space.displayName || space.name) + ': ' + err);
    }
  });

  saveList(props, PROP_SEEN_CHATS, seen, 120);  // message names are ~50 chars; stay under 9KB
  props.setProperty(PROP_CHAT_CHECKED, String(checkedAt));
  if (fresh.length === 0) return [];

  if (READ_MODE === 'count') {
    const countText = '新しいチャットが' + fresh.length + '件届いたよ。';
    return [{ type: 'chat', text: countText, notice: countText, bubble: 'チャット ' + fresh.length + '件' }];
  }

  const names = loadNames(props);
  const items = fresh.slice(0, MAX_CHATS_PER_POLL).map(item => {
    const name = senderDisplayName(item.message.sender || {}, names);
    const text = clip(item.message.argumentText || item.message.text || '（添付ファイル）', 40);
    const spaceName = isDirectSpace(item.space) || !item.space.displayName ? '' : clip(item.space.displayName, 15);
    const where = spaceName ? spaceName + 'で、' : '';
    return {
      type: 'chat',
      text: where + name + 'さんからチャット。' + text,
      // Spoken instead of text unless the device reads chat bodies: the space for space posts, the sender for DMs
      notice: spaceName ? spaceName + 'にチャットが届いたよ。' : name + 'さんからチャットが届いたよ。',
      bubble: name + '：' + text,
    };
  });
  saveNames(props, names);
  const rest = fresh.length - items.length;
  if (rest > 0) {
    const restText = 'ほかにも' + rest + '件チャットが届いているよ。';
    items.push({ type: 'chat', text: restText, notice: restText, bubble: 'ほか ' + rest + '件' });
  }
  return items;
}

// Messages posted after `since`, oldest first (the API default order)
function listMessagesSince(space, since) {
  const messages = [];
  let pageToken;
  do {
    const res = Chat.Spaces.Messages.list(space.name, {
      filter: 'createTime > "' + since.toISOString() + '"',
      pageSize: 50,
      pageToken: pageToken,
    });
    (res.messages || []).forEach(message => messages.push(message));
    pageToken = res.nextPageToken;
  } while (pageToken);
  return messages;
}

// Run from the editor to see what the Chat part sees (does not mark anything as announced)
function debugChat() {
  const props = PropertiesService.getScriptProperties();
  Logger.log('Chat service: ' + (chatAvailable() ? 'added' : 'NOT added'));
  if (!chatAvailable()) return;
  Logger.log('CHAT_MODE: ' + CHAT_MODE + ', you are: ' + (chatSelf(props) || 'unknown'));
  Logger.log('setup at: ' + new Date(Number(props.getProperty(PROP_STARTED) || 0)) +
             ', last chat check: ' + new Date(Number(props.getProperty(PROP_CHAT_CHECKED) || 0)));
  const since = new Date(Date.now() - 2 * 60 * 60 * 1000);
  const spaces = activeSpaces(since);
  Logger.log('spaces with posts in the last 2 hours: ' + spaces.length);
  spaces.forEach(space => {
    try {
      const messages = listMessagesSince(space, since);
      Logger.log('- ' + (space.displayName || space.name) + ' (' + space.spaceType + '): ' + messages.length + ' messages');
      const names = loadNames(props);
      messages.slice(-3).forEach(m => Logger.log('    ' + m.createTime + ' ' + senderDisplayName(m.sender || {}, names) + ': ' + clip(m.text || '', 30)));
    } catch (err) {
      Logger.log('- ' + (space.displayName || space.name) + ': ERROR ' + err);
    }
  });
}

// Spaces with a message after `since` (lastActiveTime saves listing messages of quiet spaces)
function activeSpaces(since) {
  const spaces = [];
  let pageToken;
  let pages = 0;
  do {
    const res = Chat.Spaces.list({ pageSize: 1000, pageToken: pageToken });
    (res.spaces || []).forEach(space => {
      if (!space.lastActiveTime || new Date(space.lastActiveTime) > since) spaces.push(space);
    });
    pageToken = res.nextPageToken;
  } while (pageToken && ++pages < 5);
  return spaces;
}

// "users/<id>" of the script owner: the People API id is the same number; otherwise look for a space
// membership through the email alias
function chatSelf(props) {
  let self = props.getProperty(PROP_CHAT_SELF);
  if (self) return self;
  if (typeof People !== 'undefined') {
    try {
      const me = People.People.get('people/me', { personFields: 'metadata' });
      if (me.resourceName) self = me.resourceName.replace('people/', 'users/');
    } catch (e) {
      // needs the "profile" scope, which Apps Script does not request by itself; fall back to the membership
    }
  }
  if (self) {
    props.setProperty(PROP_CHAT_SELF, self);
    return self;
  }
  const email = Session.getEffectiveUser().getEmail();
  const res = Chat.Spaces.list({ pageSize: 20 });
  let lastError = null;
  for (const space of res.spaces || []) {
    try {
      const membership = Chat.Spaces.Members.get(space.name + '/members/' + email);
      if (membership.member && membership.member.name) {
        self = membership.member.name;
        break;
      }
    } catch (e) {
      lastError = e;  // try the next space
    }
  }
  if (!self && lastError) console.error('chat self via membership (' + email + '): ' + lastError);
  if (self) props.setProperty(PROP_CHAT_SELF, self);
  return self || '';
}

function mentionsMe(message, self) {
  return (message.annotations || []).some(a =>
    a.type === 'USER_MENTION' && a.userMention && a.userMention.user &&
    ((self && a.userMention.user.name === self) || a.userMention.user.name === 'users/all'));
}

// The Chat API leaves displayName empty for people under user auth; look them up with the People API
function senderDisplayName(sender, names) {
  if (sender.displayName) return clip(sender.displayName, 20);
  const id = (sender.name || '').replace('users/', '');
  if (!id) return '誰か';
  if (names[id]) return names[id];
  if (typeof People !== 'undefined') {
    try {
      const person = People.People.get('people/' + id, { personFields: 'names' });
      const name = person.names && person.names[0] && person.names[0].displayName;
      if (name) {
        names[id] = clip(name, 20);
        return names[id];
      }
    } catch (e) {
      // not visible in the directory
    }
  }
  return '誰か';
}

function loadNames(props) {
  try {
    return JSON.parse(props.getProperty(PROP_CHAT_NAMES) || '{}');
  } catch (e) {
    return {};
  }
}

// Cap the name cache so the property stays small
function saveNames(props, names) {
  const kept = {};
  Object.keys(names).slice(-100).forEach(k => { kept[k] = names[k]; });
  props.setProperty(PROP_CHAT_NAMES, JSON.stringify(kept));
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
