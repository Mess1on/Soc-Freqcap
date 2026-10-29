#!/bin/bash
# 装机迁移段（customize.sh）的台架测试：配置搬家 + 名单三方合并。
#
# 为什么要单独一份：这段只在管理器装机时跑，PC 上其余自检都碰不到它，
# 而它干的活是"决定用户改过的东西能不能活过升级"。
# 这一段最容易写的错法有两个，症状都是"看着成功、其实没生效"：
#   1) 无条件覆盖 → 升级把用户调了半年的参数打回默认；
#   2) 只补不删 / 只删不补 → 精选名单再也更新不到用户机上，
#      或者把用户自己加的包当成"上游下架"删掉。
# 还有一个必须测的：末行不带换行的文件被
# 新键和末行粘成一行（CPU_GOVERNOR=""SCREEN_UNCAP=0），
# 两个键同时失效，而 grep 每个键都"在"。
#
# 做法：把 customize.sh 里从 `DATA=` 到那两句 chmod 的整段抠出来，
# 换掉绝对路径后在临时目录里跑。抠而不是抄一份到测试里——
# 抄的那份会跟着原脚本漂移，最后两边都对不上。
SRC=$(cd "$(dirname "$0")" && pwd) || exit 1
MOD=$(dirname "$SRC") || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT

ui_print() { echo "      [ui] $1"; }

pass=0; fail=0
chk() { if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1));
        else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }
has() { [ -f "$1" ] && grep -Fxq -- "$2" "$1"; }
# 只数包名行（注释和空行不算条目）
pkgs() { grep -Ec '^[^#[:space:]]' "$1" 2>/dev/null || echo 0; }

# ── 抠出迁移段 ──────────────────────────────────────────────────
awk '/^DATA=\/data\/adb\/soc_freqcap$/{f=1} f{print}
     f && /chmod 0644 "\$DATA\/socfreq\.conf"/{exit}' "$MOD/customize.sh" > "$T/block.sh"
if [ ! -s "$T/block.sh" ]; then
  echo "  FAIL 没能从 customize.sh 里抠出迁移段（起止的那两句被改过了？）"
  exit 1
fi
# 后面每个场景都要重抠一次：DATA 换成场景自己的目录
mk_case() {
  sed -e "s#/data/adb/soc_freqcap#$T/data#g" "$T/block.sh" > "$T/case.sh"
}
runcase() { ( cd "$T" && MODPATH="$T/mod" && . ./case.sh ) >/dev/null 2>&1; }

# 包里那份名单：短、可控，带注释和空行，用来验"注释不会被当包名追加"
cat > "$T/pkg_v1.txt" <<'EOF'
# Soc智能限频 · 游戏白名单
# 注释不参与匹配
com.stay.game
com.gone.game
com.new.game
EOF
cat > "$T/pkg_v2.txt" <<'EOF'
# 换了内容的注释
com.stay.game
com.new.game
com.brand.new
EOF

echo "== 场景 1：首次装机（数据目录为空）=="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
mk_case; runcase
chk "$(has "$T/data/games.txt" com.stay.game && echo y)" "y" "名单搬进了数据目录"
chk "$([ -f "$T/data/games.base.txt" ] && echo y)" "y" "同时留下升级基准 games.base.txt"
chk "$([ -f "$T/mod/games.txt" ] && echo y || echo n)" "n" \
    "模块目录不留第二份名单（留了就会有人去改那份死的）"
chk "$(has "$T/data/games.base.txt" com.gone.game && echo y)" "y" "基准 = 本次包里那份"

echo "== 场景 2：常规升级（包内新增 + 包内下架 + 用户自己增删）=="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
cp "$T/pkg_v1.txt" "$T/data/games.txt"
cp "$T/pkg_v1.txt" "$T/data/games.base.txt"
# 用户改动：删掉 com.stay.game，加上自己需要的 com.mine.game
{ grep -v -x 'com.stay.game' "$T/data/games.txt"; printf 'com.mine.game\n'; } > "$T/g2"
mv -f "$T/g2" "$T/data/games.txt"
# 包里这版：下架 com.gone.game、新增 com.brand.new
cp "$T/pkg_v2.txt" "$T/mod/games.txt"
_c0=$(grep -c '^#' "$T/data/games.txt")
mk_case; runcase
chk "$(has "$T/data/games.txt" com.brand.new && echo y)" "y" \
    "包里新增的补进了用户名单（这就是'名单不能实时更新'的那条通道）"
chk "$(has "$T/data/games.txt" com.gone.game && echo y || echo n)" "n" "包里下架的从用户名单删掉"
chk "$(has "$T/data/games.txt" com.mine.game && echo y)" "y" \
    "用户自己加的包没被当成'下架'删掉（基准里没有它，两个差集都不含）"
chk "$(has "$T/data/games.txt" com.stay.game && echo y || echo n)" "n" \
    "用户自己删掉的包不会被下次升级塞回去（它在基准里、也在新包里，不属于新增）"
chk "$(has "$T/data/games.base.txt" com.brand.new && echo y)" "y" "基准更新成本次包里那份"
# 注释行不是包名，不参与差集，也就一条都不会被追加进来
chk "$(grep -c '^#' "$T/data/games.txt")" "$_c0" "合并没有往名单里追加任何注释行"

echo "== 场景 3：老装机直升（数据目录里没有基准）=="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
# 老版本装机只有一份 games.txt，没有 base：分不清"用户加的"和"上游删的"
printf 'com.stay.game\ncom.new.game\ncom.mine.game\n' > "$T/data/games.txt"
cp "$T/pkg_v2.txt" "$T/mod/games.txt"
mk_case; runcase
chk "$(has "$T/data/games.txt" com.brand.new && echo y)" "y" "没有基准也照样补进新增"
chk "$(has "$T/data/games.txt" com.mine.game && echo y)" "y" \
    "分不清来源时一条都不删（宁可多留，也不能删掉用户自己加的东西）"
chk "$(has "$T/data/games.base.txt" com.brand.new && echo y)" "y" "这轮之后有基准了，下次能正常合并"

echo "== 场景 4：配置只补新键，不动用户改过的值 =="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
# 用户那份是老格式：GPU_PCT 改成 30，且没有这一版新增的两个键
sed -e 's/^GPU_PCT=.*/GPU_PCT=30/' -e '/^SCREEN_UNCAP=/d' -e '/^BOOT_FULL_S=/d' \
    -e '/^SCREEN_POLICY=/d' "$MOD/socfreq.conf" > "$T/data/socfreq.conf"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"
mk_case; runcase
chk "$(sed -n 's/^GPU_PCT=//p' "$T/data/socfreq.conf")" "30" "用户改过的值一个都没被动"
chk "$(grep -c '^SCREEN_UNCAP=' "$T/data/socfreq.conf")" "1" "新增的键被补上，且只有一行"
chk "$(grep -c '^BOOT_FULL_S=' "$T/data/socfreq.conf")" "1" "BOOT_FULL_S 同样补齐"
# 行为开关例外：老用户没有 SCREEN_POLICY 时不能被升级补成 uncap。
chk "$(grep -c '^SCREEN_POLICY=' "$T/data/socfreq.conf")" "0" \
    "升级不补 SCREEN_POLICY（老用户保持 keep）"
chk "$([ -f "$T/mod/socfreq.conf" ] && echo y || echo n)" "n" \
    "补完删掉模块目录那份（两份配置并存就必然有一份是死的）"

echo "== 场景 5：末行不带换行的文件被追加 =="
# 坑在这里：包里那份 socfreq.conf
# 于是第一次往用户配置追加新键就粘成一行：
#   CPU_GOVERNOR=""CPU_PCT_BIG=65
# 粘行之后守护按 %[^=]= 解析，读到的 key 还是 CPU_GOVERNOR、
# value 变成 ""CPU_PCT_BIG=65（被调速器白名单丢掉，等于回到默认），
# 而 CPU_PCT_BIG 这一行再也不会被读到——用户调的百分比静默失效。
# api.sh 的 conf_read 用 sed "s/^KEY=//"，同样找不到粘进去的键，
# 于是页面显示的是默认值，用户以为"改了没保存"。
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
printf 'INTERVAL=5\nGPU_PCT=30\nCPU_GOVERNOR=""' > "$T/data/socfreq.conf"   # 故意不给结尾换行
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"
mk_case; runcase
# 判据用"每个键都能按 ^KEY= 找到"，不写死具体粘成了哪一行：
# 追加顺序由包里那份的行序决定，写死 SCREEN_UNCAP 的话换个键排前面就漏检
# （变异测试实测就是这样：只查 'CPU_GOVERNOR=""SCREEN' 时关掉防护仍然全绿）。
_missing=""
while read -r _l; do
  case "$_l" in ''|\#*) continue ;; esac
  case "$_l" in *=*) ;; *) continue ;; esac
  _k=${_l%%=*}
  # SCREEN_POLICY 是行为开关，约定不往老用户配置里补，单独跳过。
  [ "$_k" = SCREEN_POLICY ] && continue
  # 拼字符串写成 _missing="$(_missing $_k)" 是错的：$() 是"执行命令"，
  # _missing 为空时等于执行一条空命令名，报 command not found，
  # 而 _missing 永远留在空串上 —— 判据恒绿，比没写还坏。
  grep -q "^${_k}=" "$T/data/socfreq.conf" 2>/dev/null || _missing="$_missing $_k"
done < "$MOD/socfreq.conf"
chk "${_missing:-none}" "none" "包里的每个键在合并结果里都能按 ^KEY= 读到（没有粘行）$_missing"
chk "$(tail -c 1 "$T/data/socfreq.conf" | od -An -c | tr -d ' \n')" '\n' \
    "合并结果以换行结尾（下次追加不会再粘）"
chk "$(sed -n 's/^GPU_PCT=//p' "$T/data/socfreq.conf")" "30" "用户改过的值仍然在"

# 名单同理：末行不带换行时追加包名
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
printf 'com.stay.game\ncom.gone.game\ncom.new.game' > "$T/data/games.txt"
cp "$T/data/games.txt" "$T/data/games.base.txt"
printf 'com.stay.game\ncom.new.game\ncom.brand.new' > "$T/mod/games.txt"
mk_case; runcase
chk "$(grep -c 'gamecom\.' "$T/data/games.txt" || true)" "0" "名单追加没有粘行"
chk "$(has "$T/data/games.txt" com.brand.new && echo y)" "y" "粘行修好后新增照样进得来"

echo "== 场景 6：幂等（同一版连装两次）=="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"; cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
mk_case
runcase
n1=$(pkgs "$T/data/games.txt"); c1=$(grep -c '^' "$T/data/socfreq.conf")
cp "$T/pkg_v1.txt" "$T/mod/games.txt"; cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
runcase
n2=$(pkgs "$T/data/games.txt"); c2=$(grep -c '^' "$T/data/socfreq.conf")
chk "$n2" "$n1" "重复安装同一个版本不会把名单越装越长"
chk "$c2" "$c1" "重复安装同一个版本不会把配置越补越多"
chk "$(sort "$T/data/games.txt" | grep -Ev '^\s*(#|$)' | uniq -d | wc -l | tr -d ' ')" "0" \
    "名单里没有重复条目"

echo "== 场景 7：按芯片套默认（单一来源 adapt/*.conf）=="
# SM8750（8 Elite）→ 66 / 68，TH 75/68，PSI_GAIN 10
# SM8850（8E5）→ 64 / 69
# 只对全新安装生效；升级配置一个字节都不改（哪怕值刚好等于旧默认）。
_g() { sed -n "s/^$2=//p" "$1" | head -n 1; }
preset_case() {   # $1=芯片代号 $2=期望BIG $3=期望LITTLE $4=no|yes|yes-defaults
  rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod" "$T/mod/adapt"
  cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
  cp "$MOD"/adapt/*.conf "$T/mod/adapt/"
  cp "$T/pkg_v1.txt" "$T/mod/games.txt"
  case "$4" in
    yes)        # 用户早就装过、还把超大核改成 40 了
      sed 's/^CPU_PCT_BIG=.*/CPU_PCT_BIG=40/' "$MOD/socfreq.conf" > "$T/data/socfreq.conf" ;;
    yes-defaults)   # 装过但从没动过：数据目录里就是包里那份通用默认
      cp "$MOD/socfreq.conf" "$T/data/socfreq.conf" ;;
  esac
  # 芯片代号是从 getprop 读的，这里换掉它就行（真机上没有 getprop）
  eval "getprop() { case \"\$1\" in ro.soc.model) echo '$1';; ro.board.platform) echo 'PLATFORM';; esac; }"
  mk_case; runcase
  local b l
  b=$(_g "$T/data/socfreq.conf" CPU_PCT_BIG)
  l=$(_g "$T/data/socfreq.conf" CPU_PCT_LITTLE)
  chk "$b" "$2" "芯片 $1（$4）→ CPU_PCT_BIG=$b 期望 $2"
  chk "$l" "$3" "芯片 $1（$4）→ CPU_PCT_LITTLE=$l 期望 $3"
}
preset_case SM8750 66 68 no
preset_case SM8850 64 69 no
preset_case MT6991 65 65 no          # 其它芯片保持通用默认
preset_case SM8750 40 65 yes         # 升级 + 用户改过：40 必须活着
# 升级但值刚好等于旧默认，也不许套——没有"值等于默认就视为没改过"的启发式。
preset_case SM8750 65 65 yes-defaults
# 全新安装同时套芯片的温控/PSI 默认；百分比外的项不能顺手乱改
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod" "$T/mod/adapt"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"; cp "$MOD"/adapt/*.conf "$T/mod/adapt/"
eval "getprop() { case \"\$1\" in ro.soc.model) echo SM8750;; ro.board.platform) echo X;; esac; }"
mk_case; runcase
chk "$(_g "$T/data/socfreq.conf" CPU_PCT_MID2)" "64" "芯片默认不动 MID2"
chk "$(_g "$T/data/socfreq.conf" CPU_PCT_MID)" "66" "芯片默认不动 MID"
chk "$(_g "$T/data/socfreq.conf" GPU_PCT)" "50" "芯片默认的 GPU_PCT 是 50"
chk "$(_g "$T/data/socfreq.conf" TH_HIGH)" "75" "8E 芯片默认 TH_HIGH=75"
chk "$(_g "$T/data/socfreq.conf" TH_LOW)" "68" "8E 芯片默认 TH_LOW=68"
chk "$(_g "$T/data/socfreq.conf" PSI_GAIN)" "10" "8E 芯片默认 PSI_GAIN=10"
chk "$(grep -Ec 'CPU_PCT_BIG=.*pz|\.pz' "$T/data/socfreq.conf" || true)" "0" "临时文件没混进配置"
chk "$([ -f "$T/data/socfreq.conf.pz" ] && echo y || echo n)" "n" "预设用完把 .pz 清掉了"
# ro.soc.model 拿不到时要退到 ro.board.platform（部分 ROM 不填前者）
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod" "$T/mod/adapt"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"; cp "$MOD"/adapt/*.conf "$T/mod/adapt/"
eval "getprop() { case \"\$1\" in ro.soc.model) echo '';; ro.board.platform) echo sm8750;; esac; }"
mk_case; runcase
chk "$(_g "$T/data/socfreq.conf" CPU_PCT_LITTLE)" "68" "属性为空时退到 board.platform，且大小写不敏感"
# 升级时整个配置文件逐字节不变（连注释/空白都不许动）
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod" "$T/mod/adapt"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"; cp "$MOD"/adapt/*.conf "$T/mod/adapt/"
sed 's/^CPU_PCT_BIG=.*/CPU_PCT_BIG=42/' "$MOD/socfreq.conf" > "$T/data/socfreq.conf"
cp "$T/data/socfreq.conf" "$T/data/socfreq.conf.before"
eval "getprop() { case \"\$1\" in ro.soc.model) echo SM8750;; ro.board.platform) echo X;; esac; }"
mk_case; runcase
chk "$(cmp -s "$T/data/socfreq.conf" "$T/data/socfreq.conf.before" && echo same || echo changed)" \
    "same" "升级安装：用户配置逐字节不变"
rm -f "$T/data/socfreq.conf.before"

echo "== 场景 8：adapt/ 按机型自动装适配表 =="
# 用户侧只导出适配包，改好的表由作者放进包里的 adapt/ 随版本发布。
# 这一段要钉死的是"什么时候才装"：
#   多条 #match 必须全部命中（只命中 brand 不算）；
#   没有 #match 的文件一律不装 —— 没有条件就等于对所有机器适用；
#   只带进那四种键的行，作者打错前缀的行不会被静默装上；
#   撤表只撤"上一版由 adapt 装进去的那份"，用户自己命令行导的不能动。
ad_fresh() {
  rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod/adapt"
  cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"; cp "$T/pkg_v1.txt" "$T/mod/games.txt"
}
ad_props() {   # $1=brand $2=model $3=soc
  eval "getprop() { case \"\$1\" in
      ro.product.brand) echo '$1';; ro.product.model) echo '$2';;
      ro.soc.model) echo '$3';; ro.board.platform) echo '';; esac; }"
}
ad_exec() { mk_case; runcase; }
OV="$T/data/override.conf"
has_ov() { [ -f "$OV" ] && echo y || echo n; }

# 1) 命中：装上，第一行留来源标记
ad_fresh; ad_props OnePlus PJZ110 TESTCHIP
cat > "$T/mod/adapt/one.conf" <<'EOF'
# 这条机器的 GPU 节点不在常规路径上
#match model=PJZ110
cpu-dir /sys/devices/system/cpu/cpufreq/policy0 3532800
gpu-max /sys/class/devfreq/1a00000.qcom,kgsl-3d0/max_freq
EOF
ad_exec
chk "$(head -n 1 "$OV" 2>/dev/null)" "#from adapt/one.conf" "命中后装进数据目录，第一行写明来源"
chk "$(grep -Ec '^cpu-dir |^gpu-max ' "$OV" 2>/dev/null)" "2" "两条节点都带进去了"
chk "$(grep -c '这条机器的 GPU' "$OV" 2>/dev/null || true)" "0" "adapt 文件里的注释不会被装进去"

# 2) 多条 #match 只命中一半 → 不装
ad_fresh; ad_props OnePlus PJZ110 TESTCHIP
cat > "$T/mod/adapt/two.conf" <<'EOF'
#match brand=OnePlus
#match model=OTHERMODEL
cpu-dir /sys/devices/system/cpu/cpufreq/policy0 3532800
EOF
ad_exec
chk "$(has_ov)" "n" "多条 #match 要全部命中，只中一条不装"

# 3) 没有 #match 的文件 → 不装（没有条件就是对所有机器适用，太危险）
ad_fresh; ad_props OnePlus PJZ110 TESTCHIP
printf 'cpu-dir /sys/devices/system/cpu/cpufreq/policy0 3532800\n' > "$T/mod/adapt/nomatch.conf"
ad_exec
chk "$(has_ov)" "n" "没有 #match 的适配文件不会被装到任何机器上"

# 4) 命中但可装的行为空（前缀打错 / 全是注释）→ 不装
ad_fresh; ad_props OnePlus PJZ110 TESTCHIP
printf '#match model=PJZ110\n# 说明\nCPU-DIR /sys/x 3532800\n' > "$T/mod/adapt/bad.conf"
ad_exec
chk "$(has_ov)" "n" "认不出合法键行时不装（打错前缀不会静默生效）"

# 5) soc 按包含命中（属性带后缀也算同一颗芯片）
ad_fresh; ad_props OnePlus PJZ110 SM8750-AB
printf '#match soc=SM8750\ncpu-dir /sys/x 100\n' > "$T/mod/adapt/soc.conf"
ad_exec
chk "$(head -n 1 "$OV" 2>/dev/null)" "#from adapt/soc.conf" "soc 条件是包含匹配，属性带后缀也认"

# 6) 上一版由 adapt 装的表 + 这一版没有匹配文件 → 撤掉，回到自动探测
rm -f "$T/mod/adapt"/*.conf
printf '#match model=NOT_ME\ncpu-dir /a 1\n' > "$T/mod/adapt/other.conf"
ad_props OtherBrand XXXMODEL XXXCHIP
ad_exec
chk "$(has_ov)" "n" "作者把这台机的适配下架后，升级就回到自动探测"

# 7) 用户自己用命令行导进去的表（没有来源标记）→ 装机不许删
ad_fresh; ad_props OtherBrand XXXMODEL XXXCHIP
printf 'cpu-dir /a 1\n' > "$OV"
ad_exec
chk "$(cat "$OV" 2>/dev/null)" "cpu-dir /a 1" "不是 adapt 装的表不会被装机删掉"

# 8) model 比对不区分大小写
ad_fresh; ad_props oneplus pjz110 XXXCHIP
printf '#match model=PJZ110\ncpu-dir /a 1\n' > "$T/mod/adapt/case.conf"
ad_exec
chk "$(head -n 1 "$OV" 2>/dev/null)" "#from adapt/case.conf" "机型属性大小写不同也认"

# 9) 多个文件命中时只装第一个，不叠加
ad_fresh; ad_props OnePlus PJZ110 XXXCHIP
printf '#match brand=OnePlus\ncpu-dir /a 1\n' > "$T/mod/adapt/a-first.conf"
printf '#match model=pjz110\ncpu-dir /b 2\n' > "$T/mod/adapt/z-second.conf"
ad_exec
chk "$(grep -c '^cpu-dir ' "$OV" 2>/dev/null)" "1" "命中第一个就停（不叠成两份表）"
chk "$(grep '^cpu-dir ' "$OV" 2>/dev/null)" "cpu-dir /a 1" "装的是先命中的那个文件"

# 10) 随包发布的那份模板，落到任何机器上都不该被装出来
ad_fresh; ad_props OnePlus PJZ110 SM8750
cp "$MOD/adapt/_TEMPLATE.conf" "$T/mod/adapt/"
ad_exec
chk "$(has_ov)" "n" "包内模板文件不会装到真机上"

# 11) 适配表不能把用户改过的配置顺手带进别处（这段只管 override.conf）
ad_fresh; ad_props OnePlus PJZ110 SM8750
printf '#match model=PJZ110\ncpu-dir /a 1\n' > "$T/mod/adapt/only.conf"
ad_exec
chk "$(_g "$T/data/socfreq.conf" CPU_PCT_BIG)" "65" "装适配表不动配置文件里的百分比"

echo "== 场景 12：内置相机满血（只加已装、不动用户值）=="
mkdir -p "$T/bin" "$T/binfail"
cat > "$T/bin/pm" <<'EOF'
#!/bin/sh
printf 'package:com.android.camera\npackage:com.oplus.camera\npackage:com.third.camera\npackage:com.pm.missing\npackage:com.tencent.mm\npackage:com.tencent.mobileqq\npackage:com.alibaba.android.rimet\npackage:com.whatsapp\n'
EOF
cat > "$T/binfail/pm" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$T/bin/pm" "$T/binfail/pm"
# PATH 覆盖版：内置清单要靠 pm 判断"这台机装没装"
runcase_with() { ( cd "$T" && MODPATH="$T/mod" && PATH="$1:$PATH" && . ./case.sh ) >/dev/null 2>&1; }
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod/scripts"
cp "$MOD/app_modes.conf" "$T/mod/app_modes.conf"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"
cat > "$T/mod/scripts/builtin_modes.txt" <<'EOF'
# 测试清单
com.android.camera=full
com.oplus.camera=full
com.not.installed=full
EOF
# 用户已经给 oplus 设了 80：内置清单不能覆盖
printf 'com.oplus.camera=80\n' > "$T/data/app_modes.conf"
mk_case; runcase_with "$T/bin"
chk "$(has "$T/data/app_modes.conf" 'com.android.camera=full  #builtin' && echo y)" "y" \
    "已安装的相机被加为满血（带 #builtin 标记）"
chk "$(has "$T/data/app_modes.conf" 'com.oplus.camera=80' && echo y)" "y" \
    "用户已经设过的值不被内置清单覆盖"
chk "$(grep -c '^com.not.installed=' "$T/data/app_modes.conf")" "0" \
    "没装的相机包不写进配置"
chk "$([ -f "$T/data/builtin_modes.base.txt" ] && echo y)" "y" "留下内置清单的升级基准"

echo "== 场景 13：用户删掉内置项，升级不塞回来 =="
grep -v '^com.android.camera=' "$T/data/app_modes.conf" > "$T/am"; mv -f "$T/am" "$T/data/app_modes.conf"
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^com.android.camera=' "$T/data/app_modes.conf")" "0" \
    "删掉的内置项不会被同一份清单塞回来（它在基准里，不算新增）"

echo "== 场景 14：新版清单新增相机包，只补这一条 =="
printf 'com.third.camera=full\n' >> "$T/mod/scripts/builtin_modes.txt"
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^com.third.camera=full' "$T/data/app_modes.conf")" "1" \
    "新版清单里新增的已装相机被补进来"
chk "$(grep -c '^com.android.camera=' "$T/data/app_modes.conf")" "0" \
    "上一轮已存在（被删）的项仍然不塞回来"

echo "== 场景 15：pm 不可用时跳过，且不推进基准 =="
printf 'com.pm.missing=full\n' >> "$T/mod/scripts/builtin_modes.txt"
mk_case; runcase_with "$T/binfail"
chk "$(grep -c '^com.pm.missing=' "$T/data/app_modes.conf")" "0" "拿不到已装列表时不乱加"
chk "$(grep -c '^com.pm.missing=' "$T/data/builtin_modes.base.txt")" "0" \
    "失败那轮不推进基准（下次还能补）"
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^com.pm.missing=full' "$T/data/app_modes.conf")" "1" \
    "pm 恢复后把上一轮漏掉的补上"

echo "== 场景 16：内置深睡永久豁免（只加已装、按模板建 conf）=="
rm -rf "$T/data" "$T/mod"; mkdir -p "$T/data" "$T/mod/scripts"
cp "$MOD/app_modes.conf" "$T/mod/app_modes.conf"
cp "$MOD/socfreq.conf" "$T/mod/socfreq.conf"
cp "$T/pkg_v1.txt" "$T/mod/games.txt"
cp "$MOD/scripts/deep_sleep.sh" "$T/mod/scripts/deep_sleep.sh"
cat > "$T/mod/scripts/builtin_white.txt" <<'EOF'
# 测试清单
com.tencent.mm          # 微信
com.tencent.mobileqq    # 手机QQ
com.not.installed       # 没装
EOF
mk_case; runcase_with "$T/bin"
chk "$(has "$T/data/dsleep.conf" 'white=com.tencent.mm' && echo y)" "y" \
    "已安装的微信写进永久豁免"
chk "$(has "$T/data/dsleep.conf" 'white=com.tencent.mobileqq' && echo y)" "y" \
    "已安装的 QQ 写进永久豁免"
chk "$(grep -c '^white=com.not.installed$' "$T/data/dsleep.conf")" "0" \
    "没装的包不写"
chk "$(grep -c '^white=com.tencent.mm$' "$T/data/dsleep.conf")" "1" \
    "只写一条，不把注释里的示例也当条目"
chk "$(grep -c '^enable=0' "$T/data/dsleep.conf")" "1" \
    "新建的 dsleep.conf 仍是默认关闭"
chk "$([ -f "$T/data/builtin_white.base.txt" ] && echo y)" "y" "留下豁免清单的升级基准"

echo "== 场景 17：重复安装不重复加 =="
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^white=com.tencent.mm$' "$T/data/dsleep.conf")" "1" "重复装机不会写第二行"

echo "== 场景 18：删掉的豁免不塞回，新版只补新增 =="
grep -v '^white=com.tencent.mm$' "$T/data/dsleep.conf" > "$T/dsc"; mv -f "$T/dsc" "$T/data/dsleep.conf"
printf 'com.alibaba.android.rimet  # 钉钉\n' >> "$T/mod/scripts/builtin_white.txt"
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^white=com.tencent.mm$' "$T/data/dsleep.conf")" "0" \
    "用户删掉的微信不会被同一份清单塞回来"
chk "$(has "$T/data/dsleep.conf" 'white=com.alibaba.android.rimet' && echo y)" "y" \
    "新版清单新增的已装应用被补进来"

echo "== 场景 19：pm 不可用时跳过且不推进基准 =="
printf 'com.whatsapp\n' >> "$T/mod/scripts/builtin_white.txt"
mk_case; runcase_with "$T/binfail"
chk "$(grep -c '^white=com.whatsapp$' "$T/data/dsleep.conf")" "0" "拿不到已装列表时不乱加"
chk "$(grep -c 'com.whatsapp' "$T/data/builtin_white.base.txt")" "0" \
    "失败那轮不推进豁免基准"
mk_case; runcase_with "$T/bin"
chk "$(has "$T/data/dsleep.conf" 'white=com.whatsapp' && echo y)" "y" \
    "pm 恢复后补上上一轮漏掉的"

echo "== 场景 20：新版清单移除的包，升级时从豁免里撤下 =="
# 模拟上一版内置过 Telegram（base 与 conf 都有），新清单里已经没有它。
printf 'org.telegram.messenger\n' >> "$T/data/builtin_white.base.txt"
printf 'white=org.telegram.messenger\n' >> "$T/data/dsleep.conf"
mk_case; runcase_with "$T/bin"
chk "$(grep -c '^white=org.telegram.messenger$' "$T/data/dsleep.conf")" "0" \
    "上游移除的豁免被撤下"
chk "$(grep -c '^white=com.tencent.mobileqq$' "$T/data/dsleep.conf")" "1" \
    "同一份清单里保留的项不受影响"

echo
if [ "$fail" -eq 0 ]; then echo "装机迁移段台架：全部通过 ($pass)"; else echo "装机迁移段台架：$fail 项失败 / $pass 项通过"; fi
exit "$fail"
