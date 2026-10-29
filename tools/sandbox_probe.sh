#!/system/bin/sh
# 芯片适配兜底路径的真机自检（要 root）。
#
# 要答的问题只有一个：自动探测扑空的机器，装了 override.conf 之后到底能不能干活。
# 正常手机上探测永远是好的，那条兜底分支在生产机器上根本走不到，
# 所以这里在 /data/local/tmp/fakeroot 造一台"假机器"：
#   有 cpu0/cpu4 的逐核 cpufreq 目录，但没有 /sys/devices/system/cpu/cpufreq/policy*，
#   守护的自动探测必然扑空 —— 这正是适配包要救的那台机。
# 守护全程只在沙箱里读写，碰不到真 sysfs；跑完自己清掉。
#
# 用法：adb shell su -c 'sh /data/local/tmp/sandbox_probe.sh'   （先 push 过来）
F=/data/local/tmp/fakeroot
M=/data/adb/modules/soc_freqcap
[ "$(id -u)" = 0 ] || { echo "要 root"; exit 1; }
[ -f "$M/socfreqd" ] || { echo "模块没装：$M/socfreqd 不在"; exit 1; }

rm -rf "$F"
mkdir -p "$F/data/adb/soc_freqcap/run" \
         "$F/sys/devices/system/cpu/cpu0/cpufreq" \
         "$F/sys/devices/system/cpu/cpu4/cpufreq" \
         "$F/sys/class/misc/fakegpu"
cp "$M/socfreqd" "$F/socfreqd" && chmod 755 "$F/socfreqd"

# 两个簇：3532800 与 2803200。上限节点先写 0 当哨兵，
# 守护真按覆盖干活的话这两个文件才会变成算出来的封顶。
# 哨兵必须是"0"而不是空文件：空文件读不出数，守护按"这个节点读不到"整档跳过。
echo 3532800 > "$F/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
echo 2803200 > "$F/sys/devices/system/cpu/cpu4/cpufreq/cpuinfo_max_freq"
echo 0 > "$F/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
echo 0 > "$F/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq"
echo 0 > "$F/sys/class/misc/fakegpu/max_freq"
echo 0 > "$F/sys/class/misc/fakegpu/min_freq"

cat > "$F/data/adb/soc_freqcap/socfreq.conf" <<'EOF'
INTERVAL=2
CPU_PCT_BIG=50
CPU_PCT_MID2=50
CPU_PCT_MID=50
CPU_PCT_LITTLE=50
GPU_PCT=50
GPU_MIN_LOCK=0
SCREEN_UNCAP=0
BOOT_FULL_S=0
LEARN=0
EOF

RD=0; PS=0
chk() { PS=$((PS + 1)); if [ "$1" = "$2" ]; then echo "  ok   $3"; RD=$((RD + 1));
      else echo "  FAIL $3：要 [$2] 得 [$1]"; fi; }
# 节点文件读回来要去掉行尾：守护写 sysfs 那套是按"整值覆盖"设计的，
# 普通文件上会留下上一次写的尾巴，直接 cat 会看到两个数粘在一行。
rdval() { head -c 16 "$1" 2>/dev/null | tr -d '\r\n '; }

echo "== A 沙箱里不给覆盖：自动探测必须扑空 =="
PA=$(chroot "$F" /socfreqd probe 2>&1)
chk "$(printf '%s' "$PA" | grep -c 'hw=')" "0" "没有覆盖时一个簇都探测不到"
chk "$(printf '%s' "$PA" | grep -c 'node=无')" "1" "GPU 也探测不到"

echo "== B 装上作者随版本发布的适配表 =="
# 第一行 #from adapt/… 是装机脚本留下的来源标记：守护必须认得"这不是节点行"。
cat > "$F/data/adb/soc_freqcap/override.conf" <<'EOF'
#from adapt/EXAMPLE.conf
cpu-dir /sys/devices/system/cpu/cpu0/cpufreq 3532800
cpu-dir /sys/devices/system/cpu/cpu4/cpufreq 2803200
gpu-max /sys/class/misc/fakegpu/max_freq
gpu-min /sys/class/misc/fakegpu/min_freq
gpu-freqs 1100000000 967000000 443000000 160000000
EOF
PB=$(chroot "$F" /socfreqd probe 2>&1)
chk "$(printf '%s' "$PB" | grep -c 'hw=')" "2" "两个簇都按覆盖列出来了"
chk "$(printf '%s' "$PB" | grep -c 'hw=3532800')" "1" "硬件上限取的是覆盖里给的数"
# 名字取的是路径里能分清簇的那一段：逐核目录的末段每条都叫 cpufreq，
# 照抄末段页面就是一排重名，看不出哪一档管哪几颗核。
chk "$(printf '%s' "$PB" | grep -c '  cpu0 ')" "1" "第一簇显示成 cpu0（不是重名的 cpufreq）"
chk "$(printf '%s' "$PB" | grep -c '  cpu4 ')" "1" "第二簇显示成 cpu4"
# GPU 封顶取频率表里不超过 50% 的那一档：550MHz 表里没有 → 443MHz
chk "$(printf '%s' "$PB" | sed -n 's/.*封顶=\([0-9]*\)Hz.*/\1/p')" "443000000" \
    "GPU 用覆盖里的频率表吸附"

echo "== C 真跑 8 秒，看封顶落不落到节点上 =="
# 前台 + timeout：后台跑再 kill 的那次试过，守护根本没起来（stderr 被 /dev/null 吞了，
# 查不到原因），而这条能稳定看到"启动→施加→恢复"三段日志。
CS=$(timeout 8 chroot "$F" /socfreqd start 2>&1)
CR=$?
L=$(cat "$F/data/adb/soc_freqcap/socfreqd.log" 2>/dev/null)
echo "  ..   start rc=$CR  日志 $(printf '%s\n' "$L" | wc -l) 行"
printf '%s\n' "$L" | tail -4 | sed 's/^/  ..   日志尾: /'
chk "$(printf '%s' "$L" | grep -c '上限 -> 1766400')" "1" "50% 的 3532800 = 1766400 写下去了"
chk "$(printf '%s' "$L" | grep -c '上限 -> 1401600')" "1" "50% 的 2803200 = 1401600 写下去了"
chk "$(printf '%s' "$L" | grep -c '已恢复')" "1" "退出时按 journal 恢复节点"
# 哨兵文件是普通文件，不是 sysfs：守护按"整值覆盖"写（不带截断），
# 恢复时写回的短值会留下上一次的尾巴，所以这里只当参考，判定以上面三条日志为准。
V0=$(rdval "$F/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq")
V1=$(rdval "$F/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq")
echo "  ..   节点回读 cpu0=[$V0] cpu4=[$V1]（普通文件有尾巴，不参与判定）"
[ -n "$CS" ] && printf '%s\n' "$CS" | head -3 | sed 's/^/  ..   start 输出: /'

echo "== D 撤掉覆盖：同一台沙箱应立刻回到探测不到 =="
rm -f "$F/data/adb/soc_freqcap/override.conf"
PD=$(chroot "$F" /socfreqd probe 2>&1)
chk "$(printf '%s' "$PD" | grep -cE 'cpu0|cpu4')" "0" "覆盖没了就回到探测不到（生效顺序只在这一条路上）"

echo "== E 沙箱没动到真机 =="
chk "$(ls -1d /sys/devices/system/cpu/cpufreq/policy* 2>/dev/null | wc -l | tr -d ' ')" "2" \
    "真 policy 目录还在（沙箱没遮掉任何东西）"
chk "$(ls "$F/data/adb/soc_freqcap/override.conf" /data/adb/soc_freqcap/override.conf 2>/dev/null | wc -l | tr -d ' ')" "0" \
    "两处 override.conf 都不存在（D 步已清，数据目录本来就不该有）"
chk "$(ps -A | grep -c socfreqd)" "1" "真守护只有一份在跑（沙箱那份没留在系统里）"

rm -rf "$F"
echo
echo "芯片适配兜底自检：$RD/$PS 通过"
[ "$RD" = "$PS" ] || exit 1
