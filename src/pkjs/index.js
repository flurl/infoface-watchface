// Fetches today's calendar items from the companion Android app's local HTTP
// server and relays them to the watch via AppMessage. See PROTOCOL.md.
//
// Why this indirection instead of the companion app sending AppMessage
// directly: the external PebbleKitAndroid2 IPC path hits a
// FailedDifferentAppOpen bug against this watchface specifically (confirmed
// via a controlled A/B test against a plain watchapp, which worked fine).
// PebbleKit JS runs inside the Pebble/Core app itself and does not hit the
// same bug.

// Clay auto-registers 'showConfiguration'/'webviewclosed' listeners and
// sends the settings dict via Pebble.sendAppMessage() itself -- no manual
// wiring needed here (see node_modules/@rebble/clay/index.js).
var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var defaults = require('./config-defaults');
var clay = new Clay(clayConfig); // eslint-disable-line no-unused-vars

var REFRESH_INTERVAL_MS = 15 * 60 * 1000;
var XHR_TIMEOUT_MS = 5000;
var MAX_PANELS = 4;
var MAX_ITEMS_PER_PANEL = 8;

// ServerUrl is a Clay setting (see config.js) but only PKJS reads it -- the
// watch's C code has no use for it. Clay still writes it to localStorage
// under 'clay-settings' on every save, same as any other config field.
function getServerUrl() {
  try {
    var settings = JSON.parse(localStorage.getItem('clay-settings') || '{}');
    return settings.ServerUrl || defaults.DEFAULT_SERVER_URL;
  } catch (e) {
    return defaults.DEFAULT_SERVER_URL;
  }
}

// Accepts either the current (v3) {panels: [{title, items}, ...]} shape or the
// legacy (v2) flat {items: [...]} shape -- the latter is wrapped as a single
// untitled panel so an older/third-party server implementing just the v2
// endpoint still works unchanged. See PROTOCOL.md "Local HTTP API".
function normalizePanels(data) {
  var panels = data.panels;
  if (!panels) {
    panels = [{ title: '', items: data.items || [] }];
  }
  return panels.slice(0, MAX_PANELS);
}

// Flattens panels into the ordered list of AppMessage dicts to send: one
// PanelCount reset, then per panel one ItemCount/PanelTitle reset followed by
// its items. Kept as a flat list (rather than nested recursion) so the single
// sender below doesn't need to know about panel/item structure at all -- it
// just walks the list, one message per outbox turnaround. See PROTOCOL.md
// "Message flow".
function buildSteps(panels) {
  var steps = [{ 'PanelCount': panels.length }];
  panels.forEach(function (panel, panelIndex) {
    var items = (panel.items || []).slice(0, MAX_ITEMS_PER_PANEL);
    steps.push({
      'PanelIndex': panelIndex,
      'ItemCount': items.length,
      'PanelTitle': panel.title || ''
    });
    items.forEach(function (item, itemIndex) {
      // ItemType defaults to event (1) if the source omits it, for back-compat with
      // JSON that predates the type field. Dividers send empty prefix/text. See PROTOCOL.md.
      var type = (typeof item.type === 'number') ? item.type : 1;
      steps.push({
        'PanelIndex': panelIndex,
        'ItemIndex': itemIndex,
        'ItemType': type,
        'ItemPrefix': item.prefix || '',
        'ItemText': item.text || ''
      });
    });
  });
  return steps;
}

// Sends steps[index] then, on ack, recurses to the next one -- Pebble's
// outbox is single-buffered, so each sendAppMessage() call must await its
// success/failure callback before the next is sent.
function sendStepAt(steps, index) {
  if (index >= steps.length) {
    console.log('pkjs: sync complete, ' + steps.length + ' message(s)');
    return;
  }
  Pebble.sendAppMessage(
    steps[index],
    function () {
      sendStepAt(steps, index + 1);
    },
    function (e) {
      console.log('pkjs: step ' + index + ' send failed: ' + JSON.stringify(e));
    }
  );
}

function sendPanels(panels) {
  sendStepAt(buildSteps(panels), 0);
}

function fetchAndSync() {
  var xhr = new XMLHttpRequest();
  xhr.timeout = XHR_TIMEOUT_MS;
  xhr.onload = function () {
    if (xhr.status !== 200) {
      console.log('pkjs: fetch failed, status ' + xhr.status);
      return;
    }
    try {
      var data = JSON.parse(xhr.responseText);
      sendPanels(normalizePanels(data));
    } catch (e) {
      console.log('pkjs: JSON parse error: ' + e);
    }
  };
  xhr.onerror = function () {
    console.log('pkjs: XHR error - is the companion app running?');
  };
  xhr.ontimeout = function () {
    console.log('pkjs: XHR timed out - is the companion app running?');
  };
  xhr.open('GET', getServerUrl(), true);
  xhr.send();
}

Pebble.addEventListener('ready', function () {
  console.log('pkjs: ready');
  fetchAndSync();
  setInterval(fetchAndSync, REFRESH_INTERVAL_MS);
});

// Re-fetch immediately on save so a new Info Source URL takes effect right
// away instead of waiting up to REFRESH_INTERVAL_MS.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) {
    return;
  }
  fetchAndSync();
});
