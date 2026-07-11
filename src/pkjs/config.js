// Clay configuration schema for the watch's Settings screen.
// See https://developer.repebble.com/guides/user-interfaces/app-configuration/
module.exports = [
  {
    "type": "heading",
    "defaultValue": "Info Watchface Settings"
  },
  {
    "type": "toggle",
    "messageKey": "ShowBattery",
    "label": "Show Battery Indicator",
    "defaultValue": true
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
