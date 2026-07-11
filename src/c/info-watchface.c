#include <pebble.h>

// FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM only exists on the newer color
// platforms; older ones fall back to the largest font they do have.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT) || defined(PBL_PLATFORM_GABBRO)
#define TIME_FONT_KEY FONT_KEY_LECO_60_BOLD_NUMBERS_AM_PM
#define TIME_HEIGHT_WANTED 64
#else
#define TIME_FONT_KEY FONT_KEY_ROBOTO_BOLD_SUBSET_49
#define TIME_HEIGHT_WANTED 54
#endif

// ---------------------------------------------------------------------------
// Info Watchface
//
// Top half:    digital clock (HH:MM) + date (Weekday YYYY-MM-DD)
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

static char s_time_buf[8];
static char s_date_buf[24];

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

// AppMessage inbox: see PROTOCOL.md for the full contract. Two shapes of
// message arrive from the companion app:
//   - {ItemCount: N}                                  -- resets the list
//   - {ItemIndex: i, ItemPrefix: "...", ItemText: "..."} -- one item
static void prv_inbox_received_handler(DictionaryIterator *iterator, void *context) {
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
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  int mid = bounds.size.h / 2;

  // TextLayer draws text top-anchored, not vertically centered, so the
  // clock box must be sized to the font's actual height rather than
  // stretched to fill the top half -- otherwise the extra height just
  // becomes dead space below the digits. Clamp to the space actually
  // available so smaller screens still don't overlap the date.
  const int top_margin = 4;
  const int date_height = 28;
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

  s_info_layer = layer_create(GRect(0, mid, bounds.size.w, bounds.size.h - mid));
  layer_set_update_proc(s_info_layer, prv_info_update_proc);
  layer_add_child(window_layer, s_info_layer);
}

static void prv_window_unload(Window *window) {
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
  layer_destroy(s_info_layer);
}

static void prv_init(void) {
  prv_load_dummy_items();

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

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  prv_update_time();
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
