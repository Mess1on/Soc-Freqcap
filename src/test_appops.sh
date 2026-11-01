#!/system/bin/sh
# 真机验证 appops 唤醒锁账本：只在微信上做一次 set→get→restore，结束必须回到原值。
# 用法（root）： sh /data/local/tmp/test_appops.sh [模块目录]
MOD=${1:-/data/local/tmp/sfmod}
TMP=/data/local/tmp/socfreq_appops_test
rm -rf "$TMP"; mkdir -p "$TMP/run"
export SOCFREQ_DATA_DIR="$TMP" DS_FREEZE=0 DS_APPOPS=1
export DS_POWER_DIR="$TMP/power" DS_STATUS="$TMP/status"
. "$MOD/scripts/deep_sleep.sh"

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }

trap 'ds_ops_restore_all >/dev/null 2>&1; rm -rf "$TMP"' EXIT
trap 'exit 1' 1 2 3 15

OLD="$(ds_ops_get com.tencent.mm)"
echo "  old=$OLD"
if [ "$OLD" = "allow" ] || [ "$OLD" = "default" ]; then
    ds_ops_apply com.tencent.mm
    ok "$(ds_ops_get com.tencent.mm)" "ignore" "WAKE_LOCK 设为 ignore"
    ok "$(grep -c '^com.tencent.mm|' "$DS_OPS_LEDGER" 2>/dev/null)" "1" "唤醒锁账本已记录"
    ds_ops_restore_all
    ok "$(ds_ops_get com.tencent.mm)" "$OLD" "唤醒锁恢复成原值"
    ok "$([ -f "$DS_OPS_LEDGER" ] && echo yes || echo no)" "no" "唤醒锁账本已删除"
else
    echo "  ..   当前不是 allow/default（$OLD），跳过 set 测试，避免破坏现状"
fi

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
