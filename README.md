# Binary Time

A Pebble binary (BCD) watchface, rebuilt from scratch from a closed-source watchface that has no
published source.

## Build

```
nix-shell                       # pebble.nix pinned with npins (see shell.nix)
pebble sdk install 4.9.169      # once; newer SDKs need pebble-tool >= 5.1
pebble build                    # -> build/<directory-name>.pbw (aplite basalt chalk diorite emery)
pebble install --emulator basalt
```

## Sleep screen

While Pebble Health reports the wearer asleep, the watchface is replaced by a terminal-style panel for the
people around them (`src/c/sleep_screen.c`; not on aplite, which has no Health API). It shows the sleep phase,
when the night started, time slept against the typical night, deep sleep time and number of deep periods,
wake-ups, recent movement, heart rate with the night's range (only on watches with a sensor), the next alarm
with a countdown, and Quiet Time. Health reports sleep with a delay, so the panel appears some minutes after
the wearer falls asleep.
While it is shown the hourly vibration is skipped, unless "Hourly vibration while asleep" is turned on in the
settings (that option is hidden on aplite).

Pebble firmware only distinguishes light and restful (deep) sleep. The phases are a table in
`sleep_screen.c`; when a firmware reports REM sleep, set `HEALTH_ACTIVITY_REM_SLEEP` to its activity bit and
the REM phase, its row (time and number of REM periods) and detection follow.

A watchface cannot read the system Alarms app, so alarms are pushed to it by AppMessage, e.g. from a phone
automation through PebbleKit (app UUID `3ddc5ba8-09d8-4f37-bc40-5930ba1fced1`):

| key | id | value |
|---|---|---|
| `ALARM_TIME` | 10034 | UTC seconds of the next alarm; 0 clears it; a time already passed means "ringing now" |
| `ALARM_SNOOZED` | 10035 | 1 when that alarm is a snooze (optional) |

When the alarm time is reached the watchface comes back. A snooze (`ALARM_SNOOZED=1` with the snooze end)
brings the sleep screen back; any other new alarm does not, so pushing tomorrow's alarm right after
dismissing today's keeps the watchface up. The new keys are appended to `messageKeys` so the ids of the
existing ones do not change.

For screenshots, `SLEEP_SCREEN_TEST=1 pebble build` compiles in a test mode: an AppMessage `SLEEP_TEST`
(id 10037) with a phase (1 light, 2 deep, 0 off) fakes that sleep phase with fixed readings, instead of
Pebble Health, and clears the alarm state. Normal builds ignore the key.

## Visual regression tests

Screenshots are compared pixel by pixel with committed baselines (`tools/lib/png-diff.js`, no npm deps).
A run writes `current/`, plus red-on-grey diff images in `diff/` for anything that changed. It exits non-zero
when an image differs by more than `--threshold` (fraction of pixels, default 0.001) or is missing.
After an intended change, rerun with `--update` and commit the new baselines.

| what | command (inside nix-shell) | baselines |
|---|---|---|
| watchface in the emulator (18 scenes × 5 platforms) | `tools/screenshots.sh [--update] [--only REGEX] [platform...]` | `test/screenshots/baseline/` |
| settings page in headless Chromium (7 scenes × 5 platforms) | `node tools/settings-screenshots.js [--update] [--only REGEX] [platform...]` | `test/settings-screenshots/baseline/` |
| PNG diff library | `node --test test/png-diff.test.js` | |

The watchface scenes send their settings through the real phone path: `pebble emu-app-config` runs with
`tools/lib/fake-config-browser.js` as the browser, which returns the scene's settings JSON. The scenes also set the
emulated battery, quick view and tap state, and pin the clock (UTC) right before each shot. Each scene's
settings are merged over `test/screenshots/base-config.json`. The emulator runs from a private
`test/screenshots/.state` and is shut down at the end. The settings page runs with `Date` frozen at the same moment.
