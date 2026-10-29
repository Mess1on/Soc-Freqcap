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

P=""
cleanup(){
    ds_freeze_thaw_all >/dev/null 2>&1
    ds_snap_restore_if_any >/dev/null 2>&1
    [ -n "$P" ] && kill "$P" 2>/dev/null
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

ds_freeze_group_init
ok "$([ "$ds_freeze_frozen" = "0" ] && echo y || echo n)" "y" "建组后未冻结"
ds_freeze_build_queue
ok "$(grep -c . "$DS_FREEZE_QUEUE")" "1" "候选队列里有 dummy 进程"
ok "$(cat "$DS_FREEZE_QUEUE")" "$P|$(ds_proc_start "$P")|com.test.freeze" \
   "队列记下 pid|starttime|包名"
ds_freeze_step
ok "$ds_freeze_frozen" "1" "第一批后组状态为 FROZEN"
ok "$([ -f "$DS_FREEZE_LEDGER" ] && echo y || echo n)" "y" "账本已落盘"
ok "$(grep -c "^$P|" "$DS_FREEZE_LEDGER")" "1" "账本里有 dummy 的 pid"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "冻结后进程还在（没被杀）"
case "$ds_freeze_backend" in
    v1)
        ok "$(cat "$ds_freeze_group/freezer.state")" "FROZEN" "freezer.state=FROZEN"
        case "$(ds_proc_state "$P")" in
            D|T) ok y y "进程状态变成 $(ds_proc_state "$P")（停住了）" ;;
            *) ok "$(ds_proc_state "$P")" "D/T" "冻结后进程应该是停止态" ;;
        esac ;;
    v2)
        ok "$(grep -c '^frozen 1' "$ds_freeze_group/cgroup.events")" "1" "cgroup.events frozen=1" ;;
esac

echo "== 单包解冻（媒体/新豁免走这条路）=="
ds_freeze_thaw_pkg com.test.freeze
ok "$ds_frozen" "" "解冻后不再挂账"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "解冻后进程还在"
case "$ds_freeze_backend" in
    v1|v2)
        ok "$([ -d "$ds_freeze_group" ] && echo yes || echo no)" "no" "组里没进程后删掉" ;;
esac

echo "== 崩溃后按账本还原 =="
ds_freeze_group_init
ds_freeze_build_queue
ds_freeze_step
ok "$ds_freeze_frozen" "1" "再次冻住 dummy"
# 新进程（模拟守护被 kill 后重启）：内存状态是空的，只能靠账本
( unset DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
  . "$MOD/scripts/deep_sleep.sh"
  ds_freeze_restore_if_any ) >/dev/null 2>&1
ok "$([ -f "$DS_FREEZE_LEDGER" ] && echo yes || echo no)" "no" "账本已删除"
ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "还原后进程还在"
case "$ds_freeze_backend" in
    v1|v2)
        ok "$([ -d "$ds_freeze_group" ] && echo yes || echo no)" "no" "残留组已清理" ;;
esac
ds_frozen=""; ds_freeze_frozen=0
kill "$P" 2>/dev/null; P=""

echo "== 进入/退出整链（只冻 dummy，不动真实应用）=="
sleep 300 &
P=$!
DS_FREEZE_TEST_PID=$P
DS_FREEZE_TEST_PKG=com.test.freeze
export DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
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
case "$ds_freeze_backend" in
    v1) ok "$(cat "$ds_freeze_group/freezer.state")" "FROZEN" "组状态 FROZEN" ;;
    v2) ok "$(grep -c '^frozen 1' "$ds_freeze_group/cgroup.events")" "1" "cgroup.events frozen=1" ;;
esac
ok "$(ds_proc_state "$P")" "D" "dummy 停住（D）"
ds_exit "台架"
ds_state_write
sleep 1
ok "$ds_active" "0" "ds_exit 退出"
ok "$([ -d "$ds_freeze_group" ] && echo yes || echo no)" "no" "退出后组已删除"
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

echo "== 收尾 =="
ds_freeze_restore_if_any
ok "$([ -d "$ds_freeze_group" ] && echo yes || echo no)" "no" "没有残留冻结组"

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
