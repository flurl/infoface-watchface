#include <pebble.h>
#include <stdlib.h>

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define TIME_FONT_KEY FONT_KEY_LECO_60_NUMBERS_AM_PM
#define TIME_HEIGHT_WANTED 64
#define DATE_FONT_KEY FONT_KEY_GOTHIC_18_BOLD
#define DATE_HEIGHT 26
#else
// Flint's panel is 144x168, much smaller than emery/gabbro -- LECO_60 clips
// almost entirely off the ~34px available above the date on a screen this size.
// A smaller date font frees up a few more of those px for a bigger clock.
#define TIME_FONT_KEY FONT_KEY_LECO_32_BOLD_NUMBERS
#define TIME_HEIGHT_WANTED 36
#define DATE_FONT_KEY FONT_KEY_GOTHIC_14_BOLD
#define DATE_HEIGHT 20
#endif

// ---------------------------------------------------------------------------
// Info Watchface
//
// Top half:    digital clock (HH:MM) + date (Weekday YYYY-MM-DD), battery
//              icon top-right, notification area top-left (quiet-time,
//              Bluetooth-disconnected, ... packed left-aligned as needed).
// Bottom half: up to MAX_PANELS independent, scrolling-capable info panels --
// only one visible at a time, rotated by a quadruple wrist tap (page-turning
// within a panel is a separate triple tap, see below). Each panel's item
// model (InfoItem) is intentionally source-agnostic: a companion app pushes
// items here from calendars, weather, RSS/feeds, social streams,
// notifications, etc. via AppMessage. See PROTOCOL.md for the wire format.
// The most recently received set of panels is cached to persistent storage
// and shown on launch, before the first message of a given session arrives;
// an empty cache (or an empty update from the phone) renders as
// "Nothing to see".
// ---------------------------------------------------------------------------

static Window *s_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_info_layer;
static Layer *s_battery_layer;
static Layer *s_notification_layer;

static char s_time_buf[8];
static char s_date_buf[24];

// Watch settings (Clay config page, see PROTOCOL.md).
#define PERSIST_KEY_SHOW_BATTERY 1
#define PERSIST_KEY_SHOW_QUIET_TIME 2
#define PERSIST_KEY_SHOW_BLUETOOTH 3
// Info panel cache: PERSIST_KEY_PANEL_COUNT holds the active panel count, and
// per panel p (0..MAX_PANELS-1): PERSIST_KEY_PANEL_ITEM_COUNT_BASE+p holds its
// item count, PERSIST_KEY_PANEL_TITLE_BASE+p its title, and each item i is
// stored under its own key (PERSIST_KEY_PANEL_ITEM_BASE + p*MAX_INFO_ITEMS+i)
// as a raw InfoItem blob -- one key per item rather than one blob for a whole
// panel because PERSIST_DATA_MAX_LENGTH (256 bytes) is smaller than
// MAX_INFO_ITEMS * sizeof(InfoItem).
#define PERSIST_KEY_PANEL_COUNT 5
#define PERSIST_KEY_PANEL_ITEM_COUNT_BASE 30
#define PERSIST_KEY_PANEL_TITLE_BASE 40
#define PERSIST_KEY_PANEL_ITEM_BASE 100
// Tap-gesture tuning (see PROTOCOL.md): kept well clear of the panel/item key
// ranges above so raising MAX_INFO_ITEMS/MAX_PANELS later can't collide.
#define PERSIST_KEY_TAP_AXIS_X 20
#define PERSIST_KEY_TAP_AXIS_Y 21
#define PERSIST_KEY_TAP_AXIS_Z 22
#define PERSIST_KEY_TAP_THRESHOLD_MG 23
#define PERSIST_KEY_TAP_RINGDOWN_MS 24
#define PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS 25
#define PERSIST_KEY_ENABLE_PAGINATION 26
#define PERSIST_KEY_ENABLE_ACCEL_TAPS 27
// Only meaningful when PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE is defined -- see the
// QuickLaunchRotationEvent block below.
#define PERSIST_KEY_PANEL_ROTATION_EVENT 28
#define PERSIST_KEY_PAGE_ROTATION_EVENT 29
static bool s_show_battery = true;
static bool s_show_quiet_time = true;
static bool s_show_bluetooth_alert = true;

// Master switch for the info-feed pagination feature (extra items spilling
// onto additional pages within a panel, turned by the triple-tap gesture).
// Off by default. Unlike v1/v2, this no longer single-handedly gates the
// accelerometer subscription -- see prv_update_accel_subscription() -- since
// panel rotation (quadruple tap) needs the accelerometer too, independently
// of pagination.
static bool s_enable_pagination = false;

// Overall master switch for wrist-tap gestures (both the triple-tap page
// turn and the quadruple-tap panel rotation). On by default. See
// prv_update_accel_subscription() -- unlike s_enable_pagination and
// s_panel_count, which OR together to decide whether taps can currently do
// anything, this ANDs in ahead of that: off means the accelerometer is
// never subscribed to no matter what pagination/panel state says.
static bool s_enable_accel_taps = true;

#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
// An alternate, button-driven way to trigger panel/page rotation, alongside the wrist-tap
// gestures above -- only available when built against an SDK generated from a firmware that has
// quick_launch_button_service (this project's own PebbleOS fork; see PROTOCOL.md). Each of the
// two Clay dropdowns below picks one of these (or "None") independently, so e.g. "Hold Select"
// can drive panel rotation while "Tap Up" drives page rotation, or the same event can drive both.
// The numbering here is this watchface's own encoding (sent by config.js as a Clay "select"
// value, always a decimal-digit string on the wire) -- it does not correspond to PebbleOS's
// internal ButtonId values, deliberately, since the two are unrelated address spaces.
typedef enum {
  QUICK_LAUNCH_ROTATION_EVENT_NONE = 0,
  QUICK_LAUNCH_ROTATION_EVENT_TAP_UP = 1,
  QUICK_LAUNCH_ROTATION_EVENT_TAP_DOWN = 2,
  QUICK_LAUNCH_ROTATION_EVENT_HOLD_UP = 3,
  QUICK_LAUNCH_ROTATION_EVENT_HOLD_DOWN = 4,
  QUICK_LAUNCH_ROTATION_EVENT_HOLD_SELECT = 5,
  QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK = 6,
} QuickLaunchRotationEvent;

static QuickLaunchRotationEvent s_panel_rotation_event = QUICK_LAUNCH_ROTATION_EVENT_NONE;
static QuickLaunchRotationEvent s_page_rotation_event = QUICK_LAUNCH_ROTATION_EVENT_NONE;
#endif

// Tap-recognition parameters, tunable at runtime from the Clay settings page
// (see PROTOCOL.md) so they can be tweaked without recompiling. See
// prv_accel_data_handler (below, with the rest of the tap-gesture code) for
// how these are actually used; they live up here, alongside the other Clay
// settings, only because the AppMessage inbox handler needs to write them.
// Defaults match the values this feature originally shipped with as fixed
// constants, and are used until a Clay save (or a persisted prior save)
// overrides them.
#define ACCEL_TAP_SAMPLING_RATE ACCEL_SAMPLING_25HZ
#define ACCEL_TAP_SAMPLES_PER_UPDATE 4
#define ACCEL_TAP_DEFAULT_THRESHOLD_MG 300
#define ACCEL_TAP_DEFAULT_RINGDOWN_MS 160
#define ACCEL_TAP_DEFAULT_MULTI_TAP_WINDOW_MS 400

// Which accelerometer axes contribute to the jolt magnitude. A disabled
// axis's delta is treated as 0, so motion on that axis alone can't
// register a tap. (If all three are disabled, no jolt can ever exceed the
// threshold and taps stop working entirely -- allowed, not guarded
// against, since this is a debugging/tuning control.)
static bool s_tap_axis_x = true;
static bool s_tap_axis_y = true;
static bool s_tap_axis_z = true;

// Jolt magnitude (milli-Gs) and gesture timings (ms), as configured.
static int32_t s_tap_threshold_mg = ACCEL_TAP_DEFAULT_THRESHOLD_MG;
static int32_t s_tap_ringdown_ms = ACCEL_TAP_DEFAULT_RINGDOWN_MS;
static int32_t s_tap_multi_tap_window_ms = ACCEL_TAP_DEFAULT_MULTI_TAP_WINDOW_MS;

// Derived from the above (squared threshold; ms converted to samples at
// ACCEL_TAP_SAMPLING_RATE, defined with the rest of the tap-gesture code
// below) so prv_accel_data_handler can compare plain ints per sample
// instead of redoing this math every time. Recomputed on load and whenever
// a setting changes.
static int32_t s_tap_threshold_sq;
static int s_tap_ringdown_samples;
static int s_tap_multi_tap_window_samples;

static void prv_recompute_tap_params(void) {
  s_tap_threshold_sq = s_tap_threshold_mg * s_tap_threshold_mg;
  s_tap_ringdown_samples = (s_tap_ringdown_ms * ACCEL_TAP_SAMPLING_RATE) / 1000;
  s_tap_multi_tap_window_samples = (s_tap_multi_tap_window_ms * ACCEL_TAP_SAMPLING_RATE) / 1000;
}

// Last-seen phone connection state, so the disconnect alert only fires on a
// real connected -> disconnected transition (not when the handler is primed
// at launch, and not on reconnects). Seeded from a peek in prv_init().
static bool s_bt_connected = true;

// Info item type (shared by *value* with the companion app / PKJS, see
// PROTOCOL.md). Divider rows render as a horizontal rule and ignore
// prefix/text; event/weather/json/other rows render the prefix + text
// columns (ITEM_TYPE_JSON needs no dedicated rendering branch -- it falls
// through to the same default single-line layout as event/other, see
// prv_draw_rows). Deliberately only ONE code for all weather conditions --
// the specific icon is picked watch-side by parsing ItemText for a keyword
// (see prv_weather_icon_for below), not by adding a type per condition, so a
// new condition never needs a protocol change.
#define ITEM_TYPE_DIVIDER 0
#define ITEM_TYPE_EVENT 1
#define ITEM_TYPE_WEATHER 2
#define ITEM_TYPE_FEED 3
#define ITEM_TYPE_JSON 4
#define ITEM_TYPE_OTHER 255

// Generic info item: `prefix` is a short left column (a weekday+time, a temp,
// a source tag like "RSS", etc.), `text` is the main line (event title,
// headline, weather condition, ...). Sized to 60 usable chars (rather than the 39 every other
// item type actually needs) so a feed headline can fill both lines of ITEM_TYPE_FEED's two-line
// layout at this screen's full width -- measured on-device: GOTHIC_18 fits ~58 chars of
// representative prose across two full-width (width - 8) lines on a 200px-wide screen, see
// PROTOCOL.md's "Feed" section.
typedef struct {
  uint8_t type;
  char prefix[12];
  char text[61];
} InfoItem;

#define MAX_INFO_ITEMS 8

// One info panel: its own item list/count plus an optional short title shown
// as the panel-header/divider label (see prv_draw_panel_header). Up to
// MAX_PANELS of these are active at once; only s_panel is drawn, the rest sit
// idle until a quadruple tap rotates to them (see "Panel rotation" below).
#define MAX_PANELS 4
typedef struct {
  InfoItem items[MAX_INFO_ITEMS];
  int item_count;
  char title[16];
} InfoPanel;

static InfoPanel s_panels[MAX_PANELS];
static int s_panel_count = 0;

// Current panel (0-based, rotated by a quadruple tap) and current page within
// that panel (0-based, turned by a triple tap -- see the pagination helpers
// below). Rotating panels always resets the page, since a page position from
// one panel has no meaning in another.
static int s_panel = 0;
static int s_page = 0;

static const int ROW_HEIGHT = 22;
static const int DIVIDER_ROW_HEIGHT = 12;
// Height of a single line within a two-line feed row. Deliberately tighter than ROW_HEIGHT
// (which includes padding sized for a single-line row): at ROW_HEIGHT's 22px, two full feed
// rows (2 * 2*22 = 88px) don't fit in the ~84px actually left over once PAGE_INDICATOR_H is
// reserved for a multi-page feed panel on a 200x228 screen -- pagination then falls back to
// just 1 item/page. 20px keeps two full items (2 * 2*20 = 80px) comfortably inside that budget
// while still fitting GOTHIC_18 without clipping.
static const int FEED_LINE_HEIGHT = 20;
static const int FEED_ROW_HEIGHT = 2 * FEED_LINE_HEIGHT;
// Reserved strip at the bottom of the info layer for the page-dot indicator,
// only actually consumed (i.e. subtracted from the pagination height) when
// there's more than one page.
static const int PAGE_INDICATOR_H = 10;
// Height of the panel-title/rule header band, only consumed when the current
// panel has a non-empty title (see prv_draw_panel_header) -- an untitled
// panel (or the common single-panel case) instead draws the plain
// full-width rule from v1/v2 at zero extra height, unchanged.
static const int PANEL_HEADER_H = 14;
static const int PANEL_HEADER_TITLE_W = 70; // same column width as the item prefix column below

static void prv_persist_item(int panel_index, int item_index) {
  persist_write_data(PERSIST_KEY_PANEL_ITEM_BASE + panel_index * MAX_INFO_ITEMS + item_index,
                      &s_panels[panel_index].items[item_index], sizeof(InfoItem));
}

static void prv_persist_panel_item_count(int panel_index) {
  persist_write_int(PERSIST_KEY_PANEL_ITEM_COUNT_BASE + panel_index, s_panels[panel_index].item_count);
}

static void prv_persist_panel_title(int panel_index) {
  persist_write_string(PERSIST_KEY_PANEL_TITLE_BASE + panel_index, s_panels[panel_index].title);
}

static void prv_persist_panel_count(void) {
  persist_write_int(PERSIST_KEY_PANEL_COUNT, s_panel_count);
}

// Loads the last-cached panel set from persistent storage, so the watchface
// shows real (if possibly stale) data immediately on launch rather than
// waiting for the phone. No cache yet (fresh install) or an empty cached set
// both leave s_panel_count at 0, which prv_info_update_proc renders as
// "Nothing to see".
static void prv_load_cached_panels(void) {
  s_panel_count = 0;
  for (int p = 0; p < MAX_PANELS; p++) {
    s_panels[p].item_count = 0;
    s_panels[p].title[0] = '\0';
  }
  if (!persist_exists(PERSIST_KEY_PANEL_COUNT)) {
    return;
  }

  int count = persist_read_int(PERSIST_KEY_PANEL_COUNT);
  if (count > MAX_PANELS) {
    count = MAX_PANELS;
  }
  if (count < 0) {
    count = 0;
  }
  for (int p = 0; p < count; p++) {
    int item_count = 0;
    if (persist_exists(PERSIST_KEY_PANEL_ITEM_COUNT_BASE + p)) {
      item_count = persist_read_int(PERSIST_KEY_PANEL_ITEM_COUNT_BASE + p);
      if (item_count > MAX_INFO_ITEMS) {
        item_count = MAX_INFO_ITEMS;
      }
    }
    if (persist_exists(PERSIST_KEY_PANEL_TITLE_BASE + p)) {
      persist_read_string(PERSIST_KEY_PANEL_TITLE_BASE + p, s_panels[p].title, sizeof(s_panels[p].title));
    }
    for (int i = 0; i < item_count; i++) {
      persist_read_data(PERSIST_KEY_PANEL_ITEM_BASE + p * MAX_INFO_ITEMS + i, &s_panels[p].items[i],
                         sizeof(InfoItem));
    }
    s_panels[p].item_count = item_count;
  }
  s_panel_count = count;
}

// Defined further down. Forward declared here because prv_inbox_received_handler and prv_init
// need to update the accelerometer subscription on PanelCount/EnablePagination changes, and
// prv_advance_panel/prv_advance_page are now also called directly from
// prv_quick_launch_button_handler (below), ahead of their own definitions (which sit next to
// their other caller, the gesture dispatch in prv_accel_data_handler).
static void prv_subscribe_accel(void);
static void prv_unsubscribe_accel(void);
static void prv_update_accel_subscription(void);
static void prv_advance_page(void);
static void prv_advance_panel(void);

#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
// Maps a raw (button, press type) pair to this watchface's own QuickLaunchRotationEvent encoding
// -- see the enum's definition above for why the numbering is independent of PebbleOS's ButtonId.
static QuickLaunchRotationEvent prv_encode_quick_launch_event(ButtonId button_id,
                                                               QuickLaunchPressType press_type) {
  switch (button_id) {
    case BUTTON_ID_UP:
      return press_type == QuickLaunchPressType_Long ? QUICK_LAUNCH_ROTATION_EVENT_HOLD_UP
                                                       : QUICK_LAUNCH_ROTATION_EVENT_TAP_UP;
    case BUTTON_ID_DOWN:
      return press_type == QuickLaunchPressType_Long ? QUICK_LAUNCH_ROTATION_EVENT_HOLD_DOWN
                                                       : QUICK_LAUNCH_ROTATION_EVENT_TAP_DOWN;
    case BUTTON_ID_SELECT:
      // Select only ever reaches us on a hold -- a short Select press is always PebbleOS's
      // launcher shortcut instead (see PROTOCOL.md), never delivered here.
      return QUICK_LAUNCH_ROTATION_EVENT_HOLD_SELECT;
    case BUTTON_ID_BACK:
      // Same for Back: a short press always dismisses the timeline peek instead.
      return QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK;
    default:
      return QUICK_LAUNCH_ROTATION_EVENT_NONE;
  }
}

// quick_launch_button_service_subscribe() callback: fires when a quick-launch button configured
// (Settings > Quick Launch, on-watch) to target this already-running watchface is pressed -- see
// PROTOCOL.md. Panel and page rotation are triggered independently, so the same event can drive
// both, one, or neither, depending on the two Clay dropdowns below.
static void prv_quick_launch_button_handler(ButtonId button_id, QuickLaunchPressType press_type) {
  QuickLaunchRotationEvent event = prv_encode_quick_launch_event(button_id, press_type);
  if (event == QUICK_LAUNCH_ROTATION_EVENT_NONE) {
    return;
  }
  if (event == s_panel_rotation_event) {
    prv_advance_panel();
  }
  if (event == s_page_rotation_event) {
    prv_advance_page();
  }
}
#endif  // PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE

// AppMessage inbox: see PROTOCOL.md for the full contract. Messages arrive
// in one of these shapes:
//   - {ShowBattery: 0|1, ShowQuietTime: 0|1, ShowBluetooth: 0|1, EnablePagination: 0|1,
//      EnableAccelTaps: 0|1, TapAxisX/Y/Z: 0|1,
//      TapThresholdMg/TapRingdownMs/TapMultiTapWindowMs: N,
//      PanelRotationEvent/PageRotationEvent: "0".."6",
//      ServerUrl: "..."} (any subset) -- from the Clay settings page, all
//      changed fields in one message
//   - {PanelCount: P}                                              -- resets all panels
//   - {PanelIndex: p, ItemCount: N, PanelTitle: "..."}              -- resets one panel
//   - {PanelIndex: p, ItemIndex: i, ItemPrefix: "...", ItemText: "..."} -- one item
// PanelIndex is optional on the last two shapes and defaults to panel 0, so a
// legacy v2-only sender (bare ItemCount/ItemIndex, no PanelCount ever) still
// lands its items in panel 0 and renders exactly as it did pre-panels.
// Quick-launch-button-driven rotation itself no longer arrives here at all -- it's delivered
// straight from firmware via quick_launch_button_service_subscribe() (see
// prv_quick_launch_button_handler above), bypassing AppMessage/PKJS entirely.
static void prv_inbox_received_handler(DictionaryIterator *iterator, void *context) {
  bool handled_setting = false;

  Tuple *show_battery_tuple = dict_find(iterator, MESSAGE_KEY_ShowBattery);
  if (show_battery_tuple) {
    s_show_battery = show_battery_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_SHOW_BATTERY, s_show_battery);
    layer_set_hidden(s_battery_layer, !s_show_battery);
    handled_setting = true;
  }

  Tuple *show_quiet_time_tuple = dict_find(iterator, MESSAGE_KEY_ShowQuietTime);
  if (show_quiet_time_tuple) {
    s_show_quiet_time = show_quiet_time_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_SHOW_QUIET_TIME, s_show_quiet_time);
    layer_mark_dirty(s_notification_layer);
    handled_setting = true;
  }

  Tuple *show_bluetooth_tuple = dict_find(iterator, MESSAGE_KEY_ShowBluetooth);
  if (show_bluetooth_tuple) {
    s_show_bluetooth_alert = show_bluetooth_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_SHOW_BLUETOOTH, s_show_bluetooth_alert);
    layer_mark_dirty(s_notification_layer);
    handled_setting = true;
  }

  Tuple *enable_pagination_tuple = dict_find(iterator, MESSAGE_KEY_EnablePagination);
  if (enable_pagination_tuple) {
    bool new_value = enable_pagination_tuple->value->uint8 != 0;
    if (new_value != s_enable_pagination) {
      s_enable_pagination = new_value;
      persist_write_bool(PERSIST_KEY_ENABLE_PAGINATION, s_enable_pagination);
      if (!s_enable_pagination) {
        s_page = 0;
      }
      prv_update_accel_subscription();
      layer_mark_dirty(s_info_layer);
    }
    handled_setting = true;
  }

  Tuple *enable_accel_taps_tuple = dict_find(iterator, MESSAGE_KEY_EnableAccelTaps);
  if (enable_accel_taps_tuple) {
    bool new_value = enable_accel_taps_tuple->value->uint8 != 0;
    if (new_value != s_enable_accel_taps) {
      s_enable_accel_taps = new_value;
      persist_write_bool(PERSIST_KEY_ENABLE_ACCEL_TAPS, s_enable_accel_taps);
      prv_update_accel_subscription();
    }
    handled_setting = true;
  }

#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
  // Clay's "select" component always sends its value as a decimal-digit string (see config.js),
  // unlike the slider fields below which arrive as real int32s -- read via ->value->cstring, not
  // ->value->int32. Out-of-range values (a mismatched/stale config.js, say) are ignored rather
  // than stored, leaving whatever was previously in effect.
  Tuple *panel_rotation_event_tuple = dict_find(iterator, MESSAGE_KEY_PanelRotationEvent);
  if (panel_rotation_event_tuple) {
    int value = atoi(panel_rotation_event_tuple->value->cstring);
    if (value >= QUICK_LAUNCH_ROTATION_EVENT_NONE && value <= QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK) {
      s_panel_rotation_event = (QuickLaunchRotationEvent)value;
      persist_write_int(PERSIST_KEY_PANEL_ROTATION_EVENT, s_panel_rotation_event);
    }
    handled_setting = true;
  }

  Tuple *page_rotation_event_tuple = dict_find(iterator, MESSAGE_KEY_PageRotationEvent);
  if (page_rotation_event_tuple) {
    int value = atoi(page_rotation_event_tuple->value->cstring);
    if (value >= QUICK_LAUNCH_ROTATION_EVENT_NONE && value <= QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK) {
      s_page_rotation_event = (QuickLaunchRotationEvent)value;
      persist_write_int(PERSIST_KEY_PAGE_ROTATION_EVENT, s_page_rotation_event);
    }
    handled_setting = true;
  }
#endif  // PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE

  Tuple *tap_axis_x_tuple = dict_find(iterator, MESSAGE_KEY_TapAxisX);
  if (tap_axis_x_tuple) {
    s_tap_axis_x = tap_axis_x_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_TAP_AXIS_X, s_tap_axis_x);
    handled_setting = true;
  }

  Tuple *tap_axis_y_tuple = dict_find(iterator, MESSAGE_KEY_TapAxisY);
  if (tap_axis_y_tuple) {
    s_tap_axis_y = tap_axis_y_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_TAP_AXIS_Y, s_tap_axis_y);
    handled_setting = true;
  }

  Tuple *tap_axis_z_tuple = dict_find(iterator, MESSAGE_KEY_TapAxisZ);
  if (tap_axis_z_tuple) {
    s_tap_axis_z = tap_axis_z_tuple->value->uint8 != 0;
    persist_write_bool(PERSIST_KEY_TAP_AXIS_Z, s_tap_axis_z);
    handled_setting = true;
  }

  // Clay's "slider" component sends a real number (unlike "input", which
  // sends a string) -- PebbleKit JS encodes it as a signed 32-bit int, so
  // these must be read via ->value->int32, not ->value->uint8 (which would
  // silently truncate anything over 255, unlike the small 0/1 toggles above).
  Tuple *tap_threshold_tuple = dict_find(iterator, MESSAGE_KEY_TapThresholdMg);
  if (tap_threshold_tuple) {
    s_tap_threshold_mg = tap_threshold_tuple->value->int32;
    persist_write_int(PERSIST_KEY_TAP_THRESHOLD_MG, s_tap_threshold_mg);
    handled_setting = true;
  }

  Tuple *tap_ringdown_tuple = dict_find(iterator, MESSAGE_KEY_TapRingdownMs);
  if (tap_ringdown_tuple) {
    s_tap_ringdown_ms = tap_ringdown_tuple->value->int32;
    persist_write_int(PERSIST_KEY_TAP_RINGDOWN_MS, s_tap_ringdown_ms);
    handled_setting = true;
  }

  Tuple *tap_window_tuple = dict_find(iterator, MESSAGE_KEY_TapMultiTapWindowMs);
  if (tap_window_tuple) {
    s_tap_multi_tap_window_ms = tap_window_tuple->value->int32;
    persist_write_int(PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS, s_tap_multi_tap_window_ms);
    handled_setting = true;
  }

  if (handled_setting) {
    prv_recompute_tap_params();
    return;
  }

  Tuple *panel_count_tuple = dict_find(iterator, MESSAGE_KEY_PanelCount);
  if (panel_count_tuple) {
    int count = panel_count_tuple->value->uint8;
    if (count > MAX_PANELS) {
      count = MAX_PANELS;
    }
    s_panel_count = count;
    for (int p = 0; p < MAX_PANELS; p++) {
      s_panels[p].item_count = 0;
      s_panels[p].title[0] = '\0';
    }
    s_panel = 0;
    s_page = 0;
    prv_persist_panel_count();
    prv_update_accel_subscription();
    layer_mark_dirty(s_info_layer);
    return;
  }

  // Every remaining message shape (ItemCount/PanelTitle reset, or an item)
  // targets one panel -- PanelIndex is optional and defaults to 0.
  Tuple *panel_index_tuple = dict_find(iterator, MESSAGE_KEY_PanelIndex);
  int panel_index = panel_index_tuple ? panel_index_tuple->value->uint8 : 0;
  if (panel_index < 0 || panel_index >= MAX_PANELS) {
    return;
  }

  // Back-compat: a legacy v2-only sender never sends PanelCount at all, so
  // s_panel_count would otherwise stay 0 forever and panel 0's items would
  // never actually render. The very first item/count message it ever sends
  // (necessarily targeting panel 0, since it never sends PanelIndex either)
  // bumps the count to 1 -- exactly like a real {PanelCount: 1} would have.
  if (s_panel_count == 0 && panel_index == 0) {
    s_panel_count = 1;
    prv_persist_panel_count();
    prv_update_accel_subscription();
  }

  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_ItemCount);
  if (count_tuple) {
    int count = count_tuple->value->uint8;
    if (count > MAX_INFO_ITEMS) {
      count = MAX_INFO_ITEMS;
    }
    s_panels[panel_index].item_count = count;
    prv_persist_panel_item_count(panel_index);

    Tuple *title_tuple = dict_find(iterator, MESSAGE_KEY_PanelTitle);
    if (title_tuple) {
      strncpy(s_panels[panel_index].title, title_tuple->value->cstring,
              sizeof(s_panels[panel_index].title) - 1);
      s_panels[panel_index].title[sizeof(s_panels[panel_index].title) - 1] = '\0';
    } else {
      s_panels[panel_index].title[0] = '\0';
    }
    prv_persist_panel_title(panel_index);

    layer_mark_dirty(s_info_layer);
    return;
  }

  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_ItemIndex);
  if (!index_tuple) {
    return;
  }

  int index = index_tuple->value->uint8;
  if (index >= MAX_INFO_ITEMS) {
    return;
  }

  InfoPanel *panel = &s_panels[panel_index];

  // ItemType is optional for back-compat: a message without it is an event.
  // Dividers may omit (or send empty) prefix/text.
  Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_ItemType);
  panel->items[index].type = type_tuple ? type_tuple->value->uint8 : ITEM_TYPE_EVENT;

  Tuple *prefix_tuple = dict_find(iterator, MESSAGE_KEY_ItemPrefix);
  if (prefix_tuple) {
    strncpy(panel->items[index].prefix, prefix_tuple->value->cstring, sizeof(panel->items[index].prefix) - 1);
    panel->items[index].prefix[sizeof(panel->items[index].prefix) - 1] = '\0';
  } else {
    panel->items[index].prefix[0] = '\0';
  }

  Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_ItemText);
  if (text_tuple) {
    strncpy(panel->items[index].text, text_tuple->value->cstring, sizeof(panel->items[index].text) - 1);
    panel->items[index].text[sizeof(panel->items[index].text) - 1] = '\0';
  } else {
    panel->items[index].text[0] = '\0';
  }

  if (index + 1 > panel->item_count) {
    panel->item_count = index + 1;
    prv_persist_panel_item_count(panel_index);
  }
  prv_persist_item(panel_index, index);
  layer_mark_dirty(s_info_layer);
}

static void prv_inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage inbox dropped, reason: %d", (int)reason);
}

// Row height for one item, by type. Shared by the pagination helpers below and prv_draw_rows so
// they can never disagree about how tall a row is.
static int prv_row_height(const InfoItem *item) {
  if (item->type == ITEM_TYPE_DIVIDER) {
    return DIVIDER_ROW_HEIGHT;
  }
  if (item->type == ITEM_TYPE_FEED) {
    return FEED_ROW_HEIGHT;
  }
  return ROW_HEIGHT;
}

// --- Pagination -------------------------------------------------------
// The info feed fills a page with as many rows as fit in `height`, then
// spills the rest onto the next page. These helpers share the exact same
// row-height rules as the draw loop below so a page's contents always match
// what was measured. They take an explicit (items, count) pair rather than
// reading globals so they work against whichever panel is currently shown.

// Index of the first item that does NOT fit on a page starting at `start`
// within `height` pixels. Always admits at least one item (even if it alone
// overflows `height`) so an oversized row can't stall pagination.
static int prv_page_end(const InfoItem *items, int count, int start, int height) {
  int y = 6;
  int i = start;
  while (i < count) {
    int row_h = prv_row_height(&items[i]);
    if (i > start && y + row_h > height) {
      break;
    }
    y += row_h;
    i++;
  }
  return i;
}

static int prv_num_pages(const InfoItem *items, int count, int height) {
  if (count == 0) {
    return 1;
  }
  int pages = 0;
  int start = 0;
  while (start < count) {
    start = prv_page_end(items, count, start, height);
    pages++;
  }
  return pages;
}

static int prv_page_start(const InfoItem *items, int count, int page, int height) {
  int start = 0;
  for (int p = 0; p < page && start < count; p++) {
    start = prv_page_end(items, count, start, height);
  }
  return start;
}

// Shared by the draw proc and the tap handler so both agree on how many
// pages there are: reserve the indicator strip only if the items actually
// need more than one page at the full layer height, otherwise let rows use
// the whole layer (and there's exactly one page).
static int prv_layout_num_pages(const InfoItem *items, int count, int full_height, int *out_content_h) {
  int content_h = full_height;
  int num_pages = prv_num_pages(items, count, content_h);
  if (num_pages > 1) {
    content_h = full_height - PAGE_INDICATOR_H;
    num_pages = prv_num_pages(items, count, content_h);
  }
  if (out_content_h) {
    *out_content_h = content_h;
  }
  return num_pages;
}

static void prv_draw_page_indicator(GContext *ctx, GRect slot, int num_pages, int page) {
  const int dot_r = 2;
  const int dot_gap = 8;
  int total_w = (num_pages - 1) * dot_gap;
  int x = slot.origin.x + (slot.size.w - total_w) / 2;
  int cy = slot.origin.y + slot.size.h / 2;

  for (int p = 0; p < num_pages; p++) {
    GColor color = (p == page) ? GColorWhite : GColorDarkGray;
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, GPoint(x + p * dot_gap, cy), dot_r);
  }
}

// Which small icon represents a weather condition, drawn beside the
// temperature in a weather item's prefix column (see prv_draw_rows). Kept
// deliberately generic ("partly" covers any part-cloudy phrasing, etc.) since
// the whole point of a single ITEM_TYPE_WEATHER (see its #define above) is
// that recognizing more conditions later is a watch-side wording tweak, not a
// protocol change.
typedef enum {
  WEATHER_ICON_SUN,
  WEATHER_ICON_CLOUD,
  WEATHER_ICON_RAIN,
  WEATHER_ICON_SNOW,
  WEATHER_ICON_PARTLY,
} WeatherIcon;

// Case-insensitive substring match against the item's text (e.g. "Rain 60%",
// "Cloudy", "Sunny") to pick an icon. Falls back to the sun icon for
// "sun"/"clear" and anything unrecognized, so a condition word this list
// doesn't know yet still renders sensibly instead of drawing nothing.
static WeatherIcon prv_weather_icon_for(const char *text) {
  char lower[61]; // matches InfoItem.text's capacity, see the struct above
  size_t len = strlen(text);
  if (len >= sizeof(lower)) {
    len = sizeof(lower) - 1;
  }
  for (size_t i = 0; i < len; i++) {
    char c = text[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lower[len] = '\0';

  if (strstr(lower, "rain") || strstr(lower, "shower")) {
    return WEATHER_ICON_RAIN;
  }
  if (strstr(lower, "snow")) {
    return WEATHER_ICON_SNOW;
  }
  if (strstr(lower, "partly") || strstr(lower, "partial")) {
    return WEATHER_ICON_PARTLY;
  }
  if (strstr(lower, "cloud") || strstr(lower, "overcast")) {
    return WEATHER_ICON_CLOUD;
  }
  return WEATHER_ICON_SUN;
}

// Small (roughly 14x14) weather icon drawers, centered in `slot`. Simple
// filled/stroked shapes in the same spirit as the corner notification icons
// (quiet-time moon, Bluetooth rune) further down -- all three target
// platforms (emery/flint/gabbro) are color, so these use color freely
// (unlike the corner icons, which predate that assumption and still guard
// with PBL_IF_COLOR_ELSE).
static void prv_draw_weather_icon(GContext *ctx, GRect slot, WeatherIcon icon) {
  int cx = slot.origin.x + slot.size.w / 2;
  int cy = slot.origin.y + slot.size.h / 2;

  switch (icon) {
    case WEATHER_ICON_RAIN:
      graphics_context_set_fill_color(ctx, GColorLightGray);
      graphics_fill_circle(ctx, GPoint(cx, cy - 2), 5);
      graphics_context_set_stroke_color(ctx, GColorVividCerulean);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, GPoint(cx - 3, cy + 4), GPoint(cx - 5, cy + 8));
      graphics_draw_line(ctx, GPoint(cx + 1, cy + 4), GPoint(cx - 1, cy + 8));
      graphics_draw_line(ctx, GPoint(cx + 4, cy + 4), GPoint(cx + 2, cy + 8));
      break;
    case WEATHER_ICON_SNOW:
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, GPoint(cx - 5, cy), GPoint(cx + 5, cy));
      graphics_draw_line(ctx, GPoint(cx, cy - 5), GPoint(cx, cy + 5));
      graphics_draw_line(ctx, GPoint(cx - 4, cy - 4), GPoint(cx + 4, cy + 4));
      graphics_draw_line(ctx, GPoint(cx - 4, cy + 4), GPoint(cx + 4, cy - 4));
      break;
    case WEATHER_ICON_CLOUD:
      graphics_context_set_fill_color(ctx, GColorLightGray);
      graphics_fill_circle(ctx, GPoint(cx - 3, cy + 1), 4);
      graphics_fill_circle(ctx, GPoint(cx + 2, cy - 1), 5);
      graphics_fill_circle(ctx, GPoint(cx + 6, cy + 1), 3);
      break;
    case WEATHER_ICON_PARTLY:
      graphics_context_set_fill_color(ctx, GColorYellow);
      graphics_fill_circle(ctx, GPoint(cx - 3, cy - 3), 4);
      graphics_context_set_fill_color(ctx, GColorLightGray);
      graphics_fill_circle(ctx, GPoint(cx + 2, cy + 2), 5);
      break;
    case WEATHER_ICON_SUN:
    default:
      graphics_context_set_fill_color(ctx, GColorYellow);
      graphics_fill_circle(ctx, GPoint(cx, cy), 5);
      break;
  }
}

// How many leading bytes of `text` fit on one line at `narrow_w` pixels, breaking only at a
// space (never mid-word), when rendered in `font`. Used by the ITEM_TYPE_FEED branch below to
// find where the heading's first line (squeezed next to the feed-name column) ends, so the rest
// can be redrawn on a second line at the full row width instead of continuing to wrap inside the
// narrow column. Greedily grows a candidate word-by-word, measuring each with
// graphics_text_layout_get_content_size (the same layout engine graphics_draw_text itself uses)
// against a single line's height at `narrow_w` -- the moment a candidate would wrap to a second
// line, the previous (fitting) candidate's length is the split point. Returns strlen(text) if
// the whole string already fits on one line.
static size_t prv_feed_line1_len(const char *text, GFont font, int narrow_w) {
  size_t len = strlen(text);
  if (len == 0) {
    return 0;
  }

  int line_h = graphics_text_layout_get_content_size(
                   "Ag", font, GRect(0, 0, 1000, 1000), GTextOverflowModeWordWrap, GTextAlignmentLeft)
                   .h;

  char candidate[61]; // matches InfoItem.text's capacity, see the struct above
  size_t last_fit = 0;
  size_t i = 0;
  while (i <= len) {
    size_t next = i;
    while (next < len && text[next] != ' ') {
      next++;
    }
    size_t clen = next;
    if (clen >= sizeof(candidate)) {
      clen = sizeof(candidate) - 1;
    }
    memcpy(candidate, text, clen);
    candidate[clen] = '\0';

    GSize size = graphics_text_layout_get_content_size(
        candidate, font, GRect(0, 0, narrow_w, 1000), GTextOverflowModeWordWrap, GTextAlignmentLeft);
    if (size.h > line_h) {
      break; // adding this word pushed the candidate onto a second line -- stop before it
    }
    last_fit = next;
    if (next >= len) {
      break; // the whole string fits on one line
    }
    i = next + 1; // skip the space, try the next word
  }
  return last_fit;
}

// Draws items[start, end) top-anchored at y = y_offset + 6, at the given
// width. Shared by both the paginated and non-paginated rendering paths in
// prv_info_update_proc so they can't drift apart. `y_offset` is the height
// already consumed above by prv_draw_panel_header (0 for an untitled panel).
static void prv_draw_rows(GContext *ctx, GFont prefix_font, GFont text_font, int width,
                           const InfoItem *items, int start, int end, int y_offset) {
  int y = y_offset + 6;
  for (int i = start; i < end; i++) {
    bool is_divider = items[i].type == ITEM_TYPE_DIVIDER;
    int row_h = prv_row_height(&items[i]);

    if (is_divider) {
      int line_y = y + row_h / 2;
      graphics_context_set_stroke_color(ctx, GColorLightGray);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, GPoint(4, line_y), GPoint(width - 4, line_y));
      y += row_h;
      continue;
    }

    if (items[i].type == ITEM_TYPE_FEED) {
      // Two-line layout: the feed name sits top-aligned in the normal single-line prefix
      // column. The heading's first line is squeezed into the narrow column beside it (like
      // every other item type), but its second line uses the FULL row width -- including the
      // space the prefix column occupies on line 1 -- rather than staying confined to the
      // narrow column for both lines. graphics_draw_text can't vary a box's width per wrapped
      // line, so prv_feed_line1_len() finds the word-wrap split point at the narrow width and
      // the two lines are drawn as two separate calls with two different box widths.
      int narrow_w = width - 80;
      size_t split = prv_feed_line1_len(items[i].text, text_font, narrow_w);

      char line1[61]; // matches InfoItem.text's capacity
      size_t line1_len = split < sizeof(line1) ? split : sizeof(line1) - 1;
      memcpy(line1, items[i].text, line1_len);
      line1[line1_len] = '\0';

      GRect name_rect = GRect(4, y, 70, FEED_LINE_HEIGHT);
      GRect line1_rect = GRect(76, y, narrow_w, FEED_LINE_HEIGHT);
      graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite));
      graphics_draw_text(ctx, items[i].prefix, prefix_font, name_rect,
                          GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      graphics_context_set_text_color(ctx, GColorWhite);
      graphics_draw_text(ctx, line1, text_font, line1_rect,
                          GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

      size_t text_len = strlen(items[i].text);
      if (split < text_len) {
        const char *line2 = items[i].text + split;
        if (*line2 == ' ') {
          line2++; // split lands on the space between words -- skip it, not part of either line
        }
        GRect line2_rect = GRect(4, y + FEED_LINE_HEIGHT, width - 8, FEED_LINE_HEIGHT);
        graphics_draw_text(ctx, line2, text_font, line2_rect,
                            GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      }

      y += row_h;
      continue;
    }

    GRect prefix_rect = GRect(4, y, 70, row_h);
    GRect text_rect = GRect(76, y, width - 80, row_h);

    if (items[i].type == ITEM_TYPE_WEATHER) {
      GRect icon_slot = GRect(4, y, 16, row_h);
      prv_draw_weather_icon(ctx, icon_slot, prv_weather_icon_for(items[i].text));
      prefix_rect = GRect(22, y, 52, row_h);
    }

    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite));
    graphics_draw_text(ctx, items[i].prefix, prefix_font, prefix_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, items[i].text, text_font, text_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    y += row_h;
  }
}

// Draws the top-of-info-area indicator for the current panel: if `title` is
// non-empty, a left-aligned label followed by a rule filling the rest of the
// width (e.g. "Events----------------"), and returns PANEL_HEADER_H so the
// caller can offset the row area below it. If `title` is empty (or there's
// only one panel, which the companion app sends untitled), draws the plain
// full-width rule from v1/v2 instead and returns 0 -- unchanged behavior for
// the common single-panel case.
static int prv_draw_panel_header(GContext *ctx, GRect bounds, const char *title) {
  if (title[0] == '\0') {
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
    return 0;
  }

  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GRect title_rect = GRect(4, 0, PANEL_HEADER_TITLE_W, PANEL_HEADER_H);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, title, title_font, title_rect,
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  int rule_y = PANEL_HEADER_H / 2;
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(4 + PANEL_HEADER_TITLE_W, rule_y), GPoint(bounds.size.w - 4, rule_y));
  return PANEL_HEADER_H;
}

static void prv_info_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GFont text_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);

  if (s_panel_count == 0) {
    // No panels configured at all: plain rule, no header to draw.
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
    GRect empty_rect = GRect(4, 6, bounds.size.w - 8, ROW_HEIGHT);
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "Nothing to see", text_font, empty_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_panel >= s_panel_count) {
    s_panel = 0;
  }
  InfoPanel *panel = &s_panels[s_panel];

  int header_h = prv_draw_panel_header(ctx, bounds, panel->title);
  int content_h_full = bounds.size.h - header_h;

  GFont prefix_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  if (panel->item_count == 0) {
    GRect empty_rect = GRect(4, header_h + 6, bounds.size.w - 8, ROW_HEIGHT);
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "Nothing to see", text_font, empty_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  if (!s_enable_pagination) {
    // Pagination off: just fill the whole layer with as many items as fit,
    // same as before the pagination feature existed. No page indicator, no
    // reserved strip, and s_page is never consulted.
    int end = prv_page_end(panel->items, panel->item_count, 0, content_h_full);
    prv_draw_rows(ctx, prefix_font, text_font, bounds.size.w, panel->items, 0, end, header_h);
    return;
  }

  int content_h;
  int num_pages = prv_layout_num_pages(panel->items, panel->item_count, content_h_full, &content_h);
  if (s_page >= num_pages) {
    s_page = 0;
  }

  int start = prv_page_start(panel->items, panel->item_count, s_page, content_h);
  int end = prv_page_end(panel->items, panel->item_count, start, content_h);
  prv_draw_rows(ctx, prefix_font, text_font, bounds.size.w, panel->items, start, end, header_h);

  if (num_pages > 1) {
    GRect indicator_slot = GRect(0, bounds.size.h - PAGE_INDICATOR_H, bounds.size.w, PAGE_INDICATOR_H);
    prv_draw_page_indicator(ctx, indicator_slot, num_pages, s_page);
  }
}

// --- Page-advance / panel-rotate gestures --------------------------------
// A wrist tap is used (rather than touch or buttons) because watchfaces get
// neither: touch is reserved for watchapps, and the system shell owns all
// four buttons while a watchface is on screen.
//
// This deliberately does NOT use accel_tap_service_subscribe(): that service
// is fed by the system's shake-detection subsystem, whose threshold is only
// adjustable via the shared "Motion Sensitivity" setting (Settings > System),
// not from app code -- and at its default it took a fairly hard knock to
// register. Sampling raw accelerometer data instead lets this watchface pick
// its own, more sensitive, threshold without touching that global setting.
//
// The tunable parameters themselves (axis mask, threshold, ringdown, and
// multi-tap window) plus ACCEL_TAP_SAMPLING_RATE/ACCEL_TAP_SAMPLES_PER_UPDATE
// are declared up near the other Clay settings, not here, since the
// AppMessage inbox handler needs to write them -- see prv_recompute_tap_params()
// for how the ms-based settings become the sample counts used below.
//
// A tap sequence starts on the first jolt and stays open, extending its wait
// window on every further jolt, until the window elapses with no new jolt;
// the sequence's final tap count then decides what to do: exactly 3 turns
// the page within the current panel (only if pagination is enabled), exactly
// 4 rotates to the next panel (only if more than one panel is active).
// Anything else (1, 2, 5+) is discarded.
#define ACCEL_TAP_PAGE_COUNT 3
#define ACCEL_TAP_PANEL_COUNT 4

static bool s_accel_have_prev = false;
static int16_t s_accel_prev_x, s_accel_prev_y, s_accel_prev_z;
static int s_accel_ringdown = 0;
static bool s_accel_tap_pending = false;
static int s_accel_tap_count = 0;
static int s_accel_pending_countdown = 0;
static bool s_accel_subscribed = false;

static void prv_advance_page(void) {
  if (!s_enable_pagination) {
    return;
  }
  InfoPanel *panel = &s_panels[s_panel];
  int header_h = panel->title[0] != '\0' ? PANEL_HEADER_H : 0;
  int content_h_full = layer_get_bounds(s_info_layer).size.h - header_h;
  int num_pages = prv_layout_num_pages(panel->items, panel->item_count, content_h_full, NULL);
  s_page = (s_page + 1) % num_pages;
  layer_mark_dirty(s_info_layer);
}

static void prv_advance_panel(void) {
  if (s_panel_count <= 1) {
    return;
  }
  s_panel = (s_panel + 1) % s_panel_count;
  s_page = 0;
  layer_mark_dirty(s_info_layer);
}

// accel_data_service_subscribe() callback: scans each new batch of raw
// samples for sudden jolts (large sample-to-sample deltas) and counts them
// into taps. See the block comment above for what each final tap count does.
static void prv_accel_data_handler(AccelData *data, uint32_t num_samples) {
  for (uint32_t i = 0; i < num_samples; i++) {
    if (data[i].did_vibrate) {
      // Our own vibration motor would otherwise look like a huge jolt.
      s_accel_have_prev = false;
      continue;
    }

    if (s_accel_ringdown > 0) {
      s_accel_ringdown--;
    }

    bool jolt = false;
    if (s_accel_have_prev) {
      int32_t dx = s_tap_axis_x ? (int32_t)(data[i].x - s_accel_prev_x) : 0;
      int32_t dy = s_tap_axis_y ? (int32_t)(data[i].y - s_accel_prev_y) : 0;
      int32_t dz = s_tap_axis_z ? (int32_t)(data[i].z - s_accel_prev_z) : 0;
      int32_t delta_sq = dx * dx + dy * dy + dz * dz;
      jolt = delta_sq > s_tap_threshold_sq && s_accel_ringdown == 0;
    }

    if (jolt) {
      s_accel_ringdown = s_tap_ringdown_samples;
      if (s_accel_tap_pending) {
        // Another tap arrived within the window -- extend the sequence.
        s_accel_tap_count++;
      } else {
        // First tap of a new sequence.
        s_accel_tap_pending = true;
        s_accel_tap_count = 1;
      }
      // Reset the wait window after every tap, not just the first, so each
      // tap in the sequence gets its own full window to be followed by the
      // next one.
      s_accel_pending_countdown = s_tap_multi_tap_window_samples;
    } else if (s_accel_tap_pending) {
      if (s_accel_pending_countdown > 0) {
        s_accel_pending_countdown--;
      } else {
        // No further tap arrived in time -- the sequence is finished.
        if (s_accel_tap_count == ACCEL_TAP_PAGE_COUNT) {
          prv_advance_page();
        } else if (s_accel_tap_count == ACCEL_TAP_PANEL_COUNT) {
          prv_advance_panel();
        }
        s_accel_tap_pending = false;
        s_accel_tap_count = 0;
      }
    }

    s_accel_prev_x = data[i].x;
    s_accel_prev_y = data[i].y;
    s_accel_prev_z = data[i].z;
    s_accel_have_prev = true;
  }
}

// Subscribes to raw accelerometer data for tap detection, resetting any
// leftover detection state first (stale samples/pending taps from before a
// gap in subscription shouldn't bleed into freshly-resumed detection).
static void prv_subscribe_accel(void) {
  s_accel_have_prev = false;
  s_accel_ringdown = 0;
  s_accel_tap_pending = false;
  s_accel_tap_count = 0;
  s_accel_pending_countdown = 0;
  accel_service_set_sampling_rate(ACCEL_TAP_SAMPLING_RATE);
  accel_data_service_subscribe(ACCEL_TAP_SAMPLES_PER_UPDATE, prv_accel_data_handler);
}

static void prv_unsubscribe_accel(void) {
  accel_data_service_unsubscribe();
}

// Keeps the accelerometer subscription in sync with whether the tap gesture
// can currently do anything: pagination needs it for the triple-tap page
// turn, and more than one active panel needs it for the quadruple-tap panel
// rotation -- either alone is enough to justify the subscription (and its
// battery cost), independently of the other -- but s_enable_accel_taps is
// the overall master switch, checked first: off means the accelerometer is
// never subscribed regardless of pagination/panel state. Safe to call
// redundantly (e.g. from both the PanelCount and EnablePagination inbox
// branches in the same settings save) since it only actually (un)subscribes
// on a real transition.
static void prv_update_accel_subscription(void) {
  bool should_subscribe = s_enable_accel_taps && (s_enable_pagination || s_panel_count > 1);
  if (should_subscribe && !s_accel_subscribed) {
    prv_subscribe_accel();
    s_accel_subscribed = true;
  } else if (!should_subscribe && s_accel_subscribed) {
    prv_unsubscribe_accel();
    s_accel_subscribed = false;
  }
}

// Charging fill-climb animation for the battery icon (see
// prv_battery_update_proc): a 1fps timer-driven phase that climbs
// 0% -> 25% -> 50% -> 75% -> 100%, one step per second, stopping at
// whichever quartile contains the real charge level before looping back to
// 0% and climbing again -- only running while actually charging/plugged in,
// so it costs nothing off the charger.
//
// BATTERY_CHARGE_ANIM_STEPS (60) is a wraparound bound for the raw phase
// counter, not the number of visible frames -- it's the LCM of the possible
// per-bracket cycle lengths (2/3/4/5, see prv_battery_update_proc), chosen
// so phase % (bracket_step + 1) always lands on a clean 0..bracket_step
// cycle regardless of when the raw counter itself wraps.
#define BATTERY_CHARGE_ANIM_STEPS 60
#define BATTERY_CHARGE_ANIM_INTERVAL_MS 1000
static AppTimer *s_battery_anim_timer = NULL;
static int s_battery_anim_phase = 0;
static bool s_battery_was_charging = false;

static void prv_battery_anim_timer_callback(void *data) {
  s_battery_anim_phase = (s_battery_anim_phase + 1) % BATTERY_CHARGE_ANIM_STEPS;
  layer_mark_dirty(s_battery_layer);
  s_battery_anim_timer = app_timer_register(BATTERY_CHARGE_ANIM_INTERVAL_MS,
                                             prv_battery_anim_timer_callback, NULL);
}

// Starts/stops the pulse timer on charging-state transitions; a no-op if
// `charging` matches the current state already.
static void prv_battery_set_charging_anim(bool charging) {
  if (charging == s_battery_was_charging) {
    return;
  }
  s_battery_was_charging = charging;
  if (charging) {
    s_battery_anim_phase = 0;
    s_battery_anim_timer = app_timer_register(BATTERY_CHARGE_ANIM_INTERVAL_MS,
                                               prv_battery_anim_timer_callback, NULL);
  } else if (s_battery_anim_timer) {
    app_timer_cancel(s_battery_anim_timer);
    s_battery_anim_timer = NULL;
  }
}

// Battery icon: outline + a fill bar. Not charging: fill proportional to the
// real charge, red when low (<=20%). Charging and not yet full: fill climbs
// at 1fps through 0% -> 25% -> 50% -> 75% -> whichever of those is the top
// of the quartile bracket containing the real charge, then loops back to 0%
// and climbs again -- e.g. at 40% (25-50% bracket) the sequence is
// 0, 25, 50, 0, 25, 50, ... Charging and full (100%): static, solid green
// fill, no more animation since there's nothing left to indicate.
static void prv_battery_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  BatteryChargeState state = battery_state_service_peek();
  int percent = state.charge_percent;
  bool charging = state.is_charging || state.is_plugged;

  const int body_w = 22;
  const int body_h = 13;
  const int nub_w = 2;
  const int nub_h = 5;
  const int body_x = 0;
  const int body_y = (bounds.size.h - body_h) / 2;

  GRect body_rect = GRect(body_x, body_y, body_w, body_h);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, body_rect);

  GRect nub_rect = GRect(body_x + body_w, body_y + (body_h - nub_h) / 2, nub_w, nub_h);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, nub_rect, 0, GCornerNone);

  int display_percent;
  GColor fill_color;
  if (!charging) {
    display_percent = percent;
    fill_color = (percent <= 20) ? GColorRed : GColorWhite;
  } else if (percent >= 100) {
    display_percent = 100;
    fill_color = GColorGreen;
  } else {
    int bracket_step = (percent <= 25) ? 1 : (percent <= 50) ? 2 : (percent <= 75) ? 3 : 4;
    int frame_index = s_battery_anim_phase % (bracket_step + 1);
    display_percent = frame_index * 25;
    fill_color = GColorWhite;
  }

  // Fill inset 2px inside the outline, width proportional to display_percent.
  const int pad = 2;
  int fill_max_w = body_w - 2 * pad;
  int fill_w = (fill_max_w * display_percent) / 100;
  if (fill_w < 0) {
    fill_w = 0;
  }
  if (fill_w > fill_max_w) {
    fill_w = fill_max_w;
  }

  graphics_context_set_fill_color(ctx, fill_color);
  if (fill_w > 0) {
    graphics_fill_rect(ctx, GRect(body_x + pad, body_y + pad, fill_w, body_h - 2 * pad), 0, GCornerNone);
  }
}

static void prv_battery_handler(BatteryChargeState charge) {
  prv_battery_set_charging_anim(charge.is_charging || charge.is_plugged);
  layer_mark_dirty(s_battery_layer);
}

// ---------------------------------------------------------------------------
// Notification area (top-left corner): a small, left-aligned row of status
// icons that appear only when relevant. Each entry below is independent --
// whether it's shown (is_active) and how it's drawn (draw) into its
// allotted slot. To add a new notification icon: write an is_active/draw
// pair and add a row to NOTIFICATION_ICONS; the layout packs itself.
// ---------------------------------------------------------------------------

// Quiet-time icon: a crescent moon. There's no subscribe/event API for
// quiet time (only the peek-style quiet_time_is_active()), so this is
// re-checked on every minute tick alongside the clock, not pushed on
// change like the Bluetooth connection state is.
static bool prv_quiet_time_should_show(void) {
  return s_show_quiet_time && quiet_time_is_active();
}

static void prv_draw_quiet_time_icon(GContext *ctx, GRect slot) {
  const int radius = 6;
  GPoint center = GPoint(slot.origin.x + radius, slot.origin.y + slot.size.h / 2);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, center, radius);

  // Punch out a crescent by overpainting with the (always-black) window
  // background, offset up and to the right.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(center.x + 3, center.y - 2), radius);
}

// Bluetooth-disconnected icon: the "ᛒ" rune. Construction (see the real
// logo): a vertical spine, two triangles on the RIGHT (upper + lower flags),
// and two arms on the LEFT that run straight from a flag tip through the
// centre out to the opposite corner -- NOT triangles. Concretely five
// strokes, where the two long diagonals cross the spine exactly at centre:
//   spine   T -> Bo
//   flag    T -> UR      (upper-right flag, top edge)
//   diag    UR -> LL     (upper-right tip through centre to lower-left arm)
//   flag    Bo -> LR     (lower-right flag, bottom edge)
//   diag    LR -> UL     (lower-right tip through centre to upper-left arm)
static bool prv_bluetooth_should_show(void) {
  return s_show_bluetooth_alert && !connection_service_peek_pebble_app_connection();
}

static void prv_draw_bluetooth_icon(GContext *ctx, GRect slot) {
  const int icon_w = 10;
  const int icon_h = 14;
  const int ix = slot.origin.x;
  const int iy = slot.origin.y + (slot.size.h - icon_h) / 2;
  const int cx = ix + icon_w / 2;
  const int left = ix;
  const int right = ix + icon_w;
  const int top = iy;
  const int bottom = iy + icon_h;
  const int q1 = iy + icon_h / 4;
  const int q3 = iy + 3 * icon_h / 4;

  const GPoint T  = { cx, top };
  const GPoint Bo = { cx, bottom };
  const GPoint UR = { right, q1 };
  const GPoint LR = { right, q3 };
  const GPoint UL = { left, q1 };
  const GPoint LL = { left, q3 };

  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_context_set_antialiased(ctx, false);
  graphics_draw_line(ctx, T, Bo);   // spine
  graphics_draw_line(ctx, T, UR);   // upper-right flag top edge
  graphics_draw_line(ctx, UR, LL);  // upper-right tip -> centre -> lower-left arm
  graphics_draw_line(ctx, Bo, LR);  // lower-right flag bottom edge
  graphics_draw_line(ctx, LR, UL);  // lower-right tip -> centre -> upper-left arm
  graphics_context_set_antialiased(ctx, true);
}

typedef struct {
  bool (*should_show)(void);
  void (*draw)(GContext *ctx, GRect slot);
  int slot_width;
} NotificationIcon;

static const NotificationIcon NOTIFICATION_ICONS[] = {
  { prv_quiet_time_should_show, prv_draw_quiet_time_icon, 13 },
  { prv_bluetooth_should_show, prv_draw_bluetooth_icon, 12 },
};
#define NUM_NOTIFICATION_ICONS (int)(sizeof(NOTIFICATION_ICONS) / sizeof(NOTIFICATION_ICONS[0]))

static void prv_notification_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  const int icon_gap = 4;
  int x = 0;

  for (int i = 0; i < NUM_NOTIFICATION_ICONS; i++) {
    if (!NOTIFICATION_ICONS[i].should_show()) {
      continue;
    }
    GRect slot = GRect(x, 0, NOTIFICATION_ICONS[i].slot_width, bounds.size.h);
    NOTIFICATION_ICONS[i].draw(ctx, slot);
    x += NOTIFICATION_ICONS[i].slot_width + icon_gap;
  }
}

// Deliberately obtrusive disconnect buzz: four long pulses (on/off ms,
// starting with "on"). Meant to be hard to miss versus a stock double pulse.
static const uint32_t s_disconnect_vibe_segments[] = {
  500, 200, 500, 200, 500, 200, 700,
};

// connection_service_subscribe() callback. Marks the notification area
// dirty (it re-checks the live connection state itself via peek in
// prv_bluetooth_should_show()) and fires the vibration alert -- but only on a
// genuine connected -> disconnected transition, so priming at launch and
// reconnects stay silent.
static void prv_bluetooth_handler(bool connected) {
  layer_mark_dirty(s_notification_layer);
  if (s_bt_connected && !connected && s_show_bluetooth_alert) {
    VibePattern pattern = {
      .durations = s_disconnect_vibe_segments,
      .num_segments = ARRAY_LENGTH(s_disconnect_vibe_segments),
    };
    vibes_enqueue_custom_pattern(pattern);
  }
  s_bt_connected = connected;
}

static void prv_update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  strftime(s_time_buf, sizeof(s_time_buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  text_layer_set_text(s_time_layer, s_time_buf);

  strftime(s_date_buf, sizeof(s_date_buf), "%A %Y-%m-%d", t);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_time();
  layer_mark_dirty(s_notification_layer);
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int mid = bounds.size.h / 2;

  // Corner/notification icons occupy their own row above the clock so
  // neither ever draws over the other. Kept short so the clock below still
  // gets its full font height (a taller row squeezes the time and clips the
  // LECO_60 glyphs at the bottom).
  const int icon_row_margin = 3;
  const int icon_row_h = 14;
  const int icon_row_gap = 3;
  const int top_margin = icon_row_margin + icon_row_h + icon_row_gap;

  // TextLayer draws text top-anchored, not vertically centered, so the
  // clock box must be sized to the font's actual height rather than
  // stretched to fill the top half -- otherwise the extra height just
  // becomes dead space below the digits. Clamp to the space actually
  // available so smaller screens still don't overlap the date.
  const int date_height = DATE_HEIGHT;
  const int gap = 4;
  int available = mid - date_height - top_margin - gap;
  int time_height = available < TIME_HEIGHT_WANTED ? available : TIME_HEIGHT_WANTED;

  s_time_layer = text_layer_create(GRect(0, top_margin, bounds.size.w, time_height));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(TIME_FONT_KEY));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_date_layer = text_layer_create(GRect(0, top_margin + time_height + gap, bounds.size.w, date_height));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorLightGray);
  text_layer_set_font(s_date_layer, fonts_get_system_font(DATE_FONT_KEY));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_date_layer));

  const int corner_icon_w = 24;

  s_battery_layer = layer_create(GRect(bounds.size.w - corner_icon_w - icon_row_margin,
                                        icon_row_margin, corner_icon_w, icon_row_h));
  layer_set_update_proc(s_battery_layer, prv_battery_update_proc);
  layer_set_hidden(s_battery_layer, !s_show_battery);
  layer_add_child(window_layer, s_battery_layer);

  const int notification_area_w = 40;
  s_notification_layer = layer_create(GRect(icon_row_margin, icon_row_margin,
                                             notification_area_w, icon_row_h));
  layer_set_update_proc(s_notification_layer, prv_notification_update_proc);
  layer_add_child(window_layer, s_notification_layer);

  s_info_layer = layer_create(GRect(0, mid, bounds.size.w, bounds.size.h - mid));
  layer_set_update_proc(s_info_layer, prv_info_update_proc);
  layer_add_child(window_layer, s_info_layer);
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_battery_layer);
  layer_destroy(s_notification_layer);
  layer_destroy(s_info_layer);
}

static void prv_init(void) {
  prv_load_cached_panels();

  if (persist_exists(PERSIST_KEY_SHOW_BATTERY)) {
    s_show_battery = persist_read_bool(PERSIST_KEY_SHOW_BATTERY);
  }
  if (persist_exists(PERSIST_KEY_SHOW_QUIET_TIME)) {
    s_show_quiet_time = persist_read_bool(PERSIST_KEY_SHOW_QUIET_TIME);
  }
  if (persist_exists(PERSIST_KEY_SHOW_BLUETOOTH)) {
    s_show_bluetooth_alert = persist_read_bool(PERSIST_KEY_SHOW_BLUETOOTH);
  }
  if (persist_exists(PERSIST_KEY_TAP_AXIS_X)) {
    s_tap_axis_x = persist_read_bool(PERSIST_KEY_TAP_AXIS_X);
  }
  if (persist_exists(PERSIST_KEY_TAP_AXIS_Y)) {
    s_tap_axis_y = persist_read_bool(PERSIST_KEY_TAP_AXIS_Y);
  }
  if (persist_exists(PERSIST_KEY_TAP_AXIS_Z)) {
    s_tap_axis_z = persist_read_bool(PERSIST_KEY_TAP_AXIS_Z);
  }
  if (persist_exists(PERSIST_KEY_TAP_THRESHOLD_MG)) {
    s_tap_threshold_mg = persist_read_int(PERSIST_KEY_TAP_THRESHOLD_MG);
  }
  if (persist_exists(PERSIST_KEY_TAP_RINGDOWN_MS)) {
    s_tap_ringdown_ms = persist_read_int(PERSIST_KEY_TAP_RINGDOWN_MS);
  }
  if (persist_exists(PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS)) {
    s_tap_multi_tap_window_ms = persist_read_int(PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS);
  }
  if (persist_exists(PERSIST_KEY_ENABLE_PAGINATION)) {
    s_enable_pagination = persist_read_bool(PERSIST_KEY_ENABLE_PAGINATION);
  }
  if (persist_exists(PERSIST_KEY_ENABLE_ACCEL_TAPS)) {
    s_enable_accel_taps = persist_read_bool(PERSIST_KEY_ENABLE_ACCEL_TAPS);
  }
#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
  if (persist_exists(PERSIST_KEY_PANEL_ROTATION_EVENT)) {
    int value = persist_read_int(PERSIST_KEY_PANEL_ROTATION_EVENT);
    if (value >= QUICK_LAUNCH_ROTATION_EVENT_NONE && value <= QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK) {
      s_panel_rotation_event = (QuickLaunchRotationEvent)value;
    }
  }
  if (persist_exists(PERSIST_KEY_PAGE_ROTATION_EVENT)) {
    int value = persist_read_int(PERSIST_KEY_PAGE_ROTATION_EVENT);
    if (value >= QUICK_LAUNCH_ROTATION_EVENT_NONE && value <= QUICK_LAUNCH_ROTATION_EVENT_HOLD_BACK) {
      s_page_rotation_event = (QuickLaunchRotationEvent)value;
    }
  }
#endif  // PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
  prv_recompute_tap_params();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_register_inbox_dropped(prv_inbox_dropped_handler);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  battery_state_service_subscribe(prv_battery_handler);
  BatteryChargeState initial_battery = battery_state_service_peek();
  prv_battery_set_charging_anim(initial_battery.is_charging || initial_battery.is_plugged);

  // Seed the connection tracker without buzzing. The icon itself renders from
  // a live peek in prv_bluetooth_should_show(), so the first window draw is
  // already correct; we only need s_bt_connected primed so the handler can
  // detect the next real disconnect transition.
  s_bt_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_bluetooth_handler,
  });

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  prv_update_time();

#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
  // Independent of (and additional to) the wrist-tap gestures below: fires only when a
  // quick-launch button is explicitly configured, in Settings, to target this watchface -- see
  // prv_quick_launch_button_handler.
  quick_launch_button_service_subscribe(prv_quick_launch_button_handler);
#endif

  // Watchfaces get no touch or button input; a wrist tap is the only
  // gesture available, so it drives both info-feed pagination (triple tap)
  // and panel rotation (quadruple tap) -- see the comment above
  // prv_accel_data_handler. Subscribed based on the cached settings/panel
  // count loaded above, same logic as any later settings/panel-count change.
  prv_update_accel_subscription();
}

static void prv_deinit(void) {
  if (s_accel_subscribed) {
    prv_unsubscribe_accel();
  }
  if (s_battery_anim_timer) {
    app_timer_cancel(s_battery_anim_timer);
    s_battery_anim_timer = NULL;
  }
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  tick_timer_service_unsubscribe();
#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE
  quick_launch_button_service_unsubscribe();
#endif
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
