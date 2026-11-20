// Charging screen: while the watch is on its charger the watchface is replaced by a charge ring with
// a bolt and the percentage in the middle, and below it the estimated time until full and the
// estimated charging power.
#pragma once
#include <pebble.h>

typedef struct {
  GColor bg;      // background
  GColor accent;  // filled part of the ring, bolt, percentage, readings
  GColor label;   // labels
  GColor track;   // empty part of the ring
} ChargingScreenStyle;

typedef void (*ChargingScreenStyleProvider)(ChargingScreenStyle *style);
// Called whenever the screen starts or stops replacing the watchface.
typedef void (*ChargingScreenModeHandler)(bool active);

// The screen is shown while this is true.
static inline bool charging_screen_wanted(BatteryChargeState state) {
  return state.is_plugged || state.is_charging;
}

// Adds the screen as a layer on top of `parent`'s children; it stays hidden while unplugged.
void charging_screen_init(Layer *parent, BatteryChargeState state, ChargingScreenStyleProvider style,
                          ChargingScreenModeHandler on_mode_change);
void charging_screen_deinit(void);
bool charging_screen_active(void);
// Call on every battery state change.
void charging_screen_battery(BatteryChargeState state);
// Call on every tick (seconds while the screen is active: the countdown can show seconds).
void charging_screen_tick(void);
// Handles the CHARGE_TEST key of test builds (see charging_screen.c).
void charging_screen_inbox(DictionaryIterator *it);
