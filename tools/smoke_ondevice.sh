#!/system/bin/sh
# 真机冒烟：PC 上的测试替不掉这一份。
#
# 这一路上已经栽过两次：
#  * CRLF 的行尾匹配不上——PC 上 MSYS 替 grep 翻译了行尾，永远绿；
#  * mktemp 在真机上落到不存在的 /tmp——PC 上 /tmp 一直在，永远绿。
# 两类问题都只在设备上才成立，所以要在设备上跑一次最小验证。
#
# 用法（需要 root）：
#   adb push tools/smoke_ondevice.sh /data/local/tmp/
#   su -c '/data/local/tmp/smoke_ondevice.sh'
#   su -c '/data/local/tmp/smoke_ondevice.sh /data/adb/ap/modules/soc_freqcap'   # APatch
#
# 目录看 module.prop 里的 id，不是模块显示名：安装后是 soc_freqcap，
# 不是 Soc智能限频。写错的话五项会一起红，而且报的是"名单匹配不上"
# 这类下游症状，会把人往错方向带。
#
# 只查八件事，任何一件不合格就非零退出——不修任何东西，只报告。
#
# 注意调用方式：socfreqd 是 ELF，要直接执行，不能写成 sh "$D/socfreqd"。
# shell 对脚本操作数是"读进来当命令解析"，不是 execve，
# 拿 ELF 去解析只会得到 cannot execute binary file，stdout 一行都没有。
# 用 sh 包着的结果是空字符串，于是下面几项恒空判断——
# 就算代码全对也报 FAIL，看起来像坏了，其实是根本没执行。
# api.sh 确实是脚本，那几处 sh "$API" 是对的，别一起改掉。

# 配置搬到模块目录外面了（覆盖安装不会碰它），
# 冒烟脚本必须跟着搬：还在读 "$D/games.txt" 的话，
# 名单明明好好的，几项会一起报"读不到"。
# 老装机第一次跑这一版时 /data/adb/soc_freqcap 可能还是空的
#（迁移在 customize.sh / api.sh 里做，未必已经触发），
# 所以留一条回退到模块目录的分支——两处都有时以 DATA 为准，
# 那才是守护真正在读的那一份。
D=${1:-/data/adb/modules/soc_freqcap}
# 这里故意不跟 SOCFREQ_DATA_DIR：那是 api.sh 的测试钩子，
# 守护侧 SF_DATADIR 是编译期的，不读环境变量。
# 冒烟要验的是"守护真正在用哪份配置"，跟着钩子走会验到一个守护根本不读的路径。
DATA=/data/adb/soc_freqcap
API="$D/webroot/api.sh"
BAD=0

# 名单路径要在每次读之前重新解析，不能开头算一次就存下来。
# 原因是 api.sh 自己带迁移：第一次调它会把模块目录里的 games.txt
# 用 mv 搬去 DATA。开头那次解析如果拿到了模块目录的旧路径，
# 等真去数行/数 CR 时文件已经被搬走了——读到空，
# 报出来的却是"COUNT 与条数对不上"，看着像名单坏了，其实是脚本自己的顺序问题。
pick_list() {
  [ -f "$DATA/games.txt" ] && { echo "$DATA/games.txt"; return; }
  echo "$D/games.txt"
}

# 目录不对就当场说清楚，别再往下跑：api.sh 不存在时，
# 后面的 sh 会报 command not found，然后被 2>/dev/null 吞掉，
# 最后只剩一句莫名其妙的 FAIL。越早退出越好。
if [ ! -d "$D" ]; then
  echo "FAIL 找不到模块目录：$D"
  echo "     这个目录名取自 module.prop 的 id，不是模块显示名。"
  echo "     用法：$0 [/data/adb/modules/soc_freqcap]"
  exit 1
fi

say() { echo "  $1"; }
bad() { echo "  FAIL $1"; BAD=$((BAD+1)); }

echo "== 1. WebUI 能否列游戏 =="
# 列不出任何一行是最典型的症状：名单几百条，页面显示"本机已装 0 个"。
# 这类静默失败不会让命令失败，所以必须单独数行数。
N=$(sh "$API" games_list 2>/dev/null | grep -c .)
if [ "$N" -gt 0 ]; then
  say "games_list 输出 $N 行"
else
  # "列不出"有两个原因，症状一样，处理方式完全相反：
  #   a) 名单文件本身空了/读不到——装包时就坏了，和本机装了什么游戏无关；
  #   b) 名单是好的，只是这台机上一个都没装——完全正常，不用管。
  # 合在一起报，会把 b 当成 a 去排查，绕一大圈。
  # games_list all 绕过 pm 直接吐名单，是唯一能分开这两者的手段。
  ALL=$(sh "$API" games_list all 2>/dev/null | grep -c .)
  if [ "$ALL" -gt 0 ]; then
    say "名单本身有 $ALL 条，本机已装 0 个——没装游戏，正常"
    # 名单是好的，但一个都没匹配上，还是值得看一眼行尾。
    # 这条是"安装期那次 tr 真的跑了"的唯一证据：
    # CRLF 在 PC 上永远测不出来（MSYS 会替 grep 翻译行尾），
    # 只有在设备上直接看 games.txt 里还有没有 CR 才算数。
    #
    # 用 tr -cd '\r' | wc -c 数 CR 的个数。
    # 不用 grep -c 数行数：grep 一个 CR 都没有时退出码是 1（没匹配上），
    # `|| echo 0` 于是又吐一个 0，变量成了 "0\n0"——两行、还不是整数。
    # wc -c 成功时退出码 0，不会触发 || 分支，永远是一行一个整数。
    LIST=$(pick_list)
    CR=$(tr -cd '\r' < "$LIST" 2>/dev/null | wc -c | tr -d ' ')
    [ -n "$CR" ] || CR=0
    if [ "$CR" -eq 0 ]; then
      say "games.txt 没有残留 CR（安装期规范化生效）"
    else
      bad "games.txt 里还有 $CR 个 CR——安装期那次 tr 没生效，装机脚本改过吗"
    fi
  else
    LIST=$(pick_list)
    bad "games_list all 也是空的 → 名单文件本身为空或不可读，不是匹配问题"
    echo "       读的是 $LIST"
    echo "       games.txt：$(ls -l "$LIST" 2>&1 | head -1)"
    echo "       直连接口原样输出："
    sh "$API" games_list all 2>&1 | head -5 | sed 's/^/       /'
  fi
fi

echo "== 2. about 的 COUNT 与名单条数一致 =="
C=$(sh "$API" about 2>/dev/null | sed -n 's/^COUNT=//p')
# 用 awk 数，不要用 `grep -cv ... || echo 0`。
# 那个写法在计数为 0 时会吐两行：grep 没匹配到任何行，退出码是 1，
# || 分支又吐一个 0，命令替换把两行都收进去，变量成了 "0\n0"。
# 麻烦就在这儿：只有名单真的空了/全是注释时才会遇到，
# 而那正是这一项要抓的场景——结果报出来的是
# "COUNT='0' 与 games.txt 实际 0<换行>0 条对不上"，
# 数字看着还对，多出来那一行把人往另一个坑里带。
# awk 的 END 总会跑、打印整数、退出码恒 0，永远是一行一个数。
LIST=$(pick_list)
REAL=$(awk '!/^[[:space:]]*$/ && !/^[[:space:]]*#/{n++} END{print n+0}' "$LIST" 2>/dev/null)
[ -n "$REAL" ] || REAL=0
if [ -n "$C" ] && [ "$C" = "$REAL" ]; then
  say "COUNT=$C 与 games.txt 条数一致"
else
  bad "COUNT='$C' 与 games.txt 实际 $REAL 条对不上"
fi

# COUNT 只是数行数，CRLF 下它照样对——真正会错的是逐行匹配，
# 所以还要确认关于页那段数据不是空的。
sh "$API" about 2>/dev/null | grep -q -- '---PROP---' \
  && say "about 带着 ---PROP--- 分隔符" \
  || bad "about 里没有 ---PROP---（关于页会空白）"

echo "== 3. 守护认得 SoC =="
S=$("$D/socfreqd" export 2>/dev/null | sed -n 's/^soc=//p')
if [ -n "$S" ] && [ "$S" != "0" ]; then
  say "soc=$S"
else
  bad "soc='$S'（读不到 SoC 型号，限频分档会退化成默认值）"
fi

echo "== 4. 逐核频率表（修没修好在这里一眼可判）==="
# 这三条对应三个独立的坑，都是"不报错、只是少东西"的类型，
# 光看 socfreqd export 的输出容易滑过去，得逐条钉死。
EX=$("$D/socfreqd" export 2>/dev/null)
printf '%s\n' "$EX" | grep -q '^ncores=[1-9]' \
  || bad "ncores=0：逐核路径没读到（CPUFREQ 是 policy 目录，里面没有 cpuN）"
printf '%s\n' "$EX" | grep -q '^coremax_0=[1-9]' \
  || bad "coremax_0 为 0：CPUBASE 的逐核路径又拼错了"
printf '%s\n' "$EX" | grep -Eq '^corepol_[0-9]+=[0-9]' \
  || bad "corepol 全是 -1：readlink 失败（多半是路径带了尾斜杠）"
N=$(printf '%s\n' "$EX" | sed -n 's/^ncores=//p')
[ -n "$N" ] && [ "$N" != "0" ] \
  && say "逐核表：ncores=$N coremax_0=$(printf '%s\n' "$EX" | sed -n 's/^coremax_0=//p') corepol_0=$(printf '%s\n' "$EX" | sed -n 's/^corepol_0=//p')"

echo "== 5. 新键接上了 + 守护读的是搬家后的配置 =="
# screen_uncap / boot_full 是这一版新增的两项。export 里少一行通常不是
# printf 忘了写，而是 load_cfg 的解析没跟上（键名拼错、clampi 区间写反），
# 表现是 WebUI 存了值但守护永远用默认——页面上一切正常，只有行为不对。
printf '%s\n' "$EX" | grep -q '^screen_uncap=' \
  || bad "export 里没有 screen_uncap（熄屏解除没接上）"
printf '%s\n' "$EX" | grep -q '^boot_full=' \
  || bad "export 里没有 boot_full（开机满血没接上）"
printf '%s\n' "$EX" | grep -q '^nmode=' \
  || bad "export 里没有 nmode（分应用档位没接上）"
printf '%s\n' "$EX" | grep -q '^learn=' \
  || bad "export 里没有 learn（智能学习的开关没接上，页面那个开关是空的）"

# 档位这条只读不动用户的 app_modes.conf：真机上"顺手加一条再删掉"
# 会在守护的一个巡检周期里把那个应用按测试包名改档，
# 而且删除失败就留下一条谁都看不懂的残留。
ML=$(sh "$API" modes_list 2>&1); MLRC=$?
[ "$MLRC" -eq 0 ] || bad "modes_list 退出码 $MLRC（一条档位都没设过时就该安静返回）"
sh "$API" about 2>/dev/null | grep -q '^NMODE=' \
  || bad "about 里没有 NMODE（关于页看不出用户设过几条档位）"
# 搬家后的配置必须是守护真正读的那一份。
# 判据：conf 里的 CPU_PCT_BIG 要等于 export 的 pct_big。
# 守护要是还在读模块目录里的旧副本（SF_DATADIR 和 api.sh 的 DATA 不一致、
# 或迁移没跑），这里就会对不上——而且差的是"用户改成 40、实际跑 65"，
# 页面上滑杆位置、保存回执全是好的，只有 sysfs 不配合。
# 只在 conf 值是纯数字时比：留空或写了注释时守护本来就该走 fallback。
CONF="$DATA/socfreq.conf"
if [ -f "$CONF" ]; then
  CB=$(sed -n 's/^CPU_PCT_BIG=//p' "$CONF" 2>/dev/null | head -n 1 \
      | sed 's/[[:space:]]*#.*$//; s/^[[:space:]]*//; s/[[:space:]]*$//')
  PB=$(printf '%s\n' "$EX" | sed -n 's/^pct_big=//p')
  case "$CB" in
    ''|*[!0-9]*) say "conf 里 CPU_PCT_BIG='$CB'（非纯数字，跳过一致性比对）" ;;
    *) if [ "$CB" = "$PB" ]; then
         say "守护读的是 $CONF（CPU_PCT_BIG=$CB = pct_big）"
       else
         bad "CPU_PCT_BIG=$CB 但 pct_big=$PB → 守护没读搬家后的配置"
       fi ;;
  esac
else
  bad "$CONF 不存在（迁移没跑成：覆盖安装会拿默认值盖掉用户配置）"
fi

echo "== 6. 守护活着 =="
ST=$("$D/socfreqd" status 2>/dev/null | sed -n 's/^state=//p')
if [ -n "$ST" ]; then
  say "state=$ST"
else
  bad "status 没有 state=（守护没跑起来，或 ELF 被 loader 拒了）"
  "$D/socfreqd" status 2>&1 | head -3 | sed 's/^/       /'
fi

echo "== 7. 应用名与声明的图标（APK 解析只能在这一步验）=="
# AXML / resources.arsc 那两份读法在 PC 上是拿 aapt2 对答案验的，
# 到这里验的是**这台机上的真包能不能读出来**——酷安那种把资源名缩短过的包
# 只有上了真机才知道。这一节一条都不合格也别慌：
# 名字读不出来时页面只是退回显示包名，不影响限频本身。
PKGS=$(sh "$API" apps_list 2>/dev/null | tr -d '\r' | head -n 6)
if [ -z "$PKGS" ]; then
  say "没读到第三方包，这一节跳过（不算不合格）"
else
  HIT=0
  for p in $PKGS; do
    AP=$(pm path "$p" 2>/dev/null | sed -n 's/^package://p' | head -n 1)
    [ -n "$AP" ] || continue
    NM=$("$D/socfreqd" label "$AP" 2>/dev/null | head -n 1)
    IC=$("$D/socfreqd" iconpath "$AP" 2>/dev/null | head -n 1)
    say "$p → 名字[${NM:-无}] 声明图标[${IC:-无}]"
    [ -n "$NM" ] && HIT=$((HIT + 1))
    case "$IC" in
      '') : ;;                                # 矢量图标/读不出：页面会退回打分那一轮
      res/*) : ;;
      /*) bad "iconpath 给的该是包内相对路径，实到 [$IC]" ;;
    esac
  done
  # 一个名字都没有 = 解析在这台机上整个失效（多半是某个偏移和这台机的打包方式对不上）
  if [ "$HIT" -gt 0 ]; then
    say "$HIT/$(printf '%s\n' $PKGS | grep -c .) 个包读出了应用名"
  else
    bad "$PKGS 里没有一个包读得出应用名（PC 上对 aapt2 是过的，这里要查这台机的包）"
  fi
  # 走 api.sh 那一条：缓存文件必须落在数据目录里，不能落进模块目录
  sh "$API" labels $PKGS >/dev/null 2>&1
  if [ -f "$DATA/labels.tsv" ]; then
    say "labels.tsv 在 $DATA（缓存写在模块外面，卸载时随目录一起清）"
  else
    bad "labels.tsv 没落到 $DATA —— 路径和 api.sh 里那条不一致"
  fi
  # 数 TAB 别往这个文件里打一个裸制表符：它会被编辑器或打包链变成空格，
  # 于是这一条在 PC 上永远数出 0，等上真机才发现"报错了"其实是量具坏了。
  N=$(sh "$API" labels $PKGS 2>/dev/null | tr -d '\r' | tr '\t' '|' | grep -c '|.')
  say "api.sh labels 给出 $N 行带名字的（点名那几个包，TAB 分两列）"
fi

echo "== 8. 智能学习链路（采样文件 ↔ advice 判据）=="
# 这一段全部只读：advice 不改配置，清零那种动作留给用户自己在页面上做。
LV=$(sed -n 's/^LEARN=//p' "$DATA/socfreq.conf" 2>/dev/null | head -n 1 | tr -d ' \r\t')
[ -n "$LV" ] || LV=1   # 老配置的 conf 里没这一行，守护按开处理，这里同口径
say "LEARN=$LV"

AD=$(sh "$API" advice 2>&1); ARC=$?
if [ "$ARC" -ne 0 ]; then
  bad "advice 退出码 $ARC —— 判据整个挂在 api.sh 里，页面那一块会直接显示取建议失败"
  say "$AD"
fi
printf '%s\n' "$AD" | grep -q '^learn=' \
  || bad "advice 没给 learn= 这一行（前端要按它决定显示建议还是显示原因）"
printf '%s\n' "$AD" | grep -q '^mode=' \
  || bad "advice 没给 mode= 这一行（前端要显示学习偏好）"
printf '%s\n' "$AD" | grep -q '^reset=' \
  || bad "advice 没给 reset= 这一行（旧格式清空后页面要提示重新积累）"
LR=$(printf '%s\n' "$AD" | sed -n 's/^rows=//p' | head -n 1)
RC=$(printf '%s\n' "$AD" | grep -c -- '---REC---')
say "advice 认到 ${LR:-0} 行样本、给出 $RC 条记录"
[ "$RC" -eq 0 ] || printf '%s\n' "$AD" | grep -q '^top2=' \
  || bad "建议记录里没有 top2= / p99=（页面证据行会显示 0）"

# 列数必须是 31（键、档、采样/顶帽/被压、24 桶、天位图、配置帽）：
# 守护写盘少一列或判据那边多读一列，表现都是"有样本但全是 0%"。
# 表头 #fmt=2 不算数据行。
if [ -f "$DATA/learn.tsv" ]; then
  say "样本文件 $(wc -l < "$DATA/learn.tsv" | tr -d ' ') 行：$DATA/learn.tsv"
  BW=$(awk '$1 ~ /^#/ { next } NF != 31 { n++ } END { print n + 0 }' "$DATA/learn.tsv" 2>/dev/null)
  [ "${BW:-0}" = 0 ] || bad "learn.tsv 里有 $BW 行的列数不是 31（写盘被截断，或两端格式没同步）"
  printf '%s\n' "$AD" | grep -q -- '---REC---' \
    || say "  有文件但没出记录：这一档的采样时长还不够，或桶全空（刚装机属正常）"
else
  if [ "$LV" = 1 ]; then
    say "还没有 learn.tsv：守护攒满 5 分钟才落一次盘，而且只在封顶真的生效时累计"
    say "  （熄屏、开机满血窗口、游戏或分应用满血期间都不算，所以亮着屏用一会儿才会有）"
  else
    bad "LEARN=0 却留着 learn.tsv：关闭时应当把它删掉"
  fi
fi
if [ -f "$DATA/learn_app.tsv" ]; then
  ABW=$(awk '$1 ~ /^#/ { next } NF != 31 { n++ } END { print n + 0 }' "$DATA/learn_app.tsv" 2>/dev/null)
  [ "${ABW:-0}" = 0 ] || bad "learn_app.tsv 里有 $ABW 行的列数不是 31（和全局那份必须同构）"
fi

echo
if [ "$BAD" -eq 0 ]; then
  echo "真机冒烟：全部通过"
  exit 0
fi
echo "真机冒烟：$BAD 项不合格"
exit 1