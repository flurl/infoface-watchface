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
    "type": "heading",
    "defaultValue": "Info Feed Pagination"
  },
  {
    "type": "toggle",
    "messageKey": "EnablePagination",
    "label": "Enable Pagination",
    "description": "Off by default: the info feed shows only as many events as fit on one screen. Turn on to spill extra events onto additional pages, turned with the triple-tap gesture below.",
    "defaultValue": false
  },
  {
    "type": "heading",
    "defaultValue": "Wrist Tap Gestures"
  },
  {
    "type": "toggle",
    "messageKey": "EnableAccelTaps",
    "label": "Use Accelerometer Taps for Input",
    "description": "Master switch for all wrist-tap gestures. A TRIPLE tap on the watch turns the info-feed page (only relevant when Enable Pagination, above, is on); a QUADRUPLE tap rotates to the next info panel (Events, Weather, ...) -- this works regardless of the Enable Pagination setting. Single and double taps are always ignored. On by default. Turn this off to disable both gestures and switch the accelerometer off completely -- no tap detection, no battery cost from it -- even if pagination is on or multiple panels are configured.",
    "defaultValue": true
  },
  {
    "type": "text",
    "defaultValue": "The settings below tune how taps are detected while the switch above is on -- see PROTOCOL.md for the full explanation of each one."
  },
  {
    "type": "toggle",
    "messageKey": "TapAxisX",
    "label": "Use X Axis",
    "description": "Include left/right motion when checking for a tap.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "TapAxisY",
    "label": "Use Y Axis",
    "description": "Include up/down motion when checking for a tap.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "TapAxisZ",
    "label": "Use Z Axis",
    "description": "Include forward/backward (face-normal) motion when checking for a tap.",
    "defaultValue": true
  },
  {
    "type": "slider",
    "messageKey": "TapThresholdMg",
    "label": "Tap Sensitivity Threshold (mG)",
    "description": "Minimum sudden movement, in milli-Gs, to register as a tap. Lower = more sensitive.",
    "min": 50,
    "max": 1500,
    "step": 10,
    "defaultValue": 300
  },
  {
    "type": "slider",
    "messageKey": "TapRingdownMs",
    "label": "Tap Ringdown (ms)",
    "description": "Minimum gap between two jolts for them to count as separate taps, so one knock's mechanical wobble isn't counted twice.",
    "min": 0,
    "max": 1000,
    "step": 20,
    "defaultValue": 160
  },
  {
    "type": "slider",
    "messageKey": "TapMultiTapWindowMs",
    "label": "Multi-Tap Window (ms)",
    "description": "How long to wait after each tap for the next one. The page turns only if exactly three taps land within this window of each other -- any other count (1, 2, 4+) is ignored.",
    "min": 0,
    "max": 1500,
    "step": 20,
    "defaultValue": 400
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
