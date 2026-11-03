#!/system/bin/sh
# 真机验证唤醒锁账本的"还原"：冻结移除后只剩这一条路径了
# （升级上来的机器可能有旧账本，必须能按账本把 WAKE_LOCK 写回去）。
# 只读当前值 + 用同一个值写一份假账 + 跑还原，全程不改真实状态。
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

trap 'rm -rf "$TMP"' EXIT
trap 'exit 1' 1 2 3 15

echo "== 冻结移除后不应再有\"设 ignore\"的入口 =="
ok "$(command -v ds_ops_apply 2>/dev/null || echo none)" "none" "ds_ops_apply 已随冻结删除"
ok "$(grep -c 'WAKE_LOCK ignore' "$MOD/scripts/deep_sleep.sh" 2>/dev/null)" "0" \
   "脚本里不再有 WAKE_LOCK ignore"

echo "== 旧账本能还原（用当前值写假账，不改真实状态）=="
P=com.tencent.mm
OLD="$(ds_ops_get "$P")"
echo "  old=$OLD"
case "$OLD" in
  allow|deny|ignore|default)
    printf '%s|%s\n' "$P" "$OLD" > "$DS_OPS_LEDGER"
    ds_ops_restore_all >/dev/null 2>&1
    ok "$(ds_ops_get "$P")" "$OLD" "按账本写回后仍是原值"
    ok "$([ -f "$DS_OPS_LEDGER" ] && echo yes || echo no)" "no" "账本已删除"
    ;;
  *)
    echo "  ..   读不到当前值（$OLD），跳过（不猜、不改）"
    ;;
esac

echo "== 非法账本不执行（防注入）=="
printf 'not-a-pkg|allow\n' > "$DS_OPS_LEDGER"
ds_ops_restore_all >/dev/null 2>&1
ok "$([ -f "$DS_OPS_LEDGER" ] && echo yes || echo no)" "yes" "非法包名的账本保留、不动系统"
rm -f "$DS_OPS_LEDGER"

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
