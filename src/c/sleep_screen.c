#include "sleep_screen.h"

#if defined(PBL_HEALTH)

#if defined(PBL_PLATFORM_EMERY)
#define TUI_FONT RESOURCE_ID_FONT_TUI_16
#else
#define TUI_FONT RESOURCE_ID_FONT_TUI_12
#endif

#define PERSIST_KEY_ALARM 3

#define NIGHT_LOOKBACK (16 * SECONDS_PER_HOUR)
// Sleep sessions separated by less than this belong to the same night (the gap is a wake-up).
#define WAKE_GAP_MAX (60 * SECONDS_PER_MINUTE)
// An alarm that passed longer ago than this (watchface not running at the time) is dropped
// without switching back to the watchface.
#define ALARM_TRIGGER_WINDOW (30 * SECONDS_PER_MINUTE)
#define MOVE_MINUTES 10
#define MOVE_CELLS 8
#define LABEL_COLS 7

// ---------------------------------------------------------------------------
// Sleep phases
// ---------------------------------------------------------------------------

typedef enum {
  SLEEP_PHASE_AWAKE,
  SLEEP_PHASE_LIGHT,
  SLEEP_PHASE_DEEP,
  SLEEP_PHASE_REM,
  SLEEP_PHASE_COUNT
} SleepPhase;

// Health activity that marks REM sleep. Pebble firmware only reports sleep and restful (deep)
// sleep, so this is 0 and the REM phase is never detected. Once a firmware reports REM, set
// this to its HealthActivity bit: phase detection, the REM row (time spent and number of REM
// periods, i.e. cycles) and the night summary pick it up from the table below.
#define HEALTH_ACTIVITY_REM_SLEEP ((HealthActivityMask)0)

typedef struct {
  const char *label;
  HealthActivityMask activity;  // activity reported while in this phase, 0 = never reported
  bool summary;                 // show a row with time spent and number of periods tonight
} PhaseDef;

// Detection picks the highest entry whose activity is currently reported. LIGHT must come
// before the specific phases since HealthActivitySleep is reported for every sleep phase.
static const PhaseDef PHASES[SLEEP_PHASE_COUNT] = {
  [SLEEP_PHASE_AWAKE] = { "AWAKE", HealthActivityNone,         false },
  [SLEEP_PHASE_LIGHT] = { "LIGHT", HealthActivitySleep,        false },
  [SLEEP_PHASE_DEEP]  = { "DEEP",  HealthActivityRestfulSleep, true  },
  [SLEEP_PHASE_REM]   = { "REM",   HEALTH_ACTIVITY_REM_SLEEP,  true  },
};

static SleepPhase phase_from_activities(HealthActivityMask current) {
  for (int p = SLEEP_PHASE_COUNT - 1; p > SLEEP_PHASE_AWAKE; p--) {
    if (PHASES[p].activity && (current & PHASES[p].activity)) return (SleepPhase)p;
  }
  return SLEEP_PHASE_AWAKE;
}

// ---------------------------------------------------------------------------
// Test mode
// ---------------------------------------------------------------------------

// Built with SLEEP_SCREEN_TEST=1 (see wscript) the screen can be driven by a SLEEP_TEST
// AppMessage instead of Pebble Health, which the emulator cannot put to sleep. The value is the
// phase to fake (0 = awake); every such message also clears the alarm state, so screenshot
// scenes start from the same point. Readings are fixed (see test_info) so screenshots are stable.
#if defined(SLEEP_SCREEN_TEST)
static int s_test_phase = -1;  // -1: use Pebble Health
#endif

static SleepPhase current_phase(void) {
#if defined(SLEEP_SCREEN_TEST)
  if (s_test_phase >= 0) return (SleepPhase)s_test_phase;
#endif
  return phase_from_activities(health_service_peek_current_activities());
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
  int32_t time;     // UTC time of the next alarm, 0 = none
  uint8_t snoozed;  // the pending alarm is a snooze
  uint8_t woke;     // an alarm went off: show the watchface until awake or snoozed
} AlarmState;

typedef struct {
  SleepPhase phase;
  time_t night_start;                    // 0 = unknown
  int32_t slept;                         // seconds asleep tonight, wake gaps excluded
  int wakes;
  int32_t phase_time[SLEEP_PHASE_COUNT]; // for PHASES[].summary
  int phase_count[SLEEP_PHASE_COUNT];
  int32_t typical;                       // average daily sleep, 0 = no history
  int move;                              // 0..MOVE_CELLS, -1 = no data
  bool hr_supported;
  int hr, hr_min, hr_max;                // 0 = no reading
} SleepInfo;

static Layer *s_layer;
static SleepScreenStyleProvider s_style;
static SleepScreenModeHandler s_on_mode_change;
static GFont s_font;
static bool s_asleep;
static bool s_active;
static time_t s_asleep_seen;  // when this app first saw the current sleep
static AlarmState s_alarm;
static SleepInfo s_info;

static void alarm_save(void) {
  persist_write_data(PERSIST_KEY_ALARM, &s_alarm, sizeof s_alarm);
}

// ---------------------------------------------------------------------------
// Health readings
// ---------------------------------------------------------------------------

typedef struct {
  time_t now;
  time_t start, end;
  int32_t slept;
  int sessions;
} NightScan;

// Sessions arrive newest first; chain them while the gaps between them are short.
static bool night_cb(HealthActivity activity, time_t start, time_t end, void *context) {
  NightScan *n = context;
  if (end > n->now) end = n->now;
  if (n->sessions == 0) {
    if (end < n->now - WAKE_GAP_MAX) return false;  // last night, not this one
    n->end = end;
  } else if (end < n->start - WAKE_GAP_MAX) {
    return false;
  } else if (end > n->start) {
    end = n->start;  // overlapping records
  }
  if (end > start) n->slept += end - start;
  if (n->sessions == 0 || start < n->start) n->start = start;
  n->sessions++;
  return true;
}

typedef struct {
  time_t from, to;
  int32_t total;
  int count;
} PhaseScan;

static bool phase_cb(HealthActivity activity, time_t start, time_t end, void *context) {
  PhaseScan *p = context;
  if (start < p->from) start = p->from;
  if (end > p->to) end = p->to;
  if (end > start) {
    p->total += end - start;
    p->count++;
  }
  return true;
}

static int move_level(time_t now) {
  HealthMinuteData data[MOVE_MINUTES];
  time_t start = now - MOVE_MINUTES * SECONDS_PER_MINUTE, end = now;
  uint32_t n = health_service_get_minute_history(data, MOVE_MINUTES, &start, &end);
  uint32_t sum = 0, valid = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (data[i].is_invalid) continue;
    sum += data[i].vmc;
    valid++;
  }
  if (!valid) return -1;
  // logarithmic: every cell doubles the average movement (16, 32, ... 2048 VMC per minute)
  uint32_t avg = sum / valid;
  int level = 0;
  for (uint32_t v = avg / 8; v > 1 && level < MOVE_CELLS; v >>= 1) level++;
  return level;
}

// Heart rate sensors only exist on Pebble 2 HR (diorite) and Pebble Time 2 (emery). The
// accessibility check alone is not enough: the basalt emulator reports heart rate as available.
// It does catch diorite watches without a sensor (Pebble 2 SE) and a denied permission.
#if defined(PBL_PLATFORM_DIORITE) || defined(PBL_PLATFORM_EMERY)
#define HR_SENSOR_POSSIBLE 1
#else
#define HR_SENSOR_POSSIBLE 0
#endif

static bool hr_supported(time_t now) {
  if (!HR_SENSOR_POSSIBLE) return false;
  HealthServiceAccessibilityMask m = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
  return !(m & (HealthServiceAccessibilityMaskNotSupported | HealthServiceAccessibilityMaskNoPermission));
}

static int hr_aggregate(time_t from, time_t to, HealthAggregation agg) {
  if (!(health_service_metric_aggregate_averaged_accessible(HealthMetricHeartRateBPM, from, to, agg,
          HealthServiceTimeScopeOnce) & HealthServiceAccessibilityMaskAvailable)) return 0;
  return health_service_aggregate_averaged(HealthMetricHeartRateBPM, from, to, agg, HealthServiceTimeScopeOnce);
}

static void refresh_heart_rate(time_t now) {
  SleepInfo *i = &s_info;
#if defined(SLEEP_SCREEN_TEST)
  if (s_test_phase >= 0) return;  // keep the fixed readings
#endif
  i->hr_supported = hr_supported(now);
  i->hr = i->hr_min = i->hr_max = 0;
  if (!i->hr_supported) return;
  i->hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
  if (i->night_start && i->night_start < now) {
    i->hr_min = hr_aggregate(i->night_start, now, HealthAggregationMin);
    i->hr_max = hr_aggregate(i->night_start, now, HealthAggregationMax);
  }
}

#if defined(SLEEP_SCREEN_TEST)
static void test_info(time_t now) {
  const time_t minute = now - now % SECONDS_PER_MINUTE;
  s_info = (SleepInfo){
    .phase = current_phase(),
    .night_start = minute - (2 * SECONDS_PER_HOUR + 14 * SECONDS_PER_MINUTE),
    .slept = 2 * SECONDS_PER_HOUR + 3 * SECONDS_PER_MINUTE,
    .wakes = 1,
    .typical = 7 * SECONDS_PER_HOUR + 40 * SECONDS_PER_MINUTE,
    .move = 2,
    .hr_supported = HR_SENSOR_POSSIBLE,
    .hr = 54, .hr_min = 48, .hr_max = 61,
  };
  for (int p = 0; p < SLEEP_PHASE_COUNT; p++) {
    if (!PHASES[p].summary || !PHASES[p].activity) continue;
    s_info.phase_time[p] = 52 * SECONDS_PER_MINUTE;
    s_info.phase_count[p] = 2;
  }
}
#endif

static void refresh_info(void) {
  const time_t now = time(NULL);
  SleepInfo *i = &s_info;
#if defined(SLEEP_SCREEN_TEST)
  if (s_test_phase >= 0) {
    test_info(now);
    return;
  }
#endif
  i->phase = current_phase();

  NightScan night = { .now = now };
  health_service_activities_iterate(HealthActivitySleep, now - NIGHT_LOOKBACK, now,
                                    HealthIterationDirectionPast, night_cb, &night);
  if (night.sessions) {
    i->night_start = night.start;
    i->wakes = night.sessions - 1;
    // the session still in progress may not be recorded up to the current minute yet
    i->slept = night.slept + (s_asleep && now > night.end ? now - night.end : 0);
  } else {
    i->night_start = s_asleep_seen;
    i->wakes = 0;
    i->slept = s_asleep_seen ? now - s_asleep_seen : 0;
  }

  for (int p = 0; p < SLEEP_PHASE_COUNT; p++) {
    i->phase_time[p] = 0;
    i->phase_count[p] = 0;
    if (!PHASES[p].summary || !PHASES[p].activity || !i->night_start) continue;
    PhaseScan scan = { .from = i->night_start, .to = now };
    health_service_activities_iterate(PHASES[p].activity, i->night_start, now,
                                      HealthIterationDirectionFuture, phase_cb, &scan);
    i->phase_time[p] = scan.total;
    i->phase_count[p] = scan.count;
  }

  const time_t day = time_start_of_today();
  i->typical = 0;
  if (health_service_metric_averaged_accessible(HealthMetricSleepSeconds, day, day + SECONDS_PER_DAY,
          HealthServiceTimeScopeDaily) & HealthServiceAccessibilityMaskAvailable) {
    i->typical = health_service_sum_averaged(HealthMetricSleepSeconds, day, day + SECONDS_PER_DAY,
                                             HealthServiceTimeScopeDaily);
  }

  i->move = move_level(now);
  refresh_heart_rate(now);
}

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------

static void update_mode(void) {
  const bool active = s_asleep && !s_alarm.woke;
  if (active == s_active) return;
  s_active = active;
  if (active) refresh_info();
  layer_set_hidden(s_layer, !active);
  if (s_on_mode_change) s_on_mode_change(active);
}

static void read_sleep_state(void) {
  const bool asleep = current_phase() != SLEEP_PHASE_AWAKE;
  if (asleep && !s_asleep) s_asleep_seen = time(NULL);
  if (!asleep) {
    s_asleep_seen = 0;
    if (s_alarm.woke) {
      s_alarm.woke = 0;
      alarm_save();
    }
  }
  s_asleep = asleep;
}

static void alarm_check(time_t now) {
  if (!s_alarm.time || now < s_alarm.time) return;
  const bool recent = now - s_alarm.time < ALARM_TRIGGER_WINDOW;
  s_alarm.time = 0;
  s_alarm.snoozed = 0;
  if (recent && s_asleep) s_alarm.woke = 1;
  alarm_save();
}

static void health_handler(HealthEventType event, void *context) {
  switch (event) {
    case HealthEventSignificantUpdate:
    case HealthEventSleepUpdate:
      read_sleep_state();
      update_mode();
      if (s_active) refresh_info();
      break;
    case HealthEventHeartRateUpdate:
      if (s_active) refresh_heart_rate(time(NULL));
      break;
    default:
      return;
  }
  if (s_active) layer_mark_dirty(s_layer);
}

void sleep_screen_minute_tick(void) {
  alarm_check(time(NULL));
  read_sleep_state();
  update_mode();
  if (s_active) {
    refresh_info();
    layer_mark_dirty(s_layer);
  }
}

static int32_t tuple_int(const Tuple *t) {
  const bool sign = t->type == TUPLE_INT;
  switch (t->length) {
    case 1: return sign ? t->value->int8 : t->value->uint8;
    case 2: return sign ? t->value->int16 : t->value->uint16;
    default: return sign ? t->value->int32 : (int32_t)t->value->uint32;
  }
}

void sleep_screen_inbox(DictionaryIterator *it) {
#if defined(SLEEP_SCREEN_TEST)
  Tuple *test = dict_find(it, MESSAGE_KEY_SLEEP_TEST);
  if (test) {
    const int32_t phase = tuple_int(test);
    s_test_phase = (phase > 0 && phase < SLEEP_PHASE_COUNT) ? phase : SLEEP_PHASE_AWAKE;
    s_alarm = (AlarmState){ 0 };
    alarm_save();
    read_sleep_state();
    update_mode();
    if (s_active) refresh_info();
    layer_mark_dirty(s_layer);
  }
#endif
  Tuple *t = dict_find(it, MESSAGE_KEY_ALARM_TIME);
  if (!t) return;
  const int32_t when = tuple_int(t);
  Tuple *z = dict_find(it, MESSAGE_KEY_ALARM_SNOOZED);
  s_alarm.time = when > 0 ? when : 0;
  s_alarm.snoozed = (s_alarm.time && z && tuple_int(z)) ? 1 : 0;
  // A snooze sends the wearer back to sleep. A newly scheduled alarm does not: it is often
  // pushed right after the previous one was dismissed, while the wearer is waking up.
  if (s_alarm.snoozed) s_alarm.woke = 0;
  alarm_save();
  alarm_check(time(NULL));  // a time that already passed means "ringing now"
  update_mode();
  layer_mark_dirty(s_layer);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void fmt_clock(char *buf, size_t n, time_t t, bool hour24) {
  struct tm *tm = localtime(&t);
  if (hour24) {
    snprintf(buf, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
  } else {
    int h = tm->tm_hour % 12;
    snprintf(buf, n, "%d:%02d%c", h ? h : 12, tm->tm_min, tm->tm_hour < 12 ? 'a' : 'p');
  }
}

static void fmt_duration(char *buf, size_t n, int32_t sec) {
  if (sec < 0) sec = 0;
  snprintf(buf, n, "%dh%02d", (int)(sec / SECONDS_PER_HOUR), (int)(sec / SECONDS_PER_MINUTE % 60));
}

// Short countdown: +3h48, +13h, +2d
static void fmt_countdown(char *buf, size_t n, int32_t sec) {
  if (sec < 0) sec = 0;
  if (sec < 10 * SECONDS_PER_HOUR) {
    buf[0] = '+';
    fmt_duration(buf + 1, n - 1, sec);
  } else if (sec < SECONDS_PER_DAY) {
    snprintf(buf, n, "+%dh", (int)(sec / SECONDS_PER_HOUR));
  } else {
    snprintf(buf, n, "+%dd", (int)(sec / SECONDS_PER_DAY));
  }
}

#define MAX_ROWS (6 + SLEEP_PHASE_COUNT)
#define VALUE_LEN 20

typedef struct {
  const char *label;
  char value[VALUE_LEN];
} Row;

static int build_rows(Row *rows, const SleepScreenStyle *st) {
  const SleepInfo *i = &s_info;
  const time_t now = time(NULL);
  char a[10], b[10];
  int n = 0;

  rows[n].label = "PHASE";
  snprintf(rows[n++].value, VALUE_LEN, "%s", PHASES[i->phase].label);

  rows[n].label = "SINCE";
  if (i->night_start) fmt_clock(rows[n].value, VALUE_LEN, i->night_start, st->hour24);
  else snprintf(rows[n].value, VALUE_LEN, "--");
  n++;

  rows[n].label = "SLEPT";
  fmt_duration(a, sizeof a, i->slept);
  if (i->typical) {
    fmt_duration(b, sizeof b, i->typical);
    snprintf(rows[n].value, VALUE_LEN, "%s/%s", a, b);  // tonight / typical night
  } else {
    snprintf(rows[n].value, VALUE_LEN, "%s", a);
  }
  n++;

  for (int p = 0; p < SLEEP_PHASE_COUNT; p++) {
    if (!PHASES[p].summary || !PHASES[p].activity) continue;
    rows[n].label = PHASES[p].label;
    fmt_duration(a, sizeof a, i->phase_time[p]);
    snprintf(rows[n++].value, VALUE_LEN, "%s x%d", a, i->phase_count[p]);
  }

  rows[n].label = "WAKES";
  snprintf(rows[n++].value, VALUE_LEN, "%d", i->wakes);

  rows[n].label = "MOVE";
  if (i->move < 0) {
    snprintf(rows[n].value, VALUE_LEN, "--");
  } else {
    char *v = rows[n].value;
    v[0] = '[';
    for (int c = 0; c < MOVE_CELLS; c++) v[1 + c] = c < i->move ? '#' : '.';
    v[1 + MOVE_CELLS] = ']';
    v[2 + MOVE_CELLS] = 0;
  }
  n++;

  if (i->hr_supported) {
    rows[n].label = "HR";
    if (!i->hr) snprintf(rows[n].value, VALUE_LEN, "--");
    else if (i->hr_min && i->hr_max) snprintf(rows[n].value, VALUE_LEN, "%d %d-%d", i->hr, i->hr_min, i->hr_max);
    else snprintf(rows[n].value, VALUE_LEN, "%d BPM", i->hr);
    n++;
  }

  rows[n].label = s_alarm.snoozed ? "SNOOZE" : "ALARM";
  if (s_alarm.time) {
    fmt_clock(a, sizeof a, s_alarm.time, st->hour24);
    fmt_countdown(b, sizeof b, s_alarm.time - now);
    snprintf(rows[n].value, VALUE_LEN, "%s %s", a, b);
  } else {
    snprintf(rows[n].value, VALUE_LEN, "NONE");
  }
  n++;

  rows[n].label = "QUIET";
  snprintf(rows[n++].value, VALUE_LEN, "%s", quiet_time_is_active() ? "ON" : "OFF");
  return n;
}

static void text(GContext *ctx, const char *s, int x, int y, int w, int h, GTextAlignment align) {
  graphics_draw_text(ctx, s, s_font, GRect(x, y, w, h), GTextOverflowModeTrailingEllipsis, align, NULL);
}

static GSize font_box(const char *sample) {
  return graphics_text_layout_get_content_size(sample, s_font, GRect(0, 0, 500, 100),
                                               GTextOverflowModeWordWrap, GTextAlignmentLeft);
}

// Width of `cols` characters of the monospace font.
static int text_width(int cols) {
  static int s_cw;
  if (!s_cw) s_cw = font_box("0000000000").w / 10;
  return cols * (s_cw > 0 ? s_cw : 1);
}

static int line_height(void) {
  static int s_lh;
  if (!s_lh) s_lh = font_box("Ag").h + 1;
  return s_lh;
}

// Layout in text rows of height `lh`: a title row, the readings, and half a row of bottom margin.
// Text drawn at row y has its capitals from about y + lh / 4 to y + 5 * lh / 6.
static void layer_update(Layer *layer, GContext *ctx) {
  static Row rows[MAX_ROWS];  // static: the app stack is small and text rendering needs most of it
  SleepScreenStyle style;
  s_style(&style);
  const SleepScreenStyle *st = &style;
  const GRect bounds = layer_get_unobstructed_bounds(layer);
  const int n = build_rows(rows, st);
  const int W = bounds.size.w, H = bounds.size.h;
  const int pad = PBL_IF_ROUND_ELSE(H / 12, 2);
  int lh = line_height();
  if ((2 * n + 3) * lh / 2 > H - 2 * pad) lh = (H - 2 * pad) * 2 / (2 * n + 3);  // squeeze to fit
  const int text_h = lh + lh / 2;  // room for descenders
  const int total = (2 * n + 3) * lh / 2;
  const int top = bounds.origin.y + (H - total) / 2;
  const int body_cols = LABEL_COLS + VALUE_LEN / 2 + 1;
  const int body_w = text_width(body_cols);
  const int left = bounds.origin.x + (W - body_w) / 2;

  graphics_context_set_fill_color(ctx, st->bg);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  graphics_context_set_text_color(ctx, st->label);
  graphics_context_set_stroke_color(ctx, st->label);

  char clock[10], stamp[12];
  fmt_clock(clock, sizeof clock, time(NULL), st->hour24);
  snprintf(stamp, sizeof stamp, "[%s]", clock);
  static const char TITLE[] = "[ASLEEP]";
  const int title_w = text_width(sizeof TITLE - 1), stamp_w = text_width(strlen(stamp));

#if defined(PBL_ROUND)
  // the corners would cut a box off: title and clock centred, rules above and below the readings
  const int gap = text_width(1);
  const int tx = bounds.origin.x + (W - title_w - gap - stamp_w) / 2;
  text(ctx, TITLE, tx, top, title_w + 4, text_h, GTextAlignmentLeft);
  text(ctx, stamp, tx + title_w + gap, top, stamp_w + 4, text_h, GTextAlignmentLeft);
  const int rule_w = text_width(body_cols - 4), rx = bounds.origin.x + (W - rule_w) / 2;
  const int rule_top = top + lh + lh / 4, rule_bottom = top + (n + 1) * lh + lh / 4;
  graphics_draw_line(ctx, GPoint(rx, rule_top), GPoint(rx + rule_w, rule_top));
  graphics_draw_line(ctx, GPoint(rx, rule_bottom), GPoint(rx + rule_w, rule_bottom));
#else
  // box whose top edge runs through the title row, title and clock sit in gaps of that edge
  const int fx = bounds.origin.x + 1, fr = bounds.origin.x + W - 2;
  const int fb = top + total - 1;
  const int cap_mid = top + lh * 3 / 5;  // vertical middle of the title's capitals
  const int cw = text_width(1);
  graphics_draw_rect(ctx, GRect(fx, cap_mid, fr - fx + 1, fb - cap_mid + 1));
  const int title_x = fx + cw, stamp_x = fr - cw - stamp_w;
  graphics_context_set_fill_color(ctx, st->bg);
  graphics_fill_rect(ctx, GRect(title_x - 1, cap_mid, title_w + 2, 1), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(stamp_x - 1, cap_mid, stamp_w + 2, 1), 0, GCornerNone);
  text(ctx, TITLE, title_x, top, title_w + 4, text_h, GTextAlignmentLeft);
  text(ctx, stamp, stamp_x, top, stamp_w + 4, text_h, GTextAlignmentLeft);
#endif

  const int label_w = text_width(LABEL_COLS);
  for (int r = 0; r < n; r++) {
    const int y = top + (r + 1) * lh;
    graphics_context_set_text_color(ctx, st->label);
    text(ctx, rows[r].label, left, y, label_w, text_h, GTextAlignmentLeft);
    graphics_context_set_text_color(ctx, st->value);
    text(ctx, rows[r].value, left + label_w, y, body_w - label_w, text_h, GTextAlignmentLeft);
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool sleep_screen_active(void) { return s_active; }

void sleep_screen_init(Layer *parent, SleepScreenStyleProvider style, SleepScreenModeHandler on_mode_change) {
  s_style = style;
  s_layer = layer_create(layer_get_bounds(parent));
  layer_set_update_proc(s_layer, layer_update);
  layer_set_hidden(s_layer, true);
  layer_add_child(parent, s_layer);
  s_font = fonts_load_custom_font(resource_get_handle(TUI_FONT));
  if (persist_exists(PERSIST_KEY_ALARM) && persist_get_size(PERSIST_KEY_ALARM) == (int)sizeof s_alarm)
    persist_read_data(PERSIST_KEY_ALARM, &s_alarm, sizeof s_alarm);
  read_sleep_state();
  alarm_check(time(NULL));
  // the handler (if any) runs after the state above is set up
  s_on_mode_change = on_mode_change;
  update_mode();
  health_service_events_subscribe(health_handler, NULL);
}

void sleep_screen_deinit(void) {
  health_service_events_unsubscribe();
  layer_destroy(s_layer);
  fonts_unload_custom_font(s_font);
}

#endif  // PBL_HEALTH
