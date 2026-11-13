// Binary Time - rebuilt watch-side code.
//
// The layout maths is a port of the live-preview renderer on the original
// configuration page (see config/config-page.html); bit shapes come
// from the preview images (config/img). Behaviours that the
// preview does not show (alerts, quick view) were measured on the original
// app in the emulator.
#include <pebble.h>
#include "sleep_screen.h"

#define PERSIST_KEY 2
// Settings added later live under their own key, so the stored Settings blob (and with it
// every existing setting) stays valid across updates.
#define PERSIST_KEY_HOURLY_ASLEEP 4

// Tunables measured against the original app.
#define HINT_Y_FUDGE (LARGE_SCREEN ? -5 : -4)
#define HINT_BOX_W 16
#define HINT_BOX_DX 0
#define BATT_LOW_PERCENT 19  // "low" is <= 19 % and not charging

#if defined(PBL_PLATFORM_EMERY)
#define LARGE_SCREEN 1
#else
#define LARGE_SCREEN 0
#endif

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum { DF_OFF, DF_SOLID, DF_DOTTED, DF_SHADED, DF_OUTLINE };
enum { HINT_OFF, HINT_TAP, HINT_ALWAYS };
enum { ALIGN_CENTER, ALIGN_RIGHT, ALIGN_LEFT };

typedef struct {
  uint8_t true_binary;
  uint8_t display_false;        // DF_*
  uint8_t show_impossible;
  uint8_t true_border, false_border;
  GColor bg, on, off, hint;
  GColor d_on, d_off, d_hint;
  uint8_t seconds;
  uint8_t sec_interval;         // 1, 5, 10, 15, 20, 30
  uint8_t time_align, time_format, time_hint, time_deco;
  uint8_t show_date, date_pos;  // date_pos: 0 = date on top, 1 = date at bottom
  uint8_t date_align, date_format, date_hint, date_deco;
  uint8_t bt_icon, bt_vibe, bt_reconnect_vibe;
  uint8_t batt_icon, batt_vibe;
  uint8_t hourly_vibe, no_vibe_charging;
  uint16_t tap_timeout_ms;
  uint8_t quick_view;
} Settings;

static Settings s;
static uint8_t s_hourly_vibe_asleep;  // keep the hourly vibration while the sleep screen is up

static void tick_handler(struct tm *t, TimeUnits changed);
static void layout_changed(void) {}

static void settings_defaults(void) {
  s = (Settings){
    .true_binary = 0,
    .display_false = PBL_IF_COLOR_ELSE(DF_SOLID, DF_DOTTED),
    .show_impossible = 0,
    .true_border = 0, .false_border = 0,
    .bg = GColorBlack,
    .on = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite),
    .off = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite),
    .hint = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite),
    .d_on = PBL_IF_COLOR_ELSE(GColorSpringBud, GColorWhite),
    .d_off = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite),
    .d_hint = PBL_IF_COLOR_ELSE(GColorSpringBud, GColorWhite),
    .seconds = 1, .sec_interval = 1,
    .time_align = ALIGN_CENTER, .time_format = 0, .time_hint = HINT_OFF, .time_deco = 0,
    .show_date = 0, .date_pos = 0,
    .date_align = ALIGN_CENTER, .date_format = 0, .date_hint = HINT_OFF, .date_deco = 0,
    .bt_icon = 1, .bt_vibe = 3, .bt_reconnect_vibe = 3,
    .batt_icon = 1, .batt_vibe = 0,
    .hourly_vibe = 0, .no_vibe_charging = 0,
    .tap_timeout_ms = 5000,
    .quick_view = 0,
  };
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static Window *s_window;
static Layer *s_canvas;
static GFont s_hint_font;
static struct tm s_tm;
static bool s_bt_connected = true;
static bool s_tapped;
static AppTimer *s_tap_timer;
static BatteryChargeState s_batt;
static bool s_hour24;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline int ceil_half(int t) { return t / 2 + t % 2; }

static bool hour24(void) {
  if (s.time_format == 1) return true;
  if (s.time_format == 2) return false;
  return s_hour24;
}

// 2 bits per channel steps, mirrors brighterColor()/shadeColor() of the config page.
static GColor brighter(GColor c) {
#if defined(PBL_COLOR)
  c.r = c.r < 3 ? c.r + 1 : c.r;
  c.g = c.g < 3 ? c.g + 1 : c.g;
  c.b = c.b < 3 ? c.b + 1 : c.b;
#endif
  return c;
}
static GColor shade(GColor c) {
#if defined(PBL_COLOR)
  c.r = c.r ? c.r - 1 : 0;
  c.g = c.g ? c.g - 1 : 0;
  c.b = c.b ? c.b - 1 : 0;
#endif
  return c;
}

// ---------------------------------------------------------------------------
// Vibrations
// ---------------------------------------------------------------------------

static const uint32_t HEARTBEAT[] = { 200, 100, 200, 400, 200, 100, 200 };
static const uint32_t HALFBEAT[] = { 200, 100, 200 };

static void vibrate(uint8_t kind) {
  if (kind == 0 || quiet_time_is_active()) return;
  if (s.no_vibe_charging && s_batt.is_charging) return;
  switch (kind) {
    case 1: vibes_short_pulse(); break;
    case 2: vibes_long_pulse(); break;
    case 3: vibes_double_pulse(); break;
    case 4: vibes_enqueue_custom_pattern((VibePattern){ HEARTBEAT, ARRAY_LENGTH(HEARTBEAT) }); break;
    case 5: vibes_enqueue_custom_pattern((VibePattern){ HALFBEAT, ARRAY_LENGTH(HALFBEAT) }); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Time / date digits and bit masks
// ---------------------------------------------------------------------------

typedef struct {
  int time[6];       // HH MM SS digits
  int time_cols;     // 4 or 6
  int date[6];
  int date_cols;     // 4 or 6
  char time_deco;
  int time_deco_cols;
  char date_deco;
  int date_deco_cols;
} Digits;

static int displayed_seconds(void) {
  int sec = s_tm.tm_sec;
  return sec - (sec % s.sec_interval);
}

static void digits_update(Digits *d) {
  int h = s_tm.tm_hour;
  if (!hour24()) {
    if (h == 0) h = 12; else if (h > 12) h -= 12;
  }
  d->time[0] = h / 10; d->time[1] = h % 10;
  d->time[2] = s_tm.tm_min / 10; d->time[3] = s_tm.tm_min % 10;
  if (s.seconds) {
    int sec = displayed_seconds();
    d->time[4] = sec / 10; d->time[5] = sec % 10;
    d->time_cols = 6; d->time_deco_cols = 2;
  } else {
    d->time_cols = 4; d->time_deco_cols = 1;
  }
  d->time_deco = ':';

  int day = s_tm.tm_mday, mon = s_tm.tm_mon + 1, yy = (s_tm.tm_year + 1900) % 100;
  d->date_cols = 4; d->date_deco_cols = 1; d->date_deco = '/';
  switch (s.date_format) {
    case 2:  // DDMMYY
      d->date[4] = yy / 10; d->date[5] = yy % 10; d->date_cols = 6;
      // fall through
    case 0:  // DDMM
      d->date[0] = day / 10; d->date[1] = day % 10; d->date[2] = mon / 10; d->date[3] = mon % 10;
      d->date_deco_cols = 2; d->date_deco = '.';
      break;
    case 3:  // MMDDYY
      d->date[4] = yy / 10; d->date[5] = yy % 10; d->date_cols = 6; d->date_deco_cols = 2;
      // fall through
    case 1:  // MMDD
      d->date[0] = mon / 10; d->date[1] = mon % 10; d->date[2] = day / 10; d->date[3] = day % 10;
      break;
    case 4:  // YYMMDD
      d->date[0] = yy / 10; d->date[1] = yy % 10; d->date[2] = mon / 10; d->date[3] = mon % 10;
      d->date[4] = day / 10; d->date[5] = day % 10; d->date_cols = 6; d->date_deco_cols = 2;
      break;
  }
}

// Which bits can ever be set: [row][col], BCD rows are 8,4,2,1 (top to bottom).
static const uint8_t TIME_MASK_BCD[4][6] = {
  {0,1,0,1,0,1}, {0,1,1,1,1,1}, {1,1,1,1,1,1}, {1,1,1,1,1,1},
};
static const uint8_t TIME_MASK_TB[3][6] = {
  {0,1,1,1,1,1}, {1,1,1,1,1,1}, {1,1,1,1,1,1},
};
static const uint8_t DATE_MASK_BCD[5][4][6] = {
  { {0,1,0,1,0,0}, {0,1,0,1,0,0}, {1,1,0,1,0,0}, {1,1,1,1,0,0} },  // DDMM
  { {0,1,0,1,0,0}, {0,1,0,1,0,0}, {0,1,1,1,0,0}, {1,1,1,1,0,0} },  // MMDD
  { {0,1,0,1,0,1}, {0,1,0,1,1,1}, {1,1,0,1,1,1}, {1,1,1,1,1,1} },  // DDMMYY
  { {0,1,0,1,0,1}, {0,1,0,1,1,1}, {0,1,1,1,1,1}, {1,1,1,1,1,1} },  // MMDDYY
  { {0,1,0,1,0,1}, {1,1,0,1,0,1}, {1,1,0,1,1,1}, {1,1,1,1,1,1} },  // YYMMDD
};
static const uint8_t DATE_MASK_TB[5][3][6] = {
  { {0,1,1,1,1,1}, {0,0,1,1,1,1}, {0,0,0,0,0,0} },
  { {0,0,1,1,1,1}, {0,1,1,1,1,1}, {0,0,0,0,0,0} },
  { {0,1,1,1,1,1}, {0,0,1,1,1,1}, {1,1,1,1,1,1} },
  { {0,0,1,1,1,1}, {0,1,1,1,1,1}, {1,1,1,1,1,1} },
  { {1,1,1,1,1,1}, {0,0,1,1,1,1}, {0,1,1,1,1,1} },
};

// ---------------------------------------------------------------------------
// Layout (port of the preview renderer)
// ---------------------------------------------------------------------------

typedef struct {
  int bit_size;
  int time_dx, date_dx, dy;
  int time_x, time_y, date_x, date_y;
  int time_rows, date_rows;
  int time_hx, date_hx, time_hy, date_hy;   // hint x / baseline y
  int time_hadj, date_hadj;
  int font_adj;
  int hint_h;
} Layout;

enum { QV_SQUEEZE, QV_SHRINK, QV_CENTER, QV_DISPLACE, QV_DRAW_OVER };

// W x Hfull is the screen, Hu the height left by a Timeline Quick View.
static void compute_layout(const Digits *d, int W, int Hfull, int Hu, Layout *L) {
  const bool obstructed = Hu < Hfull;
  const bool resize = obstructed && (s.quick_view == QV_SQUEEZE || s.quick_view == QV_SHRINK);
  const int H = resize ? Hu : Hfull;
  const bool round = PBL_IF_ROUND_ELSE(true, false);
  const int hfh = LARGE_SCREEN ? 13 : 10;      // hint font height
  const int hfo = LARGE_SCREEN ? -13 : -10;    // hint font baseline offset
  const bool tb = s.true_binary, show_date = s.show_date, secs = s.seconds;
  const int side_h = (obstructed && s.quick_view == QV_SHRINK) ? Hu : Hfull;
  const int side = W > side_h ? side_h : W;

  int small = side / (round ? 15 : 13); while (!(small % 2)) --small;
  int large = side / (round ? 11 : 9);  while (!(large % 2)) --large;
  int narrow = (2 * small * 4) / 5;     while (!(narrow % 2)) --narrow;

  int time_cols = d->time_cols, date_cols = d->date_cols;
  int time_rows = 4, date_rows = 4;
  int bit = large;
  int tdx = 2 * large;
  int ddx = (date_cols == 6) ? 2 * small : 2 * large;
  int dy = 2 * large;
  int tx = ceil_half(W - (3 * tdx + large));
  int ty = ceil_half(H - (3 * dy + large));
  int dx = ceil_half(W - 11 * small);
  int dyy = 0;
  int thx = tx;
  int dhbit = (date_cols == 6) ? small : bit;
  int dhx = (date_cols == 6) ? ceil_half(W - 11 * dhbit) : thx;
  int thy_top = (ty - hfh) / 2 - hfo;
  int thy_bot = ceil_half((H - ty - (3 * dy + large)) - hfh) - hfo + ty + (3 * dy + large);
  int thadj = 0, dhadj = 0;
  const int date_pos = s.date_pos;

  if (round) {
    dhadj = (date_cols == 6 && !secs && !tb) ? W / 45 : 0;
    if (dhadj) dhx = ceil_half(W - (5 * (ddx - dhadj) + dhbit));
    thy_top += H / 20;
    thy_bot -= H / 20;
  }

  if (tb) {
    time_rows = d->time_cols / 2; time_cols = 6;
    date_rows = d->date_cols / 2; date_cols = 6;
    bit = small; dhbit = bit;
    tdx = 2 * small; ddx = 2 * small;
    dy = round ? ((show_date && (date_rows == 3 || time_rows == 3)) ? narrow : 2 * small) : 2 * small;
    tx = ceil_half(W - 11 * small);
    thx = tx; dhx = dx;
    int top_rows = date_pos ? time_rows : date_rows;
    int bot_rows = date_pos ? date_rows : time_rows;
    if (show_date) {
      int gap = round ? H / 10 : 2 * small;
      int rows = (top_rows + bot_rows - 2) * dy + 2 * small + gap;
      int top_y = ceil_half(H - rows);
      int bot_y = top_y + (top_rows - 1) * dy + small + gap;
      thy_top = (top_y - hfh) / 2 - hfo;
      thy_bot = ceil_half((H - top_y - rows) - hfh) - hfo + top_y + rows;
      if (round) {
        int div = 100 - (top_rows + bot_rows) * 10;
        thy_top += H / div;
        thy_bot -= H / div;
      }
      ty = date_pos ? top_y : bot_y;
      dyy = date_pos ? bot_y : top_y;
    } else {
      int span = (time_rows - 1) * dy + small;
      ty = ceil_half(H - span);
      thy_top = (ty - hfh) / 2 - hfo;
      thy_bot = ceil_half((H - ty - span) - hfh) - hfo + ty + span;
      if (round) { thy_top += H / 25; thy_bot -= H / 25; }
    }
    if (time_rows == 2) {
      if (s.time_align == ALIGN_CENTER) thx = tx + tdx;
      else if (s.time_align == ALIGN_RIGHT) thx = tx + 2 * tdx;
      else thx = tx;
    } else if (round && show_date) {
      thadj = W / 25;
      thx = ceil_half(W - (5 * (tdx - thadj) + bit));
    }
    if (date_rows == 2) {
      if (s.date_align == ALIGN_CENTER) dhx = dx + ddx;
      else if (s.date_align == ALIGN_RIGHT) dhx = dx + 2 * ddx;
      else dhx = dx;
    } else if (round && show_date) {
      dhadj = W / 25;
      dhx = ceil_half(W - (5 * (ddx - dhadj) + dhbit));
    }
  } else if (show_date || time_cols == 6) {
    bit = small;
    tdx = 2 * small; ddx = 2 * small;
    tx = ceil_half(W - (5 * tdx + small));
    if (show_date) {
      dy = narrow;
      if (round) {
        tdx = narrow; ddx = narrow;
        tx = ceil_half(W - (5 * tdx + small));
        dx = ceil_half(W - (5 * ddx + small));
      }
    } else {
      dy = 2 * small;
    }
    if (time_cols == 6) {
      ty = ceil_half(H - (3 * dy + small));
      thy_top = (ty - hfh) / 2 - hfo;
      thy_bot = ceil_half((H - ty - (3 * dy + small)) - hfh) - hfo + ty + (3 * dy + small);
      if (round) { thy_top += H / 18; thy_bot -= H / 18; }
    }
    if (time_cols == 4) {
      if (round) thx = tx + tdx;
      if (s.time_align == ALIGN_CENTER) tx += tdx;
      else if (s.time_align == ALIGN_RIGHT) tx += 2 * tdx;
      if (!round) thx = tx;
    } else {
      if (round && show_date) {
        thadj = W / 25;
        thx = ceil_half(W - (5 * (tdx - thadj) + bit));
      } else {
        thx = tx;
      }
    }
    if (date_cols == 4) {
      if (round) {
        if (show_date) dhx = dx + ddx;
        dhadj = 0;
      }
      if (s.date_align == ALIGN_CENTER) dx += ddx;
      else if (s.date_align == ALIGN_RIGHT) dx += 2 * ddx;
      if (!round) dhx = dx;
    } else {
      if (round) {
        if (show_date) dhadj = W / 25;
        dhx = ceil_half(W - (5 * (ddx - dhadj) + dhbit));
      } else {
        dhx = dx;
      }
    }
    if (round && !show_date) dhx = dx;
    if (show_date) {
      int rows = 3 * dy + small;
      int gap = (H - 2 * rows) / 3;
      int top_y = gap;
      if (2 == (H - 2 * rows) % 3) ++top_y; else ++gap;
      if (round) {
        // measured on the original: a fixed gap between date and time, centred as a pair
        gap = H / 18;
        top_y = ceil_half(H - 2 * rows - gap);
      }
      int bot_y = top_y + rows + gap;
      thy_top = (top_y - hfh) / 2 - hfo;
      thy_bot = ceil_half(H - bot_y - rows - hfh) - hfo + bot_y + rows;
      if (round) { thy_top += H / 90; thy_bot -= H / 90; }
      ty = date_pos ? top_y : bot_y;
      dyy = date_pos ? bot_y : top_y;
    }
  }

  if (resize && tb && show_date) {
    // True binary + date: blocks are separated by (row spacing - half a hint) and the
    // row spacing is reduced until at least one hint height of margin remains.
    const int hint_space = hfh + 1;
    const int gap_cut = (hfh + 2) / 2;
    int fit = dy;
    for (;; fit--) {
      int total = (time_rows + date_rows - 2) * fit + 2 * bit + (fit - gap_cut);
      if ((H - total) / 2 >= hint_space || fit <= bit + 1) break;
    }
    dy = fit;
    int gap = dy - gap_cut;
    int ht = (time_rows - 1) * dy + bit, hd = (date_rows - 1) * dy + bit;
    int total = ht + hd + gap;
    int top_y = ceil_half(H - total);
    int first_h = date_pos ? ht : hd;
    int bot_y = top_y + first_h + gap;
    ty = date_pos ? top_y : bot_y;
    dyy = date_pos ? bot_y : top_y;
    int bottom_edge = top_y + total;
    thy_top = (top_y - hfh) / 2 - hfo;
    thy_bot = ceil_half((H - bottom_edge) - hfh) - hfo + bottom_edge;
  } else if (resize) {
    // Quick View: shrink the row gap until blocks and hint space fit the free area.
    const int rows_t = tb ? time_rows : 4, rows_d = tb ? date_rows : 4;
    const int blocks = show_date ? 2 : 1, slots = blocks + 1;
    int fit = dy;
    for (;; fit--) {
      int total = (rows_t - 1) * fit + bit + (show_date ? (rows_d - 1) * fit + bit : 0);
      if ((H - total) > (hfh + 1) * slots || fit <= bit + 1) break;
    }
    if (fit != dy) {
      dy = fit;
      int ht = (rows_t - 1) * dy + bit, hd = (rows_d - 1) * dy + bit;
      int left = H - ht - (show_date ? hd : 0);
      int top_y, bottom_edge;
      if (show_date) {
        int first_h = date_pos ? ht : hd;
        int m, gap;
        if (s.quick_view == QV_SQUEEZE) {
          // content is centred, margins never drop below the hint space
          m = (left + 2) / 3;
          if (m < hfh + 1) m = hfh + 1;
          gap = left - 2 * m;
        } else {
          // same remainder handling as the regular two-block layout
          gap = left / 3; m = gap;
          if (2 == left % 3) ++m; else ++gap;
        }
        top_y = m;
        int bot_y = top_y + first_h + gap;
        ty = date_pos ? top_y : bot_y;
        dyy = date_pos ? bot_y : top_y;
        bottom_edge = bot_y + (date_pos ? hd : ht);
      } else {
        ty = top_y = (left + 1) / 2;
        bottom_edge = ty + ht;
      }
      thy_top = (top_y - hfh) / 2 - hfo;
      thy_bot = ceil_half((H - bottom_edge) - hfh) - hfo + bottom_edge;
    }
  } else if (obstructed) {
    int off = 0;
    if (s.quick_view == QV_DISPLACE) off = -(Hfull - Hu);
    else if (s.quick_view == QV_CENTER) off = -ceil_half(Hfull - Hu);
    ty += off; dyy += off; thy_top += off; thy_bot += off;
  }

  L->bit_size = bit;
  L->time_dx = tdx; L->date_dx = ddx; L->dy = dy;
  L->time_x = tx; L->time_y = ty; L->date_x = dx; L->date_y = dyy;
  L->time_rows = time_rows; L->date_rows = date_rows;
  L->time_hx = thx; L->date_hx = dhx;
  L->time_hy = date_pos ? thy_top : thy_bot;
  L->date_hy = date_pos ? thy_bot : thy_top;
  L->time_hadj = thadj; L->date_hadj = dhadj;
  L->font_adj = (small == bit) ? (LARGE_SCREEN ? 2 : 1) : (LARGE_SCREEN ? 5 : 3);
  L->hint_h = hfh;
}

// ---------------------------------------------------------------------------
// Bit drawing (shapes from the preview images)
// ---------------------------------------------------------------------------

typedef struct {
  GColor on, off;
  GColor on_bright, on_shade, off_bright, off_shade;
} BitColors;

static GColor s_bg_now;  // background used for the current frame

static void make_colors(BitColors *c, GColor on, GColor off) {
  c->on = on;
  c->off = s.display_false ? off : s_bg_now;
  c->on_bright = brighter(on);
  c->on_shade = shade(on);
  c->off_bright = s.display_false ? (s.display_false == DF_SOLID ? brighter(off) : off) : s_bg_now;
  c->off_shade = s.display_false ? shade(off) : s_bg_now;
}

static void fill_cut(GContext *ctx, int x, int y, int n, GColor c) {
  graphics_context_set_fill_color(ctx, c);
  graphics_fill_rect(ctx, GRect(x + 1, y, n - 2, n), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(x, y + 1, n, n - 2), 0, GCornerNone);
}

static void draw_border(GContext *ctx, int x, int y, int n, GColor bright, GColor dark) {
  graphics_context_set_stroke_color(ctx, bright);
  graphics_draw_line(ctx, GPoint(x + 1, y), GPoint(x + n - 2, y));
  graphics_draw_line(ctx, GPoint(x, y + 1), GPoint(x, y + n - 2));
  graphics_context_set_stroke_color(ctx, dark);
  graphics_draw_line(ctx, GPoint(x + 1, y + n - 1), GPoint(x + n - 2, y + n - 1));
  graphics_draw_line(ctx, GPoint(x + n - 1, y + 1), GPoint(x + n - 1, y + n - 2));
}

static void draw_bit(GContext *ctx, int x, int y, int n, bool set, const BitColors *c) {
  if (set) {
    fill_cut(ctx, x, y, n, c->on);
    if (s.true_border) draw_border(ctx, x, y, n, c->on_bright, c->on_shade);
    return;
  }
  if (!s.display_false) return;
  switch (s.display_false) {
    case DF_SOLID:
      fill_cut(ctx, x, y, n, c->off);
      break;
    case DF_DOTTED:
      graphics_context_set_stroke_color(ctx, c->off);
      for (int j = 2; j <= n - 3; j += 2)
        for (int i = 2; i <= n - 3; i += 2) graphics_draw_pixel(ctx, GPoint(x + i, y + j));
      break;
    case DF_SHADED:
      graphics_context_set_stroke_color(ctx, c->off);
      for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
          if (!((i + j) & 1)) graphics_draw_pixel(ctx, GPoint(x + i, y + j));
      break;
    case DF_OUTLINE:
      graphics_context_set_stroke_color(ctx, c->off);
      graphics_draw_line(ctx, GPoint(x + 1, y), GPoint(x + n - 2, y));
      graphics_draw_line(ctx, GPoint(x + 1, y + n - 1), GPoint(x + n - 2, y + n - 1));
      graphics_draw_line(ctx, GPoint(x, y + 1), GPoint(x, y + n - 2));
      graphics_draw_line(ctx, GPoint(x + n - 1, y + 1), GPoint(x + n - 1, y + n - 2));
      break;
  }
  if (s.false_border) draw_border(ctx, x, y, n, c->off_bright, c->off_shade);
}

static void draw_possible(GContext *ctx, int x, int y, int n, bool possible, bool set, const BitColors *c) {
  if (possible) {
    draw_bit(ctx, x, y, n, set, c);
  } else if (s.show_impossible) {
    draw_bit(ctx, x, y, n, false, c);
  }
}

static void draw_bcd(GContext *ctx, int x0, int y0, int dx, int dy, int n, const int *digits, int cols,
                     const uint8_t (*mask)[6], const BitColors *c) {
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < cols; col++) {
      bool set = (digits[col] >> (3 - row)) & 1;
      draw_possible(ctx, x0 + col * dx, y0 + row * dy, n, mask[row][col], set, c);
    }
  }
}

static void draw_tb(GContext *ctx, int x0, int y0, int dx, int dy, int n, const int *values, int rows,
                    const uint8_t (*mask)[6], const BitColors *c) {
  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < 6; col++) {
      bool set = (values[row] >> (5 - col)) & 1;
      draw_possible(ctx, x0 + col * dx, y0 + row * dy, n, mask[row][col], set, c);
    }
  }
}

// ---------------------------------------------------------------------------
// Hints, battery and bluetooth indicators
// ---------------------------------------------------------------------------

static void draw_glyph(GContext *ctx, const char *txt, int x, int baseline_y) {
  GRect r = GRect(x + HINT_BOX_DX, baseline_y - (LARGE_SCREEN ? 13 : 10) + HINT_Y_FUDGE, HINT_BOX_W, 20);
  graphics_draw_text(ctx, txt, s_hint_font, r, GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void draw_hints(GContext *ctx, const int *digits, int cols, int hx, int hy, int dx, int adj,
                       int font_adj, GColor color, bool deco, char deco_char, int deco_cols,
                       int deco_extra) {
  graphics_context_set_text_color(ctx, color);
  char t[2] = { 0, 0 };
  for (int i = 0; i < cols; i++) {
    t[0] = '0' + digits[i];
    draw_glyph(ctx, t, hx + i * (dx - adj) + font_adj + (digits[i] == 1 ? 1 : 0), hy);  // the original nudges "1" right
  }
  if (deco) {
    t[0] = deco_char;
    for (int j = 0; j < deco_cols; j++) {
      int x = deco_extra + ((dx - adj) + (dx - adj) / 2) + hx + j * (dx - adj) * 2 + font_adj;
      draw_glyph(ctx, t, x, hy);
    }
  }
}

static bool tap_active(void) { return s_tapped; }

static bool battery_low(void) {
  return s_batt.charge_percent <= BATT_LOW_PERCENT && !s_batt.is_charging;
}

static bool battery_visible(void) {
  switch (s.batt_icon) {
    case 1: return battery_low();
    case 2: return tap_active();
    case 3: return tap_active() || battery_low();
    case 4: return true;
    default: return false;
  }
}

// Status colour of the icons when the background is black (colour watches only).
#if defined(PBL_COLOR)
static GColor battery_status_color(void) {
  switch (s_batt.charge_percent) {
    case 0: case 10: return GColorRed;
    case 20: case 30: return GColorOrange;
    case 40: case 50: return GColorChromeYellow;
    case 60: case 70: return GColorYellow;
    default: return GColorGreen;
  }
}
#endif

static void draw_battery(GContext *ctx, GRect b) {
  int bx, by;
#if defined(PBL_ROUND)
  bx = b.size.w - 22; by = b.size.h / 2 - 4;
#else
  bx = b.size.w - 18; by = 4;
#endif
  GColor c = PBL_IF_COLOR_ELSE(gcolor_equal(s_bg_now, GColorBlack) ? battery_status_color() : s.on, s.on);
  graphics_context_set_stroke_color(ctx, c);
  graphics_context_set_fill_color(ctx, c);
  graphics_draw_rect(ctx, GRect(bx, by, 14, 8));
  graphics_draw_line(ctx, GPoint(bx + 14, by + 2), GPoint(bx + 14, by + 5));
  int w = s_batt.charge_percent / 10;
  if (w > 0) graphics_fill_rect(ctx, GRect(bx + 2, by + 2, w, 4), 0, GCornerNone);
}

// Bluetooth connection icon: a 10 point outline, struck through while the link is lost.
static GPoint s_bt_points[10] = {
  {2, 2}, {5, 5}, {5, 9}, {8, 6}, {5, 3}, {5, 0}, {8, 3}, {6, 5}, {5, 4}, {2, 7}
};
static const GPathInfo BT_PATH_INFO = { .num_points = 10, .points = s_bt_points };
static GPath *s_bt_path;
static AppTimer *s_bt_timer;
static int s_bt_state;          // 0 = never lost, 1 = lost, 2 = reconnected
static bool s_bt_loss_vibed;

static void bt_icon_destroy(void) {
  if (s_bt_path) { gpath_destroy(s_bt_path); s_bt_path = NULL; }
}

static void draw_bluetooth(GContext *ctx, GRect b) {
  if (!s_bt_path) return;
  GPoint o;
#if defined(PBL_ROUND)
  o = GPoint(12, b.size.h / 2 - 5);
#else
  o = GPoint(4, 4);
#endif
  const bool black_bg = gcolor_equal(s_bg_now, GColorBlack);
  GColor c = PBL_IF_COLOR_ELSE(black_bg ? (s_bt_state == 1 ? GColorRed : GColorGreen) : s.on, s.on);
  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_stroke_color(ctx, c);
  gpath_move_to(s_bt_path, o);
  gpath_draw_outline_open(ctx, s_bt_path);
  if (s_bt_state == 1 && !black_bg) {
    // on a custom background the loss is shown by a diagonal slash edged with the background
    graphics_draw_line(ctx, GPoint(o.x, o.y + 9), GPoint(o.x + 9, o.y));
    graphics_draw_line(ctx, GPoint(o.x + 1, o.y + 9), GPoint(o.x + 10, o.y));
    graphics_context_set_stroke_color(ctx, s_bg_now);
    graphics_draw_line(ctx, GPoint(o.x - 1, o.y + 9), GPoint(o.x + 8, o.y));
    graphics_draw_line(ctx, GPoint(o.x + 2, o.y + 9), GPoint(o.x + 11, o.y));
  }
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (sleep_screen_active()) return;  // covered by the sleep screen layer
  Digits d;
  digits_update(&d);
  Layout L;
  compute_layout(&d, b.size.w, b.size.h, layer_get_unobstructed_bounds(layer).size.h, &L);

  // "Switch colors": while the phone is disconnected the background takes the on-bit
  // colour and the bits take the background colour.
  const bool swapped = s.bt_icon == 2 && !s_bt_connected;
  GColor bg = swapped ? s.on : s.bg;
  s_bg_now = bg;
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  BitColors tc, dc;
  make_colors(&tc, swapped ? s.bg : s.on, (swapped && gcolor_equal(s.on, s.off)) ? s.bg : s.off);
  make_colors(&dc, swapped ? s.bg : s.d_on, (swapped && gcolor_equal(s.d_on, s.d_off)) ? s.bg : s.d_off);

  if (s.true_binary) {
    int tv[3] = { d.time[0] * 10 + d.time[1], d.time[2] * 10 + d.time[3], d.time[4] * 10 + d.time[5] };
    uint8_t mask[3][6];
    memcpy(mask, TIME_MASK_TB, sizeof mask);
    if (!hour24()) mask[0][1] = 0;
    draw_tb(ctx, L.time_x, L.time_y, L.time_dx, L.dy, L.bit_size, tv, d.time_cols / 2, (const uint8_t (*)[6])mask, &tc);
    if (s.show_date) {
      int dv[3] = { d.date[0] * 10 + d.date[1], d.date[2] * 10 + d.date[3], d.date[4] * 10 + d.date[5] };
      draw_tb(ctx, L.date_x, L.date_y, L.date_dx, L.dy, L.bit_size, dv, d.date_cols / 2, DATE_MASK_TB[s.date_format], &dc);
    }
  } else {
    uint8_t mask[4][6];
    memcpy(mask, TIME_MASK_BCD, sizeof mask);
    if (!hour24()) mask[2][0] = 0;
    draw_bcd(ctx, L.time_x, L.time_y, L.time_dx, L.dy, L.bit_size, d.time, d.time_cols, (const uint8_t (*)[6])mask, &tc);
    if (s.show_date)
      draw_bcd(ctx, L.date_x, L.date_y, L.date_dx, L.dy, L.bit_size, d.date, d.date_cols, DATE_MASK_BCD[s.date_format], &dc);
  }

  if (s.time_hint == HINT_ALWAYS || (tap_active() && s.time_hint == HINT_TAP)) {
    draw_hints(ctx, d.time, d.time_cols, L.time_hx, L.time_hy, L.time_dx, L.time_hadj, L.font_adj,
               s.hint, s.time_deco, d.time_deco, d.time_deco_cols, 3);
  }
  if (s.date_hint == HINT_ALWAYS || (tap_active() && s.date_hint == HINT_TAP)) {
    int extra = (d.date_deco_cols == d.date_cols / 2) ? -2 : 2;
    if (d.date_deco == '.') extra += 1;  // the "." glyph sits one pixel right of the preview font
    draw_hints(ctx, d.date, d.date_cols, L.date_hx, L.date_hy, L.date_dx, L.date_hadj, L.font_adj,
               s.d_hint, s.date_deco, d.date_deco, d.date_deco_cols, extra);
  }

  draw_bluetooth(ctx, b);
  if (battery_visible()) draw_battery(ctx, b);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void resubscribe_tick(void) {
  tick_timer_service_unsubscribe();
  // the sleep screen only changes once a minute
  tick_timer_service_subscribe(s.seconds && !sleep_screen_active() ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
}

static void sleep_screen_style(SleepScreenStyle *style) {
  *style = (SleepScreenStyle){
    .bg = s.bg, .value = s.on, .label = PBL_IF_COLOR_ELSE(s.d_on, s.on), .hour24 = hour24(),
  };
}

static void sleep_mode_changed(bool active) {
  resubscribe_tick();
  layer_mark_dirty(s_canvas);
}

static void tap_timeout_cb(void *data) {
  s_tap_timer = NULL;
  s_tapped = false;
  layer_mark_dirty(s_canvas);
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
  s_tapped = true;
  if (s_tap_timer) app_timer_cancel(s_tap_timer);
  s_tap_timer = app_timer_register(s.tap_timeout_ms, tap_timeout_cb, NULL);
  layer_mark_dirty(s_canvas);
}

static void update_tap_subscription(void) {
  accel_tap_service_unsubscribe();
  bool needed = s.time_hint == HINT_TAP || s.date_hint == HINT_TAP ||
                s.batt_icon == 2 || s.batt_icon == 3;
  if (needed) accel_tap_service_subscribe(tap_handler);
}

static void tick_handler(struct tm *t, TimeUnits changed) {
  int old_shown = displayed_seconds();
  bool hour_changed = (t->tm_hour != s_tm.tm_hour);
  s_tm = *t;
  if (changed & MINUTE_UNIT) sleep_screen_minute_tick();
  if (hour_changed && t->tm_min == 0 && (s_hourly_vibe_asleep || !sleep_screen_active())) vibrate(s.hourly_vibe);
  // seconds update only when the displayed value changes (see SHOW_SECONDS_INTERVAL)
  if ((changed & MINUTE_UNIT) || !s.seconds || displayed_seconds() != old_shown)
    layer_mark_dirty(s_canvas);
}

static void bt_icon_timeout(void *data) {
  s_bt_timer = NULL;
  bt_icon_destroy();
  layer_mark_dirty(s_canvas);
}

static void bt_handler(bool connected) {
  bt_icon_destroy();
  if (connected) {
    if (s_bt_loss_vibed) {
      s_bt_loss_vibed = false;
      vibrate(s.bt_reconnect_vibe);
    }
    if (s_bt_state != 0) {
      s_bt_state = 2;
      if (s.bt_icon == 1) {
        s_bt_path = gpath_create(&BT_PATH_INFO);
        if (s_bt_timer) app_timer_cancel(s_bt_timer);
        s_bt_timer = app_timer_register(5000, bt_icon_timeout, NULL);
      }
    }
  } else {
    s_bt_state = 1;
    if (s.bt_icon == 1) {
      s_bt_path = gpath_create(&BT_PATH_INFO);
      if (s_bt_timer) { app_timer_cancel(s_bt_timer); s_bt_timer = NULL; }
    }
    if (!s_bt_loss_vibed) {
      s_bt_loss_vibed = true;
      vibrate(s.bt_vibe);
    }
  }
  s_bt_connected = connected;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void batt_handler(BatteryChargeState state) {
  s_batt = state;
  if (battery_low()) vibrate(s.batt_vibe);
  layer_mark_dirty(s_canvas);
}

#define U8(KEY, FIELD) if ((t = dict_find(it, MESSAGE_KEY_##KEY))) s.FIELD = (uint8_t)t->value->int32
#if defined(PBL_COLOR)
#define COLOR(KEY, FIELD) if ((t = dict_find(it, MESSAGE_KEY_##KEY))) s.FIELD = GColorFromHEX(t->value->int32)
#else
// black & white watches: anything that is not black is white
#define COLOR(KEY, FIELD) if ((t = dict_find(it, MESSAGE_KEY_##KEY))) \
  s.FIELD = (t->value->int32 & 0xFFFFFF) ? GColorWhite : GColorBlack
#endif

static void send_time_format(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_TIME_FORMAT, clock_is_24h_style() ? 1 : 0);
    app_message_outbox_send();
  }
}

static void inbox_received(DictionaryIterator *it, void *ctx) {
  Tuple *t;
  if (dict_find(it, MESSAGE_KEY_READY)) {
    send_time_format();
  }
  sleep_screen_inbox(it);
  if (!dict_find(it, MESSAGE_KEY_TRUE_BINARY)) return;  // not a configuration message

  U8(TRUE_BINARY, true_binary); U8(DISPLAY_FALSE, display_false); U8(DISPLAY_IMPOSSIBLE, show_impossible);
  U8(TRUE_BORDER, true_border); U8(FALSE_BORDER, false_border);
  COLOR(BACKGROUND_COLOR, bg); COLOR(TRUE_COLOR, on); COLOR(FALSE_COLOR, off); COLOR(HINT_COLOR, hint);
  COLOR(DATE_TRUE_COLOR, d_on); COLOR(DATE_FALSE_COLOR, d_off); COLOR(DATE_HINT_COLOR, d_hint);
  U8(DISPLAY_SECONDS, seconds); U8(SHOW_SECONDS_INTERVAL, sec_interval);
  U8(TIME_ALIGNMENT, time_align); U8(TIME_FORMAT, time_format); U8(TIME_HINT, time_hint); U8(TIME_DECO, time_deco);
  U8(SHOW_DATE, show_date); U8(DATE_POS, date_pos); U8(DATE_ALIGNMENT, date_align);
  U8(DATE_FORMAT, date_format); U8(DATE_HINT, date_hint); U8(DATE_DECO, date_deco);
  U8(BLUETOOTH_ICON, bt_icon); U8(BLUETOOTH_VIBRATE, bt_vibe); U8(BLUETOOTH_RECONNECT_VIBRATE, bt_reconnect_vibe);
  U8(BATTERY_ICON, batt_icon); U8(BATTERY_VIBRATE, batt_vibe);
  U8(VIBRATE_HOURLY, hourly_vibe); U8(NO_VIBE_CHARGING, no_vibe_charging);
  U8(QUICK_VIEW_MODE, quick_view);
  if ((t = dict_find(it, MESSAGE_KEY_TAPPING_EFFECT_TIMEOUT))) s.tap_timeout_ms = (uint16_t)t->value->int32;
  if ((t = dict_find(it, MESSAGE_KEY_VIBRATE_HOURLY_ASLEEP))) s_hourly_vibe_asleep = t->value->int32 ? 1 : 0;
  if (s.sec_interval == 0) s.sec_interval = 1;
  if (s.date_format > 4) s.date_format = 0;
  if (s.display_false > DF_OUTLINE) s.display_false = DF_SOLID;

  persist_write_data(PERSIST_KEY, &s, sizeof s);
  persist_write_int(PERSIST_KEY_HOURLY_ASLEEP, s_hourly_vibe_asleep);
  resubscribe_tick();
  update_tap_subscription();
  layout_changed();
  layer_mark_dirty(s_canvas);
}

#if !defined(PBL_PLATFORM_APLITE)
static void unobstructed_change(AnimationProgress progress, void *ctx) {
  layer_mark_dirty(s_canvas);
}
#endif

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *w) {
  layer_destroy(s_canvas);
}

static void init(void) {
  settings_defaults();
  if (persist_exists(PERSIST_KEY) && persist_get_size(PERSIST_KEY) == (int)sizeof s)
    persist_read_data(PERSIST_KEY, &s, sizeof s);
  s_hourly_vibe_asleep = persist_read_int(PERSIST_KEY_HOURLY_ASLEEP) ? 1 : 0;  // 0 when never set
  s_hour24 = clock_is_24h_style();

  s_hint_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_PEBBLE_SQUARE_HINT));

  time_t now = time(NULL);
  s_tm = *localtime(&now);
  s_bt_connected = connection_service_peek_pebble_app_connection();
  s_bt_state = s_bt_connected ? 0 : 1;
  s_batt = battery_state_service_peek();

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  bt_handler(s_bt_connected);
  sleep_screen_init(window_get_root_layer(s_window), sleep_screen_style, sleep_mode_changed);

  resubscribe_tick();
  update_tap_subscription();
  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = bt_handler });
  battery_state_service_subscribe(batt_handler);
  batt_handler(s_batt);
#if !defined(PBL_PLATFORM_APLITE)
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers){ .change = unobstructed_change }, NULL);
#endif
  app_message_register_inbox_received(inbox_received);
  app_message_open(app_message_inbox_size_maximum() > 512 ? 512 : app_message_inbox_size_maximum(), 64);
}

static void deinit(void) {
  sleep_screen_deinit();
#if !defined(PBL_PLATFORM_APLITE)
  unobstructed_area_service_unsubscribe();
#endif
  accel_tap_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  fonts_unload_custom_font(s_hint_font);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
