#include <pebble.h>

// FONT_KEY_LECO_60_NUMBERS_AM_PM only exists on the newer color
// platforms; older ones fall back to the largest font they do have.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT) || defined(PBL_PLATFORM_GABBRO)
#define TIME_FONT_KEY FONT_KEY_LECO_60_NUMBERS_AM_PM
#define TIME_HEIGHT_WANTED 64
#else
#define TIME_FONT_KEY FONT_KEY_ROBOTO_BOLD_SUBSET_49
#define TIME_HEIGHT_WANTED 54
#endif

// ---------------------------------------------------------------------------
// Info Watchface
//
// Top half:    digital clock (HH:MM) + date (Weekday YYYY-MM-DD), battery
//              icon top-right, notification area top-left (quiet-time,
//              Bluetooth-disconnected, ... packed left-aligned as needed).
// Bottom half: a generic scrolling-capable info feed. The item model below
// (InfoItem) is intentionally source-agnostic: a companion app pushes items
// here from calendars, RSS/feeds, social streams, notifications, etc. via
// AppMessage. See PROTOCOL.md for the wire format. The most recently
// received set of items is cached to persistent storage and shown on
// launch, before the first message of a given session arrives; an empty
// cache (or an empty update from the phone) renders as "Nothing to see".
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
// Info feed cache: PERSIST_KEY_ITEM_COUNT holds the item count, and each
// item i is stored under its own key (PERSIST_KEY_ITEM_BASE + i) as a raw
// InfoItem blob -- one key per item rather than one blob for the whole
// list because PERSIST_DATA_MAX_LENGTH (256 bytes) is smaller than
// MAX_INFO_ITEMS * sizeof(InfoItem).
#define PERSIST_KEY_ITEM_COUNT 4
#define PERSIST_KEY_ITEM_BASE 10
// Tap-gesture tuning (see PROTOCOL.md): kept well clear of PERSIST_KEY_ITEM_BASE's
// range so raising MAX_INFO_ITEMS later can't collide with these.
#define PERSIST_KEY_TAP_AXIS_X 20
#define PERSIST_KEY_TAP_AXIS_Y 21
#define PERSIST_KEY_TAP_AXIS_Z 22
#define PERSIST_KEY_TAP_THRESHOLD_MG 23
#define PERSIST_KEY_TAP_RINGDOWN_MS 24
#define PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS 25
#define PERSIST_KEY_ENABLE_PAGINATION 26
static bool s_show_battery = true;
static bool s_show_quiet_time = true;
static bool s_show_bluetooth_alert = true;

// Master switch for the whole pagination feature (info-feed paging + the
// triple-tap gesture that drives it). Off by default: the info feed just
// shows as many events as fit on one screen, same as before pagination
// existed, and the accelerometer isn't even subscribed to (see prv_init()/
// prv_inbox_received_handler(), which subscribe/unsubscribe on transitions
// of this flag -- no point sampling the accelerometer for a gesture that
// can't do anything).
static bool s_enable_pagination = false;

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
// prefix/text; event/other rows render the prefix + text columns.
#define ITEM_TYPE_DIVIDER 0
#define ITEM_TYPE_EVENT 1
#define ITEM_TYPE_OTHER 255

// Generic info item: `prefix` is a short left column (a weekday+time, a source
// tag like "RSS", etc.), `text` is the main line (event title, headline, ...).
typedef struct {
  uint8_t type;
  char prefix[12];
  char text[40];
} InfoItem;

#define MAX_INFO_ITEMS 8
static InfoItem s_items[MAX_INFO_ITEMS];
static int s_item_count = 0;

// Current page of the info feed (0-based). Any accelerometer tap advances to
// the next page, wrapping back to 0 after the last one.
static int s_page = 0;

static const int ROW_HEIGHT = 22;
static const int DIVIDER_ROW_HEIGHT = 12;
// Reserved strip at the bottom of the info layer for the page-dot indicator,
// only actually consumed (i.e. subtracted from the pagination height) when
// there's more than one page.
static const int PAGE_INDICATOR_H = 10;

static void prv_persist_item(int i) {
  persist_write_data(PERSIST_KEY_ITEM_BASE + i, &s_items[i], sizeof(InfoItem));
}

static void prv_persist_item_count(void) {
  persist_write_int(PERSIST_KEY_ITEM_COUNT, s_item_count);
}

// Loads the last-cached item list from persistent storage, so the watchface
// shows real (if possibly stale) data immediately on launch rather than
// waiting for the phone. No cache yet (fresh install) or an empty cached
// list both leave s_item_count at 0, which prv_info_update_proc renders as
// "Nothing to see".
static void prv_load_cached_items(void) {
  s_item_count = 0;
  if (!persist_exists(PERSIST_KEY_ITEM_COUNT)) {
    return;
  }

  int count = persist_read_int(PERSIST_KEY_ITEM_COUNT);
  if (count > MAX_INFO_ITEMS) {
    count = MAX_INFO_ITEMS;
  }
  for (int i = 0; i < count; i++) {
    persist_read_data(PERSIST_KEY_ITEM_BASE + i, &s_items[i], sizeof(InfoItem));
  }
  s_item_count = count;
}

// Defined further down, after prv_accel_data_handler (which they reference).
// Forward declared here because prv_inbox_received_handler and prv_init need
// to subscribe/unsubscribe the accelerometer on EnablePagination transitions.
static void prv_subscribe_accel(void);
static void prv_unsubscribe_accel(void);

// AppMessage inbox: see PROTOCOL.md for the full contract. Messages arrive
// in one of these shapes:
//   - {ShowBattery: 0|1, ShowQuietTime: 0|1, ShowBluetooth: 0|1, EnablePagination: 0|1,
//      TapAxisX/Y/Z: 0|1, TapThresholdMg/TapRingdownMs/TapMultiTapWindowMs: N,
//      ServerUrl: "..."} (any subset) -- from the Clay settings page, all
//      changed fields in one message
//   - {ItemCount: N}                                  -- resets the list
//   - {ItemIndex: i, ItemPrefix: "...", ItemText: "..."} -- one item
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
      if (s_enable_pagination) {
        prv_subscribe_accel();
      } else {
        prv_unsubscribe_accel();
        s_page = 0;
      }
      layer_mark_dirty(s_info_layer);
    }
    handled_setting = true;
  }

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

  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_ItemCount);
  if (count_tuple) {
    int count = count_tuple->value->uint8;
    if (count > MAX_INFO_ITEMS) {
      count = MAX_INFO_ITEMS;
    }
    s_item_count = count;
    prv_persist_item_count();
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

  // ItemType is optional for back-compat: a message without it is an event.
  // Dividers may omit (or send empty) prefix/text.
  Tuple *type_tuple = dict_find(iterator, MESSAGE_KEY_ItemType);
  s_items[index].type = type_tuple ? type_tuple->value->uint8 : ITEM_TYPE_EVENT;

  Tuple *prefix_tuple = dict_find(iterator, MESSAGE_KEY_ItemPrefix);
  if (prefix_tuple) {
    strncpy(s_items[index].prefix, prefix_tuple->value->cstring, sizeof(s_items[index].prefix) - 1);
    s_items[index].prefix[sizeof(s_items[index].prefix) - 1] = '\0';
  } else {
    s_items[index].prefix[0] = '\0';
  }

  Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_ItemText);
  if (text_tuple) {
    strncpy(s_items[index].text, text_tuple->value->cstring, sizeof(s_items[index].text) - 1);
    s_items[index].text[sizeof(s_items[index].text) - 1] = '\0';
  } else {
    s_items[index].text[0] = '\0';
  }

  if (index + 1 > s_item_count) {
    s_item_count = index + 1;
    prv_persist_item_count();
  }
  prv_persist_item(index);
  layer_mark_dirty(s_info_layer);
}

static void prv_inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage inbox dropped, reason: %d", (int)reason);
}

// --- Pagination -------------------------------------------------------
// The info feed fills a page with as many rows as fit in `height`, then
// spills the rest onto the next page. These helpers share the exact same
// row-height rules as the draw loop below so a page's contents always match
// what was measured.

// Index of the first item that does NOT fit on a page starting at `start`
// within `height` pixels. Always admits at least one item (even if it alone
// overflows `height`) so an oversized row can't stall pagination.
static int prv_page_end(int start, int height) {
  int y = 6;
  int i = start;
  while (i < s_item_count) {
    int row_h = (s_items[i].type == ITEM_TYPE_DIVIDER) ? DIVIDER_ROW_HEIGHT : ROW_HEIGHT;
    if (i > start && y + row_h > height) {
      break;
    }
    y += row_h;
    i++;
  }
  return i;
}

static int prv_num_pages(int height) {
  if (s_item_count == 0) {
    return 1;
  }
  int pages = 0;
  int start = 0;
  while (start < s_item_count) {
    start = prv_page_end(start, height);
    pages++;
  }
  return pages;
}

static int prv_page_start(int page, int height) {
  int start = 0;
  for (int p = 0; p < page && start < s_item_count; p++) {
    start = prv_page_end(start, height);
  }
  return start;
}

// Shared by the draw proc and the tap handler so both agree on how many
// pages there are: reserve the indicator strip only if the items actually
// need more than one page at the full layer height, otherwise let rows use
// the whole layer (and there's exactly one page).
static int prv_layout_num_pages(int full_height, int *out_content_h) {
  int content_h = full_height;
  int num_pages = prv_num_pages(content_h);
  if (num_pages > 1) {
    content_h = full_height - PAGE_INDICATOR_H;
    num_pages = prv_num_pages(content_h);
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

// Draws items[start, end) top-anchored at y=6, at the given width. Shared by
// both the paginated and non-paginated rendering paths in
// prv_info_update_proc so they can't drift apart.
static void prv_draw_rows(GContext *ctx, GFont prefix_font, GFont text_font, int width,
                           int start, int end) {
  int y = 6;
  for (int i = start; i < end; i++) {
    bool is_divider = s_items[i].type == ITEM_TYPE_DIVIDER;
    int row_h = is_divider ? DIVIDER_ROW_HEIGHT : ROW_HEIGHT;

    if (is_divider) {
      int line_y = y + row_h / 2;
      graphics_context_set_stroke_color(ctx, GColorLightGray);
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_line(ctx, GPoint(4, line_y), GPoint(width - 4, line_y));
      y += row_h;
      continue;
    }

    GRect prefix_rect = GRect(4, y, 70, row_h);
    GRect text_rect = GRect(76, y, width - 80, row_h);

    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite));
    graphics_draw_text(ctx, s_items[i].prefix, prefix_font, prefix_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_items[i].text, text_font, text_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    y += row_h;
  }
}

static void prv_info_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Divider along the top edge of the info feed.
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));

  GFont prefix_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont text_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);

  if (s_item_count == 0) {
    GRect empty_rect = GRect(4, 6, bounds.size.w - 8, ROW_HEIGHT);
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "Nothing to see", text_font, empty_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  if (!s_enable_pagination) {
    // Pagination off: just fill the whole layer with as many items as fit,
    // same as before the pagination feature existed. No page indicator, no
    // reserved strip, and s_page is never consulted.
    int end = prv_page_end(0, bounds.size.h);
    prv_draw_rows(ctx, prefix_font, text_font, bounds.size.w, 0, end);
    return;
  }

  int content_h;
  int num_pages = prv_layout_num_pages(bounds.size.h, &content_h);
  if (s_page >= num_pages) {
    s_page = 0;
  }

  int start = prv_page_start(s_page, content_h);
  int end = prv_page_end(start, content_h);
  prv_draw_rows(ctx, prefix_font, text_font, bounds.size.w, start, end);

  if (num_pages > 1) {
    GRect indicator_slot = GRect(0, bounds.size.h - PAGE_INDICATOR_H, bounds.size.w, PAGE_INDICATOR_H);
    prv_draw_page_indicator(ctx, indicator_slot, num_pages, s_page);
  }
}

// --- Page-advance gesture -----------------------------------------------
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
// Only a TRIPLE tap turns the page -- a single tap or a double tap is
// deliberately ignored. A tap sequence starts on the first jolt and stays
// open, extending its wait window on every further jolt, until the window
// elapses with no new jolt; the sequence's final tap count then decides
// whether to act (exactly 3) or discard (anything else, including 1, 2, or
// 4+).
#define ACCEL_TAP_TARGET_COUNT 3

static bool s_accel_have_prev = false;
static int16_t s_accel_prev_x, s_accel_prev_y, s_accel_prev_z;
static int s_accel_ringdown = 0;
static bool s_accel_tap_pending = false;
static int s_accel_tap_count = 0;
static int s_accel_pending_countdown = 0;

static void prv_advance_page(void) {
  int num_pages = prv_layout_num_pages(layer_get_bounds(s_info_layer).size.h, NULL);
  s_page = (s_page + 1) % num_pages;
  layer_mark_dirty(s_info_layer);
}

// accel_data_service_subscribe() callback: scans each new batch of raw
// samples for sudden jolts (large sample-to-sample deltas) and counts them
// into taps. The page only turns when a sequence's final count is exactly
// ACCEL_TAP_TARGET_COUNT (3) -- see the block comment above.
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
        if (s_accel_tap_count == ACCEL_TAP_TARGET_COUNT) {
          prv_advance_page();
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
// gap in subscription -- e.g. pagination having been off for a while --
// shouldn't bleed into freshly-resumed detection). Called from prv_init()
// (if EnablePagination starts enabled) and from prv_inbox_received_handler()
// on an EnablePagination off->on transition.
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

// Battery icon: outline + a fill bar proportional to charge, red when
// charge is low (<=20%) and not charging, plus a small lightning bolt
// overlay while charging/plugged in.
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

  // Fill inset 2px inside the outline, width proportional to charge.
  const int pad = 2;
  int fill_max_w = body_w - 2 * pad;
  int fill_w = (fill_max_w * percent) / 100;
  if (fill_w < 0) {
    fill_w = 0;
  }
  if (fill_w > fill_max_w) {
    fill_w = fill_max_w;
  }

  GColor fill_color = (percent <= 20 && !charging) ? GColorRed : GColorWhite;
  graphics_context_set_fill_color(ctx, fill_color);
  if (fill_w > 0) {
    graphics_fill_rect(ctx, GRect(body_x + pad, body_y + pad, fill_w, body_h - 2 * pad), 0, GCornerNone);
  }

  if (charging) {
    GPoint bolt_points[] = {
      {body_x + 12, body_y + 1},
      {body_x + 7, body_y + 8},
      {body_x + 11, body_y + 8},
      {body_x + 8, body_y + 12},
      {body_x + 15, body_y + 5},
      {body_x + 11, body_y + 5},
    };
    GPathInfo bolt_info = {
      .num_points = 6,
      .points = bolt_points,
    };
    GPath *bolt_path = gpath_create(&bolt_info);
    graphics_context_set_fill_color(ctx, GColorBlack);
    gpath_draw_filled(ctx, bolt_path);
    gpath_destroy(bolt_path);
  }
}

static void prv_battery_handler(BatteryChargeState charge) {
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
  const int date_height = 26;
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
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
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
  prv_load_cached_items();

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

  // Watchfaces get no touch or button input; a wrist tap is the only
  // gesture available, so it drives info-feed pagination. See the comment
  // above prv_accel_data_handler for why this samples raw data instead of
  // using accel_tap_service_subscribe(). Only subscribed at all if
  // pagination is enabled -- see s_enable_pagination's comment.
  if (s_enable_pagination) {
    prv_subscribe_accel();
  }
}

static void prv_deinit(void) {
  if (s_enable_pagination) {
    prv_unsubscribe_accel();
  }
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
