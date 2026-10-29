#!/system/bin/sh
# KernelSU 热安装后执行（customize.sh 里的 MODULE_HOT_RUN_SCRIPT 指向这里）。
#
# 热安装时 KernelSU 已经用新版本替换了 /data/adb/modules/soc_freqcap，
# 但**不会**执行 service.sh，也不会替我们停掉旧进程。所以这里做三件事：
#   1) 让旧的深睡守护自己退出（它会解冻、unforce、撤临时白名单）；
#   2) 用新二进制 restart 主守护（restart 会等旧实例退出再接上）；
#   3) 深睡开关是开的就按新脚本重新拉起。
# 模块处于「暂停」状态时保持停住，不替用户恢复。
# 不做任何 system/ 分区操作，也不碰配置与名单。

set -u
MODDIR=$(cd "${0%/*}/.." 2>/dev/null && pwd) || exit 0
DATA=${SOCFREQ_DATA_DIR:-/data/adb/soc_freqcap}
BIN="$MODDIR/socfreqd"
DS_PID="$DATA/run/dsleep.pid"
DS_SH="$MODDIR/scripts/deep_sleep.sh"
PAUSE="$DATA/pause"

say() { echo "  $*"; }

# 管理器已标记卸载：不启动任何东西。
if [ -f "$MODDIR/remove" ]; then
  say "模块已标记卸载，跳过启动"
  exit 0
fi

# 1) 深睡守护：先停旧的。TERM 会走 ds_exit（解冻 + unforce + 撤临时白名单）；
#    万一卡住，4 秒后 kill -9，冻结账本和快照由新脚本启动时还原。
_p=$(cat "$DS_PID" 2>/dev/null)
case "$_p" in
  ''|*[!0-9]*) ;;
  *)
    if [ -d "/proc/$_p" ] && tr '\0' ' ' < "/proc/$_p/cmdline" 2>/dev/null | grep -q 'deep_sleep\.sh'; then
      kill "$_p" 2>/dev/null
      _i=0
      while [ "$_i" -lt 20 ] && [ -d "/proc/$_p" ]; do sleep 0.2; _i=$((_i + 1)); done
      [ -d "/proc/$_p" ] && kill -9 "$_p" 2>/dev/null
      say "已停止旧的深睡守护"
    fi
    ;;
esac
rm -f "$DS_PID" 2>/dev/null

# 2) 主守护。restart 会等旧实例把频率节点恢复完再启动新版本；
#    暂停状态只用 stop 清掉旧实例，不启动。
if [ -x "$BIN" ]; then
  if [ -f "$PAUSE" ]; then
    "$BIN" stop >/dev/null 2>&1
    say "模块处于暂停状态，未启动守护"
  else
    "$BIN" restart >/dev/null 2>&1
    sleep 0.5
    _st=$(sed -n 's/^pid=//p' "$DATA/run/status" 2>/dev/null | head -n 1)
    say "守护已按新版本启动${_st:+（pid $_st）}"
  fi
else
  say "警告：找不到可执行的 socfreqd，守护未启动"
fi

# 3) 深睡开关开着就拉起新脚本（之前没在跑也一样）。
if [ -f "$DS_SH" ] && [ ! -f "$PAUSE" ]; then
  _en=$(awk '
    /^[[:space:]]*enable[[:space:]]*=/ {
      v = $0; sub(/^[^=]*=/, "", v); sub(/#.*/, "", v); gsub(/[[:space:]]/, "", v)
      print v; exit
    }' "$DATA/dsleep.conf" 2>/dev/null)
  if [ "$_en" = "1" ]; then
    if command -v setsid >/dev/null 2>&1; then
      setsid sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
    else
      nohup sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
    fi
    say "深睡守护已启动"
  fi
fi

say "热安装完成：WebUI / action.sh / 模块信息已生效，无需重启"
exit 0
