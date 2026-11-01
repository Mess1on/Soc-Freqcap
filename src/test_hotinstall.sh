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

echo "== pid 文件守卫：不能把新实例的 pid 抹掉 =="
# 真机上的竞态：hotinstall 停旧深睡的这几秒里，service.sh 的 60 秒轮询可能
# 已经拉起了新深睡、并由它重写过 run/dsleep.pid。旧代码无条件 rm 这个文件，
# 抹掉的是**新实例**的 pid；之后看门狗每轮都判"没在跑"→ 再起一个抢不到运行锁
# 的进程，每个都在 dsleep.log 里刷一行（这台机上刷了 5692 行、一天涨 430KB）。
mkdir -p "$T/data/run" "$T/fakeds"
# 假深睡：收到 TERM 时先把 pid 文件改写成"新实例的 pid"再退出，
# 把真机上那几秒的窗口压成确定性的先后顺序。
# 文件名必须叫 deep_sleep.sh —— hotinstall 是按 cmdline 里有没有这个名字
# 来认深睡进程的，换个名字它就认不出来、根本不会去停。
cat > "$T/fakeds/deep_sleep.sh" <<'EOF'
#!/bin/sh
trap 'printf "%s\n" "$FAKE_NEW_PID" > "$FAKE_PID_FILE"; exit 0' TERM
while true; do sleep 0.2; done
EOF
chmod +x "$T/fakeds/deep_sleep.sh"

# 424242 只当"一个已经不存在的 pid"用：hotinstall 认深睡还要核 cmdline，
# 这个号即便真被别的进程占着也不会被误认成深睡。
FAKE_NEW_PID=424242
export FAKE_NEW_PID FAKE_PID_FILE="$T/data/run/dsleep.pid"
printf 'enable=1\n' > "$T/data/dsleep.conf"

sh "$T/fakeds/deep_sleep.sh" &
_old=$!
printf '%s\n' "$_old" > "$FAKE_PID_FILE"
sleep 0.3
runhot
ok "$(cat "$FAKE_PID_FILE" 2>/dev/null)" "$FAKE_NEW_PID" \
   "旧实例退出前重写过 pid 文件：新 pid 保住了，没被无条件 rm 抹掉"

# 反过来：pid 文件里还是刚被停掉那个（或者本来就是一条陈旧记录），就该删。
# 留着它，下一轮看门狗又要按"pid 文件在、进程不在"白起一个。
printf '%s\n' "$FAKE_NEW_PID" > "$FAKE_PID_FILE"
runhot
ok "$([ -f "$FAKE_PID_FILE" ] && echo y || echo n)" "n" \
   "没人改写过 pid 文件：按陈旧记录删掉"

echo
if [ "$fail" -eq 0 ]; then echo "热安装台架：全部通过 ($pass)"; else echo "热安装台架：$fail 项失败 / $pass 项通过"; fi
exit "$fail"
