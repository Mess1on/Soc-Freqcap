#!/system/bin/sh
# Soc智能限频 · 启动入口
MODDIR=${0%/*}
DAEMON="$MODDIR/socfreqd"
# 管理器已经标记卸载（remove）：即使 service.sh 还被调到，也绝不拉起守护。
# 否则卸载到重启之间会继续写频率节点，看起来像"删了还在限频"。
[ -f "$MODDIR/remove" ] && exit 0# 可变状态位于模块目录之外，升级模块不会把它一起删掉。
# 路径需与守护的 SF_DATADIR、api.sh 的 DATA 三处一致。
DATA=/data/adb/soc_freqcap
LOCK="$DATA/socfreqd.lock"
PAUSE="$DATA/pause"
HB="$DATA/run/heartbeat"
DS_PID="$DATA/run/dsleep.pid"
DS_SH="$MODDIR/scripts/deep_sleep.sh"

[ -f "$PAUSE" ] && exit 0

# 守护是否真的活着。
# 不能只看锁文件在不在：守护被 kill -9 时不会走 cleanup，锁文件会残留，
# 那样看门狗会永远认为"有人在跑"而再也不拉起，只能重启手机。
# 正确判据是锁文件里的 pid 是否还存在于 /proc。
alive() {
  [ -f "$LOCK" ] || return 1
  PID=$(cat "$LOCK" 2>/dev/null)
  case "$PID" in ''|*[!0-9]*) return 1 ;; esac
  [ -d "/proc/$PID" ] || return 1
  # pid 可能已被系统回收给别的进程，再确认一下进程名
  [ -r "/proc/$PID/comm" ] && grep -q socfreqd "/proc/$PID/comm" 2>/dev/null
}

# 心跳过期：没有文件 / 读不到 mtime / 超过 90 秒。
# 比 alive 多抓一种情况：进程还在但主循环卡死，pid 检查永远认为它活着。
stale() {
  [ -f "$HB" ] || return 0
  _now=$(date +%s 2>/dev/null)
  _mt=$(stat -c %Y "$HB" 2>/dev/null | head -n 1)
  case "$_now" in ''|*[!0-9]*) return 0 ;; esac
  case "$_mt" in ''|*[!0-9]*) return 0 ;; esac
  [ "$((_now - _mt))" -gt 90 ]
}

# 深度睡眠是独立 shell 进程，靠 pid 文件 + cmdline 认人。
# 只看 pid 文件不够：kill -9 后文件残留，看护会永远以为它还活着。
ds_alive() {
  [ -f "$DS_PID" ] || return 1
  _p=$(cat "$DS_PID" 2>/dev/null)
  case "$_p" in ''|*[!0-9]*) return 1 ;; esac
  [ -d "/proc/$_p" ] || return 1
  tr '\0' ' ' < "/proc/$_p/cmdline" 2>/dev/null | grep -q 'deep_sleep\.sh'
}

ds_start() {
  [ -f "$DS_SH" ] || return 0
  # 总开关是关的就不常驻；用户在页面上打开时 api.sh 会立刻拉起。
  [ -f "$DATA/dsleep.conf" ] || return 0
  _e=$(awk '
    /^[[:space:]]*enable[[:space:]]*=/ {
      v = $0
      sub(/^[^=]*=/, "", v)
      sub(/#.*/, "", v)
      gsub(/[[:space:]]/, "", v)
      print v
      exit
    }' "$DATA/dsleep.conf" 2>/dev/null)
  [ "$_e" = "1" ] || return 0
  ds_alive && return 0
  rm -f "$DS_PID" 2>/dev/null
  if command -v setsid >/dev/null 2>&1; then
    setsid sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
  else
    nohup sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
  fi
}

# 等待、启动、看门狗整体放在 ( ... ) & 里，脚本本身立刻返回。
# KernelSU / Magisk 同步执行 service.sh，前台阻塞会占住 runner，
# 拖住排在本模块之后的所有 service；等开机的轮询最长 150×2+5 秒，
# 只把 while true 挪进子 shell 不够。
(
  # 等开机完成，让厂商调度先跑起来，本模块再接管。
  # 守护写 sysfs 前必须先拿到原始值快照，否则还原的是已被自己改过的值。
  i=0
  while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 150 ]; do
    sleep 2
    i=$((i + 1))
  done
  sleep 5

  # ── 开机满血窗口 ────────────────────────────────────────────
  # 只写"到第几秒结束"的时间戳，用 /proc/uptime 口径而不用墙钟
  # （开机阶段时间要等 NTP 校准，date +%s 会跳）。
  # 判定、交权、到点压回都由守护做：service.sh 跑得很早，
  # cpufreq 节点和厂商调度未必就绪，此处直接写 sysfs 会静默失败。
  mkdir -p "$DATA/run" 2>/dev/null
  BOOT=$(sed -n 's/^BOOT_FULL_S=//p' "$DATA/socfreq.conf" 2>/dev/null \
         | head -n 1 | tr -d ' \r\t')
  # 键缺失时按守护的默认值 60 处理：conf / 守护 dflt / WebUI 滑杆 / 这里四处默认值需一致。
  # 值为 0 时不写文件，表示用户在页面上明确关掉；最终判定在守护（再查 cfg.boot_full_s > 0）。
  case "$BOOT" in ''|*[!0-9]*) BOOT=60 ;; esac
  [ "$BOOT" -gt 600 ] && BOOT=600
  if [ "$BOOT" -gt 0 ]; then
    UP=$(cut -d' ' -f1 /proc/uptime 2>/dev/null | cut -d. -f1)
    case "$UP" in ''|*[!0-9]*) UP='' ;; esac
    if [ -n "$UP" ]; then
      echo $((UP + BOOT)) > "$DATA/run/boot_boost_until" 2>/dev/null
    else
      rm -f "$DATA/run/boot_boost_until" 2>/dev/null
    fi
  fi

  # 看门狗：以守护每轮写的心跳 mtime 为准，超过 90s 没更新就拉起。
  # 进程还活着但主循环卡死时，先 kill 旧进程，避免清掉锁文件后跑出两个实例。
  # socfreqd 用 flock 保证单实例，但 flock 只在同一把锁文件上有效。
  # 深度睡眠守护独立成进程，没有心跳文件，只按 pid/cmdline 看护。
  ds_start
  while true; do
    if [ ! -f "$PAUSE" ] && stale; then
      if alive; then
        PID=$(cat "$LOCK" 2>/dev/null)
        case "$PID" in ''|*[!0-9]*) ;; *) kill -9 "$PID" 2>/dev/null ;; esac
        sleep 1
      fi
      [ -f "$LOCK" ] && rm -f "$LOCK" 2>/dev/null
      nohup "$DAEMON" start >/dev/null 2>&1 </dev/null &
    fi
    [ ! -f "$PAUSE" ] && ds_start
    sleep 60
  done
) &
exit 0