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
// AppMessage. See PROTOCOL.md for the wire format. Dummy data is shown
// until the first real message arrives.
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
static bool s_show_battery = true;
static bool s_show_quiet_time = true;
static bool s_show_bluetooth_alert = true;

// Generic info item: `prefix` is a short left column (a time, a source tag
// like "RSS", etc.), `text` is the main line (event title, headline, ...).
typedef struct {
  char prefix[8];
  char text[40];
} InfoItem;

#define MAX_INFO_ITEMS 8
static InfoItem s_items[MAX_INFO_ITEMS];
static int s_item_count = 0;

static const int ROW_HEIGHT = 22;

static void prv_load_dummy_items(void) {
  s_item_count = 0;

  strncpy(s_items[s_item_count].prefix, "09:00", sizeof(s_items[s_item_count].prefix));
  strncpy(s_items[s_item_count].text, "Standup", sizeof(s_items[s_item_count].text));
  s_item_count++;

  strncpy(s_items[s_item_count].prefix, "12:30", sizeof(s_items[s_item_count].prefix));
  strncpy(s_items[s_item_count].text, "Lunch w/ Sam", sizeof(s_items[s_item_count].text));
  s_item_count++;

  strncpy(s_items[s_item_count].prefix, "15:00", sizeof(s_items[s_item_count].prefix));
  strncpy(s_items[s_item_count].text, "1:1", sizeof(s_items[s_item_count].text));
  s_item_count++;

  strncpy(s_items[s_item_count].prefix, "18:30", sizeof(s_items[s_item_count].prefix));
  strncpy(s_items[s_item_count].text, "Gym", sizeof(s_items[s_item_count].text));
  s_item_count++;
}

// AppMessage inbox: see PROTOCOL.md for the full contract. Messages arrive
// in one of these shapes:
//   - {ShowBattery: 0|1, ShowQuietTime: 0|1, ShowBluetooth: 0|1,
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

  if (handled_setting) {
    return;
  }

  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_ItemCount);
  if (count_tuple) {
    int count = count_tuple->value->uint8;
    if (count > MAX_INFO_ITEMS) {
      count = MAX_INFO_ITEMS;
    }
    s_item_count = count;
    layer_mark_dirty(s_info_layer);
    return;
  }

  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_ItemIndex);
  Tuple *prefix_tuple = dict_find(iterator, MESSAGE_KEY_ItemPrefix);
  Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_ItemText);
  if (!index_tuple || !prefix_tuple || !text_tuple) {
    return;
  }

  int index = index_tuple->value->uint8;
  if (index >= MAX_INFO_ITEMS) {
    return;
  }

  strncpy(s_items[index].prefix, prefix_tuple->value->cstring, sizeof(s_items[index].prefix) - 1);
  s_items[index].prefix[sizeof(s_items[index].prefix) - 1] = '\0';

  strncpy(s_items[index].text, text_tuple->value->cstring, sizeof(s_items[index].text) - 1);
  s_items[index].text[sizeof(s_items[index].text) - 1] = '\0';

  if (index + 1 > s_item_count) {
    s_item_count = index + 1;
  }
  layer_mark_dirty(s_info_layer);
}

static void prv_inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage inbox dropped, reason: %d", (int)reason);
}

static void prv_info_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Divider along the top edge of the info feed.
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));

  GFont prefix_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont text_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);

  int y = 6;
  for (int i = 0; i < s_item_count; i++) {
    if (y + ROW_HEIGHT > bounds.size.h) {
      // Doesn't fit in the visible area; later this becomes a scroll offset
      // rather than a hard stop.
      break;
    }

    GRect prefix_rect = GRect(4, y, 50, ROW_HEIGHT);
    GRect text_rect = GRect(56, y, bounds.size.w - 60, ROW_HEIGHT);

    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite));
    graphics_draw_text(ctx, s_items[i].prefix, prefix_font, prefix_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_items[i].text, text_font, text_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    y += ROW_HEIGHT;
  }
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

// connection_service_subscribe() callback. Marks the notification area
// dirty (it re-checks the live connection state itself via peek in
// prv_bluetooth_should_show()) and fires the vibration alert on the
// transition to disconnected.
static void prv_bluetooth_handler(bool connected) {
  layer_mark_dirty(s_notification_layer);
  if (!connected && s_show_bluetooth_alert) {
    vibes_double_pulse();
  }
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
  prv_load_dummy_items();

  if (persist_exists(PERSIST_KEY_SHOW_BATTERY)) {
    s_show_battery = persist_read_bool(PERSIST_KEY_SHOW_BATTERY);
  }
  if (persist_exists(PERSIST_KEY_SHOW_QUIET_TIME)) {
    s_show_quiet_time = persist_read_bool(PERSIST_KEY_SHOW_QUIET_TIME);
  }
  if (persist_exists(PERSIST_KEY_SHOW_BLUETOOTH)) {
    s_show_bluetooth_alert = persist_read_bool(PERSIST_KEY_SHOW_BLUETOOTH);
  }

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

  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_bluetooth_handler,
  });
  // Show the correct state from the start rather than waiting for the next
  // connection change event.
  prv_bluetooth_handler(connection_service_peek_pebble_app_connection());

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  prv_update_time();
}

static void prv_deinit(void) {
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
