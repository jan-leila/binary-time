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
