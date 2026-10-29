#!/system/bin/sh
# Soc智能限频 · 卸载清理
#
# 还原顺序很重要：先停守护（它持有原始值快照，会自己恢复），
# 再由守护的 uninstall 子命令按 journal 强制兜底。
# 两道都做完，sysfs 上才真正干净。

MODDIR=${0%/*}
DAEMON="$MODDIR/socfreqd"
# 可变状态位于模块目录之外（见 customize.sh 的迁移段）。
DATA=/data/adb/soc_freqcap

# 先看有没有账本，再让守护自己恢复（uninstall 成功后会把账本删掉）。
# 没有账本说明守护可能从没成功跑过，但早期版本也可能留下过节点改动，
# 最后兜一层"CPU 上限 = 硬件最高"。
_had_journal=0
for _b in "$DATA" "$MODDIR"; do
  [ -f "$_b/run/nodes.saved" ] && _had_journal=1
done

[ -f "$DAEMON" ] && "$DAEMON" uninstall 2>/dev/null

# 深度睡眠：先发 TERM 让脚本自己 unforce + 还原常量；进程没来得及退出时
# 快照还在，再 source 一次按存档兜底。快照删掉才是正常退出的标记。
DS_SH="$MODDIR/scripts/deep_sleep.sh"
DS_PID="$DATA/run/dsleep.pid"
if [ -f "$DS_PID" ]; then
  DSD=$(cat "$DS_PID" 2>/dev/null)
  case "$DSD" in
    ''|*[!0-9]*) ;;
    *)
      kill "$DSD" 2>/dev/null
      i=0
      while [ "$i" -lt 5 ] && [ -d "/proc/$DSD" ]; do sleep 1; i=$((i + 1)); done
      [ -d "/proc/$DSD" ] && kill -9 "$DSD" 2>/dev/null
      ;;
  esac
  rm -f "$DS_PID" 2>/dev/null
fi
[ -f "$DS_SH" ] && ( . "$DS_SH" >/dev/null 2>&1; ds_freeze_restore_if_any; ds_snap_restore_if_any )

# 守护可能已被管理器删掉，或因 ELF 被 loader 拒绝而跑不起来，
# 此时只剩 run/*.saved 里的原始值，逐条读回作为最后兜底。三本账都要兜：
#   nodes.saved  数值节点的原始值 + 权限
#   perms.saved  只记权限（游戏期交还调度时放宽到 666 的那些节点）
#   govs.saved   调速器名字
# 漏掉后两本时数值已还原，但节点仍是 666、调速器仍停在改过的值上，且无日志提示。
restore_from() {
  J="$1/run/nodes.saved"
  [ -f "$J" ] || return 0
  while read -r NODE VAL MODE; do
    [ -e "$NODE" ] || continue
    case "$VAL" in ''|*[!0-9]*) continue ;; esac
    chmod 0644 "$NODE" 2>/dev/null
    echo "$VAL" > "$NODE" 2>/dev/null
    chmod "$MODE" "$NODE" 2>/dev/null
  done < "$J"
  rm -f "$J"
}

# 权限兜底：只 chmod，不写值。
# 三列格式同 nodes.saved（node val mode），但 VAL 恒为 0，只表示该行只记权限。
# 因此只取 MODE：max_pwrlevel 一类节点上 0 是合法索引值，写回会真的改掉频率。
restore_perms() {
  P="$1/run/perms.saved"
  [ -f "$P" ] || return 0
  while read -r NODE VAL MODE; do
    [ -e "$NODE" ] || continue
    case "$MODE" in ''|*[!0-7]*) MODE=644 ;; esac
    chmod "$MODE" "$NODE" 2>/dev/null
  done < "$P"
  rm -f "$P"
}

# 调速器兜底。govs.saved 每个节点两行：
#   "<path> 0 <mode>"      journal_add 记的权限
#   "<path> <governor>"    记的调速器名字，只有两列
# 只取第二列非纯数字的那行，避免把权限行当成名字写回。
restore_govs() {
  G="$1/run/govs.saved"
  [ -f "$G" ] || return 0
  while read -r A B; do
    case "$B" in ''|*[0-9]*) continue ;; esac     # 只取带名字的那行
    [ -e "$A" ] || continue
    chmod 0644 "$A" 2>/dev/null; echo "$B" > "$A" 2>/dev/null
  done < "$G"
  rm -f "$G"
}

# 两处都要兜：账本现在在 $DATA/run，旧版本写在 $MODDIR/run，
# 从旧版本直升时账本可能仍留在模块目录里，只兜一处等于留一半。
#
# 传进去的是父目录，三个 restore_* 内部自行拼 "$1/run/xxx.saved"；
# 路径拼错会让函数开头的 `[ -f ... ] || return 0` 静默返回，
# 数值完全不还原，而卸载输出照样打"已恢复启动前状态"。
for BASE in "$DATA" "$MODDIR"; do
  [ -d "$BASE/run" ] || continue
  restore_from "$BASE"
  restore_perms "$BASE"
  restore_govs "$BASE"
done

# 账本不在：守护可能从未成功启动，但节点也可能被早期版本写过。
# 按硬件最高放回去，至少不会是"卸载后还封着"。
if [ "$_had_journal" = 0 ]; then
  for _p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -r "$_p/cpuinfo_max_freq" ] \
      && cat "$_p/cpuinfo_max_freq" > "$_p/scaling_max_freq" 2>/dev/null
  done
fi

# 深睡快照还在 = 守护没走正常退出（脚本缺失/被杀）。
# 直接按快照还原 Doze 常量并撤掉本次临时加的白名单，不依赖脚本文件。
SNAP="$DATA/ds_snap.conf"
if [ -f "$SNAP" ]; then
  _c=$(sed -n 's/^constants=//p' "$SNAP" 2>/dev/null | head -n 1)
  dumpsys deviceidle unforce >/dev/null 2>&1
  if [ "$_c" = "@none" ]; then
    settings delete global device_idle_constants >/dev/null 2>&1
  elif [ -n "$_c" ]; then
    settings put global device_idle_constants "$_c" >/dev/null 2>&1
  fi
  sed -n 's/^added=//p' "$SNAP" 2>/dev/null | while read -r _p; do
    [ -n "$_p" ] && dumpsys deviceidle whitelist "-$_p" >/dev/null 2>&1
  done
  rm -f "$SNAP"
fi

# MTK GPU limit_table 快照：守护没走正常退出时先写回，再删。
# 这是 procfs 命令节点，不在 nodes.saved 的"路径=值"账本里。
GSNAP="$DATA/run/gpu_mtk.saved"
if [ -f "$GSNAP" ]; then
  _id=$(sed -n 's/^id=//p' "$GSNAP" 2>/dev/null | head -n 1)
  _gc=$(sed -n 's/^ceil=//p' "$GSNAP" 2>/dev/null | head -n 1)
  _gf=$(sed -n 's/^floor=//p' "$GSNAP" 2>/dev/null | head -n 1)
  [ -n "$_gc" ] || _gc=-1
  [ -n "$_gf" ] || _gf=-1
  case "$_id" in
    ''|*[!0-9]*) ;;
    *) [ -e /proc/gpufreqv2/limit_table ] \
         && printf 'set %s %s %s\n' "$_id" "$_gc" "$_gf" \
            > /proc/gpufreqv2/limit_table 2>/dev/null ;;
  esac
  rm -f "$GSNAP"
fi

rm -f "$MODDIR/pause" "$MODDIR/state" "$MODDIR/socfreqd.lock" 2>/dev/null
rm -rf "$MODDIR/run" 2>/dev/null
# 同模块的待更新目录：不删的话重启后 KernelSU 会把新版本装回来，
# 表现就是"卸载了、重启后还在限频"。
rm -rf /data/adb/modules_update/soc_freqcap 2>/dev/null
# 图标缓存在 $DATA/icon，随数据目录一起删；
# /data/local/tmp/sficon 是旧版遗留路径，不在模块目录内，需单独清理。
rm -rf /data/local/tmp/sficon 2>/dev/null
# 配置、名单、日志都在这里，卸载一并删掉，避免留下无人可清的 root 私有目录。
# 路径写成字面量而不是 "$DATA"：变量万一为空，rm -rf 删的就是 /data/adb。
rm -rf /data/adb/soc_freqcap 2>/dev/null

echo "Soc智能限频：已恢复启动前状态"