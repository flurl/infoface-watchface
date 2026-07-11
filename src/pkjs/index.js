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

function sendItemAt(items, index, total) {
  if (index >= total) {
    console.log('pkjs: sync complete, ' + total + ' item(s)');
    return;
  }

  var item = items[index];
  Pebble.sendAppMessage(
    { 'ItemIndex': index, 'ItemPrefix': item.prefix, 'ItemText': item.text },
    function () {
      sendItemAt(items, index + 1, total);
    },
    function (e) {
      console.log('pkjs: item ' + index + ' send failed: ' + JSON.stringify(e));
    }
  );
}

function sendItems(items) {
  var total = Math.min(items.length, 8);
  Pebble.sendAppMessage(
    { 'ItemCount': total },
    function () {
      sendItemAt(items, 0, total);
    },
    function (e) {
      console.log('pkjs: ItemCount send failed: ' + JSON.stringify(e));
    }
  );
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
      sendItems(data.items || []);
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
