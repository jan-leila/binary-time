// Sleep screen: while Pebble Health reports the wearer asleep, the watchface is replaced by a
// terminal-style status panel meant for people around the wearer (sleep phase, how long they
// have slept, heart rate, the next alarm, Quiet Time).
#pragma once
#include <pebble.h>

typedef struct {
  GColor bg;      // background
  GColor value;   // readings
  GColor label;   // labels and frame
  bool hour24;    // clock times as 23:41 instead of 11:41p
} SleepScreenStyle;

// Fills in the colours and clock format of the watchface settings.
typedef void (*SleepScreenStyleProvider)(SleepScreenStyle *style);
// Called whenever the screen starts or stops replacing the watchface.
typedef void (*SleepScreenModeHandler)(bool active);

#if defined(PBL_HEALTH)

// Adds the screen as a layer on top of `parent`'s children; it stays hidden while awake.
void sleep_screen_init(Layer *parent, SleepScreenStyleProvider style, SleepScreenModeHandler on_mode_change);
void sleep_screen_deinit(void);
bool sleep_screen_active(void);
// Call on every minute change.
void sleep_screen_minute_tick(void);
// Handles the ALARM_TIME / ALARM_SNOOZED keys of an incoming AppMessage.
void sleep_screen_inbox(DictionaryIterator *it);

#else  // no Health API (aplite): the watchface is always shown

static inline void sleep_screen_init(Layer *parent, SleepScreenStyleProvider style,
                                     SleepScreenModeHandler on_mode_change) {}
static inline void sleep_screen_deinit(void) {}
static inline bool sleep_screen_active(void) { return false; }
static inline void sleep_screen_minute_tick(void) {}
static inline void sleep_screen_inbox(DictionaryIterator *it) {}

#endif
