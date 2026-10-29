#!/bin/bash
# 看门狗 liveness 判据的自检。
#
# 不能用 [ ! -f socfreqd.lock ] 判断"没在跑"：
# 守护被 kill -9 时不走 cleanup，锁文件会残留，看门狗就永远不拉起。
# 这里把三种状态各造一遍，确认 alive() 的判断正确。

pass=0; fail=0
ok(){ if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
     else echo "  FAIL $1: got=[$2] want=[$3]"; fail=$((fail+1)); fi; }

TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

# 从 service.sh 里抽出 alive() 的逻辑，保持与线上完全一致
cat > alive.sh <<'EOF'
alive() {
  [ -f "$LOCK" ] || return 1
  PID=$(cat "$LOCK" 2>/dev/null)
  case "$PID" in ''|*[!0-9]*) return 1 ;; esac
  [ -d "/proc/$PID" ] || return 1
  [ -r "/proc/$PID/comm" ] && grep -q socfreqd "/proc/$PID/comm" 2>/dev/null
}
EOF
# shellcheck disable=SC1091
. ./alive.sh

echo "== 三种状态 =="

# /proc 是 Linux 内核接口。在 Windows 的 Git Bash 下不存在，
# 那时只能验证纯 shell 分支（有 /proc 的判断会走 -d 的假值）。
HAVE_PROC=0
[ -r /proc/self/comm ] && HAVE_PROC=1

# 1) 守护活着
cp /bin/sleep ./socfreqd 2>/dev/null || cp "$(command -v sleep)" ./socfreqd
./socfreqd 30 &
REAL=$!
sleep 0.3
LOCK="./lock"
echo "$REAL" > "$LOCK"
if [ "$HAVE_PROC" = "1" ] && kill -0 "$REAL" 2>/dev/null; then
  if alive; then ok "运行中" 1 1; else ok "运行中" 0 1; fi
else
  # Android 的 sleep 是 toybox 多合一，改名为 socfreqd 后 argv0 不认识；
  # 造不出 comm=socfreqd 的进程时跳过，不把台架限制当成产品失败。
  echo "  skip 运行中（本机造不出 comm=socfreqd 的临时进程）"
fi

# 2) 守护已死，锁文件残留（kill -9 的真实情形）
kill -9 "$REAL" 2>/dev/null
wait "$REAL" 2>/dev/null
sleep 0.3
[ -f "$LOCK" ] && ok "kill -9 后锁文件确实残留" 1 1 || ok "kill -9 后锁文件确实残留" 0 1
if alive; then ok "残留锁应判为已死" 1 0; else ok "残留锁应判为已死" 0 0; fi

# 3) pid 被系统回收给别的进程（comm 不匹配）
if [ "$HAVE_PROC" = "1" ]; then
  sleep 60 &
  OTHER=$!
  echo "$OTHER" > "$LOCK"
  if alive; then ok "pid 存在但不是守护" 1 0; else ok "pid 存在但不是守护" 0 0; fi
  kill -9 "$OTHER" 2>/dev/null; wait "$OTHER" 2>/dev/null
else
  echo "  skip pid 回收（无 /proc）"
fi

# 4) 锁文件内容是垃圾
echo "not-a-pid" > "$LOCK"
if alive; then ok "垃圾内容判为已死" 1 0; else ok "垃圾内容判为已死" 0 0; fi

# 5) 锁文件为空
: > "$LOCK"
if alive; then ok "空锁文件判为已死" 1 0; else ok "空锁文件判为已死" 0 0; fi

# 6) 没有锁文件
rm -f "$LOCK"
if alive; then ok "无锁文件判为已死" 1 0; else ok "无锁文件判为已死" 0 0; fi

# 7) pid=1 特判（不能把 init 当成守护）
echo "1" > "$LOCK"
if alive; then ok "pid=1 不算守护" 1 0; else ok "pid=1 不算守护" 0 0; fi

# 8) 心跳过期：进程还在但主循环卡死时，alive 会一直说"活着"，
#    所以看门狗还要看 run/heartbeat 的 mtime。
cat > stale.sh <<'EOF'
stale() {
  [ -f "$HB" ] || return 0
  _now=$(date +%s 2>/dev/null)
  _mt=$(stat -c %Y "$HB" 2>/dev/null | head -n 1)
  case "$_now" in ''|*[!0-9]*) return 0 ;; esac
  case "$_mt" in ''|*[!0-9]*) return 0 ;; esac
  [ "$((_now - _mt))" -gt 90 ]
}
EOF
# shellcheck disable=SC1091
. ./stale.sh
mkdir -p run
HB=run/heartbeat
rm -f "$HB"
if stale; then ok "没有心跳 → 过期" 1 1; else ok "没有心跳 → 过期" 0 1; fi
: > "$HB"
if stale; then ok "刚写的心跳 → 未过期" 1 0; else ok "刚写的心跳 → 未过期" 0 0; fi
touch -t 200001010000 "$HB"
if stale; then ok "20 年前的心跳 → 过期" 1 1; else ok "20 年前的心跳 → 过期" 0 1; fi

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"