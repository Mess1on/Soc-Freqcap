#!/system/bin/sh
# 安全模式开关：进安全模式后守护完全不启动（不改频率、不冻结、不碰 Doze），
# 但模块本身还在，随时可以退出来。适合"怀疑这个模块把机器搞坏了"的时候。
#
# 用法：
#   sh safemode.sh            看当前状态
#   sh safemode.sh on         进安全模式（要重启才生效）
#   sh safemode.sh off        退出安全模式（要重启才生效）
set -u
DATA=${SOCFREQ_DATA_DIR:-/data/adb/soc_freqcap}
F="$DATA/safemode"

case "${1:-}" in
    on|1)
        mkdir -p "$DATA" 2>/dev/null
        : > "$F" 2>/dev/null && echo "已进安全模式。重启后守护不会启动；再跑 'sh $0 off' 并重启即可恢复。"
        ;;
    off|0)
        rm -f "$F" 2>/dev/null && echo "已退出安全模式。重启后恢复。"
        ;;
    *)
        if [ -f "$F" ]; then
            echo "当前：安全模式（守护不会启动）"
        else
            echo "当前：正常"
        fi
        ;;
esac
