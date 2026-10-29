#!/system/bin/sh
# Soc智能限频 · 开机早期不写 sysfs
#
# post-fs-data 阶段内核的 cpufreq 节点还不可靠，此时写入会失败，
# 失败后基线快照就取错了，卸载时还原不回真实值。
# 全部频率控制交给 service.sh 启动的 socfreqd 处理。
exit 0