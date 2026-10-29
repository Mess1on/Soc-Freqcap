#!/bin/bash
# action.sh 的音量键状态机台架。
#
# 为什么值得单独一份：这个界面的全部输入就是两个键，
# 而在 PC 上、在管理器里、在装机日志里都看不到它的行为。
# 最容易出的三类事故都只能在这儿抓：
#   1) 松手（UP 事件）被当成第二次按键 —— 一次按下算两回，选不到想选的项；
#   2) 读不到输入设备时还在等 getevent —— 表现是"点执行手机卡住"；
#   3) 二次确认的默认方向写反 —— 超时变成"执行"而不是"取消"。
#
# 做法：拿假的 getevent 把剧本好的按键序列一条条吐出来，
# 再拿假的 date 让时钟一次性跳过 30 秒，把超时那条路也走一遍。
SRC=$(cd "$(dirname "$0")" && pwd) || exit 1
MOD=$(dirname "$SRC") || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

pass=0; fail=0
ok()  { if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
        else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }
has() { case "$1" in *"$2"*) echo yes ;; *) echo no ;; esac; }

# ── 沙箱：模块目录 + 数据目录，两者分开 ─────────────────────────
M="$T/mod"; D="$T/data"
mkdir -p "$M/webroot" "$D/run"
cp "$MOD/action.sh" "$M/action.sh"
cp "$MOD/webroot/api.sh" "$M/webroot/api.sh"
cp "$MOD/socfreq.conf" "$D/socfreq.conf"
cp "$MOD/games.txt" "$D/games.txt"
# 守护桩：action.sh 会直接执行 "$DAEMON probe"，也要能答 export/status/restart
cat > "$M/socfreqd" <<'EOF'
#!/bin/sh
case "$1" in
  export)  printf 'ver=test\nalive=1\nstate_line=1\ninterval=5\ngrace=10\nsoc=TESTSOC\nncores=8\npaused=0\npid=4321\nstate=normal\n' ;;
  probe)   printf 'probe: policy0 -> 1690000\n' ;;
  restart) printf 'restart ok\n' ;;
  stop)    printf 'stop ok\n' ;;
  status)  printf 'state=normal\npid=4321\n' ;;
  *)       exit 0 ;;
esac
EOF
chmod +x "$M/socfreqd"

# fake bin 目录
B="$T/bin"; mkdir -p "$B"

# getevent：按真机（一加 PJZ110，toybox）的样子写。
# 关键：**没有 -e 这个选项**。-le 会打印 usage 到 stderr 并返回失败。
# 拿 getevent -le 判"有没有音量键"在这台机上恒为假，
# 于是明明能读到 KEY_VOLUMEUP 的机器也走成"读不到音量键"。
# 这个桩按真机行为来：想用列表选项当闸门的话这里就红。
SEQ="$T/seq"
export SEQ T
cat > "$B/getevent" <<'EOF'
#!/bin/sh
case "$1" in
  -le|-ie|-pe)
    echo "getevent: invalid option ${1#-}" >&2
    echo "Usage: getevent [-t] [-n] [-s switchmask] [-S] [-v mask] [-d] [-p] [-i] [-l] [-q] [-c count] [-r] [device]" >&2
    exit 1 ;;
esac
# -lqc 1：从 $SEQ 队列里取下一行。队列空了就什么都不输出（这台机此刻没按键）。
if [ -f "$SEQ" ]; then
  _n=$(cat "$T/cnt" 2>/dev/null || echo 0)
  _n=$((_n + 1)); echo "$_n" > "$T/cnt"
  sed -n "${_n}p" "$SEQ"
fi
exit 0
EOF

# timeout：真 timeout 的语义是"命令到点还没结束就被杀，退出码 124"。
# 假的 getevent 演的是"这台机此刻没人按键"—— 它输出空、退出 0，
# 而**真的 getevent 在这种情况下根本不会退出**，是 timeout 把它杀掉的。
# 所以这里要把"成功但没输出"翻译成 124：
# 不然 action.sh 会把"没人按键"读成"读不到输入设备"(noev) 直接退出，
# 超时那几条用例测的就不是超时，而是 noev 那一支。
# 命令自己失败（退出码非 0）时如实转给它 —— 那才是 noev 该抓的形态。
cat > "$B/timeout" <<'EOF'
#!/bin/sh
shift
out=$("$@" 2>/dev/null); _rc=$?
[ "$_rc" -eq 0 ] || exit "$_rc"
[ -n "$out" ] || exit 124
printf '%s\n' "$out"
EOF

# date：每次"用于比较的"取法走 5 秒。
# 不能用"一次跳 60 秒"那种跳法：key_wait 每轮要取两次时间
# （开头算 _t0、循环里算 _now），一跳就超时的话菜单阶段还没来得及读剧本就已经"过期"，
# 测出来的永远是第一项超时，而不是想测的那个阶段的超时。
# 每轮 5 秒则等于"按键之间隔了几秒"，90 秒预算够读十几条剧本，
# 剧本读完之后再空转几轮才判超时——两条路都能各测各的，而且 fork 次数少，
# Git Bash 上一个 fork 几十毫秒，1 秒一步会把整组台架拖到 run_act 那个 20 秒杀手前面。
#
# 硬约束：**自证日志不许吃时钟**。
# action.sh 每行 atrace 也要取一次时间（带 %H 的那种取法）。
# 让它同样推进虚拟时钟的话，"按四下选第 4 项"这类剧本会先被日志自己把
# 90 秒预算耗光 —— 台架里看到的"超时"到底是被测代码的还是日志的，就分不开了。
# 所以下面对 *%H* 的取法**原样回报当前时刻、一步都不走**。
cat > "$B/date" <<'EOF'
#!/bin/sh
if [ "${FIXEDDATE:-0}" = 1 ]; then
  exec /bin/date "$@"
fi
_n=$(cat "$T/tnow" 2>/dev/null || echo 0)
case "$1" in
  *%H*) echo "$_n 00:00:$_n"; exit 0 ;;
esac
if [ "${FIXEDDATE:-0}" = 1 ]; then exec /bin/date "$@"; fi
# FROZENDATE=1 时时钟冻在 0 不动。这是唯一能让"空转"那条判据在台架里被走到的一档：
# 正常那一档每取一次时间就跳 5 秒，读数还没攒够就已经"超时"了，
# 于是空转的分支永远测不到——而它正是这一版新加的取证手段。
if [ "${FROZENDATE:-0}" = 1 ]; then echo 0; exit 0; fi
_n=$((_n + 5)); echo "$_n" > "$T/tnow"
echo "$_n"
EOF

# sleep：**只桩掉小数秒**。台架里 0.05 那种节奏睡眠只拖时间，
# 被测逻辑不依赖真实流逝的秒（超时用的是 date，空转用的是读数次数）。
# 但整数秒必须走真 sleep —— run_act 那个"卡住就杀掉"的护栏用的就是 sleep 45，
# 全桩掉的话它当场把 action.sh 杀掉，整组会红成一片"什么都没输出"，
# 看着像脚本坏了，其实是测试自己把被测进程打死了。
cat > "$B/sleep" <<'EOF'
#!/bin/sh
case "$1" in
  *.*) exit 0 ;;
esac
exec /bin/sleep "$@"
EOF

# getprop
cat > "$B/getprop" <<'EOF'
#!/bin/sh
echo "TESTMODEL"
EOF

chmod +x "$B"/*
# 一份"没有 getevent"的假 PATH：用来测退回打印那一支。
# 不能用环境变量开关让假 getevent 自己退出——action.sh 查的是
# command -v getevent（命令在不在），不是它跑得快不快。
ORIG_PATH="$PATH"
B2="$T/bin_nogevent"; mkdir -p "$B2"
for _f in "$B"/*; do
  case "$(basename "$_f")" in getevent) continue ;; esac
  cp "$_f" "$B2/"
done
chmod +x "$B2"/* 2>/dev/null
export PATH="$B:$PATH" T

# 把绝对数据目录换成沙箱（action.sh 里只用来判断 pause 标记是否存在）
sed -e "s#^DATA=/data/adb/soc_freqcap#DATA=$D#" "$M/action.sh" > "$M/action.t" && mv "$M/action.t" "$M/action.sh"

# 跑一次 action.sh：$1 = 剧本（一行一个 getevent 输出），$2 = 额外的假环境（形如 A=1 B=2）
# PATH 单独走 USE_PATH：Windows 的 PATH 里有空格，把 "PATH=..." 当 $2 传进来
# 会被 word splitting 切成好几个参数，env 直接报错、输出为空，
# 看着像 action.sh 坏了，其实是测试自己的引号问题。
run_act() {
  printf '%s\n' "$1" > "$T/seq.raw"
  grep -v '^[[:space:]]*$' "$T/seq.raw" > "$SEQ" 2>/dev/null || : > "$SEQ"
  echo 0 > "$T/cnt"; echo 0 > "$T/tnow"
  : > "$T/out"
  # 整体超时护栏：剧本没给够按键时 key_wait 会永远等下去
  # （这里的假 date 不走时，action.sh 自己那 90 秒超时不会触发），
  # 所以每次跑都挂一个后台杀手，还没结束就判它"卡住"。
  # 45 秒而不是 20 秒：这台机的 Git Bash 一个 fork 几十毫秒，
  # "执行 +5%"那一条要连开八九趟 api.sh，机器忙的时候真跑过 20 秒被杀掉 ——
  # 表现是配置改了、屏幕上最后一行却没写出来，看着像 action.sh 的 bug。
  env SOCFREQ_DATA_DIR="$D" T="$T" SEQ="$SEQ" \
      PATH="${USE_PATH:-$B:$ORIG_PATH}" $2 \
    sh "$M/action.sh" >> "$T/out" 2>&1 &
  _p=$!
  ( sleep 45; kill -9 "$_p" 2>/dev/null ) &
  _w=$!
  # wait 一定要在当前 shell 里等。写成 _st=$(wait $_p; echo $?) 的话，
  # 命令替换开的是子 shell，子 shell 里这个 pid 不是它的子进程，
  # wait 立刻返回"not a child"，脚本这边还没跑完就去 cat 输出了 ——
  # 表现是所有断言拿到空串、整片红，而且看起来像 action.sh 坏了。
  wait "$_p"; _st=$?
  kill "$_w" 2>/dev/null; wait "$_w" 2>/dev/null
  [ "$_st" -gt 128 ] && echo "!!! action.sh 跑了 20 秒还没结束（被杀手干掉）" >> "$T/out"
  cat "$T/out"
}

UP='  EV_KEY 0072 (KEY_VOLUMEUP) DOWN'
UPREL='  EV_KEY 0072 (KEY_VOLUMEUP) UP'
DN='  EV_KEY 0073 (KEY_VOLUMEDOWN) DOWN'
DNREL='  EV_KEY 0073 (KEY_VOLUMEDOWN) UP'
OTHER='  EV_KEY 001c (KEY_ENTER) DOWN'

echo "== 0 真机回归：getevent 没有 -e 选项也必须能进菜单 =="
# 一加 PJZ110 上量到的：toybox 的 getevent 不支持 -e，
# 拿 getevent -le 当"有没有音量键"的闸门会探测恒假，
# 于是有音量键的机器也只能看到"读不到音量键"。
# 假 getevent 现在照真机的样子对 -le 报 usage 并退出 1，
# 这一条测的就是：菜单照样出来、按键照样能用。
OUT=$(run_act "$DN")
ok "$(has "$OUT" "读不到音量键")" "no" "不再因为 -le 失败就退回打印分支"
ok "$(has "$OUT" "invalid option")" "no" "getevent 的 usage 不渗进页面输出"
ok "$(has "$OUT" "已选中：1")" "yes" "音量－ 照样能选中第 1 项（菜单是活的）"

echo "== 1 没有 getevent：只打印命令，不进循环 =="
USE_PATH="$B2:$ORIG_PATH"
OUT=$(run_act "")
USE_PATH=""
ok "$(has "$OUT" "缺少 getevent")" "yes" "退回到'列出等价命令'那一支"
ok "$(has "$OUT" "conf_set")" "yes" "打印的命令里带上能照抄的 conf_set"
ok "$(has "$OUT" "你这台机没有音量键")" "no" "文案不许说成'机器没有音量键'（那是误报）"

echo "== 1b 相对路径调用也要能进菜单 =="
# 真机上就是这么撞到的：cd 到目录再 sh action.sh 时 $0 不带斜杠，
# ${0%/*} 剥不出目录 → api.sh 判成"读不到" → 留下一句"缺少 getevent"的假话。
USE_PATH="$B:$ORIG_PATH"
T0="$T/relcase"; mkdir -p "$T0/webroot"
cp "$M/action.sh" "$T0/"; cp "$MOD/webroot/api.sh" "$T0/webroot/"
cp "$M/socfreqd" "$T0/" 2>/dev/null
: > "$T/seq"; echo 0 > "$T/cnt"; echo 0 > "$T/tnow"
env SOCFREQ_DATA_DIR="$D" T="$T" SEQ="$SEQ" PATH="$B:$ORIG_PATH" \
  sh -c "cd '$T0' && timeout 6 sh action.sh" > "$T/out2" 2>&1
ok "$(has "$(cat "$T/out2")" "缺少 getevent")" "no" "相对调用时不再误报缺少 getevent"
ok "$(has "$(cat "$T/out2")" "音量＋ 切换选择项")" "yes" "相对调用也进到了菜单"
ok "$(has "$(cat "$T/out2")" "action.sh/socfreqd")" "no" "输出里没有 action.sh/... 这种拼错的路径"

echo "== 2 一次松手不能算第二次按键 =="
# 按下+松手应该只让高亮项走一格（1 -> 2）
OUT=$(run_act "$UP
$UPREL
$DN")
ok "$(has "$OUT" "已选中：2")" "yes" "按下+松手 = 前进一格（UP 事件被丢掉）"
ok "$(has "$OUT" "封顶预览")" "yes" "选中的是第 2 项"

echo "== 2b 菜单只完整打一次，移动只补一行 =="
# 管理器的「执行」输出是只能往下追加的文本区，不是终端。
# 每次按键都重打一遍八项菜单的话：按八下就是八屏重复，
# "现在选到哪一行"反而要往下翻着找。
N=$(printf '%s\n' "$OUT" | grep -c "查看运行状态")
ok "$N" "1" "整份菜单只出现一次（实到 $N 次）"
ok "$(has "$OUT" "→ 2 封顶预览")" "yes" "移动时补的是单独一行当前项"
# 连按三下：三行当前项，菜单仍然只有一份
OUT3=$(run_act "$UP
$UP
$UP
$UPREL")
ok "$(printf '%s\n' "$OUT3" | grep -c "查看运行状态")" "1" "连按三下菜单也不重打"
ok "$(printf '%s\n' "$OUT3" | grep -c '^  → ')" "3" "三行移动记录（→2 →3 →4）"
ok "$(has "$OUT3" "→ 4 CPU 封顶 +5%")" "yes" "最后一行就是当前选择"

echo "== 3 超时必须'未做任何改动' =="
OUT=$(run_act "")
ok "$(has "$OUT" "超时退出，未做任何改动")" "yes" "菜单超时退出（假时钟每步 5 秒，空转到 90 秒）"

echo "== 4 只读项直接执行，不弹二次确认 =="
OUT=$(run_act "$DN")
ok "$(has "$OUT" "已选中：1")" "yes" "音量－ 直接选中当前项"
ok "$(has "$OUT" "确认执行")" "no" "只读项不该有二次确认"

echo "== 5 改动项：确认超时 = 取消 =="
# 音量＋ ×3 → 第 4 项（CPU +5%），音量－ 选中，然后确认阶段什么都不按
ok "$(grep -c '^CPU_PCT_BIG=65' "$D/socfreq.conf")" "1" "执行前 CPU_PCT_BIG 是 65"
OUT=$(run_act "$UP
$UP
$UP
$DN")
ok "$(has "$OUT" "已选中：4")" "yes" "走到第 4 项（改动类）"
ok "$(has "$OUT" "超时取消")" "yes" "确认阶段超时按'取消'处理（默认方向是安全的）"
ok "$(grep -c '^CPU_PCT_BIG=65' "$D/socfreq.conf")" "1" "取消之后配置一个字节都没改"

echo "== 6 改动项：音量＋ 取消 =="
OUT=$(run_act "$UP
$UP
$UP
$DN
$UP")
ok "$(has "$OUT" "已取消，未做任何改动")" "yes" "确认阶段音量＋ = 取消"
ok "$(grep -c '^CPU_PCT_BIG=65' "$D/socfreq.conf")" "1" "取消之后配置仍未变"

echo "== 7 改动项：确认执行，四簇同步 +5% =="
OUT=$(run_act "$UP
$UP
$UP
$DN
$DN")
ok "$(has "$OUT" "已 +5%")" "yes" "走到了执行分支"
ok "$(sed -n 's/^CPU_PCT_BIG=//p' "$D/socfreq.conf")" "70" "CPU_PCT_BIG 65 -> 70"
ok "$(sed -n 's/^CPU_PCT_LITTLE=//p' "$D/socfreq.conf")" "70" "小核同步 +5（不是只动一簇）"
ok "$(sed -n 's/^GPU_PCT=//p' "$D/socfreq.conf")" "45" "GPU 没被牵连"

echo "== 8 加减都要夹在合法区间内 =="
# 上限：98 +5 = 103，api.sh 的校验会把这一句整个拒掉，
# 于是"按了没反应"，用户继续按、每次都失败一次。脚本自己先夹到 100。
sed -i 's/^CPU_PCT_BIG=.*/CPU_PCT_BIG=98/; s/^CPU_PCT_MID2=.*/CPU_PCT_MID2=98/;
        s/^CPU_PCT_MID=.*/CPU_PCT_MID=98/;  s/^CPU_PCT_LITTLE=.*/CPU_PCT_LITTLE=98/' "$D/socfreq.conf"
run_act "$UP
$UP
$UP
$DN
$DN" >/dev/null
ok "$(sed -n 's/^CPU_PCT_BIG=//p' "$D/socfreq.conf")" "100" "98 +5 夹到 100（不是写成 103 被拒）"
# 下限：留 20%，命令行连按几下不至于把机器压成 1%
sed -i 's/^CPU_PCT_BIG=.*/CPU_PCT_BIG=22/; s/^CPU_PCT_MID2=.*/CPU_PCT_MID2=22/;
        s/^CPU_PCT_MID=.*/CPU_PCT_MID=22/;  s/^CPU_PCT_LITTLE=.*/CPU_PCT_LITTLE=22/' "$D/socfreq.conf"
run_act "$UP
$UP
$UP
$UP
$DN
$DN" >/dev/null      # 第 5 项 = CPU -5%
ok "$(sed -n 's/^CPU_PCT_BIG=//p' "$D/socfreq.conf")" "20" "22 -5 夹到 20，不会一按就到底"

echo "== 9 暂停 / 恢复 toggle =="
OUT=$(run_act "$UP
$UP
$UP
$UP
$UP
$UP
$DN
$DN")
ok "$(has "$OUT" "暂停限频")" "yes" "没有 pause 文件时执行暂停"
ok "$([ -f "$D/pause" ] && echo yes)" "yes" "pause 落在数据目录（守护查的就是这里）"
OUT=$(run_act "$UP
$UP
$UP
$UP
$UP
$UP
$DN
$DN")
ok "$(has "$OUT" "恢复限频")" "yes" "已有 pause 时同一项变成恢复"
ok "$([ -f "$D/pause" ] && echo yes || echo no)" "no" "恢复把 pause 删掉了"

echo "== 10 未知事件不能打断等待，也不能算按键 =="
# 中间混进别的键的 DOWN、还有音量键的 UP（松手），都应该被丢掉，
# 高亮项一格都不动 —— 于是最后那下音量－ 选中的还是第 1 项。
OUT=$(run_act "$OTHER
$UPREL
$OTHER
$DN")
ok "$(has "$OUT" "已选中：1")" "yes" "别的键和松手事件都只被当作'没发生'"

echo "== 11 按别的键算'有人在动'，不该把界面耗走 =="
# 只让音量键重置空闲计时的话：先按电源键亮屏、或者误按了别的键，
# 那一下被丢掉、时钟照走，人慢慢看完八行菜单就"超时退出"。
# 现在任何 EV_KEY 都算活动 —— 这里喂"电源键 → 音量＋ → 音量－"，
# 应该正常走到第 2 项被选中，而不是中途退出。
PWR='  EV_KEY 0074 (KEY_POWER) DOWN'
OUT=$(run_act "$PWR
$UP
$DN")
ok "$(has "$OUT" "已选中：2")" "yes" "电源键之后音量键照样好使（选到第 2 项）"
ok "$(has "$OUT" "超时退出")" "no" "没有被'超时退出'吃掉"
# 确认阶段同理：按了别的键重新问一次，而不是替他取消
# （三下音量＋ → 第 4 项 CPU +5%）
OUT=$(run_act "$UP
$UP
$UP
$DN
$PWR
$DN")
ok "$(has "$OUT" "已取消")" "no" "确认阶段遇到别的键不算取消"
ok "$(has "$OUT" "已 +5%")" "yes" "别的键之后仍可确认执行（这里走到了 +5% 分支）"

echo "== 12 自证日志：出事之后能查出是谁动的手 =="
# 用户反馈"大概 15 秒，不管有没有按都退出"。屏幕上分不出
# "我们自己的 90 秒超时 / getevent 一直失败在空转 / 管理器把脚本杀了"这三种情况，
# 而三家的改法完全不同 —— 所以每一步往守护日志里写一行。
# 这一组测的就是那条通道本身：没有它，下一次反馈照样只能猜。
: > "$D/socfreqd.log"
OUT=$(run_act "$DN")
LOG=$(cat "$D/socfreqd.log" 2>/dev/null)
ok "$(has "$LOG" "action: 开始")" "yes" "开场一行，带 getevent/timeout 在不在"
ok "$(has "$LOG" "菜单已画出")" "yes" "菜单真的画出来了（日志断在这行之后=被 SIGKILL）"
ok "$(has "$LOG" "音量－ 确认选中 1")" "yes" "按键被记下来了"
ok "$(has "$LOG" "执行只读项 1")" "yes" "走到哪一项写清楚了"
ok "$(has "$LOG" "结束 rc=0")" "yes" "正常退出也有收尾（缺这行就是被外面杀的）"
# 超时那一支额外带"读数次数"：正常 90 秒该有几十次，
# 次数远小于用时 = getevent 在立刻返回 = 空转，而不是真等了 90 秒。
: > "$D/socfreqd.log"
run_act "" >/dev/null
LOG=$(cat "$D/socfreqd.log" 2>/dev/null)
ok "$(has "$LOG" "超时 lim=90s 读数=")" "yes" "超时带读数次数（空转与真等待分得开）"
ok "$(has "$LOG" "菜单超时退出")" "yes" "超时退出的那一步也记了"
# getevent 每次都立刻返回空 = 读不到 /dev/input（管理器的执行环境里常见）。
# 判据不再是退出码：管道里 $? 取到的是最后一个命令的状态，永远是 0，
# 现在看用时对不对得上读数次数：时钟冻住、读数一路涨，就该报空转而不是等满 90 秒。
: > "$D/socfreqd.log"
cat > "$B/getevent" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$B/getevent"
OUT=$(run_act "" "FROZENDATE=1")
LOG=$(cat "$D/socfreqd.log" 2>/dev/null)
ok "$(has "$OUT" "读不到 /dev/input")" "yes" "屏幕上直接说读不到输入设备，不伪装成超时"
ok "$(has "$LOG" "空转：")" "yes" "日志里写了空转和真实的读数次数"
ok "$(has "$LOG" "结束 rc=1")" "yes" "这支的收尾码是 1，和屏幕上那句已退出对得上"
ok "$(has "$OUT" "超时退出，未做任何改动")" "no" "空转不能被说成「超时退出」（那是另一种病因）"
# 还原剧本桩，别影响复用同一沙箱的其它用例
cat > "$B/getevent" <<'EOF'
#!/bin/sh
case "$1" in
  -le|-ie|-pe)
    echo "getevent: invalid option ${1#-}" >&2
    echo "Usage: getevent [-t] [-n] [-s switchmask] [-S] [-v mask] [-d] [-p] [-i] [-l] [-q] [-c count] [-r] [device]" >&2
    exit 1 ;;
esac
if [ -f "$SEQ" ]; then
  _n=$(cat "$T/cnt" 2>/dev/null || echo 0)
  _n=$((_n + 1)); echo "$_n" > "$T/cnt"
  sed -n "${_n}p" "$SEQ"
fi
exit 0
EOF
chmod +x "$B/getevent"

echo "== 13 输出管道断了不能死 =="
# 真机日志给出来的结论：管理器的输出区到一定量就不再读管道，之后任何一次
# ui_print 都拿到 SIGPIPE，默认动作是整个脚本死掉 ——
# 屏幕上就是"按键还在按，十几秒界面没了"，而日志停在"结束 rc=141 因为是 PIPE"。
# 这一条测的是：收到 SIGPIPE 只记一笔，然后继续跑到它本来该去的地方。
: > "$D/socfreqd.log"
: > "$SEQ"; echo 0 > "$T/cnt"; echo 0 > "$T/tnow"
env SOCFREQ_DATA_DIR="$D" T="$T" SEQ="$SEQ" FROZENDATE=1 PATH="$B:$ORIG_PATH" \
  sh "$M/action.sh" > "$T/out13" 2>&1 &
_P13=$!
/bin/sleep 1                       # 让它先进入读数循环（这里必须用真 sleep）
kill -PIPE "$_P13" 2>/dev/null     # 模拟管理器把读端关掉
wait "$_P13" 2>/dev/null
LOG=$(cat "$D/socfreqd.log" 2>/dev/null)
ok "$(has "$LOG" "输出管道已断")" "yes" "SIGPIPE 只记一行日志"
ok "$(has "$LOG" "空转：")" "yes" "记完之后继续跑到下一个判据（没被管道打死）"
ok "$(has "$LOG" "结束 rc=141")" "no" "收尾码不能是 141（那是被 SIGPIPE 打死）"

echo
if [ "$fail" -eq 0 ]; then echo "音量键台架：全部通过 ($pass)"; else echo "音量键台架：$fail 项失败 / $pass 项通过"; fi
exit "$fail"
