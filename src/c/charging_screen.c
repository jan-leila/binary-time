#include "charging_screen.h"

#define PERSIST_KEY_CHARGE 5

// The battery service reports the charge in 10 % steps and nothing about current or voltage, so
// the time until full and the power are estimated from when the percentage steps up: the rate is
// taken over the last STEPS step changes (the charge slows down near full, older steps would make
// the estimate optimistic).
#define STEPS 4
// Changes this soon after plugging in are not step boundaries: the reading jumps when the charger
// raises the voltage, and the step in progress at plug-in started before it.
#define SETTLE_TIME 60
// Back on screen after longer than this (another app was open), steps may have been missed:
// the estimate starts over.
#define RESUME_GAP (5 * SECONDS_PER_MINUTE)

// Battery capacity in mAh and nominal cell voltage in mV, for the power estimate. Approximate:
// models sharing a platform differ (Pebble Steel, Pebble Time Steel, Time Round 14/20 mm).
#if defined(PBL_PLATFORM_APLITE)
#define BATTERY_MAH 130
#elif defined(PBL_PLATFORM_BASALT)
#define BATTERY_MAH 150
#elif defined(PBL_PLATFORM_CHALK)
#define BATTERY_MAH 62
#elif defined(PBL_PLATFORM_DIORITE)
#define BATTERY_MAH 130
#else
#define BATTERY_MAH 150
#endif
#define BATTERY_MV 3800

#if defined(PBL_PLATFORM_EMERY)
#define LARGE_SCREEN 1
#else
#define LARGE_SCREEN 0
#endif

typedef struct {
  int32_t seen;     // last time the app saw the watch plugged in, 0 = not charging
  int32_t plugged;  // when the app first saw it plugged in
  uint8_t pct;      // last reported percentage
  uint8_t steps;    // valid entries of step_*
  uint8_t step_pct[STEPS];
  int32_t step_time[STEPS];
} ChargeLog;

static Layer *s_layer;
static ChargingScreenStyleProvider s_style;
static ChargingScreenModeHandler s_on_mode_change;
static BatteryChargeState s_state;
static bool s_active;
static ChargeLog s_log;

// Built with SLEEP_SCREEN_TEST=1 (see wscript) a CHARGE_TEST AppMessage sets a fixed charging rate
// in % per hour (0 = measure it), so screenshots show stable estimates.
#if defined(SLEEP_SCREEN_TEST)
static int s_test_rate;
#endif

// ---------------------------------------------------------------------------
// Charge log
// ---------------------------------------------------------------------------

static void log_save(void) {
  persist_write_data(PERSIST_KEY_CHARGE, &s_log, sizeof s_log);
}

static void log_update(BatteryChargeState state, time_t now) {
  if (!charging_screen_wanted(state)) {
    if (s_log.seen) {
      s_log = (ChargeLog){ 0 };
      log_save();
    }
    return;
  }
  const uint8_t pct = state.charge_percent;
  if (!s_log.seen || pct < s_log.pct) {
    s_log = (ChargeLog){ .plugged = now, .pct = pct };
  } else if (pct > s_log.pct) {
    if (now - s_log.plugged >= SETTLE_TIME) {
      if (s_log.steps == STEPS) {
        memmove(s_log.step_pct, s_log.step_pct + 1, STEPS - 1);
        memmove(s_log.step_time, s_log.step_time + 1, (STEPS - 1) * sizeof s_log.step_time[0]);
        s_log.steps--;
      }
      s_log.step_pct[s_log.steps] = pct;
      s_log.step_time[s_log.steps] = now;
      s_log.steps++;
    }
    s_log.pct = pct;
  }
  s_log.seen = now;
  log_save();
}

typedef struct {
  bool known;
  int32_t remaining;  // seconds until full
  int32_t mw;         // charging power
} Estimate;

static Estimate estimate(time_t now) {
  Estimate e = { 0 };
#if defined(SLEEP_SCREEN_TEST)
  if (s_test_rate > 0) {
    e.known = true;
    e.remaining = (100 - s_state.charge_percent) * SECONDS_PER_HOUR / s_test_rate;
    e.mw = (int64_t)s_test_rate * BATTERY_MAH * BATTERY_MV / 100000;
    return e;
  }
#endif
  if (s_log.steps < 2) return e;
  const int last = s_log.steps - 1;
  const int32_t dp = s_log.step_pct[last] - s_log.step_pct[0];
  const int32_t dt = s_log.step_time[last] - s_log.step_time[0];
  if (dp <= 0 || dt <= 0) return e;
  e.known = true;
  e.remaining = (100 - s_log.step_pct[last]) * dt / dp - (int32_t)(now - s_log.step_time[last]);
  // % per hour * capacity / 100 = mA; * V = mW
  e.mw = (int64_t)dp * SECONDS_PER_HOUR * BATTERY_MAH * BATTERY_MV / ((int64_t)dt * 100000);
  return e;
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

static void update_mode(void) {
  const bool active = charging_screen_wanted(s_state);
  if (active == s_active) return;
  s_active = active;
  layer_set_hidden(s_layer, !active);
  if (s_on_mode_change) s_on_mode_change(active);
}

bool charging_screen_active(void) { return s_active; }

void charging_screen_battery(BatteryChargeState state) {
  s_state = state;
  log_update(state, time(NULL));
  update_mode();
  if (s_active) layer_mark_dirty(s_layer);
}

void charging_screen_tick(void) {
  if (!s_active) return;
  s_log.seen = time(NULL);  // saved on the next battery change or on exit
  layer_mark_dirty(s_layer);
}

void charging_screen_inbox(DictionaryIterator *it) {
#if defined(SLEEP_SCREEN_TEST)
  Tuple *t = dict_find(it, MESSAGE_KEY_CHARGE_TEST);
  if (!t) return;
  s_test_rate = t->value->int32 > 0 ? t->value->int32 : 0;
  if (s_active) layer_mark_dirty(s_layer);
#endif
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// The two largest units needed: 1h 05m, 12m 30s, 45s
static void fmt_remaining(char *buf, size_t n, int32_t sec) {
  if (sec < 1) {
    snprintf(buf, n, "soon");
  } else if (sec >= SECONDS_PER_HOUR) {
    snprintf(buf, n, "%dh %02dm", (int)(sec / SECONDS_PER_HOUR), (int)(sec / SECONDS_PER_MINUTE % 60));
  } else if (sec >= SECONDS_PER_MINUTE) {
    snprintf(buf, n, "%dm %02ds", (int)(sec / SECONDS_PER_MINUTE), (int)(sec % 60));
  } else {
    snprintf(buf, n, "%ds", (int)sec);
  }
}

// Lightning bolt on a 12 x 18 grid.
static const GPoint BOLT[] = { {8, 0}, {0, 10}, {5, 10}, {3, 18}, {12, 7}, {7, 7} };
#define BOLT_POINTS ((int)(sizeof BOLT / sizeof BOLT[0]))

static void draw_bolt(GContext *ctx, GPoint center, int h, GColor color) {
  GPoint pts[BOLT_POINTS];
  for (int i = 0; i < BOLT_POINTS; i++) {
    pts[i] = GPoint((BOLT[i].x - 6) * h / 18, (BOLT[i].y - 9) * h / 18);
  }
  GPath path = { .num_points = BOLT_POINTS, .points = pts, .rotation = 0, .offset = center };
  graphics_context_set_fill_color(ctx, color);
  gpath_draw_filled(ctx, &path);
}

static void draw_ring(GContext *ctx, const ChargingScreenStyle *st, GRect box, int thickness, int pct) {
#if defined(PBL_COLOR)
  graphics_context_set_fill_color(ctx, st->track);
  graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, thickness, 0, TRIG_MAX_ANGLE);
#else
  // no grey on black & white watches: the track is a thin circle along the middle of the ring
  graphics_context_set_stroke_color(ctx, st->track);
  const int r = box.size.w / 2 - thickness / 2;
  graphics_draw_circle(ctx, grect_center_point(&box), r);
#endif
  if (pct > 0) {
    graphics_context_set_fill_color(ctx, st->accent);
    graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, thickness, 0, TRIG_MAX_ANGLE * pct / 100);
  }
}

static int font_size(const char *font_key) {
  if (strstr(font_key, "28")) return 28;
  if (strstr(font_key, "24")) return 24;
  if (strstr(font_key, "18")) return 18;
  return 14;
}

// Text whose capitals start at `cap_top` (Gothic leaves about a third of the font size above them).
static void text_aligned(GContext *ctx, const char *s, const char *font_key, int cap_top, int x, int w,
                         GColor color, GTextAlignment align) {
  const int size = font_size(font_key);
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, s, fonts_get_system_font(font_key), GRect(x, cap_top - size / 3 - 1, w, size + 8),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

static void text(GContext *ctx, const char *s, const char *font_key, int cap_top, int x, int w,
                 GColor color) {
  text_aligned(ctx, s, font_key, cap_top, x, w, color, GTextAlignmentCenter);
}

static int text_w(const char *s, const char *font_key) {
  return graphics_text_layout_get_content_size(s, fonts_get_system_font(font_key), GRect(0, 0, 200, 40),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft).w;
}

// A number followed by a smaller unit on the same baseline, centred in x .. x + w. The unit is
// drawn in a regular weight: the bold Gothic W has a bar across it and reads as a won sign.
static void value_with_unit(GContext *ctx, const char *num, const char *unit, const char *num_font,
                            const char *unit_font, int cap_top, int x, int w, GColor num_color,
                            GColor unit_color) {
  const int gap = 2;
  const int nw = text_w(num, num_font), uw = text_w(unit, unit_font);
  const int left = x + (w - nw - gap - uw) / 2;
  const int unit_cap = cap_top + (font_size(num_font) - font_size(unit_font)) * 7 / 10;
  text_aligned(ctx, num, num_font, cap_top, left, nw + 4, num_color, GTextAlignmentLeft);
  text_aligned(ctx, unit, unit_font, unit_cap, left + nw + gap, uw + 4, unit_color, GTextAlignmentLeft);
}

static void layer_update(Layer *layer, GContext *ctx) {
  ChargingScreenStyle style;
  s_style(&style);
  const ChargingScreenStyle *st = &style;
  const GRect bounds = layer_get_unobstructed_bounds(layer);
  const int W = bounds.size.w, H = bounds.size.h, x0 = bounds.origin.x, y0 = bounds.origin.y;
  const int pct = s_state.charge_percent;
  const bool full = !s_state.is_charging;
  const time_t now = time(NULL);

  graphics_context_set_fill_color(ctx, st->bg);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  graphics_context_set_antialiased(ctx, true);

  char pct_s[8], time_s[12], power_s[12];
  snprintf(pct_s, sizeof pct_s, "%d%%", pct);
  const Estimate e = estimate(now);
  const char *time_label = full ? "STATUS" : "FULL IN";
  const bool power_known = !full && e.known;
  if (full) {
    snprintf(time_s, sizeof time_s, "FULL");
  } else if (e.known) {
    fmt_remaining(time_s, sizeof time_s, e.remaining);
  } else {
    snprintf(time_s, sizeof time_s, "--");
  }
  snprintf(power_s, sizeof power_s, power_known ? "%d" : "--", (int)e.mw);

#if defined(PBL_ROUND)
  // the ring follows the edge of the screen, the readings sit inside its lower half
  const int thickness = 8;
  const GRect ring = grect_inset(bounds, GEdgeInsets(3));
  draw_ring(ctx, st, ring, thickness, full ? 100 : pct);
  const int cx = x0 + W / 2, cy = y0 + H / 2;
  draw_bolt(ctx, GPoint(cx, cy - 34), 32, st->accent);
  text(ctx, pct_s, FONT_KEY_GOTHIC_28_BOLD, cy - 10, x0, W, st->accent);
  text(ctx, time_s, FONT_KEY_GOTHIC_18_BOLD, cy + 24, x0, W, st->accent);
  if (power_known) {
    value_with_unit(ctx, power_s, "mW", FONT_KEY_GOTHIC_14, FONT_KEY_GOTHIC_14, cy + 44, x0, W, st->label, st->label);
  } else {
    text(ctx, power_s, FONT_KEY_GOTHIC_14, cy + 44, x0, W, st->label);
  }
#else
  // ring on top, time until full and power side by side below it
  const int bottom_h = LARGE_SCREEN ? 50 : 38;
  const int thickness = LARGE_SCREEN ? 10 : 8;
  const int area_h = H - bottom_h;
  int d = (W < area_h ? W : area_h) - 8;
  if (d < 40) d = 40;
  const int cx = x0 + W / 2, cy = y0 + 4 + d / 2;
  draw_ring(ctx, st, GRect(cx - d / 2, cy - d / 2, d, d), thickness, full ? 100 : pct);
  const int bolt_h = d * 3 / 10;
  const char *pct_font = LARGE_SCREEN ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD;
  const int pct_cap = LARGE_SCREEN ? 20 : 17;
  const int block = bolt_h + 6 + pct_cap;
  draw_bolt(ctx, GPoint(cx, cy - block / 2 + bolt_h / 2), bolt_h, st->accent);
  text(ctx, pct_s, pct_font, cy - block / 2 + bolt_h + 6, x0, W, st->accent);

  const char *label_font = LARGE_SCREEN ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14;
  const char *value_font = LARGE_SCREEN ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD;
  const int label_y = y0 + H - bottom_h + (LARGE_SCREEN ? 8 : 6);
  const int value_y = label_y + (LARGE_SCREEN ? 18 : 14);
  const int col_w = W / 2;
  text(ctx, time_label, label_font, label_y, x0, col_w, st->label);
  text(ctx, time_s, value_font, value_y, x0, col_w, st->accent);
  text(ctx, "POWER", label_font, label_y, x0 + col_w, W - col_w, st->label);
  if (power_known) {
    value_with_unit(ctx, power_s, "mW", value_font, label_font, value_y, x0 + col_w, W - col_w, st->accent,
                    st->accent);
  } else {
    text(ctx, power_s, value_font, value_y, x0 + col_w, W - col_w, st->accent);
  }
#endif
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void charging_screen_init(Layer *parent, BatteryChargeState state, ChargingScreenStyleProvider style,
                          ChargingScreenModeHandler on_mode_change) {
  s_style = style;
  s_layer = layer_create(layer_get_bounds(parent));
  layer_set_update_proc(s_layer, layer_update);
  layer_set_hidden(s_layer, true);
  layer_add_child(parent, s_layer);
  const time_t now = time(NULL);
  if (persist_exists(PERSIST_KEY_CHARGE) && persist_get_size(PERSIST_KEY_CHARGE) == (int)sizeof s_log)
    persist_read_data(PERSIST_KEY_CHARGE, &s_log, sizeof s_log);
  if (s_log.seen && (now < s_log.seen || now - s_log.seen > RESUME_GAP)) s_log = (ChargeLog){ 0 };
  s_state = state;
  log_update(state, now);
  // the handler (if any) runs after the state above is set up
  s_on_mode_change = on_mode_change;
  update_mode();
}

void charging_screen_deinit(void) {
  if (s_log.seen) log_save();
  layer_destroy(s_layer);
}
