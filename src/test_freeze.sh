#!/system/bin/sh
# 冻结清理台架（真机）：冻结功能已经移除，这里验两件事 ——
#   1) 旧版本留下的冻结（v1 组 / 账本里的 v2 进程）会被解开；
#   2) 深睡流程本身不再冻任何进程。
# 用法： sh test_freeze.sh [模块目录]
# 脚本只动自己起的 sleep 和自己建的测试组，结束一定清理干净。
MOD=${1:-/data/adb/modules/soc_freqcap}
TMP=${TMPDIR:-/data/local/tmp}/socfreq_freeze_test
rm -rf "$TMP"; mkdir -p "$TMP/bl/p0" "$TMP/power" "$TMP/run"
printf '1\n' > "$TMP/bl/p0/brightness"

export SOCFREQ_DATA_DIR="$TMP" DS_FREEZE_GROUP_NAME=socfreq_ds_test
export DS_STATUS="$TMP/status" DS_BL="$TMP/bl" DS_POWER_DIR="$TMP/power"
. "$MOD/scripts/deep_sleep.sh"

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }

V1G=/dev/freezer/socfreq_ds_test
V2G=/sys/fs/cgroup/apps/socfreq_ds_test_b
P=""
cleanup(){
    [ -n "$V1G" ] && [ -d "$V1G" ] && {
        echo THAWED > "$V1G/freezer.state" 2>/dev/null
        for _p in $(cat "$V1G/cgroup.procs" 2>/dev/null); do
            printf '%s\n' "$_p" > /dev/freezer/cgroup.procs 2>/dev/null
        done
        rmdir "$V1G" 2>/dev/null
    }
    [ -n "$V2G" ] && [ -d "$V2G" ] && {
        echo 0 > "$V2G/cgroup.freeze" 2>/dev/null
        for _p in $(cat "$V2G/cgroup.procs" 2>/dev/null); do
            printf '%s\n' "$_p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
        done
        rmdir "$V2G" 2>/dev/null
    }
    [ -n "$P" ] && kill "$P" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

echo "== 冻结开关恒为 0 =="
ok "$DS_FREEZE_ENABLED" "0" "DS_FREEZE_ENABLED 恒为 0（功能已移除）"
ds_freeze_probe
ok "$ds_freeze_backend" "off" "开关为 0 时探测直接报 off"

echo "== 账本里的 v2 冻结：按账本解冻 =="
if mkdir -p "$V2G" 2>/dev/null && [ -w "$V2G/cgroup.freeze" ]; then
    sleep 300 & P=$!
    printf '%s\n' "$P" > "$V2G/cgroup.procs" 2>/dev/null
    echo 1 > "$V2G/cgroup.freeze" 2>/dev/null
    sleep 0.4
    ok "$(cat "$V2G/cgroup.freeze" 2>/dev/null)" "1" "先冻住（模拟旧版本残留）"
    {
      echo "backend=v2"
      echo "root=/sys/fs/cgroup"
      echo "group="
      printf '%s|%s|com.test.old|/apps/socfreq_ds_test_b\n' "$P" "$(ds_proc_start "$P")"
    } > "$TMP/run/freeze.saved"
    ds_freeze_restore_if_any
    ok "$(cat "$V2G/cgroup.freeze" 2>/dev/null)" "0" "按账本写回 0（解冻）"
    ok "$([ -f "$TMP/run/freeze.saved" ] && echo yes || echo no)" "no" "账本被清掉"
    ok "$(kill -0 "$P" 2>/dev/null && echo y || echo n)" "y" "进程还在（没被杀）"
    kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=""
else
    echo "  （本机没有可写的 v2 cgroup，跳过）"
fi

echo "== 深睡流程本身不再冻任何人 =="
sleep 300 & P=$!
export DS_FREEZE_TEST_PID="$P" DS_FREEZE_TEST_PKG=com.test.freeze
ds_conf_write
printf 'enable=1\ndoze=0\n' > "$DSLEEP_CONF"
ds_conf_load

ds_enter
ok "$ds_frozen" "" "进入深睡后冻结账是空的"
ok "$([ -n "$ds_freeze_group" ] && [ -d "$ds_freeze_group" ] && echo yes || echo no)" "no" "没有建冻结组"
if mkdir -p "$V2G" 2>/dev/null && [ -w "$V2G/cgroup.freeze" ]; then
    printf '%s\n' "$P" > "$V2G/cgroup.procs" 2>/dev/null
    ds_freeze_done=0
    ds_tick 2>/dev/null
    ok "$(cat "$V2G/cgroup.freeze" 2>/dev/null)" "0" "跑一轮 tick 也不会把它冻上"
fi
unset DS_FREEZE_TEST_PID DS_FREEZE_TEST_PKG
ds_exit "台架" >/dev/null 2>&1
kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=""

echo "== 收尾 =="
ok "$([ -f "$TMP/run/freeze.saved" ] && echo yes || echo no)" "no" "没有残留账本"

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
