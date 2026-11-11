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
  { [ -f "$_b/run/nodes.saved" ] || [ -f "$_b/run/cpuset.saved" ] ||
    [ -f "$_b/run/uclamp.saved" ]; } && _had_journal=1
done

# 停掉所有还会写节点的进程。
# 光靠下一行 "$DAEMON" uninstall 不够：
#   - 二进制可能已被管理器删掉/换掉，那条 exec 直接失败，旧守护继续写到重启；
#   - service.sh 的看门狗每 60 秒一轮，心跳被删后会把守护重新拉起来；
#   - 深睡脚本也是独立进程，pid 文件丢了就没人管。
# 按 /proc 认人，先停一遍；还原完再停一遍，防止在还原窗口里被看门狗复活。
stop_procs() {
  _sig=${1:-TERM}
  for _p in /proc/[0-9]*; do
    _pid=${_p#/proc/}
    [ "$_pid" = "$$" ] && continue
    case "$(cat "$_p/comm" 2>/dev/null)" in
      socfreqd) kill -"$_sig" "$_pid" 2>/dev/null; continue ;;
    esac
    _cmd=$(tr '\0' ' ' < "$_p/cmdline" 2>/dev/null)
    case "$_cmd" in
      *"$MODDIR/service.sh"*|*"$MODDIR/scripts/deep_sleep.sh"*)
        kill -"$_sig" "$_pid" 2>/dev/null ;;
    esac
  done
}
stop_procs TERM
sleep 1

[ -f "$DAEMON" ] && "$DAEMON" uninstall 2>/dev/null
# uninstall 子命令最多等旧实例 5 秒再强杀；仍活着的（二进制缺失、旧实例在长睡）
# 在这里清掉，保证后面的还原过程没有第二个进程在写节点。
stop_procs KILL
sleep 1

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
[ -f "$DS_SH" ] && ( . "$DS_SH" >/dev/null 2>&1; ds_ops_restore_if_any; ds_freeze_restore_if_any; ds_snap_restore_if_any )

# 守护可能已被管理器删掉，或因 ELF 被 loader 拒绝而跑不起来，
# 此时只剩 run/*.saved 里的原始值，逐条读回作为最后兜底。三本账都要兜：
#   nodes.saved  数值节点的原始值 + 权限
#   perms.saved  只记权限（游戏期交还调度时放宽到 666 的那些节点）
#   govs.saved   调速器名字
# 漏掉后两本时数值已还原，但节点仍是 666、调速器仍停在改过的值上，且无日志提示。
restore_from() {
  J="$1/run/nodes.saved"
  [ -f "$J" ] || return 0
  _rf=0
  while read -r NODE VAL MODE; do
    [ -e "$NODE" ] || continue
    case "$VAL" in ''|*[!0-9]*) continue ;; esac
    chmod 0644 "$NODE" 2>/dev/null
    echo "$VAL" > "$NODE" 2>/dev/null
    _now=$(cat "$NODE" 2>/dev/null | tr -d ' \r\n')
    [ "$_now" = "$VAL" ] || {
      _rf=1
      echo "  未还原：$NODE（期望 $VAL，实际 ${_now:-读不到}）"
    }
    case "$MODE" in ''|*[!0-7]*) MODE=644 ;; esac
    chmod "$MODE" "$NODE" 2>/dev/null
  done < "$J"
  if [ "$_rf" = 0 ]; then rm -f "$J"; else echo "  保留账本：$J"; fi
}

# 权限兜底：只 chmod，不写值。
# 三列格式同 nodes.saved（node val mode），但 VAL 恒为 0，只表示该行只记权限。
# 因此只取 MODE：max_pwrlevel 一类节点上 0 是合法索引值，写回会真的改掉频率。
restore_perms() {
  P="$1/run/perms.saved"
  [ -f "$P" ] || return 0
  _rf=0
  while read -r NODE VAL MODE; do
    [ -e "$NODE" ] || continue
    case "$MODE" in ''|*[!0-7]*) MODE=644 ;; esac
    chmod "$MODE" "$NODE" 2>/dev/null
    _now=$(stat -c %a "$NODE" 2>/dev/null)
    [ "${_now#0}" = "${MODE#0}" ] || {
      _rf=1
      echo "  权限未还原：$NODE（期望 $MODE，实际 ${_now:-读不到}）"
    }
  done < "$P"
  if [ "$_rf" = 0 ]; then rm -f "$P"; else echo "  保留账本：$P"; fi
}

# 调速器兜底。govs.saved 每个节点两行：
#   "<path> 0 <mode>"      journal_add 记的权限
#   "<path> <governor>"    记的调速器名字，只有两列
# 只取第二列非纯数字的那行，避免把权限行当成名字写回。
restore_govs() {
  G="$1/run/govs.saved"
  [ -f "$G" ] || return 0
  _rf=0
  while read -r A B C; do
    # 三列 = journal_add 记的权限行；先按它把节点权限还原。
    if [ -n "${C:-}" ]; then
      [ -e "$A" ] || continue
      case "$C" in ''|*[!0-7]*) C=644 ;; esac
      chmod "$C" "$A" 2>/dev/null
      continue
    fi
    case "$B" in ''|*[0-9]*) continue ;; esac     # 只取带名字的那行
    [ -e "$A" ] || continue
    # 写名字前先记下当前（上一行刚还原的）权限，写完还回去；
    # 硬编码 0644 会让出厂 0664 的机型在卸载后写不动调速器。
    _before=$(stat -c %a "$A" 2>/dev/null)
    chmod 0644 "$A" 2>/dev/null; echo "$B" > "$A" 2>/dev/null
    _now=$(cat "$A" 2>/dev/null | tr -d ' \r\n')
    [ "$_now" = "$B" ] || {
      _rf=1
      echo "  调速器未还原：$A（期望 $B，实际 ${_now:-读不到}）"
    }
    [ -n "$_before" ] && chmod "$_before" "$A" 2>/dev/null
  done < "$G"
  if [ "$_rf" = 0 ]; then rm -f "$G"; else echo "  保留账本：$G"; fi
}

# cpuset 兜底：cpuset.saved 每行 "<path> <mask> <mode>"，掩码不是纯数字，
# 不能走 restore_from；这里只写回掩码并还原权限。
restore_cpuset() {
  C="$1/run/cpuset.saved"
  [ -f "$C" ] || return 0
  _rf=0
  while read -r NODE MASK MODE; do
    [ -e "$NODE" ] || continue
    case "$MASK" in ''|*[!0-9,-]*) continue ;; esac
    chmod 0644 "$NODE" 2>/dev/null
    echo "$MASK" > "$NODE" 2>/dev/null
    _now=$(cat "$NODE" 2>/dev/null | tr -d ' \r\n')
    [ "$_now" = "$MASK" ] || {
      _rf=1
      echo "  cpuset 未还原：$NODE（期望 $MASK，实际 ${_now:-读不到}）"
    }
    case "$MODE" in ''|*[!0-7]*) MODE=644 ;; esac
    chmod "$MODE" "$NODE" 2>/dev/null
  done < "$C"
  if [ "$_rf" = 0 ]; then rm -f "$C"; else echo "  保留账本：$C"; fi
}

# uclamp.max 兜底：原值可能是 max / 50.00 / 1024，按字符串原样写回。
restore_uclamp() {
  U="$1/run/uclamp.saved"
  [ -f "$U" ] || return 0
  _rf=0
  while read -r NODE VAL MODE; do
    [ -e "$NODE" ] || continue
    [ -n "$VAL" ] || continue
    chmod 0644 "$NODE" 2>/dev/null
    echo "$VAL" > "$NODE" 2>/dev/null
    _now=$(cat "$NODE" 2>/dev/null | tr -d ' \r\n')
    [ "$_now" = "$VAL" ] || {
      _rf=1
      echo "  uclamp.max 未还原：$NODE（期望 $VAL，实际 ${_now:-读不到}）"
    }
    case "$MODE" in ''|*[!0-7]*) MODE=644 ;; esac
    chmod "$MODE" "$NODE" 2>/dev/null
  done < "$U"
  if [ "$_rf" = 0 ]; then rm -f "$U"; else echo "  保留账本：$U"; fi
}

# 两处都要兜：账本现在在 $DATA/run，旧版本写在 $MODDIR/run，
# 从旧版本直升时账本可能仍留在模块目录里，只兜一处等于留一半。
#
# 传进去的是父目录，restore_* 内部自行拼 "$1/run/xxx.saved"；
# 路径拼错会让函数开头的 `[ -f ... ] || return 0` 静默返回，
# 数值完全不还原，而卸载输出照样打"已恢复启动前状态"。
for BASE in "$DATA" "$MODDIR"; do
  [ -d "$BASE/run" ] || continue
  restore_from "$BASE"
  restore_perms "$BASE"
  restore_govs "$BASE"
  restore_cpuset "$BASE"
  restore_uclamp "$BASE"
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

# 还原窗口里看门狗/守护可能又被拉起来了：再停一遍，确保没有进程在
# 还原之后重新压频。TERM 先给机会走正常恢复，KILL 收尾。
stop_procs TERM
sleep 1
stop_procs KILL

# 还有账本没还原（节点写不进去、或节点已不存在）时，先把原件拷到
# /data/local/tmp 再删数据目录；否则卸载后就再也找不回原始值。
_rem=0
for _j in "$DATA/run/nodes.saved" "$DATA/run/perms.saved" "$DATA/run/govs.saved" \
          "$DATA/run/cpuset.saved" "$DATA/run/uclamp.saved" \
          "$MODDIR/run/nodes.saved" "$MODDIR/run/perms.saved" "$MODDIR/run/govs.saved" \
          "$MODDIR/run/cpuset.saved" "$MODDIR/run/uclamp.saved"; do
  [ -f "$_j" ] && _rem=1
done
if [ "$_rem" = 1 ]; then
  mkdir -p /data/local/tmp/soc_freqcap_restore 2>/dev/null
  cp -f "$DATA/run"/*.saved /data/local/tmp/soc_freqcap_restore/ 2>/dev/null
  cp -f "$MODDIR/run"/*.saved /data/local/tmp/soc_freqcap_restore/ 2>/dev/null
  echo "警告：还有账本没还原，原始值已备份到 /data/local/tmp/soc_freqcap_restore"
fi

rm -f "$MODDIR/pause" "$MODDIR/state" "$MODDIR/socfreqd.lock" 2>/dev/null
rm -rf "$MODDIR/run" 2>/dev/null
# 同模块的待更新目录：不删的话重启后 KernelSU 会把新版本装回来，
# 表现就是"卸载了、重启后还在限频"。
rm -rf /data/adb/modules_update/soc_freqcap 2>/dev/null
# APatch 的待更新目录在另一棵树；漏掉它同样会在重启后装回来。
rm -rf /data/adb/ap/modules_update/soc_freqcap 2>/dev/null
# 另一个管理器根下如果还留着一份（比如从 KernelSU 换到 APatch 后只卸了一边），
# 那份的 service.sh 会照常开机启动并继续限频。不替用户删另一个根里的模块，
# 只明确报出来。
_self=$(cd "$MODDIR" 2>/dev/null && pwd)
[ -n "${_self:-}" ] || _self=$MODDIR
for _o in /data/adb/modules/soc_freqcap /data/adb/ap/modules/soc_freqcap; do
  [ "$_o" = "$_self" ] && continue
  [ -d "$_o" ] && echo "警告：另一个模块根还有一份本模块：$_o（请在对应管理器里也卸载）"
done
# 图标缓存在 $DATA/icon，随数据目录一起删；
# /data/local/tmp/sficon 是旧版遗留路径，不在模块目录内，需单独清理。
rm -rf /data/local/tmp/sficon 2>/dev/null
# 配置、名单、日志都在这里，卸载一并删掉，避免留下无人可清的 root 私有目录。
# 路径写成字面量而不是 "$DATA"：变量万一为空，rm -rf 删的就是 /data/adb。
rm -rf /data/adb/soc_freqcap 2>/dev/null

# 还原后自检：max 仍低于硬件最高时，把"本模块残留"和"别的工具在压"分开说。
# 判断要看 scaling_max_freq；scaling_cur_freq 是调度器当前跑多快，空闲时低是正常的。
_bad=0
for _p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -r "$_p/cpuinfo_max_freq" ] || continue
  _hw=$(cat "$_p/cpuinfo_max_freq" 2>/dev/null)
  _cur=$(cat "$_p/scaling_max_freq" 2>/dev/null)
  case "$_hw" in ''|*[!0-9]*) continue ;; esac
  case "$_cur" in ''|*[!0-9]*) continue ;; esac
  if [ "$_cur" -lt "$_hw" ]; then
    _bad=1
    echo "提示：${_p##*/} 上限 $_cur 仍低于硬件最高 $_hw"
  fi
done
if [ "$_bad" = 1 ]; then
  echo "      若上面没有 soc_freqcap 目录/进程，这是厂商或其它性能工具在压；"
  echo "      判断看 scaling_max_freq，不要拿 scaling_cur_freq 当上限。"
fi

if [ "$_rem" = 1 ]; then
  echo "Soc智能限频：卸载完成，但有账本未还原（备份见上）"
else
  echo "Soc智能限频：已恢复启动前状态"
fi