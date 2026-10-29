#!/system/bin/sh
# Soc智能限频 · 模块管理按钮（音量键操作）
#
# 管理器点「执行」时 stdin 不是终端，键盘输入进不来，唯一能用的物理输入是音量键。
# 做法是拿 root 轮询 getevent：一次读一个事件，读到音量键的 DOWN 算一次按键，
# 其余事件（松手、别的键、触摸屏）丢掉继续等。
# 必须防的两件事：timeout / getevent 缺一个就会无限等待；/dev/input 打不开
# 时会空转。前者靠能力检查退回"只打印命令"，后者靠每次读数自带窗口上限。
# 超时是硬性的：菜单 90 秒、二次确认 20 秒，到点自动退出，不改动任何东西。

# $0 可能不带斜杠（cd 进来再 sh action.sh），那样 ${0%/*} 剥不出目录，
# 后面 api.sh 会被判成"读不到"，屏幕上留下一句关于缺命令的假话。
_self=$0
case "$_self" in
  */*) MODDIR=${_self%/*} ;;
  *)   MODDIR=$(pwd) ;;
esac
DAEMON="$MODDIR/socfreqd"
API="$MODDIR/webroot/api.sh"
DATA=/data/adb/soc_freqcap

# ui_print 由管理器提供，点「执行」的运行环境里不一定有；没有就用 echo 顶上，
# 否则每一行都变成 "ui_print: not found" 打到 stderr，界面整页空白。
if ! command -v ui_print >/dev/null 2>&1; then
  ui_print() { echo "$@"; }
fi
say() { ui_print "  $1"; }

# 所有操作都转给 api.sh：区间校验、白名单、原子写、回执格式只有一份实现。
# 命令行里自己写配置的话，同一个值会在 WebUI 被拒、在这里被接受。
api() { sh "$API" "$@" 2>/dev/null; }
# 读一个配置项的当前值；读不到或不是纯数字就按默认值起算
confv() { api conf_get | sed -n "s/^$1=//p" | head -n 1 | tr -d ' \r\t'; }

# ── 往守护日志里记几步 ─────────────────────────────────────────
# 屏幕上分不出"我们自己超时""读不到按键""管理器不再读输出"这三种退出，
# 而它们的处理完全不同，所以每一步在日志里留一行。
# 写进守护那份日志（而不是新开文件）：WebUI 的日志卡片和菜单第 3 项已经在读它。
_T0=$(date +%s 2>/dev/null || echo 0)
atrace() {
  [ -n "$_T0" ] || return 0
  # 一次 date 同时取"秒"和"时刻"：每行日志多取一次时间，
  # 就等于在按时间算预算的循环里多扣一秒。
  _t=$(date '+%s %H:%M:%S' 2>/dev/null)
  _s=${_t% *}; _c=${_t#* }
  [ -n "$_s" ] || return 0
  printf '%s action: %s (用时 %ss)\n' "${_c:-?}" "$1" "$((_s - _T0))" \
    >> "$DATA/socfreqd.log" 2>/dev/null
  return 0
}
_KILL=""
trap '_KILL=HUP;  exit 129' HUP
trap '_KILL=TERM; exit 143' TERM
# SIGPIPE 不能退：管理器的输出区到一定量就不再读管道，之后任何一次 ui_print
# 都会拿到 SIGPIPE，默认动作是整个脚本死掉。记一笔继续跑，
# 后面的按键照样处理，只是那几行新输出没人收了。
trap 'atrace "输出管道已断，界面收不到新行，继续等按键"' PIPE
trap 'atrace "结束 rc=$?${_KILL:+ 因为是 $_KILL}"' EXIT
atrace "开始 pid=$$ getevent=$(command -v getevent >/dev/null 2>&1 && echo 有 || echo 无) timeout=$(command -v timeout >/dev/null 2>&1 && echo 有 || echo 无)"

# 音量＋ 和音量－ 常常挂在两个不同的 /dev/input 节点上，
# 所以读数不能带设备参数，要让 getevent 自己监听全部节点。
# 行的形状是 `/dev/input/eventN: EV_KEY  KEY_VOLUMEUP  DOWN`，下面只认 DOWN。
# ── 按键能力检查 ────────────────────────────────────────────────
# 只查命令在不在，不去查"设备上有没有音量键"：getevent 的列表选项在 toybox 上
# 不一定存在，拿它当闸门会一失败就把整个功能关掉。
# 防卡死靠的是每次读数都有自己的上限（见 key_wait），不是靠预先探测。
can_use_keys() {
  command -v getevent >/dev/null 2>&1 || return 1
  command -v timeout >/dev/null 2>&1 || return 1
  [ -r "$API" ] || return 1
  return 0
}

# 等一次音量键。输出 up / down / other / timeout / spin <原因>。
#
# -c 1 一次只取一个事件，松手（UP）会单独占一次读数，必须丢掉，
# 不然一次按下算两回。
# other = 别的实体键：调用处当它"有人在动"，重新计时继续等。
# 只认音量键的话，先按一下侧键亮屏、或者误按别的键，那一下被丢掉、时钟不重置，
# 人慢慢看完八行菜单就会撞上 90 秒。
#
# 读数窗口 0.1 秒 + 隔 0.05 秒再看。evdev 每次 open 是一个新客户端，
# 只在进程活着的那段收事件，两次读数之间按下的键会丢掉——窗口越短空档越大，
# 短窗口的代价是"有时要按两下才动"，长窗口（一次阻塞一秒）的代价是
# 管理器那边等不到输出就把这一趟掐掉。取短的这一边。
#
# spin = getevent 每次都立刻返回空（打不开 /dev/input：命名空间或 SELinux）。
# 判据是用时对不对得上读数次数，不看退出码——读数一旦过管道，
# $? 取到的是管道最后一个命令的状态，"124 = 被 timeout 杀掉"这个区分并不存在。
key_wait() {
  _lim=${1:-90}
  _t0=$(date +%s 2>/dev/null)
  _n=0
  while :; do
    _now=$(date +%s 2>/dev/null)
    if [ -z "$_t0" ] || [ -z "$_now" ]; then
      # date 用不了就别空转：睡一秒再看，最多是慢，不会把 CPU 烧满
      sleep 1
      continue
    fi
    _el=$((_now - _t0))
    if [ "$_el" -ge "$_lim" ]; then
      atrace "超时 lim=${_lim}s 读数=${_n} 次"
      echo "timeout"; return
    fi
    # 正常节奏一秒最多 6~7 次读数；打不开设备时是一秒几十次。
    # 阈值 12 次/秒 + 40 次余量：两边差着一个数量级，不会撞车。
    if [ "$_n" -gt $((_el * 12 + 40)) ]; then
      atrace "空转：${_el} 秒里读了 ${_n} 次（getevent 打不开输入设备）"
      echo "spin getevent 每次都立刻返回空，读不到 /dev/input"; return
    fi
    _n=$((_n + 1))
    _ev=$(timeout 0.1 getevent -lqc 1 2>/dev/null)
    case "$_ev" in
      *KEY_VOLUMEUP*DOWN*)   echo "up";    return ;;
      *KEY_VOLUMEDOWN*DOWN*) echo "down";  return ;;
      *EV_KEY*)              echo "other"; return ;;
    esac
    sleep 0.05
    # 触摸屏（EV_ABS/EV_SYN）不算活动：口袋误触、放在桌上压到都会持续出事件，
    # 拿它们续时的话这个界面就永远不退出，白占一个 root shell。
  done
}

# ── 动作表 ──────────────────────────────────────────────────────
# 只读的（1/2/3/9）按下去直接执行；改东西的五个（4~8）都要二次确认：
# 音量+ 取消、音量- 确认。只有两个键的界面里，误碰的代价就靠确认那一步兜住。
NITEMS=9

label() {
  case "$1" in
    1) echo "查看运行状态" ;;
    2) echo "封顶预览（只读，不改任何东西）" ;;
    3) echo "最近 30 行日志" ;;
    4) echo "CPU 封顶 +5%（更强性能，四簇同步）" ;;
    5) echo "CPU 封顶 -5%（更省电，四簇同步）" ;;
    6) echo "重启守护（改完配置让它立刻生效）" ;;
    7) echo "暂停 / 恢复限频" ;;
    8) echo "停掉守护并恢复到开机前状态" ;;
    9) echo "智能建议（按学习采样给每档升降，只读）" ;;
  esac
}

# 这一项是不是"只读"
readonly_item() { [ "$1" -le 3 ] || [ "$1" = 9 ]; }

# CPU 四簇一起加减：只动超大核的话，页面上四个滑杆和这里调出来的结果会互相盖。
bump_cpu() {
  _d=$1
  for _k in CPU_PCT_BIG CPU_PCT_MID2 CPU_PCT_MID CPU_PCT_LITTLE; do
    _v=$(confv "$_k")
    case "$_v" in ''|*[!0-9]*) _v=65 ;; esac
    _n=$((_v + _d))
    # 下限留 20%：1% 也是合法值，但命令行里连按几下就到底了，
    # 机器看起来像坏了，而用户不知道是谁干的。
    [ "$_n" -lt 20 ] && _n=20
    [ "$_n" -gt 100 ] && _n=100
    api conf_set "$_k" "$_n" >/dev/null
  done
}

do_item() {
  case "$1" in
    # 必须用 -E（ERE）：toybox 的 sed 在 BRE 里不认 \| 交替，
    # 那条过滤在手机上会一行都不放行 —— 第 1 项点了没任何输出的根因就在这儿。
    # 开发机是 GNU sed，认 \|，所以这类写法在 PC 上永远测不出来。
    1) api status | sed -nE 's/^(state|game_pkg|reason|fg|app_mode|paused|alive|pid|interval|grace|soc|ncores|nmode)=/    \1 = /p' ;;
    2) "$DAEMON" probe 2>&1 | sed 's/^/    /' ;;
    3) api log 30 | tail -n 30 | sed 's/^/    /' ;;
    4) bump_cpu 5;  say "已 +5%，现在四簇：$(api conf_get | sed -n 's/^CPU_PCT_[A-Z0-9]*=//p' | tr '\n' ' ')" ;;
    5) bump_cpu -5; say "已 -5%，现在四簇：$(api conf_get | sed -n 's/^CPU_PCT_[A-Z0-9]*=//p' | tr '\n' ' ')" ;;
    6) say "重启守护…"; api restart; say "完成" ;;
    7) if [ -f "$DATA/pause" ]; then say "恢复限频…"; api resume; else say "暂停限频…"; api pause; fi ;;
    8) say "停止并恢复…"; api stop; say "已恢复到开机前状态" ;;
    9) api advice | sed -e 's/^---REC---$/    ------------/' \
         -e 's/^advice=up$/    建议 = 放宽一档/' \
         -e 's/^advice=down$/    建议 = 收紧一档/' \
         -e 's/^advice=hold$/    建议 = 维持不动/' \
         -e 's/^/    /' ;;
  esac
}

# ── 头部信息 ────────────────────────────────────────────────────
# 分隔线独占一行，标题不夹在横杠中间：部分管理器会把横杠两端的文字
# 当成行首标记解析，输出里就多出竖线或方框。
ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print "  Soc智能限频"
ui_print "━━━━━━━━━━━━━━━━━━━━"
say "机型：$(getprop ro.product.model 2>/dev/null)"
SOC=$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')
[ -z "$SOC" ] && SOC=$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')
say "SoC：$SOC"
ST=$(api status | sed -n 's/^state=//p' | head -n 1)
say "状态：${ST:-未知}（暂停标记 $([ -f "$DATA/pause" ] && echo 有 || echo 无)）"
ui_print ""

if ! can_use_keys; then
  # 少命令或读不到 api.sh：只打印等价命令，不进循环。
  # 文案要说清是"少命令"，不要说成"你这台机没有音量键"。
  ui_print "  这台机缺少 getevent / timeout（或读不到 api.sh），只列出等价命令："
  ui_print ""
  say "1 $DAEMON status"
  say "2 $DAEMON probe"
  say "3 sh $API log 30"
  say "4 sh $API conf_set CPU_PCT_BIG 70   （四簇同理）"
  say "6 $DAEMON restart"
  say "7 touch $DATA/pause   ／   rm -f $DATA/pause && $DAEMON restart"
  say "8 $DAEMON stop"
  say "9 sh $API advice"
  ui_print ""
  say "参数与 WebUI 一致，改完由守护巡检生效"
  exit 0
fi

ui_print "  音量＋ 切换选择项"
ui_print "  音量－ 执行（改动的操作还要再确认一次）"
ui_print "  90 秒无按键自动退出，什么都不会改"
ui_print ""

# 菜单只完整打一次，之后每按一次只补一行当前项：
# 管理器的「执行」输出是只能往下追加的文本区，没有"刷新"这个动作，
# 每次按键重打八行就是八屏重复，选到哪一行反而要往下翻。
# 真终端（[ -t 1 ] 为真）例外，那里 ANSI 清屏序列有人认。
print_menu() {
  _i=1
  while [ "$_i" -le "$NITEMS" ]; do
    if [ "$_i" = "$CUR" ]; then say "> $_i $(label "$_i")"; else say "  $_i $(label "$_i")"; fi
    _i=$((_i + 1))
  done
  ui_print ""
  say "（音量＋ 换一个，音量－ 选中）"
}

CUR=1
SEL=""
print_menu
# 这一行在不在，是"界面被关掉了"和"我们自己退出了"的分界
atrace "菜单已画出，开始等按键（上限 90s）"
while :; do
  # 90 秒：八行菜单要边看边想，30 秒不够
  K=$(key_wait 90)
  case "$K" in
    timeout) atrace "菜单超时退出"; ui_print ""; say "超时退出，未做任何改动"; exit 0 ;;
    other)   continue ;;
    spin*)
      # 读不到输入设备：直接说原因，不要伪装成"超时退出"
      ui_print ""
      say "${K#spin }"
      say "这台机器的管理器环境里用不了音量键，已退出，未做任何改动"
      exit 1 ;;
    up)
      CUR=$((CUR + 1)); [ "$CUR" -gt "$NITEMS" ] && CUR=1
      atrace "音量＋ → 选中 $CUR"
      if [ -t 1 ]; then
        printf '\033[H\033[2J'; print_menu
      else
        say "→ $CUR $(label "$CUR")"
      fi ;;
    down) SEL="$CUR"; atrace "音量－ 确认选中 $SEL"; break ;;
  esac
done

ui_print ""
say "已选中：$SEL $(label "$SEL")"

if readonly_item "$SEL"; then
  atrace "执行只读项 $SEL"
  do_item "$SEL"
  ui_print ""
  exit 0
fi

# 改动的操作再确认一次，默认方向是"取消"：20 秒不按键就退出，什么也不改。
# 这里不再画分隔线——输出是往下追加的，多画一次框就多一叠日志。
ui_print "  音量－ 确认执行 ／ 音量＋ 取消 ／ 20 秒不按键 = 取消"
# 确认阶段也认 "other"：别的键算"人在动"，重新问一次而不是替他取消。
# 方向仍然是安全的：重新问之后仍然只有明确的音量－才会真的执行。
while :; do
  C=$(key_wait 20)
  case "$C" in spin*) say "${C#spin }，已取消"; exit 1 ;; esac
  [ "$C" = "other" ] && continue
  break
done
case "$C" in
  down) atrace "确认，执行 $SEL"; say "执行中…" ;;
  up)   atrace "用户取消 $SEL"; say "已取消，未做任何改动"; exit 0 ;;
  *)    atrace "确认超时，取消 $SEL"; say "超时取消，未做任何改动"; exit 0 ;;
esac

do_item "$SEL"
ui_print ""
exit 0
