#!/usr/bin/env bash
# Visual regression screenshots of the watchface in the Pebble emulator.
#
#   tools/screenshots.sh [--update] [--threshold F] [--only REGEX] [platform...]   # default: all five platforms
#
# Builds the app, then per platform installs it once and walks the SCENES below. Each scene sends its settings
# through the real phone path (`pebble emu-app-config` with tools/lib/fake-config-browser.js as $BROWSER, so
# src/pkjs/index.js gets a normal 'webviewclosed'), sets the emulated battery / quick view / tap state, pins the clock
# and saves test/screenshots/current/<platform>/<scene>.png (with --update: test/screenshots/baseline/...).
# Without --update the images are then compared with the baseline (tools/lib/png-diff.js): diff images go to
# test/screenshots/diff/, a table is printed, and the exit status is non-zero when any image differs by more than
# F (fraction of pixels, default 0.001) or is missing.
#
# Determinism: every scene starts from test/screenshots/base-config.json (24 h clock, no vibrations). Sending settings
# through the phone simulator resyncs the watch clock to the host, so the clock is set last: to 21:37:58 UTC
# (2026-12-28), and the shot waits for the 21:38:00 minute tick, which redraws every face (a minute-only face does not
# redraw on a clock change by itself). Seconds use a 15 s interval, so the watch shows 21:38:00 for the next 14 s
# whatever the screenshot latency. The emulator runs from a private XDG_DATA_HOME/TMPDIR under
# test/screenshots/.state, so it never touches another emulator on the machine, and it is always shut down at the end.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v pebble >/dev/null 2>&1; then
  exec nix-shell "$ROOT/shell.nix" --run "bash $(printf '%q' "${BASH_SOURCE[0]}") $(printf '%q ' "$@")"
fi
# as shell.nix's shellHook does: a leaked LD_LIBRARY_PATH (e.g. from direnv) stops qemu from loading SDL3
unset LD_LIBRARY_PATH

UPDATE=0
THRESHOLD=0.001
ONLY=''
PLATFORMS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --update) UPDATE=1 ;;
    --threshold) THRESHOLD="$2"; shift ;;
    --only) ONLY="$2"; shift ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) PLATFORMS+=("$1") ;;
  esac
  shift
done
[ ${#PLATFORMS[@]} -gt 0 ] || PLATFORMS=(aplite basalt chalk diorite emery)

FIXED_NOW=1798493878  # 2026-12-28 21:37:58 UTC; shots are taken just after the 21:38:00 minute tick

SHOTS="$ROOT/test/screenshots"
if [ "$UPDATE" = 1 ]; then DEST="$SHOTS/baseline"; else DEST="$SHOTS/current"; fi
STATE="$SHOTS/.state"
REAL_SDK_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/pebble-sdk"
mkdir -p "$STATE/xdg/pebble-sdk" "$STATE/tmp"
[ -e "$STATE/xdg/pebble-sdk/SDKs" ] || ln -s "$REAL_SDK_DIR/SDKs" "$STATE/xdg/pebble-sdk/SDKs"
export XDG_DATA_HOME="$STATE/xdg" TMPDIR="$STATE/tmp" TZ=UTC
export BROWSER="$ROOT/tools/lib/fake-config-browser.js" SCENE_CONFIG="$STATE/scene-config.json"
echo '<html></html>' >"$STATE/config-page.html"
cd "$ROOT" || exit 1

fail=0
P=""
note() { echo "[screenshots $P] $*"; }
bad() { echo "[screenshots $P] FAIL: $*" >&2; fail=1; }

kill_emulator() {  # only ever the emulator started from $STATE
  pebble kill >/dev/null 2>&1
  for sig in TERM KILL; do
    for pid in $(ps -eo pid,args | grep -E '[q]emu-pebble|[p]ypkjs' | grep -F "$STATE/" | awk '{print $1}'); do
      kill -s "$sig" "$pid" 2>/dev/null
    done
    sleep 1
  done
}
trap kill_emulator EXIT

echo "[screenshots] building"
pebble clean >/dev/null 2>&1  # a stale build/ can miss newly added message keys
# SLEEP_SCREEN_TEST: lets the sleep scenes fake a sleep phase by AppMessage (src/c/sleep_screen.c);
# without that message the app behaves exactly like a normal build
if ! SLEEP_SCREEN_TEST=1 pebble build >"$STATE/build.log" 2>&1; then
  tail -30 "$STATE/build.log" >&2
  echo "[screenshots] pebble build failed (see $STATE/build.log)" >&2
  exit 1
fi
PBW="$STATE/app.pbw"
cp "$(ls build/*.pbw | head -n1)" "$PBW"

e() { timeout 30 pebble "$1" --emulator "$P" "${@:2}" >/dev/null 2>&1; }

configure() {  # configure 'JSON merged over base-config.json'; pebble-tool returns once the page "closed"
  node -e 'const fs = require("fs"); const [base, over, out] = process.argv.slice(1);
    fs.writeFileSync(out, JSON.stringify(Object.assign(JSON.parse(fs.readFileSync(base, "utf8")), JSON.parse(over))));' \
    "$SHOTS/base-config.json" "$1" "$SCENE_CONFIG" || { bad "bad scene JSON: $1"; return 1; }
  timeout 40 pebble emu-app-config --emulator "$P" --file "$STATE/config-page.html" >/dev/null 2>&1 \
    || timeout 40 pebble emu-app-config --emulator "$P" --file "$STATE/config-page.html" >/dev/null 2>&1 \
    || { bad "settings not delivered: $1"; return 1; }
  sleep 1
}

set_clock() {  # pin the clock (the command occasionally fails; a missed set leaves the host's time on screen)
  local try
  for try in 1 2 3; do
    e emu-set-time --utc "$FIXED_NOW" && return 0
    sleep 1
  done
  return 1
}

# shot NAME [pebble emu command ...]: the scene's emulator actions, then the clock, then the screenshot, retried as a
# whole. Every `pebble ... --emulator` command reconnects the phone simulator, which resyncs the watch clock to the
# host, so nothing may run between setting the clock and the screenshot.
shot() {
  local name=$1 f="$DEST/$P/$1.png" try c
  shift
  for try in 1 2 3; do
    for c in "$@"; do
      # shellcheck disable=SC2086
      e $c || bad "$c"
      sleep 1
    done
    set_clock || { note "emu-set-time failed, retrying"; continue; }
    sleep 3  # past the 21:38:00 minute tick, which redraws every face
    rm -f "$f"
    timeout 20 pebble screenshot --no-open --emulator "$P" "$f" >/dev/null 2>&1
    [ -s "$f" ] && { note "$name"; return 0; }
    sleep 2
  done
  bad "screenshot $name failed"
}

# scene NAME 'settings JSON' [pebble emu command ...]   (each command a single quoted string, e.g. 'emu-battery --percent 10')
scene() {
  local name=$1 cfg=$2
  shift 2
  [[ -z "$ONLY" || "$name" =~ $ONLY ]] || return 0
  e emu-battery --percent 80
  [ "$P" = aplite ] || e emu-set-timeline-quick-view off
  configure "$cfg" || return
  shot "$name" "$@"
}

# The scene list. Tap scenes go last: the tap effect stays on for base-config's tappingtimeout.
run_scenes() {
  scene 01-default         '{}'
  scene 02-no-seconds      '{"displayseconds": 0}'
  scene 03-12h             '{"timeformat": 2, "displayseconds": 0}'
  scene 04-date-top        '{"showdate": 1}'
  scene 05-date-bottom     '{"showdate": 1, "dateposition": 1, "dateformat": 2, "displayseconds": 0}'
  scene 06-hints           '{"showdate": 1, "showhint": 2, "dateshowhint": 2}'
  scene 07-false-dotted    '{"displayfalse": 2, "showdate": 1}'
  scene 08-false-shaded    '{"displayfalse": 3, "showdate": 1}'
  scene 09-false-outline   '{"displayfalse": 4, "trueborder": 1, "showdate": 1}'
  scene 10-false-off       '{"displayfalse": 0, "falseborder": 1, "showdate": 1}'
  scene 11-true-binary     '{"truebinary": 1, "showimpossible": 1, "showdate": 1}'
  scene 12-align-deco      '{"timealignment": 1, "timedeco": 1, "showdate": 1, "datealignment": 2, "datedeco": 1, "dateformat": 4}'
  scene 13-colors          '{"backgroundcolor": "ffffff", "bitcolor": "ff0055", "offcolor": "aaaaaa", "showdate": 1, "datebitcolor": "0000aa"}'
  scene 14-battery-low     '{"btlevel": 1}' 'emu-battery --percent 10'
  scene 15-battery-charge  '{"btlevel": 1}' 'emu-battery --percent 50 --charging'
  scene 16-battery-always  '{"btlevel": 4}' 'emu-battery --percent 50'
  if [ "$P" != aplite ]; then
    scene 17-quick-view    '{"quickview": 1, "showdate": 1, "displayseconds": 0}' 'emu-set-timeline-quick-view on'
  fi
  if [ "$P" != aplite ]; then  # no Health API on aplite, so no sleep screen
    # (FIXED_NOW + 2 is the 21:38:00 minute the shots are taken in; alarms are set on whole minutes)
    # The test build fakes a sleep phase on SLEEP_TEST (10037: 1 light, 2 deep, 0 off), which also clears the
    # alarm. Alarms are ALARM_TIME (10034, UTC seconds) and ALARM_SNOOZED (10035).
    local msg="send-app-message --app-uuid 3ddc5ba8-09d8-4f37-bc40-5930ba1fced1 --int"
    scene 19-sleep-deep        '{}' "$msg 10037=2"
    scene 20-sleep-light-alarm '{"timeformat": 2}' "$msg 10037=1 10034=$((FIXED_NOW + 2 + 9 * 3600 + 22 * 60))"
    scene 21-sleep-snooze      '{"backgroundcolor": "ffffff", "bitcolor": "ff0055", "datebitcolor": "0000aa"}' \
                               "$msg 10037=2 10034=$((FIXED_NOW + 2 + 8 * 60)) 10035=1"
    # the alarm went off a minute ago: the watchface is back although the wearer still sleeps
    scene 22-sleep-alarm-rang  '{}' "$msg 10037=2" "emu-set-time --utc $FIXED_NOW" "$msg 10034=$((FIXED_NOW - 60))"
    scene 23-sleep-off         '{}' "$msg 10037=0"
  fi
  scene 18-tap-hints       '{"showhint": 1, "showdate": 1, "dateshowhint": 1, "btlevel": 2}' 'emu-tap' 'emu-tap'  # tapped twice: a single emulated tap is occasionally lost
}

for P in "${PLATFORMS[@]}"; do
  mkdir -p "$DEST/$P"
  for f in "$DEST/$P"/*.png; do  # with --only, keep the other scenes' images
    [ -e "$f" ] || continue
    n=${f##*/}; n=${n%.png}
    [[ -z "$ONLY" || "$n" =~ $ONLY ]] && rm -f "$f"
  done
  kill_emulator
  note "starting emulator"
  if ! timeout 120 pebble install --emulator "$P" "$PBW" >"$STATE/$P-install.log" 2>&1; then
    cat "$STATE/$P-install.log" >&2
    bad "install failed"
    continue
  fi
  sleep 3
  run_scenes
  kill_emulator
done

P=""
if [ "$UPDATE" = 1 ]; then
  echo "[screenshots] baseline written to $DEST"
  exit "$fail"
fi
rm -rf "$SHOTS/diff"
# compare only the platforms that ran
CMP_BASE="$STATE/cmp-baseline"; CMP_CUR="$STATE/cmp-current"
rm -rf "$CMP_BASE" "$CMP_CUR"; mkdir -p "$CMP_BASE" "$CMP_CUR"
for P in "${PLATFORMS[@]}"; do
  [ -d "$DEST/$P" ] && cp -r "$DEST/$P" "$CMP_CUR/$P"
  if [ -z "$ONLY" ]; then
    [ -d "$SHOTS/baseline/$P" ] && cp -r "$SHOTS/baseline/$P" "$CMP_BASE/$P"
  else  # with --only, compare just the scenes that ran
    mkdir -p "$CMP_BASE/$P"
    for f in "$DEST/$P"/*.png; do
      [ -e "$SHOTS/baseline/$P/${f##*/}" ] && cp "$SHOTS/baseline/$P/${f##*/}" "$CMP_BASE/$P/"
    done
  fi
done
node tools/lib/png-diff.js compare "$CMP_BASE" "$CMP_CUR" "$SHOTS/diff" --threshold "$THRESHOLD" || fail=1
exit "$fail"
