#!/system/bin/sh
# 出事后的取证脚本（只读）。把结果文件发给作者，比自己描述十句都准。
#
# 用法：
#   su -c 'sh collect_crash.sh'
# 输出：/data/local/tmp/socfreq_crash_<时间>.txt
#
# 重点看三件事：
#   1) bootreason —— 这次是正常重启、watchdog、还是 kernel panic
#   2) pstore / dmesg 里有没有 panic / watchdog / hung
#   3) 设备上还有哪些模块（尤其带 metamodule=1 的、被停用的）
set -u
OUT=/data/local/tmp/socfreq_crash_$(date +%Y%m%d_%H%M%S).txt
DATA=/data/adb/soc_freqcap
MOD=/data/adb/modules/soc_freqcap

say() { echo "$*" >>"$OUT"; }
sec() { echo "" >>"$OUT"; echo "===== $* =====" >>"$OUT"; }
cap() { "$@" >>"$OUT" 2>&1; }

: >"$OUT" || exit 1
say "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"

sec "1) 设备与开机原因"
cap getprop ro.product.model
cap getprop ro.build.version.release
cap getprop ro.build.display.id
cap getprop ro.boot.bootreason
cap getprop sys.boot.reason
cap getprop ro.boot.slot_suffix
cap cat /proc/cmdline
cap cat /proc/uptime

sec "2) 内核里有没有 panic / watchdog / hung"
# dmesg 可能被 SELinux 挡住；挡了就写下来，别假装没看到
dmesg 2>/dev/null | grep -iE 'panic|watchdog|hung task|BUG:|Oops|thermal.*shutdown' | tail -40
[ $? -ne 0 ] && say "（dmesg 不可读或没有匹配行）"

sec "3) pstore（上次崩溃的内核日志，有就是铁证）"
if [ -d /sys/fs/pstore ]; then
    ls -l /sys/fs/pstore >>"$OUT" 2>&1
    for f in /sys/fs/pstore/*; do
        [ -f "$f" ] || continue
        say "--- $f 末尾 60 行 ---"
        tail -n 60 "$f" >>"$OUT" 2>&1
    done
else
    say "本机没有 /sys/fs/pstore"
fi
[ -r /proc/last_kmsg ] && { say "--- /proc/last_kmsg 末尾 40 行 ---"; tail -n 40 /proc/last_kmsg >>"$OUT"; }

sec "4) root 与管理器"
cap id
ls -l /data/adb >>"$OUT" 2>&1
[ -x /data/adb/ksud ] && /data/adb/ksud -V >>"$OUT" 2>&1
cap getprop ro.kernelsu.version
cat /data/adb/modules/soc_freqcap/module.prop >>"$OUT" 2>&1

sec "5) 所有模块（★ = 被停用；metamodule 会被特别标出）"
for m in /data/adb/modules/*; do
    [ -d "$m" ] || continue
    n=${m##*/}
    tag=""
    [ -f "$m/disable" ] && tag="$tag ★停用"
    [ -f "$m/remove" ] && tag="$tag 已标记卸载"
    grep -qiE '^metamodule[[:space:]]*=[[:space:]]*(1|true|yes)' "$m/module.prop" 2>/dev/null \
        && tag="$tag [metamodule]"
    say "  $n$tag"
done

sec "6) 本模块状态"
for f in pause safemode remove; do
    say "  $f: $([ -e "$DATA/$f" ] && echo 存在 || echo 无)"
done
say "  dsleep.conf:"; sed 's/^/    /' "$DATA/dsleep.conf" >>"$OUT" 2>&1
say "  run/status:"; sed 's/^/    /' "$DATA/run/status" >>"$OUT" 2>&1
say "  进程:"; ps -A -o PID,TIME,ARGS 2>/dev/null | grep -E 'socfreqd|deep_sleep\.sh|service\.sh' | grep -v grep >>"$OUT" 2>&1
say "  cgroup 组:"; ls -d /dev/freezer/* /sys/fs/cgroup/apps/socfreq* 2>/dev/null >>"$OUT" 2>&1
say "  cgroup 内的进程:"; for g in /dev/freezer/socfreq_ds /sys/fs/cgroup/apps/socfreq*; do
    [ -d "$g" ] || continue
    echo "  $g:" >>"$OUT"
    cat "$g/cgroup.procs" 2>/dev/null | while read -r p; do
        printf '    %s %s\n' "$p" "$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | cut -c1-50)" >>"$OUT"
    done
done

sec "7) 日志末尾（各 60 行）"
for f in socfreqd.log dsleep.log service.log; do
    say "--- $f ---"
    tail -n 60 "$DATA/$f" >>"$OUT" 2>&1
done

sec "8) 频率节点现状（对照是否卡在低频）"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    echo "  ${p##*/}: gov=$(cat "$p/scaling_governor" 2>/dev/null) cur=$(cat "$p/scaling_cur_freq" 2>/dev/null) max=$(cat "$p/scaling_max_freq" 2>/dev/null) hw=$(cat "$p/cpuinfo_max_freq" 2>/dev/null)" >>"$OUT"
done

echo "已写出：$OUT"
echo "把它发给作者。里面有型号、开机原因、模块列表和日志，没有隐私数据。"
