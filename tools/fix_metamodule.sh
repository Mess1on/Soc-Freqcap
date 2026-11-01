#!/system/bin/sh
# 刷写被 metamodule 拦住时的修复工具（在 root shell 里跑）。
#
# 现象（管理器弹红框，模块根本装不进去）：
#   Installation Blocked
#   A metamodule with custom installer is active
#   Current state: Disabled
#   Action required: Re-enable or uninstall it, then reboot
#
# 这是管理器自己的闸门：装了带 metamodule=1 的模块（常见是 Hybrid Mount），
# 又把它停用了，管理器就不许再装任何模块。跟被装的那个 zip 无关，
# 所以任何模块都会撞上，改 zip 也绕不过去（拦截发生在解包之前）。
#
# 用法：
#   sh fix_metamodule.sh          只检查，列出被停用的 metamodule 和修复命令
#   sh fix_metamodule.sh --fix    把它们重新启用（只删模块目录里的 disable 标记）
#
# 这一步只删一个标记文件：不改系统分区、不碰配置、不装不卸任何模块。
set -u
MODS=${MODS:-/data/adb/modules}
fix=0
case "${1:-}" in
    --fix|-f) fix=1 ;;
esac

found=0
for m in "$MODS"/*; do
    [ -d "$m" ] || continue
    grep -qiE '^metamodule[[:space:]]*=[[:space:]]*(1|true|yes)' \
        "$m/module.prop" 2>/dev/null || continue
    name=${m##*/}
    [ -f "$m/remove" ] && { echo "  $name：已标记卸载，重启后消失"; continue; }
    if [ -f "$m/disable" ]; then
        found=1
        echo "  被停用的 metamodule：$name"
        if [ "$fix" = "1" ]; then
            rm -f "$m/disable" && echo "    已重新启用（删掉 disable 标记）"
        else
            echo "    修复命令：rm -f $m/disable"
        fi
    else
        echo "  metamodule：$name（已启用，不会拦刷写）"
    fi
done

if [ "$found" = "0" ]; then
    echo "  没有发现被停用的 metamodule，这个报错可能来自别的原因。"
    exit 0
fi

if [ "$fix" = "1" ]; then
    echo
    echo "已处理。请重启手机，再回到管理器里重新安装模块。"
else
    echo
    echo "确认无误后跑：sh $0 --fix   然后重启"
fi
exit 0
