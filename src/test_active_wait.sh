#!/system/bin/sh
# 真机验证：深睡激活后不再 2 秒轮询，亮屏事件能立刻退出。
# 只操作临时数据目录和假电源节点，不冻结真实应用、不碰 live 模块。
# 用法（root）： sh /data/local/tmp/test_active_wait.sh [模块目录]
MOD=${1:-/data/local/tmp/sfmod}
TMP=/data/local/tmp/socfreq_active_test
RUN=$TMP/runner.sh
rm -rf "$TMP"; mkdir -p "$TMP/power" "$TMP/run"
touch "$TMP/run/policy_v2" "$TMP/run/policy_v3"   # 台架不跑真实策略迁移，别动系统的 Doze 白名单
printf 'state=normal\ngame_pkg=-\nfg=com.android.launcher\n' > "$TMP/status"
printf 'enable=1\n' > "$TMP/dsleep.conf"

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }

cat > "$RUN" <<EOF
#!/system/bin/sh
export SOCFREQ_DATA_DIR="$TMP" DS_FREEZE=0
export DS_POWER_DIR="$TMP/power" DS_STATUS="$TMP/status"
. "$MOD/scripts/deep_sleep.sh"
SCREEN_OFF_DELAY=0
ALARM_GUARD_SEC=0
ds_main
EOF
chmod 0755 "$RUN"

cleanup(){
    [ -f "$TMP/run/dsleep.pid" ] && kill "$(cat "$TMP/run/dsleep.pid")" 2>/dev/null
    sleep 1
    input keyevent 223 >/dev/null 2>&1
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 1' 1 2 3 15

input keyevent 223 >/dev/null 2>&1   # 先确保熄屏
sleep 2
setsid sh "$RUN" >/dev/null 2>&1 < /dev/null &
i=0
while [ "$i" -lt 40 ] && ! grep -q '已进入' "$TMP/dsleep.log" 2>/dev/null; do
    sleep 0.2; i=$((i + 1))
done
ok "$([ "$(grep -c '已进入' "$TMP/dsleep.log" 2>/dev/null)" -ge 1 ] && echo y || echo n)" "y" \
   "熄屏后进入深睡"
ok "$([ "$(grep -c 'logcat 事件不可用' "$TMP/dsleep.log" 2>/dev/null)" -eq 0 ] && echo y || echo n)" "y" \
   "事件源可用（没有退回定时轮询）"
sleep 3   # 让守护真正进到事件等待里，再测稳态唤醒延迟

T0=$(date +%s)
input keyevent 224 >/dev/null 2>&1       # 亮屏
i=0
while [ "$i" -lt 40 ] && ! grep -q '已退出（亮屏）' "$TMP/dsleep.log" 2>/dev/null; do
    sleep 0.2; i=$((i + 1))
done
T1=$(date +%s)
ok "$([ "$(grep -c '已退出（亮屏）' "$TMP/dsleep.log" 2>/dev/null)" -ge 1 ] && echo y || echo n)" "y" \
   "亮屏事件触发退出"
ok "$([ $((T1 - T0)) -le 3 ] && echo y || echo n)" "y" \
   "亮屏到退出不超过 3 秒（实际 $((T1 - T0)) 秒）"

echo "  --- dsleep.log ---"
sed 's/^/    /' "$TMP/dsleep.log" 2>/dev/null
echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"
