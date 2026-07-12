// Clay configuration schema for the watch's Settings screen.
// See https://developer.repebble.com/guides/user-interfaces/app-configuration/
var defaults = require('./config-defaults');

module.exports = [
  {
    "type": "heading",
    "defaultValue": "Info Watchface Settings"
  },
  {
    "type": "input",
    "messageKey": "ServerUrl",
    "label": "Info Source URL",
    "description": "The http endpoint that serves the info items.",
    "attributes": {
      "type": "text",
      "placeholder": defaults.DEFAULT_SERVER_URL
    },
    "defaultValue": defaults.DEFAULT_SERVER_URL
  },
  {
    "type": "toggle",
    "messageKey": "ShowBattery",
    "label": "Show Battery Indicator",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "ShowQuietTime",
    "label": "Show Quiet Time Indicator",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "ShowBluetooth",
    "label": "Show Bluetooth Disconnect Alert",
    "description": "Vibrate and show an icon when the watch loses its connection to the phone.",
    "defaultValue": true
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
