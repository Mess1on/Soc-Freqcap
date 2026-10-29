#!/bin/bash
# 热安装脚本台架：不启动真守护，只验"停旧 / 起新 / 暂停保持 / 深睡按开关拉起"。
#
# 用法： bash src/test_hotinstall.sh
SRC=$(cd "$(dirname "$0")" && pwd) || exit 1
MOD=$(dirname "$SRC") || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
      else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }

mkdir -p "$T/mod/scripts" "$T/data"
cp "$MOD/scripts/hotinstall.sh" "$T/mod/scripts/hotinstall.sh"
cat > "$T/mod/socfreqd" <<'EOF'
#!/bin/sh
echo "$@" >> "$HOT_LOG"
exit 0
EOF
cat > "$T/mod/scripts/deep_sleep.sh" <<'EOF'
#!/bin/sh
echo ds >> "$HOT_DS_LOG"
EOF
chmod +x "$T/mod/socfreqd" "$T/mod/scripts/hotinstall.sh" "$T/mod/scripts/deep_sleep.sh"

export HOT_LOG="$T/calls" HOT_DS_LOG="$T/ds" SOCFREQ_DATA_DIR="$T/data"
printf 'enable=1\n' > "$T/data/dsleep.conf"

runhot() {
  : > "$T/calls"; : > "$T/ds"
  sh "$T/mod/scripts/hotinstall.sh" > "$T/out" 2>&1
  sleep 0.4
}

echo "== 正常热安装 =="
runhot
ok "$(cat "$T/calls")" "restart" "没有 pause 时调用 restart（等旧实例退出再起新版）"
ok "$(cat "$T/ds" 2>/dev/null)" "ds" "深睡开关开着时按新脚本重新拉起"
ok "$(grep -c '热安装完成' "$T/out")" "1" "安装输出里说明已热安装完成"

echo "== 暂停状态 =="
touch "$T/data/pause"
runhot
ok "$(cat "$T/calls")" "stop" "暂停时只 stop 旧实例，不 restart"
ok "$([ -s "$T/ds" ] && echo y || echo n)" "n" "暂停时不拉起深睡"

echo "== 深睡关着 =="
rm -f "$T/data/pause"
printf 'enable=0\n' > "$T/data/dsleep.conf"
runhot
ok "$(cat "$T/calls")" "restart" "没暂停仍会重启主守护"
ok "$([ -s "$T/ds" ] && echo y || echo n)" "n" "深睡关着不拉起"

echo "== 卸载标记 =="
touch "$T/mod/remove"
runhot
ok "$(cat "$T/calls")" "" "模块标记 remove 时什么都不启动"
ok "$([ -s "$T/ds" ] && echo y || echo n)" "n" "remove 标记下也不拉起深睡"
ok "$(grep -c '跳过启动' "$T/out")" "1" "输出说明因为已标记卸载"
rm -f "$T/mod/remove"

echo
if [ "$fail" -eq 0 ]; then echo "热安装台架：全部通过 ($pass)"; else echo "热安装台架：$fail 项失败 / $pass 项通过"; fi
exit "$fail"
