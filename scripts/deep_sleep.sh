#!/system/bin/sh
# 深度睡眠守护：熄屏且未充电满 20 秒后，先分批冻结第三方后台进程，
# 息屏 7:30 一次性 force-idle；亮屏或插电立即解冻、撤白名单并退出 Doze。
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
# 冻结方案按 ZenCore 的时序：熄屏后先冻结后台，7:30 才一次性 force-idle。
# 3 分钟内分批冻结，之后不再新增，减少冻结组里 D 状态进程把整组卡住的机会。
DS_FREEZE_ENABLED=${DS_FREEZE:-1}
DS_FREEZE_WINDOW=180
DS_FREEZE_RESCAN=60
DS_DOZE_AT=450
DS_DOZE_RETRY=300
DS_FREEZE_BATCH=8
DS_FREEZE_GROUP_NAME=${DS_FREEZE_GROUP_NAME:-socfreq_ds}
DS_FREEZE_LEDGER=$CONF_DIR/run/freeze.saved
DS_FREEZE_QUEUE=$CONF_DIR/run/freeze.queue
# 唤醒统计和媒体保护各要读一次 dumpsys（一次就是一次 system_server 往返，
# 熄屏时这条路不能每 2 秒走一遍）。计数器是累计量，隔多久读都不丢增量。
WAKE_SAMPLE_INTERVAL=${DS_WAKE_INTERVAL:-60}
MEDIA_CHECK_INTERVAL=${DS_MEDIA_INTERVAL:-30}
# 闹钟守卫：时钟类应用有这么多秒内要响的闹钟时，不进入深睡（已经在里面就退出）。
# 只让路闹钟前半小时，不是"有闹钟就永不深睡"。
ALARM_GUARD_SEC=1800
MAX_WHITE=167
MAX_WAKE_APPS=63

ds_enabled=0
ds_active=0
ds_screen_off_ts=0
ds_added=""
ds_alarm_checked=0
ds_game_checked=0
ds_was_charging=0
ds_state_last=""
ds_quit=0
# 冻结状态机
ds_freeze_backend=""
ds_freeze_root=""
ds_freeze_group=""
ds_freeze_frozen=0
ds_freeze_done=0
ds_freeze_fail=0
ds_frozen=""
ds_doze_sent=0
ds_doze_checked=0
ds_freeze_scan_ts=0
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
log_ds() { echo "$(date '+%m-%d %H:%M:%S' 2>/dev/null) $*" >>"$LOG" 2>/dev/null; }

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
enable=0
# 每行一条 white=包名，名单里的应用深睡时也允许被唤醒（等同系统电量设置里的「无限制」）。
# white=com.tencent.mm
EOF
    mv -f "$DSLEEP_CONF.tmp" "$DSLEEP_CONF" 2>/dev/null
    chmod 0644 "$DSLEEP_CONF" 2>/dev/null
}

DS_WHITE=""
ds_conf_raw=""
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
    DS_WHITE=$(awk '
        /^[[:space:]]*white[[:space:]]*=/ {
            v = $0
            sub(/^[^=]*=/, "", v)
            sub(/#.*/, "", v)
            gsub(/[[:space:]\r]/, "", v)
            if (v != "") print v
        }' "$DSLEEP_CONF" 2>/dev/null | tr '\n' ',' | sed 's/,$//')
    return 0
}

# ============================ Doze 原语 =======================================

# 只收像包名的行，滤掉 dumpsys 的段头。
ds_white_snapshot() {
    timeout 3 dumpsys deviceidle whitelist 2>/dev/null | tr -d '\r' | \
      awk 'NF>0 && $1 ~ /^[A-Za-z0-9][A-Za-z0-9_.]*$/ && index($1,".")>1 {print $1}' | tr '\n' ','
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
    } | atomic_write "$DS_SNAP" 0644
}

# 快照还在 = 上次没走正常退出。启动时立即还原并删除。
ds_snap_restore_if_any() {
    local c="" a added=""
    [ -f "$DS_SNAP" ] || return 0
    while IFS='=' read -r k v; do
        case "$k" in
            constants) c="$v" ;;
            added) [ -n "$v" ] && added="$added $v" ;;
        esac
    done <"$DS_SNAP" 2>/dev/null
    timeout 2 dumpsys deviceidle unforce >/dev/null 2>&1
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

# ============================ 后台冻结（ZenCore 方案） =======================
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

ds_proc_state() {
    local s
    s="$(ds_proc_stat_after_comm "$1")" || return 1
    printf '%s\n' "$s" | awk '{print $1}'
}

ds_proc_adj() {
    tr -d '\r\n' < "/proc/$1/oom_score_adj" 2>/dev/null
}

# 选冻结后端：v1 的 freezer 挂载点（这台机是 /dev/freezer），
# 其次 v2 的 cgroup.freeze，最后才 SIGSTOP。
ds_freeze_probe() {
    ds_freeze_backend=""
    ds_freeze_root=""
    ds_freeze_group=""
    if [ "$DS_FREEZE_ENABLED" != "1" ]; then
        ds_freeze_backend="off"
        return 0
    fi
    local m
    m="$(awk '$3=="cgroup" && $4 ~ /(^|,)freezer(,|$)/ {print $2; exit}' /proc/mounts 2>/dev/null)"
    if [ -n "$m" ] && [ -d "$m" ] && [ -w "$m" ]; then
        ds_freeze_backend="v1"
        ds_freeze_root="$m"
        ds_freeze_group="$m/$DS_FREEZE_GROUP_NAME"
        return 0
    fi
    if [ -r /sys/fs/cgroup/cgroup.controllers ] \
       && grep -q 'freezer' /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then
        ds_freeze_backend="v2"
        ds_freeze_root="/sys/fs/cgroup"
        ds_freeze_group="$ds_freeze_root/$DS_FREEZE_GROUP_NAME"
        return 0
    fi
    ds_freeze_backend="sigstop"
    return 0
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

ds_freeze_group_init() {
    case "$ds_freeze_backend" in
        off|sigstop) return 0 ;;
    esac
    ds_freeze_group_reset
    if [ "$ds_freeze_backend" = "v2" ]; then
        echo '+freezer' > "$ds_freeze_root/cgroup.subtree_control" 2>/dev/null
    fi
    mkdir -p "$ds_freeze_group" 2>/dev/null || {
        ds_freeze_backend="sigstop"
        return 0
    }
    case "$ds_freeze_backend" in
        v1)
            if [ ! -w "$ds_freeze_group/freezer.state" ]; then
                rmdir "$ds_freeze_group" 2>/dev/null
                ds_freeze_backend="sigstop"
                return 0
            fi
            echo THAWED > "$ds_freeze_group/freezer.state" 2>/dev/null
            ;;
        v2)
            if [ ! -w "$ds_freeze_group/cgroup.freeze" ]; then
                rmdir "$ds_freeze_group" 2>/dev/null
                ds_freeze_backend="sigstop"
                return 0
            fi
            echo 0 > "$ds_freeze_group/cgroup.freeze" 2>/dev/null
            ;;
    esac
}

ds_freeze_add_pid() {
    local pid="$1"
    case "$ds_freeze_backend" in
        v1)
            if [ -w "$ds_freeze_group/cgroup.procs" ]; then
                printf '%s\n' "$pid" > "$ds_freeze_group/cgroup.procs" 2>/dev/null && return 0
            fi
            printf '%s\n' "$pid" > "$ds_freeze_group/tasks" 2>/dev/null && return 0
            ;;
        v2)
            printf '%s\n' "$pid" > "$ds_freeze_group/cgroup.procs" 2>/dev/null && return 0
            ;;
        sigstop)
            kill -STOP "$pid" 2>/dev/null && return 0
            ;;
    esac
    return 1
}

ds_freeze_set() {
    case "$ds_freeze_backend" in
        v1) echo "$1" > "$ds_freeze_group/freezer.state" 2>/dev/null ;;
        v2)
            if [ "$1" = "FROZEN" ]; then
                echo 1 > "$ds_freeze_group/cgroup.freeze" 2>/dev/null
            else
                echo 0 > "$ds_freeze_group/cgroup.freeze" 2>/dev/null
            fi
            ;;
    esac
}

ds_freeze_verify() {
    local i=0
    case "$ds_freeze_backend" in
        sigstop|off) return 0 ;;
        v1)
            while [ "$i" -lt 6 ]; do
                [ "$(cat "$ds_freeze_group/freezer.state" 2>/dev/null)" = "FROZEN" ] && return 0
                sleep 0.2; i=$((i+1))
            done
            return 1
            ;;
        v2)
            while [ "$i" -lt 6 ]; do
                grep -q '^frozen 1' "$ds_freeze_group/cgroup.events" 2>/dev/null && return 0
                sleep 0.2; i=$((i+1))
            done
            return 1
            ;;
    esac
    return 1
}

ds_freeze_ledger_save() {
    {
        echo "backend=$ds_freeze_backend"
        echo "root=$ds_freeze_root"
        echo "group=$ds_freeze_group"
        printf '%s\n' "$ds_frozen"
    } | atomic_write "$DS_FREEZE_LEDGER" 0644
}

# 按账本/当前内存解冻。启动时也走这里：先 THAWED，再把进程迁回根组，
# 最后删自己的组；SIGSTOP 后端只发 CONT。
ds_freeze_thaw_all() {
    local b r g line pid led=""
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
    case "$b" in
        v1)
            [ -n "$g" ] && [ -w "$g/freezer.state" ] && echo THAWED > "$g/freezer.state" 2>/dev/null
            ;;
        v2)
            [ -n "$g" ] && [ -w "$g/cgroup.freeze" ] && echo 0 > "$g/cgroup.freeze" 2>/dev/null
            ;;
    esac
    for line in $ds_frozen; do
        # 不能用 ${line%%|*}：mksh 把 | 当成 ksh 模式的“或”，会切出空串。
        IFS='|' read -r pid _ <<EOF
$line
EOF
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        if [ "$b" = "sigstop" ]; then
            kill -CONT "$pid" 2>/dev/null
        else
            ds_freeze_backend="$b"; ds_freeze_root="$r"; ds_freeze_group="$g"
            ds_freeze_move_root "$pid"
        fi
    done
    [ -n "$g" ] && [ -d "$g" ] && rmdir "$g" 2>/dev/null
    rm -f "$DS_FREEZE_LEDGER" "$DS_FREEZE_QUEUE" 2>/dev/null
    ds_frozen=""
    ds_freeze_frozen=0
    ds_freeze_done=0
    [ "$b" != "off" ] && log_ds "冻结已还原（$b）"
}

# 启动/停止兜底：账本在就按账本还原；只有自己的组在也要解冻清空。
ds_freeze_restore_if_any() {
    ds_freeze_probe
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
ds_freeze_add_skip() {
    local p="$1"
    case "$p" in ''|*[!A-Za-z0-9._]*) return 0 ;; esac
    case " $DS_FREEZE_SKIP " in *" $p "*) return 0 ;; esac
    DS_FREEZE_SKIP="$DS_FREEZE_SKIP $p"
}

ds_freeze_build_skip() {
    local p x v ime
    DS_FREEZE_SKIP=""
    for p in $(echo "$DS_WHITE" | tr ',' ' '); do ds_freeze_add_skip "$p"; done
    for p in com.android.systemui com.android.phone com.android.server.telecom \
             com.sukisu.ultra me.weishu.kernelsu com.topjohnwu.magisk \
             com.omarea.vtools \
             com.sohu.inputmethod.sogou com.baidu.input com.iflytek.inputmethod \
             com.google.android.inputmethod.latin com.samsung.android.honeyboard \
             com.android.inputmethod.latin; do
        ds_freeze_add_skip "$p"
    done
    ime="$(settings get secure default_input_method 2>/dev/null | tr -d '\r')"
    [ -n "$ime" ] && ds_freeze_add_skip "${ime%%/*}"
    ime="$(settings get secure enabled_input_methods 2>/dev/null | tr -d '\r')"
    for x in $(echo "$ime" | tr ':' ' '); do ds_freeze_add_skip "${x%%/*}"; done
    ime="$(cmd package resolve-activity --brief -c android.intent.category.HOME 2>/dev/null \
          | sed -n 's#^[[:space:]]*\([A-Za-z0-9_.]*\)/.*#\1#p' | head -n 1)"
    [ -n "$ime" ] && ds_freeze_add_skip "$ime"
    ime="$(settings get secure enabled_accessibility_services 2>/dev/null | tr -d '\r')"
    for x in $(echo "$ime" | tr ':' ' '); do ds_freeze_add_skip "${x%%/*}"; done
    ime="$(settings get secure always_on_vpn_app 2>/dev/null | tr -d '\r')"
    [ -n "$ime" ] && ds_freeze_add_skip "$ime"
    for p in $(ds_media_playing); do ds_freeze_add_skip "$p"; done
    if [ -f "$CONF_DIR/app_modes.conf" ]; then
        while IFS='=' read -r p v; do
            case "$p" in ''|'#'*|';'*) continue ;; esac
            v="${v%%#*}"
            v="$(printf '%s' "$v" | tr -d ' \t\r')"
            case "$v" in full|FULL|keep|KEEP) ds_freeze_add_skip "$p" ;; esac
        done < "$CONF_DIR/app_modes.conf"
    fi
    p="$(awk -F= '$1=="fg"{print $2; exit}' "$DS_STATUS" 2>/dev/null | tr -d '\r')"
    case "$p" in ''|-) ;; *) ds_freeze_add_skip "$p" ;; esac
}

# 找第三方后台进程，写成 "pid|starttime|pkg" 队列。只看 -3 包；
# 只冻 oom_score_adj >= 200（缓存/后台），前台和可见应用不动。
ds_freeze_build_queue() {
    local d pid cmd pkg st stt adj third
    : > "$DS_FREEZE_QUEUE" 2>/dev/null
    [ "$DS_FREEZE_ENABLED" = "1" ] || return 0
    if [ -n "${DS_FREEZE_TEST_PID:-}" ]; then
        st="$(ds_proc_start "$DS_FREEZE_TEST_PID")"
        [ -n "$st" ] && printf '%s|%s|%s\n' "$DS_FREEZE_TEST_PID" "$st" \
            "${DS_FREEZE_TEST_PKG:-com.test.freeze}" > "$DS_FREEZE_QUEUE"
        return 0
    fi
    third="$(pm list packages -3 2>/dev/null | sed -n 's/^package://p' | tr -d '\r')"
    if [ -z "$third" ]; then
        log_ds "冻结：pm 没返回第三方应用，本次跳过"
        return 0
    fi
    ds_freeze_build_skip
    for d in /proc/[0-9]*; do
        pid="${d##*/}"
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        [ "$pid" = "$$" ] && continue
        cmd="$(cat "$d/cmdline" 2>/dev/null | tr '\0' '\n' | head -n 1)"
        [ -n "$cmd" ] || continue
        pkg="${cmd%%:*}"
        printf '%s\n' "$third" | grep -qx -- "$pkg" || continue
        case "$pkg" in
            # 输入法/桌面/闹钟这类被冻了会直接影响使用；厂商自带应用整片不碰
            # （ColorOS 的天气/健康/时钟在 -3 清单里，冻了就是闹钟不响）。
            *inputmethod*|*keyboard*|*launcher*|*alarmclock*|*deskclock*) continue ;;
            com.coloros.*|com.oplus.*|com.oppo.*|com.heytap.*|com.hihonor.*|\
            com.huawei.*|com.miui.*|com.xiaomi.*|com.google.*|com.vivo.*|\
            com.iqoo.*|com.bbk.*|com.samsung.*|com.mediatek.*|com.qualcomm.*|\
            com.transsion.*|com.tecno.*|com.infinix.*|com.sony.*|com.asus.*|\
            com.lenovo.*|com.nubia.*|com.meizu.*|com.oneplus.*|com.realme.*) continue ;;
        esac
        case " $DS_FREEZE_SKIP " in *" $pkg "*) continue ;; esac
        st="$(ds_proc_start "$pid")"
        [ -n "$st" ] || continue
        stt="$(ds_proc_state "$pid")"
        case "$stt" in Z|D|T|X) continue ;; esac
        adj="$(ds_proc_adj "$pid")"
        case "$adj" in ''|*[!0-9-]*) continue ;; esac
        [ "$adj" -ge 200 ] 2>/dev/null || continue
        printf '%s|%s|%s\n' "$pid" "$st" "$pkg" >> "$DS_FREEZE_QUEUE"
    done
    log_ds "冻结候选 $(grep -c . "$DS_FREEZE_QUEUE" 2>/dev/null) 个进程"
}

# 每轮最多冻 8 个；第一批后把组置 FROZEN，后续迁入的进程自动冻结。
# 队列空不代表冻结结束：3 分钟窗口里还有应用退到后台，由调用方补扫。
ds_freeze_step() {
    local line pid rest st pkg cur n=0
    [ "$ds_freeze_done" = "1" ] && return 0
    [ -s "$DS_FREEZE_QUEUE" ] || return 0
    while [ "$n" -lt "$DS_FREEZE_BATCH" ] && [ -s "$DS_FREEZE_QUEUE" ]; do
        line="$(head -n 1 "$DS_FREEZE_QUEUE" 2>/dev/null)"
        tail -n +2 "$DS_FREEZE_QUEUE" > "$DS_FREEZE_QUEUE.n" 2>/dev/null
        mv -f "$DS_FREEZE_QUEUE.n" "$DS_FREEZE_QUEUE" 2>/dev/null
        case "$line" in *"|"*"|"*) ;; *) continue ;; esac
        # mksh 里 | 是 ksh 模式的“或”，不能拿 ${x%%|*} 切字段。
        IFS='|' read -r pid st pkg <<EOF
$line
EOF
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        # 补扫会把已经冻住的进程再扫出来一次，按账本跳过，别重复入组。
        case "$ds_frozen" in *"$pid|"*) continue ;; esac
        cur="$(ds_proc_start "$pid")"
        [ "$cur" = "$st" ] || continue
        case " $DS_FREEZE_SKIP " in *" $pkg "*) continue ;; esac
        ds_freeze_add_pid "$pid" || continue
        ds_frozen="${ds_frozen}${pid}|${st}|${pkg}
"
        n=$((n+1))
    done
    [ -n "$ds_frozen" ] && ds_freeze_ledger_save
    if [ -n "$ds_frozen" ] && [ "$ds_freeze_frozen" != "1" ]; then
        ds_freeze_set FROZEN
        if ! ds_freeze_verify; then
            # 有 D 状态进程时整组会卡在 FREEZING。先回滚；补扫会跳过 D 状态，
            # 下一轮用剩下的进程再试。连续 3 次不行就放弃，别整晚在冻/解之间抖。
            ds_freeze_fail=$((ds_freeze_fail + 1))
            log_ds "冻结未确认（第 $ds_freeze_fail 次，可能有 D 状态进程），已回滚"
            ds_freeze_thaw_all
            if [ "$ds_freeze_fail" -ge 3 ]; then
                ds_freeze_done=1
                log_ds "连续 3 次未确认，本次深睡不再尝试冻结"
            else
                ds_freeze_scan_ts=0
            fi
            return 0
        fi
        ds_freeze_frozen=1
        log_ds "已冻结 $(printf '%s\n' "$ds_frozen" | grep -c .) 个后台进程（$ds_freeze_backend）"
    fi
}

# 媒体/新豁免应用需要跑：整组先 THAWED，移出这个包的 PID 后重新 FROZEN。
ds_freeze_thaw_pkg() {
    local pkg="$1" line keep="" thaw="" pid
    [ -n "$ds_frozen" ] || return 0
    case "$pkg" in ''|*[!A-Za-z0-9._]*) return 0 ;; esac
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        IFS='|' read -r pid _ <<EOF
$line
EOF
        case "$line" in
            *"|$pkg") thaw="$thaw $pid" ;;
            *) keep="$keep$line
" ;;
        esac
    done <<EOF
$ds_frozen
EOF
    [ -n "$thaw" ] || return 0
    ds_freeze_set THAWED
    for pid in $thaw; do
        if [ "$ds_freeze_backend" = "sigstop" ]; then
            kill -CONT "$pid" 2>/dev/null
        else
            ds_freeze_move_root "$pid"
        fi
    done
    ds_frozen="$keep"
    ds_freeze_ledger_save
    if [ -n "$ds_frozen" ]; then
        ds_freeze_set FROZEN
        ds_freeze_verify || ds_freeze_thaw_all
    else
        [ -n "$ds_freeze_group" ] && rmdir "$ds_freeze_group" 2>/dev/null
        ds_freeze_frozen=0
    fi
    log_ds "已解冻 $pkg 的后台进程（媒体/新豁免）"
}

# 7:30 一次性收口：cmd 优先，失败补发 dumpsys 兼容命令，仍失败不再重试。
ds_doze_once() {
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

# ============================ 进入 / 退出 =====================================

ds_enter() {
    local old_white pkg n=0 sms
    # 防御性再查一次（主循环也会查）：闹钟快响了就别动系统 Doze。
    if ds_alarm_soon; then
        log_ds "闹钟在 ${ALARM_GUARD_SEC} 秒内，暂不进入深睡"
        ds_state_write
        return 0
    fi
    old_white="$(ds_white_snapshot)"
    ds_added=""
    for pkg in $(echo "$DS_WHITE" | tr ',' ' '); do
        [ -n "$pkg" ] || continue
        n=$((n+1)); [ "$n" -gt "$MAX_WHITE" ] && break
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
    ds_snap_write
    # ZenCore 时序：先冻结/限流后台，7:30 才一次性 force-idle，不再 20 秒就压 Doze。
    ds_doze_sent=0
    ds_doze_checked=0
    ds_freeze_done=0
    ds_freeze_frozen=0
    ds_freeze_fail=0
    ds_frozen=""
    ds_freeze_scan_ts=$(now)
    ds_freeze_probe
    ds_freeze_group_init
    ds_freeze_build_queue
    ds_freeze_step
    ds_run_list=""
    ds_cur_base="$(ds_wakes_collect)"
    ds_wakes_save
    ds_active=1
    log_ds "已进入（保护 $n 个应用，冻结后端 $ds_freeze_backend）"
}

ds_exit() {
    local pkg
    timeout 2 dumpsys deviceidle unforce >/dev/null 2>&1
    # 先解冻再撤白名单：顺序反了会让被冻的应用在“无豁免”的空窗里等亮屏。
    ds_freeze_thaw_all
    for pkg in $ds_added; do ds_white_del "$pkg"; done
    ds_wakes_update
    [ -n "$ds_run_list" ] && echo "$ds_run_list" | awk '{print "A "$1"="$2}' \
        | atomic_write "$DS_STATS" 0644
    rm -f "$DS_SNAP" "$DS_FREEZE_QUEUE" 2>/dev/null
    ds_active=0
    ds_added=""
    ds_doze_sent=0
    ds_doze_checked=0
    log_ds "已退出（$1），冻结与白名单已还原"
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
            ds_game_checked=0
        fi
        ds_state_write
        return 0
    fi

    if [ "$charging" = "1" ]; then
        if [ "$ds_active" = "1" ]; then ds_exit "充电"; fi
        ds_screen_off_ts=0
        ds_alarm_checked=0
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
            if [ $((t - ds_alarm_checked)) -ge 60 ]; then
                ds_alarm_checked=$t
                if ds_alarm_soon; then
                    log_ds "闹钟在 ${ALARM_GUARD_SEC} 秒内，暂不进入深睡"
                    ds_state_write
                    return 0
                fi
            fi
            ds_enter
        fi
        ds_state_write
        return 0
    fi

    # ---- 已在深睡中 ----
    # 深睡期间新加入的用户白名单立即生效，不用等下一次熄屏。
    for pkg in $(echo "$DS_WHITE" | tr ',' ' '); do
        [ -n "$pkg" ] || continue
        case " $ds_added " in *" $pkg "*) continue ;; esac
        ds_white_add "$pkg" "$(ds_white_snapshot)"
        ds_freeze_thaw_pkg "$pkg"
        ds_snap_write
        log_ds "新加入白名单的应用已加白"
    done

    if [ $((t - ds_last_media)) -ge "$MEDIA_CHECK_INTERVAL" ]; then
        ds_last_media=$t
        media="$(ds_media_playing)"
        for pkg in $media; do
            case " $ds_added " in *" $pkg "*) continue ;; esac
            ds_white_add "$pkg" "$(ds_white_snapshot)"
            ds_freeze_thaw_pkg "$pkg"
            ds_snap_write
            log_ds "检测到媒体播放，已临时加白保护"
        done
    fi

    # ZenCore 时序：T0 分批冻结，3 分钟后停止新增，7:30 一次性 Doze。
    if [ "$DS_FREEZE_ENABLED" = "1" ] && [ "$ds_freeze_done" != "1" ]; then
        if [ $((t - ds_screen_off_ts)) -ge "$DS_FREEZE_WINDOW" ]; then
            : > "$DS_FREEZE_QUEUE" 2>/dev/null
            ds_freeze_done=1
            log_ds "息屏满 3 分钟：停止新增冻结"
        else
            ds_freeze_step
            # 队列清空后每分钟补扫一次，接住这几分钟里刚退到后台的进程。
            if [ ! -s "$DS_FREEZE_QUEUE" ] \
               && [ $((t - ds_freeze_scan_ts)) -ge "$DS_FREEZE_RESCAN" ]; then
                ds_freeze_scan_ts=$t
                ds_freeze_build_queue
                ds_freeze_step
            fi
        fi
    fi
    # 闹钟快响就先不收口：Doze 会把闹钟广播压到维护窗口，和“准点响”冲突。
    if [ $((t - ds_screen_off_ts)) -ge "$DS_DOZE_AT" ] && [ "$ds_doze_sent" != "1" ] \
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

ds_alive_pid() {
    local p
    [ -f "$PIDFILE" ] || return 1
    p="$(cat "$PIDFILE" 2>/dev/null)"
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    [ -d "/proc/$p" ] || return 1
    tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -q 'deep_sleep\.sh'
}

ds_shutdown() {
    [ "$ds_shut" = "1" ] && exit 0
    ds_shut=1
    [ "$ds_active" = "1" ] && ds_exit "停止"
    # 入口失败/账本残留时也兜底解冻；没有残留就是空操作。
    ds_freeze_restore_if_any
    # 退出前把 dsactive 写回 0：api.sh 停止守护后页面还会读这份 state。
    ds_state_write
    rm -f "$PIDFILE" 2>/dev/null
    exit 0
}

ds_main() {
    mkdir -p "$CONF_DIR/run" 2>/dev/null
    ds_alive_pid && exit 0
    ds_shut=0
    echo $$ >"$PIDFILE" 2>/dev/null
    trap 'ds_shutdown' 0 1 2 3 15
    ds_conf_load
    ds_freeze_probe
    ds_freeze_restore_if_any
    ds_wakes_load
    ds_snap_restore_if_any
    while true; do
        ds_quit=0
        ds_tick
        [ "$ds_quit" = "1" ] && ds_shutdown
        # 前台 sleep 时 TERM 要等它睡完才进 trap（实测 2 秒）。放到后台
        # 再 wait：wait 会被信号立刻打断，关开关时不用白等一个巡检周期。
        sleep "$POLL_INTERVAL" &
        wait $!
    done
}

case "${0##*/}" in deep_sleep.sh) ds_main ;; esac
