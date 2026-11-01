#!/system/bin/sh
# 冻结台架（真机）：只冻自己起的 sleep，验后端探测、建组、分批、账本、
# 单包解冻和崩溃后按账本还原；最后只做一次真实候选扫描，不冻任何真实应用。
#
# 用法（手机 root shell）： sh /data/local/tmp/test_freeze.sh [模块目录]
# 脚本结束一定 THAWED、删组、杀睡眠进程；不碰 /data/adb/soc_freqcap。
MOD=${1:-/data/adb/modules/soc_freqcap}
TMP=${TMPDIR:-/data/local/tmp}/socfreq_freeze_test
rm -rf "$TMP"; mkdir -p "$TMP/bl/p0" "$TMP/power" "$TMP/run"
printf '1\n' > "$TMP/bl/p0/brightness"

export SOCFREQ_DATA_DIR="$TMP" DS_FREEZE=1 DS_FREEZE_GROUP_NAME=socfreq_ds_test
export DS_STATUS="$TMP/status" DS_BL="$TMP/bl" DS_POWER_DIR="$TMP/power"
. "$MOD/scripts/deep_sleep.sh"

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }
has(){ grep -qx -- "$2" "$1" 2>/dev/null && echo yes || echo no; }

# deep_sleep.sh 里没有 ds_proc_state / ds_proc_adj：读 state 和 oom_score_adj 的
# 逻辑早先合进了 ds_freeze_build_queue 那段 awk，函数名就没了。台架按同一个口径
# 自己从 /proc 读，否则每条用到它们的断言都报 "inaccessible or not found"，
# 一串假 FAIL 会把真回归盖住。
# state 必须走 ds_proc_stat_after_comm：comm 里可能带空格和括号，直接 awk '{print $3}'
# 会切错列；先剥掉 "pid (comm) " 之后，第 1 列才是状态。
ds_proc_state(){ ds_proc_stat_after_comm "$1" 2>/dev/null | awk '{print $1}'; }
ds_proc_adj(){ cat "/proc/$1/oom_score_adj" 2>/dev/null; }

# v2 下 root 进程待在 v2 的 "/" 根组里，产品代码正确地拒绝冻它（冻了整机都停）。
# 真实第三方应用都在 /apps/uid_X/pid_Y 各自的组里，所以台架也得把 dummy 放进
# 一个 apps 子组 —— 否则测的是一个产品永远不会去冻的位置，测出来的红绿都没意义。
V2G=""
fz_place(){
    case "$ds_freeze_backend" in
        v2)
            if [ -z "$V2G" ]; then
                V2G=/sys/fs/cgroup/apps/socfreq_ds_test_b
                rmdir "$V2G" 2>/dev/null
                mkdir -p "$V2G" 2>/dev/null
            fi
            [ -d "$V2G" ] && printf '%s\n' "$1" > "$V2G/cgroup.procs" 2>/dev/null
            ;;
    esac
}
fz_frozen(){
    case "$ds_freeze_backend" in
        v2) ds_v2_is_frozen "$(ds_v2_path_of "$1")" && echo y || echo n ;;
        v1) [ "$(cat "$ds_freeze_group/freezer.state" 2>/dev/null)" = "FROZEN" ] && echo y || echo n ;;
        *)  echo n ;;
    esac
}
fz_group_left(){
    case "$ds_freeze_backend" in
        v1) [ -d "$ds_freeze_group" ] && echo yes || echo no ;;
        *)  echo no ;;
    esac
}

P=""
cleanup(){
    ds_freeze_thaw_all >/dev/null 2>&1
    ds_snap_restore_if_any >/dev/null 2>&1
    [ -n "$P" ] && kill "$P" 2>/dev/null
    if [ -n "$V2G" ] && [ -d "$V2G" ]; then
        echo 0 > "$V2G/cgroup.freeze" 2>/dev/null
        for _p in $(cat "$V2G/cgroup.procs" 2>/dev/null); do
            printf '%s\n' "$_p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
        done
        rmdir "$V2G" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 1' 1 2 3 15

echo "== 后端探测 =="
ds_freeze_probe
echo "  backend=$ds_freeze_backend root=$ds_freeze_root"
case "$ds_freeze_backend" in
    v1|v2) ok y y "拿到 cgroup freezer（$ds_freeze_backend）" ;;
    sigstop) echo "  注意：只有 SIGSTOP 后端，下面跳过 cgroup 断言" ;;
    *) ok "$ds_freeze_backend" "v1/v2" "应该能探测到 freezer" ;;
esac

echo "== 用 dummy 进程走完整冻结 =="
sleep 300 &
P=$!
DS_FREEZE_TEST_PID=$P
DS_FREEZE_TEST_PKG=com.test.freeze
export DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
fz_place "$P"

ds_freeze_group_init
ok "$([ "$ds_freeze_frozen" = "0" ] && echo y || echo n)" "y" "开始前未冻结"
ds_freeze_build_queue
ok "$(grep -c . "$DS_FREEZE_QUEUE")" "1" "候选队列里有 dummy 进程"
ok "$(cat "$DS_FREEZE_QUEUE")" "$P|$(ds_proc_start "$P")|com.test.freeze" \
   "队列记下 pid|starttime|包名"
ds_freeze_step
ok "$ds_freeze_frozen" "1" "第一批后进入冻结态"
ok "$([ -f "$DS_FREEZE_LEDGER" ] && echo y || echo n)" "y" "账本已落盘"
ok "$(grep -c "^$P|" "$DS_FREEZE_LEDGER")" "1" "账本里有 dummy 的 pid"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "冻结后进程还在（没被杀）"
ok "$(fz_frozen "$P")" "y" "dummy 真的被冻住"
case "$ds_freeze_backend" in
    v1) ok "$(cat "$ds_freeze_group/freezer.state")" "FROZEN" "freezer.state=FROZEN" ;;
    v2) ok "$(awk -F'|' -v p="$P" '$1==p{print $4}' "$DS_FREEZE_LEDGER")" \
            "$(ds_v2_path_of "$P")" "账本记下了它自己的 cgroup 路径" ;;
esac

echo "== 单包解冻（媒体/新豁免走这条路）=="
ds_freeze_thaw_pkg com.test.freeze
ok "$ds_frozen" "" "解冻后不再挂账"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "解冻后进程还在"
ok "$(fz_frozen "$P")" "n" "dummy 真的解冻了"
ok "$(fz_group_left)" "no" "没有残留组"

echo "== 崩溃后按账本还原 =="
ds_freeze_group_init
ds_freeze_build_queue
ds_freeze_step
ok "$(fz_frozen "$P")" "y" "再次冻住 dummy"
# 新进程（模拟守护被 kill 后重启）：内存状态是空的，只能靠账本
( unset DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
  . "$MOD/scripts/deep_sleep.sh"
  ds_freeze_restore_if_any ) >/dev/null 2>&1
ok "$([ -f "$DS_FREEZE_LEDGER" ] && echo yes || echo no)" "no" "账本已删除"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "还原后进程还在"
ok "$(fz_group_left)" "no" "残留组已清理"
ok "$(ds_v2_is_frozen "$(ds_v2_path_of "$P")" && echo y || echo n)" "n" \
   "v2 下按账本也真的解冻了"
ds_frozen=""; ds_freeze_frozen=0
kill "$P" 2>/dev/null; P=""

echo "== 账本 pid 前缀撞车：不能把没冻过的进程当成已冻 =="
# 真会撞的情形：账本里有 1124，补扫又扫出 124。
# 拿 *"$pid|"* 在整个账本里做子串匹配，"124|" 会命中 "1124|" 的尾巴，
# 于是 124 被跳过、永远不入组，用户看到的是"后台应用还在跑"。
sleep 300 &
P=$!
DS_FREEZE_TEST_PID=$P
DS_FREEZE_TEST_PKG=com.test.freeze
export DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
fz_place "$P"
ds_freeze_group_init
ds_freeze_build_queue
ok "$(grep -c "^$P|" "$DS_FREEZE_QUEUE" 2>/dev/null)" "1" "队列里是候选 pid 本身"
# 先塞一条 pid 正好是"9 + 候选 pid"的假账，制造前缀撞车。
# 账本是按行分隔的记录，假账也必须带行尾换行（产品里就是 ds_frozen="${ds_frozen}...
# " 这种写法）；漏了换行会让下一条记录粘在同一行末尾，断言会因为"格式不对"
# 而失败，那就不是在验前缀撞车了。
ds_frozen="9${P}|1|com.fake.prefix
"
ds_freeze_step
ok "$(printf '%s\n' "$ds_frozen" | grep -c "^$P|")" "1" \
   "候选被真的记进账本（没有被前缀撞车跳过）"
ok "$(grep -c "^$P|" "$DS_FREEZE_LEDGER" 2>/dev/null)" "1" "账本文件里也有它"
ok "$(printf '%s\n' "$ds_frozen" | grep -c "^9${P}|")" "1" "原来那条假账没被顶掉"
ds_freeze_thaw_all 2>/dev/null
ds_frozen=""; ds_freeze_frozen=0
kill "$P" 2>/dev/null; P=""

echo "== 进入/退出整链（只冻 dummy，不动真实应用）=="
sleep 300 &
P=$!
DS_FREEZE_TEST_PID=$P
DS_FREEZE_TEST_PKG=com.test.freeze
export DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
fz_place "$P"
ALARM_GUARD_SEC=0
ds_conf_write
printf 'enable=1\n' > "$DSLEEP_CONF"
ds_conf_load
ds_enter
ds_state_write
ok "$ds_active" "1" "ds_enter 进入深睡"
ok "$ds_freeze_frozen" "1" "dummy 被冻住"
ok "$([ -f "$DS_SNAP" ] && echo y || echo n)" "y" "进入时写了快照"
ok "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" "1" "state 报深睡中"
ok "$(fz_frozen "$P")" "y" "进入后 dummy 真的被冻住"
case "$ds_freeze_backend" in
    v1) ok "$(cat "$ds_freeze_group/freezer.state")" "FROZEN" "组状态 FROZEN" ;;
esac
ds_exit "台架"
ds_state_write
sleep 1
ok "$ds_active" "0" "ds_exit 退出"
ok "$(fz_group_left)" "no" "退出后没有残留组"
ok "$(fz_frozen "$P")" "n" "退出后 dummy 被解冻"
ok "$(ds_proc_state "$P")" "S" "dummy 恢复运行（S）"
ok "$([ -f "$DS_SNAP" ] && echo yes || echo no)" "no" "退出后快照已删"
ok "$(awk -F= '$1=="dsactive"{print $2}' "$STATE_FILE")" "0" "state 报已退出"
ok "$ds_added" "" "临时白名单已清空（短信应用若加过也已撤）"
kill "$P" 2>/dev/null; P=""
printf 'enable=0\n' > "$DSLEEP_CONF"
ds_conf_load

echo "== 真实候选扫描（只看不冻）=="
unset DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
ds_freeze_build_queue
N=$(grep -c . "$DS_FREEZE_QUEUE" 2>/dev/null)
echo "  候选 $N 个："
sed -n '1,12p' "$DS_FREEZE_QUEUE" 2>/dev/null
if [ "${N:-0}" -gt 0 ]; then
    ok y y "能扫出第三方后台进程"
    BAD=0
    while IFS='|' read -r pid st pkg; do
        [ -n "$pid" ] || continue
        adj="$(ds_proc_adj "$pid")"
        [ "${adj:-0}" -ge 200 ] 2>/dev/null || BAD=$((BAD+1))
        case " $DS_FREEZE_SKIP " in *" $pkg "*) BAD=$((BAD+1)) ;; esac
    done < "$DS_FREEZE_QUEUE"
    ok "$BAD" "0" "候选都是 adj>=200 且不在排除名单"
else
    echo "  （这台机当前没有可冻的第三方后台进程，扫描结果按通过记）"
    ok y y "候选为空时不报错"
fi
ok "$(has "$DS_FREEZE_QUEUE" com.android.systemui)" "no" "系统 UI 不在候选里"
: > "$DS_FREEZE_QUEUE"

echo "== 识别系统冻结器：已经被别处冻住的进程不再重复入组 =="
case "$ds_freeze_backend" in
  v1)
    # 用假目录验纯逻辑，不碰真的 /dev/freezer/frozen
    FAKE="$TMP/fakeroot"; mkdir -p "$FAKE/frozen"
    printf '424242\n' > "$FAKE/frozen/cgroup.procs"
    _save_root="$ds_freeze_root"
    ds_freeze_root="$FAKE"
    ok "$(ds_freeze_frozen_elsewhere 424242 && echo y || echo n)" "y" "系统冻结组里的 pid 能认出来"
    ok "$(ds_freeze_frozen_elsewhere 424243 && echo y || echo n)" "n" "不在系统冻结组里的 pid 不误判"
    ds_freeze_root="$_save_root"
    ;;
  v2)
    # v2 上"已被系统冻住"就是进程自己的 cgroup.events 里 frozen 1
    G0=/sys/fs/cgroup/apps/socfreq_ds_test_f
    rmdir "$G0" 2>/dev/null; mkdir -p "$G0" 2>/dev/null
    sleep 300 & P0=$!
    printf '%s\n' "$P0" > "$G0/cgroup.procs" 2>/dev/null
    ok "$(ds_freeze_frozen_elsewhere "$P0" && echo y || echo n)" "n" "没冻的不误判"
    echo 1 > "$G0/cgroup.freeze" 2>/dev/null
    sleep 0.4
    ok "$(ds_freeze_frozen_elsewhere "$P0" && echo y || echo n)" "y" "系统已冻住的进程能认出来"
    echo 0 > "$G0/cgroup.freeze" 2>/dev/null
    sleep 0.4
    printf '%s\n' "$P0" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    kill "$P0" 2>/dev/null; wait "$P0" 2>/dev/null
    rmdir "$G0" 2>/dev/null
    ;;
esac

echo "== 解冻只动自己的组：系统管着的进程不能被抢回来 =="
# ColorOS 的 HANS 和我们共用同一个 cgroup 层级。它管着的进程，我们解冻时
# 如果不先看"这条记录还是不是我们的"，会替它解冻，它下一轮又冻回去 ——
# 待机时两边对着搬正是最费电的形态。
case "$ds_freeze_backend" in
  v1)
    sleep 300 & P1=$!
    sleep 300 & P2=$!
    OTHER="$ds_freeze_root/socfreq_ds_other"
    rmdir "$OTHER" 2>/dev/null; mkdir -p "$OTHER" 2>/dev/null
    rm -f "$DS_FREEZE_LEDGER" 2>/dev/null
    ds_freeze_group_init
    ds_freeze_frozen=0
    printf '%s\n' "$P1" > "$ds_freeze_group/cgroup.procs" 2>/dev/null
    printf '%s\n' "$P2" > "$OTHER/cgroup.procs" 2>/dev/null
    ok "$(ds_pid_in_group "$P1" "$ds_freeze_group" && echo y || echo n)" "y" "P1 已进我们自己的组"
    ok "$(ds_pid_in_group "$P2" "$OTHER" && echo y || echo n)" "y" "P2 在别人（系统）的组里"
    ds_frozen="${P1}|$(ds_proc_start "$P1")|com.test.mine
${P2}|$(ds_proc_start "$P2")|com.test.other
"
    ds_freeze_thaw_all >/dev/null 2>&1
    ok "$(ds_pid_in_group "$P1" "$ds_freeze_group" && echo y || echo n)" "n" "自己的 pid 被移出组（正常解冻）"
    ok "$(ds_pid_in_group "$P2" "$OTHER" && echo y || echo n)" "y" "系统管着的 pid 原地不动（没被抢回来）"
    for p in "$P1" "$P2"; do printf '%s\n' "$p" > "$ds_freeze_root/cgroup.procs" 2>/dev/null; done
    kill "$P1" "$P2" 2>/dev/null
    wait "$P1" "$P2" 2>/dev/null
    rmdir "$OTHER" 2>/dev/null
    ;;
  v2)
    # v2 原地冻：账本里只该写我们记下的那几个 cgroup。
    # 另一个"别人管着"的 cgroup 冻着不动，验证我们没去碰它。
    G1=/sys/fs/cgroup/apps/socfreq_ds_test_c1
    G2=/sys/fs/cgroup/apps/socfreq_ds_test_c2
    rmdir "$G1" "$G2" 2>/dev/null
    mkdir -p "$G1" "$G2" 2>/dev/null
    sleep 300 & P1=$!
    sleep 300 & P2=$!
    printf '%s\n' "$P1" > "$G1/cgroup.procs" 2>/dev/null
    printf '%s\n' "$P2" > "$G2/cgroup.procs" 2>/dev/null
    echo 1 > "$G1/cgroup.freeze" 2>/dev/null
    echo 1 > "$G2/cgroup.freeze" 2>/dev/null
    sleep 0.4
    rm -f "$DS_FREEZE_LEDGER" 2>/dev/null
    ok "$(ds_v2_is_frozen /apps/socfreq_ds_test_c1 && echo y || echo n)" "y" "P1 冻住"
    ok "$(ds_v2_is_frozen /apps/socfreq_ds_test_c2 && echo y || echo n)" "y" "P2 冻住"
    ds_frozen="${P1}|$(ds_proc_start "$P1")|com.test.mine|/apps/socfreq_ds_test_c1
"
    ds_freeze_thaw_all >/dev/null 2>&1
    ok "$(ds_v2_is_frozen /apps/socfreq_ds_test_c1 && echo y || echo n)" "n" "账本里的 pid 被解冻"
    ok "$(ds_v2_is_frozen /apps/socfreq_ds_test_c2 && echo y || echo n)" "y" "没记账的进程原地不动（没被抢回来）"
    for g in "$G1" "$G2"; do
        echo 0 > "$g/cgroup.freeze" 2>/dev/null
        for p in $(cat "$g/cgroup.procs" 2>/dev/null); do
            printf '%s\n' "$p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
        done
        rmdir "$g" 2>/dev/null
    done
    kill "$P1" "$P2" 2>/dev/null
    wait "$P1" "$P2" 2>/dev/null
    ;;
  *) echo "  （后端是 $ds_freeze_backend，跳过）" ;;
esac

echo "== 收尾 =="
ds_freeze_restore_if_any
ok "$(fz_group_left)" "no" "没有残留冻结组"

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
