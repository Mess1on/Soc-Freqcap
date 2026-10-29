#!/bin/bash
# 深度睡眠状态机台架：用桩 dumpsys / settings 在 PC 上跑真脚本，
# 验进入时的系统改动、亮屏/关开关的对称还原、媒体临时白名单和崩溃快照。
#
# 这些命令在 PC 上没有，所以只替换它们；脚本本身的判定、快照格式、
# 白名单取舍全部走线上那份代码，不另抄一份状态机。
#
# 用法： bash src/test_deep_sleep.sh
SRC=$(cd "$(dirname "$0")" && pwd) || exit 1
MOD=$(dirname "$SRC") || exit 1
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

STUB="$TMP/stub"; STATE="$TMP/stubstate"; DATA="$TMP/data"
mkdir -p "$STUB" "$STATE" "$DATA"

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }
has(){ grep -qx -- "$2" "$1" 2>/dev/null && echo yes || echo no; }

# ── 桩命令 ────────────────────────────────────────────────────
cat > "$STUB/settings" <<'EOS'
#!/bin/sh
f="$STUB_STATE/settings"
case "$1" in
  get)    cat "$f" 2>/dev/null ;;
  put)    shift 3; printf '%s' "$*" > "$f" ;;
  delete) : > "$f" ;;
esac
exit 0
EOS

cat > "$STUB/dumpsys" <<'EOS'
#!/bin/sh
d="$STUB_STATE"
echo "$*" >> "$d/calls"
case "$1" in
  power)       echo "mWakefulness=$(cat "$d/wake" 2>/dev/null)" ;;
  display)     echo "mScreenState=$(cat "$d/screen" 2>/dev/null)" ;;
  media_session) cat "$d/media" 2>/dev/null ;;
  alarm)       cat "$d/alarm" 2>/dev/null ;;
  deviceidle)
    case "$2" in
      whitelist)
        case "$3" in
          +*) p=${3#+}; grep -qx -- "$p" "$d/whitelist" 2>/dev/null || echo "$p" >> "$d/whitelist" ;;
          -*) p=${3#-}; grep -vx -- "$p" "$d/whitelist" > "$d/whitelist.n" 2>/dev/null; mv "$d/whitelist.n" "$d/whitelist" ;;
          '') cat "$d/whitelist" 2>/dev/null ;;
        esac ;;
      force-idle) echo 1 > "$d/forced" ;;
      unforce)    echo 0 > "$d/forced" ;;
      '')         echo "mForceIdle=$(cat "$d/forced" 2>/dev/null)" ;;
    esac ;;
esac
exit 0
EOS
# 脚本里每个 dumpsys 都套了 timeout；台架上直接执行，不真等。
cat > "$STUB/timeout" <<'EOS'
#!/bin/sh
shift
exec "$@"
EOS
# 默认短信应用：真机走 `cmd role ...`；台架回一个固定包名。
cat > "$STUB/cmd" <<'EOS'
#!/bin/sh
echo "$*" >> "$STUB_STATE/calls"
case "$*" in
  *android.app.role.SMS*) echo "com.test.sms" ;;
  *deviceidle*force-idle*) echo 1 > "$STUB_STATE/forced" ;;
esac
exit 0
EOS
chmod +x "$STUB/settings" "$STUB/dumpsys" "$STUB/timeout" "$STUB/cmd"

printf 'Asleep\n' > "$STATE/wake"
printf 'OFF\n'    > "$STATE/screen"
echo 0            > "$STATE/forced"
: > "$STATE/whitelist"; : > "$STATE/settings"; : > "$STATE/media"; : > "$STATE/alarm"
: > "$STATE/calls"

# 背光节点：0=熄屏、>0=亮屏。线上路径是 /sys/class/backlight/*/brightness，
# 台架用 DS_BL 指到临时目录，走的还是脚本里同一段枚举逻辑。
BLD="$TMP/bl"; mkdir -p "$BLD/panel0-backlight"
mkdir -p "$TMP/power_none"
bl(){ printf '%s\n' "$1" > "$BLD/panel0-backlight/brightness"; }
bl 0

export PATH="$STUB:$PATH" SOCFREQ_DATA_DIR="$DATA" STUB_STATE="$STATE" DS_BL="$BLD"
export DS_STATUS="$TMP/status"
# 台架不跑冻结（PC 没有 cgroup）；冻结本身在真机上用 src/test_freeze.sh 验。
export DS_FREEZE=0
# 台架默认不接任何电源节点：真机上 /sys/class/power_supply 正在充电，
# 不隔离的话第一轮就会被“充电中”挡掉，后面的用例全跟着错。
export DS_POWER_DIR="$TMP/power_none"
export DS_WAKE_INTERVAL=0 DS_MEDIA_INTERVAL=0
. "$MOD/scripts/deep_sleep.sh"
SCREEN_OFF_DELAY=0

# 配置：打开 + 一个永久豁免应用
ds_conf_write
printf 'enable=1\nwhite=com.white\n' > "$DSLEEP_CONF"
ds_conf_load

echo "== 进入 =="
ds_tick
ok "$(cat "$STATE/forced")" "0" "进入深睡（不再立即 force-idle，7:30 才收口）"
ok "$(cat "$STATE/settings")" "" "不再改 device_idle_constants"
ok "$(has "$STATE/whitelist" com.white)" "yes" "用户白名单应用在进入时加白"
ok "$(has "$STATE/whitelist" com.test.sms)" "yes" "默认短信应用进入时临时加白（不靠用户名单）"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "yes" "写快照（崩溃还原依据）"
ok "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" "1" "state 报深睡中"

echo "== 深睡中：媒体保护 + 新白名单 =="
printf 'PlaybackState {state=3, position=1000} package=com.music\n' > "$STATE/media"
ds_tick
ok "$(has "$STATE/whitelist" com.music)" "yes" "正在播放音乐的应用被临时加白"
appended="yes"
grep -qx 'added=com.music' "$DS_SNAP" 2>/dev/null || appended="no"
ok "$appended" "yes" "临时加白也写进快照（崩溃后能撤）"
printf 'enable=1\nwhite=com.white\nwhite=com.new\n' > "$DSLEEP_CONF"
ds_tick
ok "$(has "$STATE/whitelist" com.new)" "yes" "深睡中新加入的豁免立即生效"

echo "== 唤醒榜增量 =="
# 段头带前导空格、同一应用两行，都是 ColorOS 实际输出的形态
printf '  Top Alarms:\n    +33s 1234 wakeups, 1234 alarms: u0a12:com.app\n    +1s 6 wakeups, 6 alarms: u0a12:com.app\n  Alarm Stats:\n' > "$STATE/alarm"
ds_tick
printf '  Top Alarms:\n    +33s 1240 wakeups, 1240 alarms: u0a12:com.app\n    +1s 6 wakeups, 6 alarms: u0a12:com.app\n  Alarm Stats:\n' > "$STATE/alarm"
ds_tick
ok "$(awk -F= '$1=="dsw"{print $2}' "$STATE_FILE" | grep -o 'com.app:6:0')" "com.app:6:0" \
   "唤醒增量进了当次榜（多行按包名求和）"

echo "== 亮屏还原 =="
printf 'Awake\n' > "$STATE/wake"
bl 1
ds_tick
ok "$(cat "$STATE/forced")" "0" "亮屏后强制空闲为 0"
ok "$(cat "$STATE/settings")" "" "常量回到进入前的空值（delete 而不是留着）"
ok "$(has "$STATE/whitelist" com.white)" "yes" "用户白名单退出后保留（永久豁免）"
ok "$(has "$STATE/whitelist" com.new)" "yes" "深睡中新加的豁免退出后也保留"
ok "$(has "$STATE/whitelist" com.music)" "no" "媒体临时白名单退出时撤掉"
ok "$(has "$STATE/whitelist" com.test.sms)" "no" "短信临时白名单退出时也撤掉"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "no" "正常退出删掉快照"
ok "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" "0" "state 报已退出"
ok "$(grep -c '^A com.app=' "$DS_STATS" 2>/dev/null)" "1" "当次榜快照落 ds_stats.conf"

echo "== 崩溃后按快照还原 =="
printf 'Asleep\n' > "$STATE/wake"; bl 0
: > "$STATE/media"
ds_tick
ok "$(cat "$STATE/forced")" "0" "再次进入（不立即压 Doze）"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "yes" "快照在"
# 模拟进程被杀：新起一个 shell，按正常启动顺序 reload 配置再按存档还原
( . "$MOD/scripts/deep_sleep.sh"; ds_conf_load; ds_snap_restore_if_any ) >/dev/null 2>&1
ok "$(cat "$STATE/forced")" "0" "崩溃快照还原（本来未强制空闲）"
ok "$(cat "$STATE/settings")" "" "常量按原值还原"
ok "$(has "$STATE/whitelist" com.music)" "no" "临时白名单被撤"
ok "$(has "$STATE/whitelist" com.white)" "yes" "用户白名单不动"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "no" "还原后删快照"
# 新进程的内存状态是空的（被 kill -9 的那份已经没了），这里照实重置
ds_active=0; ds_added=""; ds_screen_off_ts=0

echo "== 关总开关 =="
printf 'enable=1\nwhite=com.white\n' > "$DSLEEP_CONF"
printf 'Asleep\n' > "$STATE/wake"; bl 0
ds_tick
ok "$(cat "$STATE/forced")" "0" "重新进入（不立即压 Doze）"
printf 'enable=0\nwhite=com.white\n' > "$DSLEEP_CONF"
ds_tick
ok "$(cat "$STATE/forced")" "0" "关开关后立即退出"
ok "$(cat "$STATE/settings")" "" "关开关也还原常量"

echo "== 收到 TERM（api 停止守护）=="
printf 'enable=1\nwhite=com.white\n' > "$DSLEEP_CONF"
printf 'Asleep\n' > "$STATE/wake"; bl 0
ds_tick
ok "$(cat "$STATE/forced")" "0" "再次进入（不立即压 Doze）"
( ds_shutdown ) >/dev/null 2>&1
ok "$(cat "$STATE/forced")" "0" "TERM 走 ds_shutdown：先 unforce"
ok "$(cat "$STATE/settings")" "" "TERM 也还原常量"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "no" "TERM 后快照已删"
ok "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" "0" "TERM 后 state 的 dsactive 写回 0"
printf 'enable=0\n' > "$DSLEEP_CONF"

echo "== 充电判定（不能把插着电误判成没充电）=="
PWR="$TMP/power"; mkdir -p "$PWR/usb" "$PWR/battery"
printf 'Mains\n' > "$PWR/usb/type"; echo 0 > "$PWR/usb/online"
printf 'Battery\n' > "$PWR/battery/type"
printf 'Discharging\n' > "$PWR/battery/status"
ok "$(DS_POWER_DIR="$PWR" ds_charging && echo y || echo n)" "n" "online=0 + Discharging 不算充电"
echo 1 > "$PWR/usb/online"
ok "$(DS_POWER_DIR="$PWR" ds_charging && echo y || echo n)" "y" "usb online=1 算充电"
echo 0 > "$PWR/usb/online"
printf 'Charging\n' > "$PWR/battery/status"
ok "$(DS_POWER_DIR="$PWR" ds_charging && echo y || echo n)" "y" \
   "没有 online 时按 battery/status=Charging 兜底（否则深睡会在充电时压 Doze）"
printf 'Full\r\n' > "$PWR/battery/status"
ok "$(DS_POWER_DIR="$PWR" ds_charging && echo y || echo n)" "y" "status 带 CRLF 也认 Full"
rm -rf "$PWR"
ok "$(DS_POWER_DIR="$TMP/none" ds_charging && echo y || echo n)" "n" "没有任何电源节点时不算充电"

echo "== 闹钟守卫（闹钟前半小时不进深睡）=="
_t=$(date +%s)
# 毫秒时间戳用字符串拼：mksh 的 $(( )) 是 32 位，(_t+600)*1000 会在真机上溢出。
_soon=$((_t + 600)); _far=$((_t + 3600))
printf 'RTC_WAKEUP #1: Alarm{abc type 0 origWhen %s whenElapsed 1} uid 1 component ComponentInfo{com.coloros.alarmclock/.Alarm}\n' \
  "${_soon}000" > "$STATE/alarm"
ok "$(ds_alarm_soon && echo y || echo n)" "y" "30 分钟内的时钟闹钟会拦住深睡"
printf 'RTC_WAKEUP #1: Alarm{abc origWhen %s} component ComponentInfo{com.tencent.mm/.X}\n' \
  "${_soon}000" > "$STATE/alarm"
ok "$(ds_alarm_soon && echo y || echo n)" "n" "非时钟应用的闹钟不算"
printf 'RTC_WAKEUP #1: Alarm{abc origWhen %s} component ComponentInfo{com.coloros.alarmclock/.Alarm}\n' \
  "${_far}000" > "$STATE/alarm"
ok "$(ds_alarm_soon && echo y || echo n)" "n" "一小时后的闹钟不用拦"
# 集成：闹钟快响时 ds_tick 不该 force-idle
printf 'enable=1\n' > "$DSLEEP_CONF"
printf 'Asleep\n' > "$STATE/wake"; bl 0; echo 0 > "$STATE/forced"
: > "$STATE/settings"; : > "$STATE/whitelist"
printf 'RTC_WAKEUP #1: Alarm{abc origWhen %s} component ComponentInfo{com.coloros.alarmclock/.Alarm}\n' \
  "${_soon}000" > "$STATE/alarm"
ds_screen_off_ts=0; ds_alarm_checked=0; ds_active=0
ds_tick
ok "$(cat "$STATE/forced")" "0" "闹钟前 30 分钟不强制进 Doze"
ok "$([ "$(grep -c '闹钟' "$LOG" 2>/dev/null)" -ge 1 ] && echo y || echo n)" "y" \
   "日志里说明是因为闹钟没进"
: > "$STATE/alarm"; printf 'enable=0\n' > "$DSLEEP_CONF"

echo "== 巡检开销（熄屏后不能每轮都进 system_server）=="
printf 'enable=1\nwhite=com.white\n' > "$DSLEEP_CONF"
printf 'Asleep\n' > "$STATE/wake"; bl 0
: > "$STATE/media"; : > "$STATE/alarm"
ds_active=0; ds_added=""; ds_screen_off_ts=0; ds_alarm_checked=$(date +%s)
ds_scr_ok=1; ds_scr_bad=0; ds_scr_checked=0; ds_scr_hold=0
# 亮屏且不在深睡：跟一整天比起来这是最常见的状态，一轮里不该有任何系统调用
bl 1
: > "$STATE/calls"
ds_tick
ok "$(grep -c . "$STATE/calls" 2>/dev/null)" "0" \
   "亮屏巡检 0 次 dumpsys（以前每 2 秒 power + display 各一次）"

bl 0
ds_tick
ok "$(cat "$STATE/forced")" "0" "熄屏后进入深睡（不立即压 Doze）"
WAKE_SAMPLE_INTERVAL=60; MEDIA_CHECK_INTERVAL=30
ds_last_wake=$(date +%s); ds_last_media=$(date +%s)
: > "$STATE/calls"
_i=0; while [ "$_i" -lt 5 ]; do ds_tick; _i=$((_i + 1)); done
ok "$(grep -c '^alarm' "$STATE/calls" 2>/dev/null)" "0" "深睡中 5 轮巡检不读 dumpsys alarm"
ok "$(grep -c '^media_session' "$STATE/calls" 2>/dev/null)" "0" "同理不读 media_session"
ok "$(grep -c '^power' "$STATE/calls" 2>/dev/null)" "0" "节点说灭、60 秒内不重复问 power"
ok "$(grep -c '^display' "$STATE/calls" 2>/dev/null)" "0" "脚本不再调 dumpsys display"
ds_last_wake=$(( $(date +%s) - 61 )); ds_last_media=$(( $(date +%s) - 31 ))
: > "$STATE/calls"
ds_tick
ok "$(grep -c '^alarm' "$STATE/calls")" "1" "满 60 秒才读一次唤醒数"
ok "$(grep -c '^media_session' "$STATE/calls")" "1" "满 30 秒才查一次媒体"

echo "== 亮屏节点假归零（个别 ROM 亮度恒为 0）=="
printf 'Awake\n' > "$STATE/wake"; bl 0
ds_active=0; ds_screen_off_ts=0; ds_scr_ok=1; ds_scr_bad=0; ds_scr_hold=0
echo 0 > "$STATE/forced"
ds_scr_checked=$(( $(date +%s) - 61 )); ds_tick
ok "$(cat "$STATE/forced")" "0" "节点说灭、系统说 Awake：不压 Doze"
ok "$ds_scr_bad" "1" "记一次不一致"
ds_scr_hold=0; ds_scr_checked=$(( $(date +%s) - 61 )); ds_tick
ds_scr_hold=0; ds_scr_checked=$(( $(date +%s) - 61 )); ds_tick
ok "$ds_scr_ok" "0" "连续 3 次对不上就停用节点"
ok "$([ "$(grep -c '停用节点' "$LOG" 2>/dev/null)" -ge 1 ] && echo y || echo n)" "y" \
   "日志里写明停用原因"
ds_scr_checked=$(( $(date +%s) - 61 )); ds_tick
ok "$(cat "$STATE/forced")" "0" "停用后改问 dumpsys，Awake 仍不进深睡"
ds_scr_ok=1; ds_scr_bad=0; ds_scr_hold=0; ds_scr_checked=0
printf 'enable=0\n' > "$DSLEEP_CONF"

echo "== 游戏在前台（熄屏挂机）不压 Doze =="
printf 'enable=1\nwhite=com.white\n' > "$DSLEEP_CONF"
printf 'Asleep\n' > "$STATE/wake"; bl 0
: > "$STATE/media"; : > "$STATE/alarm"
printf 'state=game\nscreen=on\ngame_pkg=com.tencent.tmgp.sgame\nfg=com.tencent.tmgp.sgame\n' > "$DS_STATUS"
ds_active=0; ds_added=""; ds_screen_off_ts=0; ds_alarm_checked=$(date +%s); ds_game_checked=0
echo 0 > "$STATE/forced"
ds_tick
ok "$(cat "$STATE/forced")" "0" "游戏在前台：不 force-idle"
ok "$(grep -c '游戏在前台' "$LOG" 2>/dev/null)" "1" "日志里写明是因为游戏没进"
printf 'state=normal\ngame_pkg=com.tencent.tmgp.sgame\nfg=com.android.launcher\n' > "$DS_STATUS"
ds_screen_off_ts=0; ds_game_checked=0
ds_tick
ok "$(cat "$STATE/forced")" "0" "游戏离开前台后进入深睡（不立即压 Doze）"

echo "== 7:30 一次性收口（Doze 不再随进入即发）=="
: > "$STATE/alarm"; : > "$STATE/calls"
ok "$([ "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" = "1" ] && echo y || echo n)" "y" \
   "此时仍在深睡"
ok "$(grep -c '^deviceidle force-idle' "$STATE/calls" 2>/dev/null)" "0" \
   "进入阶段没有发出 force-idle"
ds_screen_off_ts=$(( $(date +%s) - 430 ))
ds_doze_sent=0; ds_doze_checked=0
: > "$STATE/calls"
ds_tick
ok "$(cat "$STATE/forced")" "0" "还没到 7:30 不收口"
ds_screen_off_ts=$(( $(date +%s) - 451 ))
ds_tick
ok "$(cat "$STATE/forced")" "1" "息屏满 7:30 收口一次"
ok "$(grep -c '^deviceidle force-idle deep$' "$STATE/calls")" "1" "force-idle 只发一次"
ds_tick
ok "$(grep -c '^deviceidle force-idle deep$' "$STATE/calls")" "1" "之后不重复发（不重试）"

echo "== 7:30 收口让路闹钟 =="
echo 0 > "$STATE/forced"
ds_screen_off_ts=$(( $(date +%s) - 451 )); ds_doze_sent=0; ds_doze_checked=0
printf 'RTC_WAKEUP #1: Alarm{abc origWhen %s} component ComponentInfo{com.coloros.alarmclock/.Alarm}\n' \
  "$(( ($(date +%s) + 600) ))000" > "$STATE/alarm"
: > "$STATE/calls"
ds_tick
ok "$(cat "$STATE/forced")" "0" "闹钟在 30 分钟内：7:30 不收口"
ok "$([ "$(grep -c '暂不进入系统 Doze' "$LOG" 2>/dev/null)" -ge 1 ] && echo y || echo n)" "y" \
   "日志写明是因为闹钟"
ok "$(grep -c '^deviceidle force-idle' "$STATE/calls")" "0" "闹钟窗口内一次 force-idle 都不发"
: > "$STATE/alarm"
ds_doze_checked=$(( $(date +%s) - 301 ))
ds_tick
ok "$(cat "$STATE/forced")" "1" "闹钟消失后的下一轮补收口"
printf 'enable=0\n' > "$DSLEEP_CONF"; ds_tick

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
