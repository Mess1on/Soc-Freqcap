#!/system/bin/sh
# 深度睡眠守护：熄屏且未充电满 20 秒后，按 Doze 收口：
# 熄屏 7:30 一次性 force-idle，并临时放行 push/短信/媒体；亮屏或插电立即撤白名单并退出 Doze。
# 不再冻结任何进程（墓碑功能已移除）；旧版本留下的冻结只在启动/退出时按账本解开。
# 所有系统改动先写存档/账本，退出、启动、卸载都逐项逆操作。
#
# 本文件被 service.sh 以后台进程拉起，也被 api.sh source 一次用于按存档还原。
# 只认 $0 的基名：直接执行时进主循环，被 source 时只加载函数。

CONF_DIR=${SOCFREQ_DATA_DIR:-/data/adb/soc_freqcap}
DSLEEP_CONF=$CONF_DIR/dsleep.conf
DS_SNAP=$CONF_DIR/ds_snap.conf
DS_STATS=$CONF_DIR/ds_stats.conf
DS_WAKES=$CONF_DIR/ds_wakes.conf
STATE_FILE=$CONF_DIR/ds_state
# 主守护发布的状态（前台是谁、命中的游戏是谁）。只读不改。
DS_STATUS=${DS_STATUS:-$CONF_DIR/run/status}
LOG=$CONF_DIR/dsleep.log
PIDFILE=$CONF_DIR/run/dsleep.pid

SCREEN_OFF_DELAY=20
POLL_INTERVAL=2
# 深睡激活后不再 2 秒轮询：阻塞等 screen_toggled 事件（亮屏立即醒），
# 最多 DS_ACTIVE_TICK 秒醒一次做维护（充电/7:30/名单）；事件源不可用时退 30 秒轮询。
# 亮屏是事件驱动（logcat 一有 screen_toggled 立刻回），所以这个值只影响
# "充电/媒体/名单变更"的最迟发现时间，不影响亮屏响应。
# 300 秒是为了待机：整夜每 2 分钟醒一次纯属白耗，5 分钟足够。
DS_ACTIVE_TICK=300
DS_ACTIVE_FALLBACK=30
# 冻结时序：熄屏后先冻结后台，7:30 才一次性 force-idle。
# 3 分钟内分批冻结，之后不再新增，减少冻结组里 D 状态进程把整组卡住的机会。
# 冻结（墓碑）已从深睡里移除：本模块不再冻结任何进程，只保留 Doze。
# 这里保留一个恒为 0 的开关，是为了让下面"解冻清理"的代码还能跑 ——
# 升级上来的机器可能有旧版本留下的冻结账本/冻结组，必须照样解开。
DS_FREEZE_ENABLED=0
# 冻结移除之后，深睡只剩 Doze 这一件事，所以不再单设 Doze 开关：
# 打开「自动深度睡眠」就是做 Doze，关掉就是什么都不做。恒为 1。
DS_DOZE_ENABLED=1
DS_FREEZE_WINDOW=180
DS_FREEZE_RESCAN=60
DS_DOZE_AT=450
DS_DOZE_RETRY=300
DS_FREEZE_BATCH=8
DS_FREEZE_GROUP_NAME=${DS_FREEZE_GROUP_NAME:-socfreq_ds}
DS_FREEZE_LEDGER=$CONF_DIR/run/freeze.saved
DS_FREEZE_QUEUE=$CONF_DIR/run/freeze.queue
DS_SCAN_PS=$CONF_DIR/run/freeze.ps
DS_SCAN_META=$CONF_DIR/run/freeze.meta
DS_SCAN_CAND=$CONF_DIR/run/freeze.cand
DS_EVENT_TMP=$CONF_DIR/run/ds_event.tmp
DS_EVENT_FIFO=$CONF_DIR/run/ds_event.fifo
# 深睡只做 Doze，不碰 CPU 频率/调度。
# nice= 不碰；push= 只放行指定进程（pkg[:proc]）并注入 Doze 白名单。
DS_OPS_LEDGER=$CONF_DIR/run/ops.saved
DS_LOCK_DIR=$CONF_DIR/run/dsleep.lock.d
# 只有维护轮才读 dumpsys（一次就是一次 system_server 往返）。计数器是累计量，
# 隔多久读都不丢增量：唤醒榜 15 分钟、媒体 5 分钟足够，退出时还会结算一次。
WAKE_SAMPLE_INTERVAL=${DS_WAKE_INTERVAL:-900}
MEDIA_CHECK_INTERVAL=${DS_MEDIA_INTERVAL:-300}
# 闹钟守卫：时钟类应用有这么多秒内要响的闹钟时，不进入深睡（已经在里面就退出）。
# 只让路闹钟前半小时，不是"有闹钟就永不深睡"。
ALARM_GUARD_SEC=1800
MAX_WHITE=167
MAX_WAKE_APPS=63

ds_enabled=0
ds_active=0
ds_screen_off_ts=0
ds_added=""
# 本轮深睡里"已经处理过的用户白名单包"。不能拿 ds_added 代替：
# Doze 关掉时 ds_added 永远是空的，用它做守卫会让这段每轮都重跑一遍、每轮刷一行日志。
ds_white_seen=""
ds_alarm_checked=0
ds_alarm_wait=0
ds_game_checked=0
ds_was_charging=0
ds_state_last=""
ds_quit=0
# 事件等待：logcat 可用时阻塞等亮屏，不可用就退化成定时唤醒。
ds_event_ok=1
ds_event_on=0
ds_wait_pid=""
ds_tick_stamp=""
DS_NICE=""
DS_PUSH=""
# 冻结状态：冻结功能已移除，只剩"按账本把旧版本留下的冻结解开"这套还原用的状态。
ds_freeze_backend=""
ds_freeze_root=""
ds_freeze_group=""
ds_freeze_frozen=0
ds_freeze_done=0
ds_frozen=""
ds_doze_sent=0
ds_doze_checked=0
# 亮屏判定：节点优先，连续对不上就停用它，改问系统。
ds_scr_ok=1
ds_scr_bad=0
ds_scr_checked=0
ds_scr_hold=0
ds_last_media=0
ds_last_wake=0

# 双榜单：ds_cur_list 自上次拔电/开机累计；ds_run_list 每次进入深睡清零。
# 每行 "pkg count"，最多 MAX_WAKE_APPS 个应用。
ds_cur_list=""
ds_run_list=""
ds_wcsince=0
ds_cur_base=""

now() { date +%s; }
# dsleep.log 以前是全模块唯一没人裁的日志：service.log 有 tail -c 131072，
# socfreqd.log 有 log_trim，这份是裸追加。正常一天一百来行（约 10KB）不要紧，
# 但任何一条路径开始刷屏就再也收不住 —— 这台机上出现过 5692 行重复日志、
# 一天涨 430KB，而且永不回收。口径和 service.sh 保持一致：超 256KB 只留最近
# 128KB，留下的必须是**新**的那一半（tail，不是 head）。
# 每 64 条才量一次大小，别在每条日志上都多跑一次 wc。
#
# shortcut: 不加锁。只有拿到运行锁的那个实例能把计数累加到 64 的倍数——
# 抢锁失败的进程只写一行就退出（api.sh source 本文件时也只写零星几条），
# 所以实际不存在两个进程同时裁剪、争同一个 $LOG.tmp。
ds_log_trim() {
    local n
    n=$(wc -c <"$LOG" 2>/dev/null)
    case "$n" in ''|*[!0-9]*) return 0 ;; esac
    [ "$n" -lt 262144 ] && return 0
    tail -c 131072 "$LOG" >"$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG" 2>/dev/null
    return 0
}
_ds_logn=0
log_ds() {
    echo "$(date '+%m-%d %H:%M:%S' 2>/dev/null) $*" >>"$LOG" 2>/dev/null
    _ds_logn=$((_ds_logn + 1))
    [ $((_ds_logn % 64)) -eq 0 ] || return 0
    ds_log_trim
    return 0
}

atomic_write() {
    local f="$1" perm="$2"
    cat >"$f.tmp" 2>/dev/null || return 1
    mv -f "$f.tmp" "$f" 2>/dev/null || return 1
    chmod "$perm" "$f" 2>/dev/null
    return 0
}

# ============================ 基础检测 ========================================

# 返回 0=亮屏，1=熄屏。
#
# 主判据是背光/blank 节点：一次 cat，不进 system_server。以前每 2 秒跑一遍
# dumpsys power + dumpsys display，熄屏后这一路等于让 CPU 永远停不下来，
# 而且 dumpsys 一旦卡住（display 在 system_server 忙时会被挂住），整轮巡检
# 就再也不返回 —— 亮屏了也没人解 force-idle。
#   - 节点说亮：直接信，不跑 dumpsys；
#   - 节点说灭：最多每 60 秒让 dumpsys 复核一次，防个别 ROM 亮度节点假归零；
#   - 节点连续 3 次和系统对不上：停用它，之后都问系统。
# 节点读不到时才每轮都问系统（带 timeout，读不到就按熄屏处理，和旧版一致）。
DS_BL=${DS_BL:-/sys/class/backlight}
DS_FB_BLANK=${DS_FB_BLANK:-/sys/class/graphics/fb0/blank}

ds_screen_node() {
    local b v
    if [ -r "$DS_FB_BLANK" ]; then
        v="$(cat "$DS_FB_BLANK" 2>/dev/null)"
        case "$v" in
            ''|*[!0-9]*) ;;
            *) if [ "$v" -eq 0 ]; then echo on; else echo off; fi; return 0 ;;
        esac
    fi
    for b in "$DS_BL"/*/brightness; do
        [ -r "$b" ] || continue
        v="$(cat "$b" 2>/dev/null)"
        case "$v" in ''|*[!0-9]*) continue ;; esac
        if [ "$v" -gt 0 ]; then echo on; else echo off; fi
        return 0
    done
    echo ""
}

# 系统口径：0=亮，1=灭，2=读不到。timeout 必须有，dumpsys 卡住不能带走整轮。
ds_dumpsys_awake() {
    local w
    w="$(timeout 2 dumpsys power 2>/dev/null \
         | grep -m1 -o 'mWakefulness=[A-Za-z]*' | cut -d= -f2)"
    case "$w" in
        Awake)         return 0 ;;
        Asleep|Dozing) return 1 ;;
    esac
    return 2
}

ds_screen_on() {
    local n t
    if [ "$ds_scr_ok" = "1" ]; then
        n="$(ds_screen_node)"
        case "$n" in
            on)  ds_scr_bad=0; return 0 ;;
            off)
                t=$(now)
                # 刚对不上过：先按亮屏放过一分钟，别在两次复核之间反复进出深睡
                [ "$t" -lt "$ds_scr_hold" ] && return 0
                [ $((t - ds_scr_checked)) -lt 60 ] && return 1
                ds_scr_checked=$t
                ds_dumpsys_awake
                if [ $? -eq 0 ]; then
                    ds_scr_bad=$((ds_scr_bad + 1))
                    if [ "$ds_scr_bad" -ge 3 ]; then
                        ds_scr_ok=0
                        log_ds "亮屏节点连续 3 次和系统对不上，停用节点改问 dumpsys"
                    fi
                    ds_scr_hold=$((t + 60))
                    return 0
                fi
                ds_scr_bad=0
                return 1
                ;;
        esac
    fi
    ds_dumpsys_awake
    case $? in 0) return 0 ;; *) return 1 ;; esac
}

# 扫描所有电源节点：type 非 Battery 且 online=1 即充电中。
# 有些 ROM 的充电节点没有 online（或只有 battery/status），只看 online 会把
# "插着电"误判成"没充电" —— 那样深睡会在充电时照样压进 Doze。两个信号都看。
# DS_POWER_DIR 只给台架替换路径用；线上就是 /sys/class/power_supply。
DS_POWER_DIR=${DS_POWER_DIR:-/sys/class/power_supply}
ds_charging() {
    local d t o s
    for d in "$DS_POWER_DIR"/*; do
        [ -r "$d/online" ] || continue
        t="$(cat "$d/type" 2>/dev/null)"
        [ "$t" = "Battery" ] && continue
        o="$(cat "$d/online" 2>/dev/null)"
        [ "$o" = "1" ] && return 0
    done
    s=""
    [ -r "$DS_POWER_DIR/battery/status" ] \
        && s="$(tr -d '\r\n' < "$DS_POWER_DIR/battery/status" 2>/dev/null)"
    case "$s" in
        Charging|Full|charging|full) return 0 ;;
    esac
    return 1
}

# 默认短信应用：Doze 只会把它的 SMS 广播压到出 Doze 才发，用户看到的就是
# "解锁后一下收到半小时前的短信"。用户名单里没有它时，进入时临时加白，
# 退出时撤掉（和媒体保护同一套）。优先问系统角色，再退回旧 setting 和常见包名。
ds_sms_pkg() {
    local p
    p="$(cmd role get-role-holders android.app.role.SMS 2>/dev/null | tr -d '\r' | head -n 1)"
    case "$p" in ''|null) ;; *) echo "$p"; return 0 ;; esac
    p="$(settings get secure sms_default_application 2>/dev/null | tr -d '\r')"
    case "$p" in ''|null) ;; *) echo "$p"; return 0 ;; esac
    for p in com.hihonor.mms com.huawei.mms com.android.mms \
             com.google.android.apps.messaging com.samsung.android.messaging \
             com.oplus.mms com.coloros.mms com.miui.mms com.vivo.mms; do
        pm list packages "$p" 2>/dev/null | grep -qx "package:$p" && {
            echo "$p"; return 0
        }
    done
    return 1
}

# 闹钟守卫：时钟类应用有 ALARM_GUARD_SEC 内要响的闹钟时，不进入深睡。
# 只认 dumpsys alarm 里带 alarmclock/deskclock 的 Alarm{} 行，取 origWhen。
# 这样"每天都设闹钟"也不会导致整晚不深睡：只让路闹钟前半小时。
ds_alarm_soon() {
    local nows
    nows=$(now)
    timeout 3 dumpsys alarm 2>/dev/null | awk -v now="$nows" -v win="$ALARM_GUARD_SEC" '
        /Alarm\{/ {
            if ($0 !~ /alarmclock|deskclock|AlarmClock/) next
            if (!match($0, /origWhen[ =][0-9]+/)) next
            s = substr($0, RSTART, RLENGTH)
            sub(/^origWhen[ =]/, "", s)
            t = s + 0
            # 有的 ROM 打毫秒、有的打秒
            if (t > 100000000000) t = t / 1000
            if (t > now && t - now <= win) { f = 1; exit }
        }
        END { exit !f }'
}

# 主守护报的"游戏在前台"：game_pkg 和 fg 相同才算。
# 熄屏后主守护不再查前台，fg 会停在熄屏那一刻的包；game_pkg 在离开游戏的
# 宽限期结束后清空。所以两者相等 = 熄屏前/熄屏时确实有一个游戏在前台
# （典型是熄屏挂机），这时压 Doze 会把游戏挂死，也会让系统的 idle 统计
# 把这段算成待机。
ds_game_active() {
    local gp fgv
    [ -f "$DS_STATUS" ] || return 1
    gp="$(awk -F= '$1=="game_pkg" { print $2; exit }' "$DS_STATUS" 2>/dev/null)"
    fgv="$(awk -F= '$1=="fg" { print $2; exit }' "$DS_STATUS" 2>/dev/null)"
    [ -n "$gp" ] && [ "$gp" != "-" ] && [ "$gp" = "$fgv" ]
}

# ============================ 配置 ============================================

ds_conf_write() {
    mkdir -p "$CONF_DIR/run" 2>/dev/null
    cat - >"$DSLEEP_CONF.tmp" <<'EOF'
# 深度睡眠配置
# enable=1 打开；0 关闭。关闭只停止干预，不会保留任何系统改动。
# 打开后：熄屏满 20 秒开始准备，满 7 分 30 秒让系统进一次 Doze 深度空闲，
# 并把 push= / 默认短信 / 正在播放的媒体临时加进 Doze 白名单。
enable=0
# 每行一条 white=包名：完全豁免，深睡时也不受 Doze 限制（等同系统的「无限制」）。
# white=com.tencent.mm
# nice=包名：深睡不碰它（只是保留项，不影响 Doze）。
# nice=com.tencent.mobileqq
# push=包名[:进程]：把这个包加入 Doze 白名单，保证熄屏后消息还能进来。
# push=com.tencent.mm:push
EOF
    mv -f "$DSLEEP_CONF.tmp" "$DSLEEP_CONF" 2>/dev/null
    chmod 0644 "$DSLEEP_CONF" 2>/dev/null
}

DS_WHITE=""
ds_conf_raw=""
# 读一个 0/1 的标量键；缺省或值不合法就退回默认值。
# 有了它，冻结和唤醒锁这两项可以在不动代码、不改环境变量的前提下单独关掉，
# 用来做"开/关"对照 —— 待机耗电这种问题，只有对照才说得清是谁的账。
ds_conf_opt() {
    local k="$1" d="$2" v
    [ -f "$DSLEEP_CONF" ] || { echo "$d"; return 0; }
    v=$(awk -v k="$k" '
        $0 ~ "^[[:space:]]*" k "[[:space:]]*=" {
            v = $0
            sub(/^[^=]*=/, "", v)
            sub(/#.*/, "", v)
            gsub(/[[:space:]]/, "", v)
            print v
            exit
        }' "$DSLEEP_CONF" 2>/dev/null)
    case "$v" in 0|1) echo "$v" ;; *) echo "$d" ;; esac
}
# 按内容热重载。不用 mtime：/data 下面 stat 的秒级时间戳会让
# "同一秒内连改两下"的第二下永远读不到（开关刚开又加白名单就会漏）。
ds_conf_load() {
    local raw
    [ -f "$DSLEEP_CONF" ] || ds_conf_write
    raw="$(cat "$DSLEEP_CONF" 2>/dev/null)"
    [ "$raw" = "$ds_conf_raw" ] && return 0
    ds_conf_raw="$raw"
    ds_enabled=$(awk '
        /^[[:space:]]*enable[[:space:]]*=/ {
            v = $0
            sub(/^[^=]*=/, "", v)
            sub(/#.*/, "", v)
            gsub(/[[:space:]]/, "", v)
            print v
            exit
        }' "$DSLEEP_CONF" 2>/dev/null)
    [ "$ds_enabled" = "1" ] || ds_enabled=0
    DS_WHITE=$(ds_conf_list white)
    DS_NICE=$(ds_conf_list nice)
    DS_PUSH=$(ds_conf_list push)
    return 0
}

# 取某一类列表：一行一条 key=值，值里去掉注释/空白，输出逗号串。
ds_conf_list() {
    awk -v key="$1" '
        $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
            v = $0
            sub(/^[^=]*=/, "", v)
            sub(/#.*/, "", v)
            gsub(/[[:space:]\r]/, "", v)
            if (v != "") print v
        }' "$DSLEEP_CONF" 2>/dev/null | tr '\n' ',' | sed 's/,$//'
    return 0
}

ds_list_has() {
    case ",$1," in *",$2,"*) return 0 ;; esac
    return 1
}

# 列表里出现包名即算（忽略 :proc 后缀）。
ds_pkg_in_push() {
    local e
    for e in $(echo "$DS_PUSH" | tr ',' ' '); do
        [ "${e%%:*}" = "$1" ] && return 0
    done
    return 1
}

# 进程名（cmdline 第一段，通常是 pkg 或 pkg:proc）命中 push 列表。
ds_push_match_proc() {
    local e
    for e in $(echo "$DS_PUSH" | tr ',' ' '); do
        case "$e" in
            *:*) [ "$e" = "$1" ] && return 0 ;;
            *) [ "${1%%:*}" = "$e" ] && return 0 ;;
        esac
    done
    return 1
}

# push 列表里的包（去重、去掉 :proc），用于注入 Doze 白名单。
ds_push_pkg_list() {
    echo "$DS_PUSH" | tr ',' '\n' | sed 's/:.*//' | grep -v '^$' | sort -u | tr '\n' ',' | sed 's/,$//'
}

# ============================ Doze 原语 =======================================

# 只收像包名的行，滤掉 dumpsys 的段头。
# 两种格式都要认：AOSP 老格式是裸包名，ColorOS/新 AOSP 是 "类型,包名,uid"，
# 例：user,com.android.contacts,10107。只认 $1 的话后者一个都匹配不上，
# 整个快照恒为空 —— 模块就分不清"这个包本来就在白名单里"，
# 每次进入都重新加一遍，退出时再按自己的账本删，
# 连用户自己手动加的白名单也会被当成"我们加的"撤掉。
ds_white_snapshot() {
    timeout 3 dumpsys deviceidle whitelist 2>/dev/null | tr -d '\r' | \
      awk '
        BEGIN { FS = "[,[:space:]]+" }
        NF == 0 { next }
        $1 ~ /^[A-Za-z0-9][A-Za-z0-9_.]*$/ && index($1,".") > 1 { print $1; next }
        $2 ~ /^[A-Za-z0-9][A-Za-z0-9_.]*$/ && index($2,".") > 1 { print $2 }
      ' | tr '\n' ','
}

ds_white_add() {
    local pkg="$1" old="$2"
    case ",$old," in *",$pkg,"*) return 0;; esac
    timeout 2 dumpsys deviceidle whitelist "+$pkg" >/dev/null 2>&1
    ds_added="$ds_added $pkg"
}

# 用户白名单里的包永远不移除（那是永久豁免）。
ds_white_del() {
    local pkg="$1"
    case ",$DS_WHITE," in *",$pkg,"*) return 0 ;; esac
    timeout 2 dumpsys deviceidle whitelist "-$pkg" >/dev/null 2>&1
}

# ============================ 快照 ============================================

ds_snap_write() {
    {
        for p in $ds_added; do echo "added=$p"; done
        # 记下这一轮有没有真的 force-idle 过。退出/崩溃还原时只撤销我们自己
        # 发起的 Doze：无条件 unforce 会把系统自己的（或者别的工具发起的）
        # 深度空闲一起取消，等于替别人反复重置 Doze 状态机。
        echo "dozed=${ds_doze_sent:-0}"
    } | atomic_write "$DS_SNAP" 0644
}

# 快照还在 = 上次没走正常退出。启动时立即还原并删除。
ds_snap_restore_if_any() {
    local c="" a added="" dozed=""
    [ -f "$DS_SNAP" ] || return 0
    while IFS='=' read -r k v; do
        case "$k" in
            constants) c="$v" ;;
            added) [ -n "$v" ] && added="$added $v" ;;
            dozed) dozed="$v" ;;
        esac
    done <"$DS_SNAP" 2>/dev/null
    [ "$dozed" = "1" ] && timeout 2 dumpsys deviceidle unforce >/dev/null 2>&1
    if [ "$c" = "@none" ]; then
        settings delete global device_idle_constants >/dev/null 2>&1
    elif [ -n "$c" ]; then
        settings put global device_idle_constants "$c" >/dev/null 2>&1
    fi
    for a in $added; do ds_white_del "$a"; done
    rm -f "$DS_SNAP"
    log_ds "检测到上次未正常退出，已按存档还原"
}

# ============================ 唤醒统计 ========================================

# 解析 dumpsys alarm 的 Top Alarms 段，输出 pkg=count。
# 段头在不同 ROM 上可能带前导空格（实测 OPPO/ColorOS 是 "  Top Alarms:"），
# 所以两个锚点都按行首空白容忍。同一应用会有多行（不同 alarm 类型），
# 必须按包名求和：逐行输出会让差分的基线和当前值一个取最后一行、一个取总和。
ds_wakes_collect() {
    timeout 3 dumpsys alarm 2>/dev/null | awk -v max="$MAX_WAKE_APPS" '
        /^[[:space:]]*Top Alarms:/ { in_top=1; next }
        in_top && /^[[:space:]]*Alarm Stats/ { exit }
        in_top && index($0," wakeups")>0 {
            line=$0
            pos=index(line," wakeups"); n=""
            for (i=pos-1; i>0; i--) {
                ch=substr(line,i,1)
                if (ch>="0" && ch<="9") n=ch n
                else if (ch==",") continue
                else break
            }
            if (n=="") next
            anchor=index(line,"alarms:")
            if (anchor>0) rest=substr(line,anchor+7)
            else { w=index(line," wakeups"); rest=substr(line,w+9) }
            sub(/^[[:space:]]*/,"",rest)
            if (rest ~ /^[A-Za-z0-9_]+:/) sub(/^[A-Za-z0-9_]+:/,"",rest)
            split(rest,x," "); pkg=x[1]
            gsub(/[^A-Za-z0-9._]/,"",pkg)
            if (pkg=="" || index(pkg,".")<=0 || n+0<=0) next
            if (!(pkg in sum)) {
                if (k >= max) next
                order[++k] = pkg
            }
            sum[pkg] += n
        }
        END { for (i = 1; i <= k; i++) print order[i] "=" sum[order[i]] }'
}

# 多行榜单不用 awk -v 传：toybox awk 遇到值里的换行会直接报
# "newline in string"，Android 上每轮都会失败。走环境变量，ENVIRON 原样取回。
ds_board_merge() {
    DS_BOARD_OLD="$1" awk -v max="$MAX_WAKE_APPS" '
        BEGIN {
            old = ENVIRON["DS_BOARD_OLD"]
            n=split(old, L, "\n")
            for (i=1; i<=n; i++) if (L[i] != "") { split(L[i], f, " "); board[f[1]]=f[2]+0; bcount++ }
        }
        { inc[$1]+=$2; if (!($1 in seen)) { order[++o]=$1; seen[$1]=1 } }
        END {
            for (i=1; i<=o; i++) {
                p=order[i]
                if (p in board) board[p]+=inc[p]
                else if (bcount<max) { board[p]=inc[p]; bcount++ }
            }
            for (p in board) print p, board[p]
        }'
}

# 负增量 = 系统计数器重置，丢弃该包本轮数据。
ds_wakes_delta() {
    DS_WAKES_BASE="$1" awk '
        BEGIN {
            base = ENVIRON["DS_WAKES_BASE"]
            n=split(base, L, "\n")
            for (i=1; i<=n; i++) if (L[i] != "") { split(L[i], f, "="); b[f[1]]=f[2]+0 }
        }
        { split($0, kv, "="); d=kv[2]-b[kv[1]]; if (d>=1) print kv[1], d }'
}

ds_wakes_save() {
    {
        echo "boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
        echo "since=$ds_wcsince"
        [ -n "$ds_run_list" ] && echo "$ds_run_list" | awk '{print "A "$1"="$2}'
        [ -n "$ds_cur_list" ] && echo "$ds_cur_list" | awk '{print "S "$1"="$2}'
    } | atomic_write "$DS_WAKES" 0644
}

# boot_id 变化 = 跨重启，两榜清零；读不到时按匹配处理，宁可续算不误删。
ds_wakes_load() {
    ds_cur_list=""; ds_wcsince=0
    [ -f "$DS_WAKES" ] || { ds_wcsince=$(now); return 0; }
    local saved_boot cur_boot
    saved_boot="$(awk -F= '$1=="boot" {print $2}' "$DS_WAKES" 2>/dev/null)"
    cur_boot="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
    if [ -z "$saved_boot" ] || [ -z "$cur_boot" ] || [ "$saved_boot" = "$cur_boot" ]; then
        ds_cur_list="$(awk '$1=="S" { n=split($2, kv, "="); if (n==2) print kv[1], kv[2] }' "$DS_WAKES" 2>/dev/null)"
        ds_wcsince="$(awk -F= '$1=="since" {print $2}' "$DS_WAKES" 2>/dev/null)"
    fi
    [ -n "$ds_wcsince" ] || ds_wcsince=$(now)
}

ds_wakes_reset_on_unplug() {
    ds_cur_list=""
    ds_wcsince=$(now)
    ds_cur_base="$(ds_wakes_collect)"
    ds_wakes_save
    log_ds "唤醒统计：检测到拔电，榜单重新累计"
}

ds_wakes_update() {
    local cur delta
    cur="$(ds_wakes_collect)"
    [ -n "$ds_cur_base" ] && [ -n "$cur" ] && {
        delta="$(echo "$cur" | ds_wakes_delta "$ds_cur_base")"
        if [ -n "$delta" ]; then
            ds_cur_list="$(echo "$delta" | ds_board_merge "$ds_cur_list")"
            ds_run_list="$(echo "$delta" | ds_board_merge "$ds_run_list")"
            ds_wakes_save
        fi
    }
    ds_cur_base="$cur"
}

# ============================ 媒体保护 ========================================

# 同一行里同时有 PlaybackState、{state=3、package= 才算在播。
ds_media_playing() {
    timeout 2 dumpsys media_session 2>/dev/null | awk '
        index($0,"PlaybackState") && index($0,"{state=3") && index($0,"package=") {
            line=$0
            pos=index(line,"package=")
            pkg=substr(line,pos+8)
            split(pkg,x," ")
            sub(/[^A-Za-z0-9._].*$/,"",x[1])
            if (x[1]!="") print x[1]
        }'
}

# ============================ 后台冻结 ========================================
# 冻结第三方后台进程，替代“熄屏 20 秒就把整机压进 Doze”的高频做法：
#   T0 冻结 → T+3min 停止新增 → T+7:30 一次性 force-idle。
# 只操作 /dev/freezer（或 v2 freezer）里的自有子组，不碰别的模块建的组。
# 所有改动先写 run/freeze.saved 账本；亮屏、关开关、启动、卸载都按账本解冻。

ds_proc_stat_after_comm() {
    local s
    s="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    case "$s" in *") "*) s="${s##*) }" ;; *) return 1 ;; esac
    printf '%s\n' "$s"
}

ds_proc_start() {
    local s
    s="$(ds_proc_stat_after_comm "$1")" || return 1
    printf '%s\n' "$s" | awk '{print $20}'
}

# 选冻结后端：优先 v2（每进程原地冻结），其次 v1（独立 freezer 层级），最后 SIGSTOP。
#
# 为什么 v2 优先：cgroup v2 里冻结是**核心功能**（cgroup.freeze），它不会出现在
# cgroup.controllers 里 —— 拿 controllers 去找 'freezer' 在任何 v2 系统上都不成立，
# 老代码就是这么一路掉到 SIGSTOP 的。判据改成"某个真实 cgroup 的 cgroup.freeze 可写"。
#
# 为什么 v2 上不另建顶层组：Android 已经按 uid / 进程分好了组，把进程搬出自己的
# uid 组会让 LMKD / PSI 看不见它的内存。v2 只能原地冻它自己那个 cgroup。
ds_freeze_probe() {
    ds_freeze_backend=""
    ds_freeze_root=""
    ds_freeze_group=""
    if [ "$DS_FREEZE_ENABLED" != "1" ]; then
        ds_freeze_backend="off"
        return 0
    fi
    local p m
    if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
        for p in /sys/fs/cgroup/apps/cgroup.freeze /sys/fs/cgroup/cgroup.freeze; do
            if [ -w "$p" ]; then
                ds_freeze_backend="v2"
                ds_freeze_root="/sys/fs/cgroup"
                return 0
            fi
        done
    fi
    m="$(awk '$3=="cgroup" && $4 ~ /(^|,)freezer(,|$)/ {print $2; exit}' /proc/mounts 2>/dev/null)"
    if [ -n "$m" ] && [ -d "$m" ] && [ -w "$m" ]; then
        ds_freeze_backend="v1"
        ds_freeze_root="$m"
        ds_freeze_group="$m/$DS_FREEZE_GROUP_NAME"
        return 0
    fi
    # 两套 cgroup 都用不了就干脆不冻结。
    # 以前的兜底是 SIGSTOP：它能把任意进程按死在原地，一旦按到系统正在
    # 同步调用的进程（content provider / bound service），system_server 会
    # 卡在 binder 上，watchdog 60 秒后直接把整机重启 —— 那正是"用着用着
    # 突然强制重启"最像的一条路径。宁可这次不冻，也不冒这个险。
    ds_freeze_backend="none"
    return 0
}

# ── v2：原地冻结 ────────────────────────────────────────────────────
# 只写进程自己那个 cgroup 的 cgroup.freeze，不迁移、不建组。
# 路径来自 /proc/<pid>/cgroup 里 "0::/..." 那一行（v2 统一层级）。
ds_v2_path_of() {
    awk -F: '$1 == "0" { print $3; exit }' "/proc/$1/cgroup" 2>/dev/null
}
ds_v2_file_of() {
    case "${1:-}" in ''|/) return 1 ;; esac
    printf '%s%s/cgroup.freeze\n' "${ds_freeze_root:-/sys/fs/cgroup}" "$1"
}
# 「现在是否真的冻住了」看 cgroup.events 的 frozen，而不是看 cgroup.freeze：
# 后者只是我们写下去的请求值；前者才是内核回报的实际状态。
# 也不能看 /proc 的进程状态 —— 本来就在睡眠的进程被冻住后仍然是 S。
ds_v2_is_frozen() {
    local cg="$1" f
    case "$cg" in ''|/) return 1 ;; esac
    f="${ds_freeze_root:-/sys/fs/cgroup}${cg}/cgroup.events"
    if [ -r "$f" ]; then
        grep -q '^frozen 1' "$f" 2>/dev/null && return 0
        return 1
    fi
    f="$(ds_v2_file_of "$cg")" || return 1
    [ -r "$f" ] && [ "$(cat "$f" 2>/dev/null)" = "1" ]
}
ds_v2_write() {
    local f
    f="$(ds_v2_file_of "$1")" || return 1
    [ -w "$f" ] || return 1
    printf '%s\n' "$2" > "$f" 2>/dev/null
}

ds_freeze_move_root() {
    local pid="$1"
    case "$ds_freeze_backend" in
        v1)
            # 先 cgroup.procs：tasks 只搬主线程，别的线程留在冻结组里会解不干净。
            if [ -w "$ds_freeze_root/cgroup.procs" ]; then
                printf '%s\n' "$pid" > "$ds_freeze_root/cgroup.procs" 2>/dev/null
            elif [ -w "$ds_freeze_root/tasks" ]; then
                printf '%s\n' "$pid" > "$ds_freeze_root/tasks" 2>/dev/null
            fi
            ;;
        v2)
            printf '%s\n' "$pid" > "$ds_freeze_root/cgroup.procs" 2>/dev/null
            ;;
    esac
}

# pid 是不是还待在我们自己建的那个组里。
# 解冻不能只看账本：ColorOS 自带的冻结器（HANS）在同一个 /dev/freezer 层级上
# 运转，会把进程在 frozen/thaw/根组之间搬。账本记着"这个 pid 是我冻的"，
# 它其实早被系统搬走了；这时再按账本搬一次，等于把系统刚冻上的进程解开，
# 系统下一轮又冻回去 —— 两边对着搬，待机时反复进出冻结最费电。
ds_pid_in_group() {
    local pid="$1" g="$2"
    [ -n "$pid" ] && [ -n "$g" ] && [ -f "$g/cgroup.procs" ] || return 1
    grep -qx -- "$pid" "$g/cgroup.procs" 2>/dev/null
}

# 这个 pid 是不是已经被别的冻结器冻住了。
# 两边都冻同一个进程没有意义，只会互相把对方刚冻上的解开。

# 清掉自己的残留组：先解冻，再把组里的进程迁回根组，最后删组。
ds_freeze_group_reset() {
    local g="$ds_freeze_group" p
    [ -n "$g" ] && [ -d "$g" ] || return 0
    case "$ds_freeze_backend" in
        v1)
            [ -w "$g/freezer.state" ] && echo THAWED > "$g/freezer.state" 2>/dev/null
            for p in $(cat "$g/cgroup.procs" 2>/dev/null; cat "$g/tasks" 2>/dev/null); do
                ds_freeze_move_root "$p"
            done
            rmdir "$g" 2>/dev/null
            ;;
        v2)
            [ -w "$g/cgroup.freeze" ] && echo 0 > "$g/cgroup.freeze" 2>/dev/null
            for p in $(cat "$g/cgroup.procs" 2>/dev/null); do
                ds_freeze_move_root "$p"
            done
            rmdir "$g" 2>/dev/null
            ;;
    esac
}






# 按账本/当前内存解冻。启动时也走这里：先 THAWED，再把进程迁回根组，
# 最后删自己的组；SIGSTOP 后端只发 CONT。
ds_freeze_thaw_all() {
    local b r g line pid st pkg cg led=""
    b="$ds_freeze_backend"; r="$ds_freeze_root"; g="$ds_freeze_group"
    if [ -f "$DS_FREEZE_LEDGER" ]; then
        b="$(awk -F= '$1=="backend"{print $2; exit}' "$DS_FREEZE_LEDGER" 2>/dev/null)"
        r="$(awk -F= '$1=="root"{print $2; exit}' "$DS_FREEZE_LEDGER" 2>/dev/null)"
        g="$(awk -F= '$1=="group"{print $2; exit}' "$DS_FREEZE_LEDGER" 2>/dev/null)"
        while IFS= read -r line; do
            case "$line" in *"|"*) led="$led$line
" ;; esac
        done < "$DS_FREEZE_LEDGER"
        # 账本每批都落盘，比内存里的 ds_frozen 全；两边并集会重复解冻同一 PID。
        [ -n "$led" ] && ds_frozen="$led"
    fi
    ds_freeze_backend="$b"; ds_freeze_root="$r"; ds_freeze_group="$g"
    [ "$b" = "v1" ] && [ -n "$g" ] && [ -w "$g/freezer.state" ] \
        && echo THAWED > "$g/freezer.state" 2>/dev/null
    for line in $ds_frozen; do
        # 不能用 ${line%%|*}：mksh 把 | 当成 ksh 模式的“或”，会切出空串。
        # 第 4 段是 cgroup 路径（v2 原地冻结靠它，v1 记的是自己的组）。
        IFS='|' read -r pid st pkg cg <<EOF
$line
EOF
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        if [ "$b" = "sigstop" ]; then
            kill -CONT "$pid" 2>/dev/null
        elif [ "$b" = "v2" ]; then
            # 只解冻我们当初冻的那个 cgroup，且只在这个 pid 还是当初那个进程时
            # （pid 会被回收，starttime 变了就不是它了）。
            [ -n "$cg" ] || continue
            if [ -n "$st" ] && [ "$(ds_proc_start "$pid")" != "$st" ]; then continue; fi
            ds_v2_write "$cg" 0
        else
            # 只搬还在我们组里的。被系统冻结器搬走的就别抢回来：
            # 那不是我们的冻结，解了它等于逼系统重新冻一遍。
            ds_pid_in_group "$pid" "$g" && ds_freeze_move_root "$pid"
        fi
    done
    # 账本可能漏掉后来进组的子进程：按组里的实际 PID 再搬一遍，否则组删不掉，
    # 下一轮深睡会把这些进程连带冻上。（v1 专有，v2 没有组）
    if [ -n "$g" ] && [ -d "$g" ]; then
        for pid in $(cat "$g/cgroup.procs" 2>/dev/null; cat "$g/tasks" 2>/dev/null); do
            case "$pid" in ''|*[!0-9]*) continue ;; esac
            ds_freeze_move_root "$pid"
        done
    fi
    [ -n "$g" ] && [ -d "$g" ] && rmdir "$g" 2>/dev/null
    rm -f "$DS_FREEZE_LEDGER" "$DS_FREEZE_QUEUE" 2>/dev/null
    ds_frozen=""
    ds_freeze_frozen=0
    ds_freeze_done=0
    [ "$b" != "off" ] && [ "$b" != "none" ] && log_ds "冻结已还原（$b）"
}

# 启动/停止兜底：账本在就按账本解冻；只有自己的组在也要解冻清空。
# 冻结功能已移除，这里唯一的作用是"把旧版本留下的冻结解开"。
# 所以探测时临时把开关当 1：不是要冻，是要让 probe 找出组和挂载点，
# 否则开关关着时 probe 直接返回 off，残留组就永远没人清。
ds_freeze_restore_if_any() {
    local _en="$DS_FREEZE_ENABLED"
    DS_FREEZE_ENABLED=1
    ds_freeze_probe
    DS_FREEZE_ENABLED="$_en"
    if [ -f "$DS_FREEZE_LEDGER" ]; then
        ds_freeze_thaw_all
        return 0
    fi
    if [ -n "$ds_freeze_group" ] && [ -d "$ds_freeze_group" ]; then
        ds_freeze_group_reset
        log_ds "发现残留冻结组，已解冻并清理"
    fi
    return 0
}

# 排除名单：用户永久豁免、输入法、桌面、无障碍、VPN、当前前台、满血/熄屏也封顶、
# 正在放媒体的应用、本模块/管理器和 Scene（后者负责耗电统计，冻了就断档）。
DS_FREEZE_SKIP=""


# 找第三方后台进程，写成 "pid|starttime|pkg" 队列。只看 -3 包；
# 只冻 oom_score_adj >= 200（缓存/后台），前台和可见应用不动。
#
# 一轮扫描只起固定几个子进程：pm、ps、读 stat/oom 的 awk、拼队列的 awk。
# 逐个 /proc 起 cat/tr/head/grep/awk 会产生几千次 fork，熄屏后反而把 CPU 拉醒。

# 每轮最多冻 8 个；第一批后把组置 FROZEN，后续迁入的进程自动冻结。
# 队列空不代表冻结结束：3 分钟窗口里还有应用退到后台，由调用方补扫。

# 媒体/新豁免应用需要跑：把它从这个包里单独解冻，其余保持冻结。

# 7:30 一次性收口：cmd 优先，失败补发 dumpsys 兼容命令，仍失败不再重试。
ds_doze_once() {
    [ "$DS_DOZE_ENABLED" = "1" ] || return 0
    [ "$ds_doze_sent" = "1" ] && return 0
    ds_doze_sent=1
    if timeout 2 cmd deviceidle force-idle deep >/dev/null 2>&1; then
        log_ds "系统已接受一次深度休眠请求"
    elif timeout 2 dumpsys deviceidle force-idle deep >/dev/null 2>&1; then
        log_ds "系统已接受一次兼容休眠请求"
    else
        log_ds "深度休眠命令未被接受，本次不再重试"
    fi
}

# ============================ 唤醒锁（appops） ===============================

ds_ops_get() {
    # stdin 要断开：调用方常在 `while read < 账本` 里，cmd 继承账本 fd 会读到空结果。
    cmd appops get "$1" WAKE_LOCK 2>/dev/null < /dev/null \
        | awk '$1 == "WAKE_LOCK:" { sub(/;.*/, "", $2); print $2; exit }'
}


ds_ops_restore_all() {
    local line pkg old now keep=""
    [ -f "$DS_OPS_LEDGER" ] || return 0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        IFS='|' read -r pkg old <<EOF
$line
EOF
        case "$pkg" in ''|*[!A-Za-z0-9._]*) continue ;; esac
        case "$old" in allow|deny|ignore|default) ;; *) old=default ;; esac
        # 在 while read < 账本 里，cmd 会继承账本 fd，appops 事务会直接失败；
        # 必须断开 stdin，否则旧值永远写不回去。
        cmd appops set "$pkg" WAKE_LOCK "$old" >/dev/null 2>&1 < /dev/null
        # 设置失败、被系统改回，或 AppOps 缓存还没刷新时保留账本，下次启动再试；
        # 否则会留下 ignore。最多等 0.6 秒让缓存跟上。
        now=""
        _i=0
        while [ "$_i" -lt 3 ]; do
            now="$(ds_ops_get "$pkg")"
            [ "$now" = "$old" ] && break
            sleep 0.2
            _i=$((_i + 1))
        done
        [ "$now" = "$old" ] || keep="$keep$pkg|$old
"
    done < "$DS_OPS_LEDGER"
    if [ -n "$keep" ]; then
        printf '%s' "$keep" | atomic_write "$DS_OPS_LEDGER" 0644
    else
        rm -f "$DS_OPS_LEDGER" 2>/dev/null
    fi
    ds_ops_done=""
}

ds_ops_restore_if_any() {
    [ -f "$DS_OPS_LEDGER" ] || return 0
    ds_ops_restore_all
    if [ -f "$DS_OPS_LEDGER" ]; then
        log_ds "唤醒锁还原未完成，账本保留到下次启动再试"
    else
        log_ds "唤醒锁设置已按账本还原"
    fi
}

# ============================ 策略迁移 ========================================
# V1.6 之前内置把微信/QQ/企业微信/钉钉整包放进 white=：整包不冻结、
# 不进唤醒锁管控，还进了 Doze 白名单。这里做一次性迁移：IM 保活（nice=），
# 微信只放行 :push 进程（push=），其余第三方照常冻结/管唤醒锁。
# 保留系统关键应用和用户其它 white= 条目。
ds_policy_migrate() {
    local marker="$CONF_DIR/run/policy_v2" p installed
    [ -f "$marker" ] && return 0
    [ -f "$DSLEEP_CONF" ] || return 0
    installed="$(pm list packages 2>/dev/null | sed 's/^package://' | tr -d '\r')"
    [ -n "$installed" ] || return 0
    # 备份配置和迁移前的 Doze 白名单，出问题可人工回滚。
    cp -f "$DSLEEP_CONF" "$DSLEEP_CONF.bak-policy2" 2>/dev/null
    timeout 2 dumpsys deviceidle whitelist 2>/dev/null > "$CONF_DIR/run/doze.before-policy2"
    {
        awk '
            /^[[:space:]]*white[[:space:]]*=/ {
                v = $0
                sub(/^[^=]*=/, "", v)
                sub(/#.*/, "", v)
                gsub(/[[:space:]\r]/, "", v)
                if (v == "com.tencent.mm" || v == "com.tencent.mobileqq" ||
                    v == "com.tencent.tim" || v == "com.tencent.wework" ||
                    v == "com.alibaba.android.rimet") next
            }
            { print }
        ' "$DSLEEP_CONF"
        for p in com.tencent.mm com.tencent.mobileqq com.tencent.tim \
                 com.tencent.wework com.alibaba.android.rimet; do
            printf '%s\n' "$installed" | grep -qx -- "$p" || continue
            printf 'nice=%s\n' "$p"
        done
        if printf '%s\n' "$installed" | grep -qx -- 'com.tencent.mm'; then
            printf 'push=com.tencent.mm:push\n'
        fi
    } | atomic_write "$DSLEEP_CONF" 0644 || return 0
    # 旧版把这些整包永久加进了系统 Doze 白名单；迁移时撤掉（微信 push 会在
    # 每次进入深睡时重新临时加白，QQ 等不再整包放行）。
    for p in com.tencent.mm com.tencent.mobileqq com.tencent.tim \
             com.tencent.wework com.alibaba.android.rimet; do
        timeout 2 dumpsys deviceidle whitelist "-$p" >/dev/null 2>&1
    done
    printf '%s\n' "policy_v2 $(now)" > "$marker" 2>/dev/null
    ds_conf_raw=""
    log_ds "深睡策略已迁移：微信/QQ 等改为保活（不冻结），微信 push 放行"
    return 0
}

# V3：音乐/健康/Scene 之前整包 white= 也被注入 Doze 白名单；force-idle 后这些
# 应用仍能跑，抵消了 Doze。迁到 nice=：不冻结，但不进 Doze；正在播放的音乐由
# 媒体检测临时加白。系统关键包（com.android./厂商前缀）仍保留 white= + Doze。
ds_policy_migrate_v3() {
    local marker="$CONF_DIR/run/policy_v3" p installed
    [ -f "$marker" ] && return 0
    [ -f "$DSLEEP_CONF" ] || return 0
    installed="$(pm list packages 2>/dev/null | sed 's/^package://' | tr -d '')"
    [ -n "$installed" ] || return 0
    cp -f "$DSLEEP_CONF" "$DSLEEP_CONF.bak-policy3" 2>/dev/null
    timeout 2 dumpsys deviceidle whitelist 2>/dev/null > "$CONF_DIR/run/doze.before-policy3"
    {
        awk '
            /^[[:space:]]*white[[:space:]]*=/ {
                v = $0
                sub(/^[^=]*=/, "", v)
                sub(/#.*/, "", v)
                gsub(/[[:space:]]/, "", v)
                if (v == "com.netease.cloudmusic" || v == "com.tencent.qqmusic" ||
                    v == "com.kugou.android" || v == "cn.kuwo.player" ||
                    v == "com.ximalaya.ting.android" || v == "com.mi.health" ||
                    v == "com.xiaomi.hm.health" || v == "com.huawei.health" ||
                    v == "com.hihonor.health" || v == "com.heytap.health" ||
                    v == "com.heytap.health.international" || v == "com.oplus.healthservice" ||
                    v == "com.vivo.health" || v == "com.sec.android.app.shealth" ||
                    v == "com.omarea.vtools") next
            }
            { print }
        ' "$DSLEEP_CONF"
        for p in com.netease.cloudmusic com.tencent.qqmusic com.kugou.android                  cn.kuwo.player com.ximalaya.ting.android com.mi.health                  com.xiaomi.hm.health com.huawei.health com.hihonor.health                  com.heytap.health com.heytap.health.international                  com.oplus.healthservice com.vivo.health com.sec.android.app.shealth                  com.omarea.vtools; do
            printf '%s
' "$installed" | grep -qx -- "$p" || continue
            printf 'nice=%s
' "$p"
        done
    } | atomic_write "$DSLEEP_CONF" 0644 || return 0
    for p in com.netease.cloudmusic com.tencent.qqmusic com.kugou.android              cn.kuwo.player com.ximalaya.ting.android com.mi.health              com.xiaomi.hm.health com.huawei.health com.hihonor.health              com.heytap.health com.heytap.health.international              com.oplus.healthservice com.vivo.health com.sec.android.app.shealth              com.omarea.vtools; do
        timeout 2 dumpsys deviceidle whitelist "-$p" >/dev/null 2>&1
    done
    printf '%s
' "policy_v3 $(now)" > "$marker" 2>/dev/null
    ds_conf_raw=""
    log_ds "深睡策略已迁移：音乐/健康/Scene 移出 Doze 白名单（改为保活）"
    return 0
}

# ============================ 进入 / 退出 =====================================

ds_enter() {
    local old_white pkg n=0 sms
    old_white=""
    ds_added=""
    ds_white_seen=""
    # Doze 总闸关着就一个白名单都不动：白名单是"包级豁免"，加进去等于整包
    # 不受 Doze 限制，这本身就是一条耗电来源，得能单独关掉做对照。
    [ "$DS_DOZE_ENABLED" = "1" ] && old_white="$(ds_white_snapshot)"
    for pkg in $(echo "$DS_WHITE" | tr ',' ' '); do
        [ -n "$pkg" ] || continue
        n=$((n+1)); [ "$n" -gt "$MAX_WHITE" ] && break
        ds_white_seen="$ds_white_seen $pkg"
        [ "$DS_DOZE_ENABLED" = "1" ] || continue
        ds_white_add "$pkg" "$old_white"
    done
    if [ "$DS_DOZE_ENABLED" = "1" ]; then
        # push= 的包要进 Doze 白名单：现在已经不冻进程了，Doze 是唯一会压后台的
        # 机制，不把它放进白名单，熄屏后消息会被压到维护窗口才进来。
        # 代价是这台机上那个应用不受 Doze 限制 —— 和"只留 Doze"的取舍就是这个。
        for pkg in $(ds_push_pkg_list | tr ',' ' '); do
            [ -n "$pkg" ] || continue
            case " $ds_added " in *" $pkg "*) continue ;; esac
            ds_white_add "$pkg" "$old_white"
        done
        # 默认短信应用不在名单里也要临时加白：否则 SMS 广播会被压到出 Doze 才发。
        # 退出时按 ds_added 撤掉；用户名单里的那条不会被撤。
        sms="$(ds_sms_pkg)"
        if [ -n "$sms" ]; then
            case ",$DS_WHITE," in
                *",$sms,"*) : ;;
                *) ds_white_add "$sms" "$old_white"
                   log_ds "短信应用 $sms 已临时加白" ;;
            esac
        fi
        # 正在放媒体的应用（可能在 nice=）进入时也临时进 Doze 白名单，
        # 否则 force-idle 后音乐会被压住；退出时按 ds_added 撤销。
        for pkg in $(ds_media_playing); do
            [ -n "$pkg" ] || continue
            case " $ds_added " in *" $pkg "*) continue ;; esac
            ds_white_add "$pkg" "$old_white"
            log_ds "媒体应用 $pkg 已临时加白"
        done
    fi
    ds_snap_write
    # 深睡只做 Doze：冻结（墓碑）已移除，这里不再建冻结组、不再扫候选、不再冻进程。
    ds_doze_sent=0
    ds_doze_checked=0
    ds_frozen=""
    ds_freeze_done=1
    ds_alarm_wait=0
    ds_event_ok=1
    ds_last_media=$(now)
    ds_last_wake=$(now)
    ds_run_list=""
    ds_cur_base="$(ds_wakes_collect)"
    ds_wakes_save
    ds_active=1
    log_ds "已进入（保护 $n 个应用，冻结后端 $ds_freeze_backend）"
}

ds_exit() {
    local pkg
    # 先尝试恢复唤醒锁；冻结中的进程可能让 appops 写不进去，解冻后再按留下的账本重试。
    ds_ops_restore_all
    ds_freeze_thaw_all
    [ -f "$DS_OPS_LEDGER" ] && ds_ops_restore_all
    # 只撤销我们自己发起的 Doze。没 force 过还去 unforce 的话，
    # 会把系统正在进行的深度空闲打断，下一轮它又从头进 —— 来回收口最耗电。
    [ "$ds_doze_sent" = "1" ] && timeout 2 dumpsys deviceidle unforce >/dev/null 2>&1
    for pkg in $ds_added; do ds_white_del "$pkg"; done
    ds_wakes_update
    [ -n "$ds_run_list" ] && echo "$ds_run_list" | awk '{print "A "$1"="$2}' \
        | atomic_write "$DS_STATS" 0644
    rm -f "$DS_SNAP" "$DS_FREEZE_QUEUE" 2>/dev/null
    ds_active=0
    ds_added=""
    DS_THIRD=""
    DS_FREEZE_SKIP=""
    ds_doze_sent=0
    ds_doze_checked=0
    ds_alarm_wait=0
    log_ds "已退出（$1），冻结/唤醒锁与白名单已还原"
}

# ============================ 状态文件 ========================================
# 只放本功能自己的键。dsactive=1 时前端显示「深度睡眠中」。
ds_state_write() {
    local new dsw dswc
    dsw=""
    [ -n "$ds_run_list" ] && dsw="$(echo "$ds_run_list" | sort -k2 -rn -k1,1 | head -12 | awk -v w=",$DS_WHITE," '
        { sep=(NR>1)?",":""
          flag=(index(w, "," $1 ",")>0)?1:0
          printf "%s%s:%d:%d", sep, $1, $2, flag }')"
    dswc=""
    [ -n "$ds_cur_list" ] && dswc="$(echo "$ds_cur_list" | sort -k2 -rn -k1,1 | head -12 | awk -v w=",$DS_WHITE," '
        { sep=(NR>1)?",":""
          flag=(index(w, "," $1 ",")>0)?1:0
          printf "%s%s:%d:%d", sep, $1, $2, flag }')"
    new="enable=$ds_enabled
dsactive=$ds_active
wcsince=$ds_wcsince
dsw=$dsw
dswc=$dswc"
    [ "$new" = "$ds_state_last" ] && [ -f "$STATE_FILE" ] && return 0
    if printf '%s\n' "$new" | atomic_write "$STATE_FILE" 0644; then
        ds_state_last="$new"
    fi
}

# ============================ 主循环 ==========================================

ds_tick() {
    local t media pkg charging
    # 记下这一轮的起点：事件等待用它补读“上一轮巡检之后、监听开始之前”的亮屏。
    ds_tick_stamp=$(date '+%m-%d %H:%M:%S.000')
    ds_conf_load
    t=$(now)
    charging=0; ds_charging && charging=1

    # 拔电重置与是否深睡无关：充电时拔线也要重计累计榜。
    if [ "$ds_was_charging" = "1" ] && [ "$charging" = "0" ] && [ "$ds_enabled" = "1" ]; then
        ds_wakes_reset_on_unplug
    fi
    ds_was_charging=$charging

    if [ "$ds_enabled" != "1" ]; then
        [ "$ds_active" = "1" ] && ds_exit "关开关"
        ds_screen_off_ts=0
        ds_alarm_checked=0
        ds_alarm_wait=0
        ds_game_checked=0
        ds_state_write
        # 关着就不常驻：写完这一轮状态就退，重新打开时 api.sh 会立刻拉起。
        ds_quit=1
        return 0
    fi

    if ds_screen_on; then
        if [ "$ds_active" = "1" ]; then
            ds_exit "亮屏"
        else
            ds_screen_off_ts=0
            ds_alarm_checked=0
            ds_alarm_wait=0
            ds_game_checked=0
        fi
        ds_state_write
        return 0
    fi

    if [ "$charging" = "1" ]; then
        if [ "$ds_active" = "1" ]; then ds_exit "充电"; fi
        ds_screen_off_ts=0
        ds_alarm_checked=0
        ds_alarm_wait=0
        ds_game_checked=0
        ds_state_write
        return 0
    fi

    if [ "$ds_active" != "1" ]; then
        [ "$ds_screen_off_ts" = "0" ] && ds_screen_off_ts=$t
        if [ $((t - ds_screen_off_ts)) -ge "$SCREEN_OFF_DELAY" ]; then
            # 游戏还在前台（熄屏挂机等）：先不让位给 Doze，否则游戏会被挂住，
            # 这段耗电也会被系统记成空闲/待机。日志每分钟最多一行。
            if ds_game_active; then
                if [ $((t - ds_game_checked)) -ge 60 ]; then
                    ds_game_checked=$t
                    log_ds "游戏在前台，暂不进入深睡"
                fi
                ds_state_write
                return 0
            fi
            # 闹钟前半小时不进入；最多每分钟问一次 dumpsys，别每 2 秒拉一遍。
            # 判到闹钟后必须停在等待态：否则下一轮“60 秒未到”会跳过检查直接
            # 走 ds_enter，等于每 2 秒再查一次 dumpsys alarm。
            if [ "$ds_alarm_wait" = "1" ] && [ $((t - ds_alarm_checked)) -lt 60 ]; then
                ds_state_write
                return 0
            fi
            if [ $((t - ds_alarm_checked)) -ge 60 ]; then
                ds_alarm_checked=$t
                if ds_alarm_soon; then
                    ds_alarm_wait=1
                    log_ds "闹钟在 ${ALARM_GUARD_SEC} 秒内，暂不进入深睡"
                    ds_state_write
                    return 0
                fi
            fi
            ds_alarm_wait=0
            ds_enter
        fi
        ds_state_write
        return 0
    fi

    # ---- 已在深睡中 ----
    # 深睡期间新加入的用户白名单立即生效，不用等下一次熄屏。
    # 白名单快照每轮只取一次：它是一次完整 dumpsys，按包名各取一次
    # 等于同一轮里把同一份清单 dump 了 N 遍。
    # 用 ds_white_seen 而不是 ds_added 做守卫：Doze 关着时 ds_added 恒空，
    # 用它会每轮重跑一遍并刷日志。
    _snap=""; _snap_done=0
    for pkg in $(echo "$DS_WHITE" | tr ',' ' '); do
        [ -n "$pkg" ] || continue
        case " $ds_white_seen " in *" $pkg "*) continue ;; esac
        ds_white_seen="$ds_white_seen $pkg"
        if [ "$DS_DOZE_ENABLED" = "1" ]; then
            [ "$_snap_done" = "1" ] || { _snap="$(ds_white_snapshot)"; _snap_done=1; }
            ds_white_add "$pkg" "$_snap"
        fi
        ds_snap_write
        log_ds "新加入白名单的应用已加白"
    done

    if [ $((t - ds_last_media)) -ge "$MEDIA_CHECK_INTERVAL" ]; then
        ds_last_media=$t
        media="$(ds_media_playing)"
        for pkg in $media; do
            case " $ds_added " in *" $pkg "*) continue ;; esac
            # Doze 关着就不加白：加白是"整包豁免 Doze"，正是要能关掉的那部分。
            [ "$DS_DOZE_ENABLED" = "1" ] && ds_white_add "$pkg" "$(ds_white_snapshot)"
            ds_snap_write
            log_ds "检测到媒体播放，已临时加白保护"
        done
    fi

    # 冻结（墓碑）已移除：这里不再扫描候选、不再入组、不再冻进程。
    # 闹钟快响就先不收口：Doze 会把闹钟广播压到维护窗口，和“准点响”冲突。
    if [ "$DS_DOZE_ENABLED" = "1" ] \
       && [ $((t - ds_screen_off_ts)) -ge "$DS_DOZE_AT" ] && [ "$ds_doze_sent" != "1" ] \
       && [ $((t - ds_doze_checked)) -ge "$DS_DOZE_RETRY" ]; then
        ds_doze_checked=$t
        if ds_alarm_soon; then
            log_ds "闹钟在 ${ALARM_GUARD_SEC} 秒内，暂不进入系统 Doze"
        else
            ds_doze_once
        fi
    fi

    # 计数器是累计量：隔一分钟读一次和每 2 秒读一次入账的增量完全一样。
    if [ $((t - ds_last_wake)) -ge "$WAKE_SAMPLE_INTERVAL" ]; then
        ds_last_wake=$t
        ds_wakes_update
    fi

    ds_state_write
}

# 深睡激活后的等待：阻塞等 screen_toggled，亮屏立刻醒；最多 DS_ACTIVE_TICK 秒
# 醒一次做维护（充电/7:30/名单/媒体）。事件源不可用时退 30 秒定时。
# 返回 0 = 拿到事件，1 = 超时或事件源不可用。
ds_screen_event() {
    local t0 t1 wp lp wd stamp backlog
    ds_event_on=0
    : > "$DS_EVENT_TMP" 2>/dev/null
    t0=$(now)
    # 时间戳必须在起子 shell 之前取：后台进程真正跑起来可能晚几百毫秒。
    stamp=$(date '+%m-%d %H:%M:%S.000')
    if [ "$ds_event_ok" = "1" ] && command -v logcat >/dev/null 2>&1; then
        # 先非阻塞补读：上一轮 tick 之后、本次监听开始之前发生的亮屏不能漏。
        if [ -n "$ds_tick_stamp" ]; then
            backlog="$(timeout 2 logcat -b events -d -s screen_toggled \
                -T "$ds_tick_stamp" 2>/dev/null | grep 'screen_toggled' | tail -n 1)"
            if [ -n "$backlog" ]; then
                case "$backlog" in *"screen_toggled: 1"*) ds_event_on=1 ;; esac
                return 0
            fi
        fi
        (
            rm -f "$DS_EVENT_FIFO"
            mkfifo "$DS_EVENT_FIFO" 2>/dev/null || exit 0
            logcat -b events -s screen_toggled -T "$stamp" > "$DS_EVENT_FIFO" 2>/dev/null &
            lp=$!
            ( sleep "$DS_ACTIVE_TICK"; kill "$lp" 2>/dev/null ) &
            wd=$!
            grep -m1 'screen_toggled' < "$DS_EVENT_FIFO" > "$DS_EVENT_TMP"
            kill "$lp" "$wd" 2>/dev/null
            rm -f "$DS_EVENT_FIFO"
        ) &
        wp=$!
        ds_wait_pid=$wp
        # 监听已经挂上；补读之后才亮屏的，也让它走 tick 退出。
        if ds_screen_on; then
            ds_event_on=1
            ds_kill_children "$wp"
            wait "$wp" 2>/dev/null
            rm -f "$DS_EVENT_FIFO"
            ds_wait_pid=""
            return 0
        fi
        if ds_charging; then
            ds_kill_children "$wp"
            wait "$wp" 2>/dev/null
            rm -f "$DS_EVENT_FIFO"
            ds_wait_pid=""
            return 0
        fi
        wait "$wp"
        ds_wait_pid=""
        t1=$(now)
        if [ -s "$DS_EVENT_TMP" ]; then
            case "$(cat "$DS_EVENT_TMP" 2>/dev/null)" in *"screen_toggled: 1"*) ds_event_on=1 ;; esac
            return 0
        fi
        # 立刻返回空 = logcat 不可用/-T 不支持，退化成定时唤醒。
        if [ $((t1 - t0)) -lt 5 ]; then
            ds_event_ok=0
            log_ds "logcat 事件不可用，深睡维护改为 ${DS_ACTIVE_FALLBACK} 秒巡检"
        fi
        return 1
    fi
    sleep "$DS_ACTIVE_FALLBACK" &
    ds_wait_pid=$!
    wait $!
    ds_wait_pid=""
    return 1
}

# 已在深睡中的主循环：冻结窗口内 2 秒一步，之后只等事件/维护轮。
ds_active_wait() {
    local t busy
    while [ "$ds_active" = "1" ] && [ "$ds_quit" != "1" ]; do
        t=$(now)
        busy=0
        if [ $((t - ds_screen_off_ts)) -lt "$DS_FREEZE_WINDOW" ]; then
            if [ "$DS_FREEZE_ENABLED" = "1" ] && [ "$ds_freeze_done" != "1" ]; then
                busy=1
            fi
        fi
        if [ "$busy" = "1" ]; then
            sleep 2 &
            ds_wait_pid=$!
            wait $!
            ds_wait_pid=""
            ds_tick
            [ "$ds_quit" = "1" ] && return 0
            continue
        fi
        ds_screen_event
        if [ "$ds_event_on" = "1" ]; then
            ds_exit "亮屏"
            return 0
        fi
        ds_tick
        [ "$ds_quit" = "1" ] && return 0
    done
}

ds_lock_acquire() {
    local p i age
    i=0
    while [ "$i" -lt 10 ]; do
        if mkdir "$DS_LOCK_DIR" 2>/dev/null; then
            echo $$ > "$DS_LOCK_DIR/pid" 2>/dev/null
            return 0
        fi
        p="$(cat "$DS_LOCK_DIR/pid" 2>/dev/null)"
        case "$p" in
            ''|*[!0-9]*) ;;
            *)
                if [ -d "/proc/$p" ] && tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q 'deep_sleep.sh'; then
                    log_ds "已有深睡实例在跑（pid $p），本次不重复启动"
                    return 1
                fi
                ;;
        esac
        sleep 0.2
        i=$((i + 1))
    done
    # 刚创建、还没写 pid 的锁是“另一个实例正在启动”，绝不能抢；只有陈旧锁才清。
    age=$(( $(now) - $(stat -c %Y "$DS_LOCK_DIR" 2>/dev/null || echo 0) ))
    if [ "$age" -lt 30 ]; then
        log_ds "运行锁刚被创建，按启动中处理，本次不重复启动"
        return 1
    fi
    rm -rf "$DS_LOCK_DIR" 2>/dev/null
    mkdir "$DS_LOCK_DIR" 2>/dev/null || return 1
    echo $$ > "$DS_LOCK_DIR/pid" 2>/dev/null
}

ds_lock_release() {
    [ "$(cat "$DS_LOCK_DIR/pid" 2>/dev/null)" = "$$" ] && rm -rf "$DS_LOCK_DIR" 2>/dev/null
}

ds_alive_pid() {
    local p
    [ -f "$PIDFILE" ] || return 1
    p="$(cat "$PIDFILE" 2>/dev/null)"
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    [ -d "/proc/$p" ] || return 1
    tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -q 'deep_sleep\.sh'
}

ds_kill_children() {
    local p="$1" c
    [ -n "$p" ] || return 0
    for c in $(ps -A -o pid,ppid 2>/dev/null | awk -v pp="$p" '$2 == pp {print $1}'); do
        ds_kill_children "$c"
    done
    kill "$p" 2>/dev/null
}

ds_shutdown() {
    [ "$ds_shut" = "1" ] && exit 0
    ds_shut=1
    [ -n "$ds_wait_pid" ] && ds_kill_children "$ds_wait_pid"
    [ "$ds_active" = "1" ] && ds_exit "停止"
    # 入口失败/账本残留时也兜底还原；先解冻，再恢复冻结期间写不进去的 appops。
    ds_freeze_restore_if_any
    ds_ops_restore_if_any
    # 退出前把 dsactive 写回 0：api.sh 停止守护后页面还会读这份 state。
    ds_state_write
    rm -f "$PIDFILE" "$DS_EVENT_TMP" "$DS_EVENT_FIFO" 2>/dev/null
    ds_lock_release
    exit 0
}

ds_main() {
    mkdir -p "$CONF_DIR/run" 2>/dev/null
    ds_alive_pid && exit 0
    ds_shut=0
    ds_lock_acquire || exit 0
    echo $$ > "$PIDFILE" 2>/dev/null
    trap 'ds_shutdown' 0 1 2 3 15
    ds_conf_load
    ds_policy_migrate
    ds_conf_load
    ds_policy_migrate_v3
    ds_conf_load
    ds_freeze_probe
    ds_freeze_restore_if_any
    ds_ops_restore_if_any
    ds_wakes_load
    ds_snap_restore_if_any
    # 启动时先量一次大小。光靠 log_ds 里那个"每 64 条"不够：_ds_logn 是
    # 每进程计数器，而守护会因为「退出（停止）」、看门狗重拉而重启，重启就
    # 从 0 数起。文件一旦在重启前就已经超标，可能永远凑不满 64 条 ——
    # 这台机上 520KB 就是这样挂了两小时没掉（两个生命周期分别只攒到 59 和 25 条）。
    ds_log_trim
    ds_wait_pid=""
    rm -f "$DS_EVENT_TMP" "$DS_EVENT_FIFO" 2>/dev/null
    while true; do
        ds_quit=0
        ds_tick
        [ "$ds_quit" = "1" ] && ds_shutdown
        if [ "$ds_active" = "1" ]; then
            # 深睡中不再 2 秒轮询：事件等待 + 维护轮。
            ds_active_wait
            [ "$ds_quit" = "1" ] && ds_shutdown
        else
            # 前台 sleep 时 TERM 要等它睡完才进 trap（实测 2 秒）。放到后台
            # 再 wait：wait 会被信号立刻打断，关开关时不用白等一个巡检周期。
            sleep "$POLL_INTERVAL" &
            ds_wait_pid=$!
            wait $!
            ds_wait_pid=""
        fi
    done
}

case "${0##*/}" in deep_sleep.sh) ds_main ;; esac
