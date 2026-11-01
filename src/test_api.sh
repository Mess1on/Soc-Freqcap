#!/bin/bash
# api.sh 的信任边界测试：验证非法输入被拒绝、注入无法生效、文件不被破坏。
#
# 在一次性临时目录里跑，绝不碰模块自身的 socfreq.conf / games.txt。
SRC=$(cd "$(dirname "$0")" && pwd) || exit 1
MOD=$(dirname "$SRC") || exit 1
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/webroot"
cp "$MOD/webroot/api.sh" "$TMP/webroot/api.sh"
cp "$MOD/socfreq.conf"  "$TMP/socfreq.conf"
cp "$MOD/games.txt"     "$TMP/games.txt"
# 名单筛选的用例需要一个"一定在名单里"的包名做锚点。
# 不加的话断言比的是空对空：games_list 返回空是因为名单里根本没有那个包，
# 不是筛错了，看着却是绿的。
printf 'com.example.game\n' >> "$TMP/games.txt"
cd "$TMP" || exit 1

# 真实部署里数据目录在模块目录之外（/data/adb/soc_freqcap）。
# 这一组用例先让 DATA 等于临时模块目录，好让既有的断言继续比对同一个文件；
# 后面"数据目录分离"那组再把 DATA 指到别处，专门验两目录分开时的行为。
export SOCFREQ_DATA_DIR="$TMP"

pass=0; fail=0
# 原始 PATH 必须在任何插桩之前存下来。
ORIG_PATH="$PATH"
A() { sh webroot/api.sh "$@"; }
ok(){ if [ "$1" = "$2" ]; then echo "  ok   $3"; pass=$((pass+1)); else echo "  FAIL $3: got=[$1] want=[$2]"; fail=$((fail+1)); fi; }
reject(){ # 期望拒绝
  local out rc
  out=$(A "$@" 2>&1); rc=$?
  if [ "$rc" -ne 0 ]; then echo "  ok   拒绝: $*"; pass=$((pass+1));
  else echo "  FAIL 竟然接受了: $* (输出=$out)"; fail=$((fail+1)); fi
}

echo "== 正常读取 =="
ok "$(A conf_get | grep -c '^INTERVAL=')" "1" "conf_get 返回键值行"
ok "$(A games_list all | head -1 | grep -c '^[a-z]')" "1" "games_list 首行是包名"
# ping 的回执是 PONG 不是 OK：别让前端把两种回执搞混
ok "$(A ping)" "PONG" "ping 回 PONG"
# about 给的是模块自检信息，count 要等于名单实际条数
ok "$(A about | grep -c '^COUNT=')" "1" "about 给出名单条数"
ok "$(A about | sed -n 's/^COUNT=//p')" "$(grep -vc '^[[:space:]]*\(#.*\)\?$' games.txt)" \
   "about 的 COUNT 和名单实际条数一致"

echo "== 合法写入 =="
A conf_set CPU_PCT_BIG 60 >/dev/null
ok "$(grep '^CPU_PCT_BIG=' socfreq.conf)" "CPU_PCT_BIG=60" "合法百分比已落盘"
A conf_set CPU_GOVERNOR walt >/dev/null
ok "$(grep '^CPU_GOVERNOR=' socfreq.conf)" "CPU_GOVERNOR=walt" "合法调速器已落盘"
A conf_set CPU_GOVERNOR "" >/dev/null
ok "$(grep '^CPU_GOVERNOR=' socfreq.conf)" 'CPU_GOVERNOR=' "空调速器已落盘"
A games_add com.test.game >/dev/null
ok "$(grep -c -x 'com.test.game' games.txt)" "1" "合法包名已加入"
# 写完必须读回一致：只信返回码会把问题留到下次重启才暴露
ok "$(A conf_set GPU_PCT 55 | head -1)" "OK GPU_PCT=55" "conf_set 回执带键值，便于核对"
ok "$(grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=55" "GPU_PCT 已落盘"
ok "$(A conf_set ADAPT_UP_MAX 20 | head -1)" "OK ADAPT_UP_MAX=20" "合计放宽总闸可写"
ok "$(A conf_set ADAPT_DOWN_MAX 0 | head -1)" "OK ADAPT_DOWN_MAX=0" "收紧总闸 0 = 封死这一侧，合法"
ok "$(grep '^ADAPT_UP_MAX=' socfreq.conf)" "ADAPT_UP_MAX=20" "ADAPT_UP_MAX 已落盘"
ok "$(grep '^ADAPT_DOWN_MAX=' socfreq.conf)" "ADAPT_DOWN_MAX=0" "ADAPT_DOWN_MAX 已落盘"
A conf_set ADAPT_UP_MAX 15 >/dev/null
A conf_set ADAPT_DOWN_MAX 30 >/dev/null
ok "$(A conf_set BATT_STEP 0 | head -1)" "OK BATT_STEP=0" "电量阶梯可关"
ok "$(A conf_set BATT_STEP 1 | head -1)" "OK BATT_STEP=1" "电量阶梯可开"
ok "$(A conf_set BATT_LOW 25 >/dev/null; grep '^BATT_LOW=' socfreq.conf)" "BATT_LOW=25" "低电量阈值可写"
ok "$(A conf_set BATT_CRIT 8 >/dev/null; grep '^BATT_CRIT=' socfreq.conf)" "BATT_CRIT=8" "第二档阈值可写"
ok "$(A conf_set BATT_LOW_CUT 7 >/dev/null; grep '^BATT_LOW_CUT=' socfreq.conf)" "BATT_LOW_CUT=7" "第一刀可写"
ok "$(A conf_set BATT_CRIT_CUT 12 >/dev/null; grep '^BATT_CRIT_CUT=' socfreq.conf)" "BATT_CRIT_CUT=12" "第二刀可写"
reject conf_set BATT_CRIT 25
reject conf_set BATT_LOW 8
A conf_set BATT_CRIT 10 >/dev/null
A conf_set BATT_LOW 20 >/dev/null
A conf_set BATT_LOW_CUT 5 >/dev/null
A conf_set BATT_CRIT_CUT 10 >/dev/null

echo "== 批量保存（conf_set_many）=="
# 页面一次保存要发三十多个键。以前逐个 conf_set = 三十多次 root IPC，
# 手机上要十几秒；现在一个进程里跑完，页面只发一次。
ok "$(A conf_set_many CPU_PCT_BIG=55 CPU_PCT_MID2=56 2>&1)" "OK 2" "一次调用写入多个键"
ok "$(A conf_get | sed -n 's/^CPU_PCT_BIG=//p')" "55" "批量里的第一个键写进去了"
ok "$(A conf_get | sed -n 's/^CPU_PCT_MID2=//p')" "56" "批量里的第二个键也写进去了"
ok "$(A conf_set_many CPU_PCT_BIG=55 2>&1)" "OK 1" "值没变的键照常计入（不报错）"
BATCHOUT=$(A conf_set_many CPU_PCT_BIG=66 CPU_PCT_BIG=999 2>&1); BRC=$?
ok "$BRC" "7" "批量遇到坏值返回非零（7）"
ok "$(printf '%s' "$BATCHOUT" | grep -c 'CPU_PCT_BIG')" "1" "错误里点名是哪个键，不是一句 WRITE"
ok "$(A conf_get | sed -n 's/^CPU_PCT_BIG=//p')" "66" "坏值之前那几条已经生效（和逐条保存同语义）"
A conf_set_many CPU_PCT_BIG=60 CPU_PCT_MID2=65 >/dev/null
reject conf_set_many
reject conf_set_many CPU_PCT_BIG
reject conf_set_many CPU_PCT_BIG=999

echo "== 拒绝非法值 =="
reject conf_set CPU_PCT_BIG 999
reject conf_set CPU_PCT_BIG 0
reject conf_set CPU_PCT_BIG abc
reject conf_set GPU_PCT '50;echo pwned'
reject conf_set INTERVAL 1
reject conf_set INTERVAL 999
reject conf_set ADAPT_UP_MAX -1
reject conf_set ADAPT_UP_MAX 101
reject conf_set ADAPT_DOWN_MAX -1
reject conf_set ADAPT_DOWN_MAX 101
reject conf_set ADAPT_UP_MAX abc
reject conf_set BATT_STEP 2
reject conf_set BATT_STEP abc
reject conf_set BATT_LOW 4
reject conf_set BATT_LOW 96
reject conf_set BATT_LOW_CUT -1
reject conf_set BATT_LOW_CUT 31
reject conf_set BATT_CRIT 0
reject conf_set BATT_CRIT 96
reject conf_set BATT_CRIT_CUT 31
reject conf_set GPU_MIN_LOCK 2
reject conf_set CPU_GOVERNOR 'scx'
reject conf_set CPU_GOVERNOR 'walt;rm -rf /'
reject conf_set EVIL_KEY 1
reject games_add 'foo bar'
reject games_add '$(id)'
reject games_add ''
reject games_del 'foo bar'
# 包名校验比"没有非法字符"更严：必须含点，且不允许首尾点和连续点
reject games_add 'nodots'
reject games_add 'com..bad'
reject games_add '.com.leading'
reject games_add 'com.trailing.'
# 数字位数封顶：大数虽然会超范围被拒，但 5 位以内也要挡住畸形输入
reject conf_set CPU_PCT_BIG 0000099999

echo "== 拒绝: 不静默夹到边界 =="
# 越界要报错，不能偷偷改成 100。用户改了没生效却看不出来是最糟的。
BIG=$(A conf_set CPU_PCT_BIG 999 2>&1); BIGRC=$?
ok "$BIGRC" "1" "越界拒绝时退出码非零"
ok "$(grep '^CPU_PCT_BIG=' socfreq.conf)" "CPU_PCT_BIG=60" "越界后原值没被动过（没被夹到 100）"
case "$BIG" in
  E/RANGE*) ok "E/RANGE" "E/RANGE" "错误是机器可读的码，不是混着数据的中文串" ;;
  *) ok "$BIG" "E/RANGE*" "错误走 stderr 的错误码通道（got=[$BIG]）" ;;
esac

echo "== 写完必须读回核对 =="
# 这条防线自己得有一个"真的会让它失效"的用例。
# 变异测试发现：把 read-back 校验整行删掉，全套测试照样全绿——
# 因为现有用例里没有任何一次写入会"返回成功但内容不对"。
# 做法：把 mv 换成空实现（返回 0 但什么都不做），
# 模拟"写入返回成功、文件却没更新"。只信返回码的脚本会以为成功。
#
# 前提是先把当前值定住：mv 一旦被桩掉，写入就真的落不了地，
# 所以先在真 mv 下写一个已知值，桩掉之后再写另一个值并断言失败。
ok "$(A conf_set GPU_PCT 45 >/dev/null; grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=45" \
   "前置：真 mv 下先写入 45"
# 桩放在独立目录，且只在这一条命令的 PATH 前面，绝不 export。
# 测试自己清临时目录也用 mv：桩一旦挂到全局 PATH 上，
# trap 的 rm -rf "$TMP" 就失效，后面十几条用例会被带崩。
mkdir -p "$TMP/stub"
printf '#!/bin/sh\nexit 0\n' > "$TMP/stub/mv"
chmod +x "$TMP/stub/mv"
run_with_stub_mv() { PATH="$TMP/stub:$ORIG_PATH" sh webroot/api.sh "$@"; }
ok "$(grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=45" "桩只在 api.sh 那一侧，测试自身没受影响"
ok "$(run_with_stub_mv conf_set GPU_PCT 44 2>&1 >/dev/null | head -1)" "E/VERIFY" \
   "mv 空转时报 E/VERIFY（只信 mv 返回码就会静默当成功）"
ok "$(grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=45" "mv 空转时旧值没被截断"
ok "$(A conf_set GPU_PCT 45 >/dev/null; grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=45" \
   "真 mv 下写入正常"

echo "== 新增开关的取值校验 =="
# SCREEN_UNCAP 是开关：只认 0/1。默认必须是"熄屏不解除"，
# 所以任何非 1 的值都不该被当成开启。
ok "$(A conf_set SCREEN_UNCAP 1 >/dev/null; grep '^SCREEN_UNCAP=' socfreq.conf)" "SCREEN_UNCAP=1" "熄屏解除可开"
ok "$(A conf_set SCREEN_UNCAP 0 >/dev/null; grep '^SCREEN_UNCAP=' socfreq.conf)" "SCREEN_UNCAP=0" "熄屏解除可关"
reject conf_set SCREEN_UNCAP 2
reject conf_set SCREEN_UNCAP abc
ok "$(A conf_set SCREEN_POLICY uncap >/dev/null; grep '^SCREEN_POLICY=' socfreq.conf)" \
   "SCREEN_POLICY=uncap" "熄屏策略可设 uncap"
ok "$(A conf_set SCREEN_POLICY keep >/dev/null; grep '^SCREEN_POLICY=' socfreq.conf)" \
   "SCREEN_POLICY=keep" "熄屏策略可设 keep"
reject conf_set SCREEN_POLICY auto
reject conf_set SCREEN_POLICY Uncap
ok "$(A conf_set SOFF_INTERVAL 5 >/dev/null; grep '^SOFF_INTERVAL=' socfreq.conf)" \
   "SOFF_INTERVAL=5" "熄屏周期下限 5s 可写"
ok "$(A conf_set SOFF_INTERVAL 300 >/dev/null; grep '^SOFF_INTERVAL=' socfreq.conf)" \
   "SOFF_INTERVAL=300" "熄屏周期上限 300s 可写"
reject conf_set SOFF_INTERVAL 4
reject conf_set SOFF_INTERVAL 301
ok "$(A conf_set SCREEN_STATE_PATH /sys/class/backlight/panel0-backlight/brightness >/dev/null; \
      grep '^SCREEN_STATE_PATH=' socfreq.conf)" \
   "SCREEN_STATE_PATH=/sys/class/backlight/panel0-backlight/brightness" \
   "亮屏信号可指定 sysfs 路径"
ok "$(A conf_set SCREEN_STATE_PATH '' >/dev/null; grep '^SCREEN_STATE_PATH=' socfreq.conf)" \
   "SCREEN_STATE_PATH=" "留空 = 自动挑选信号"
reject conf_set SCREEN_STATE_PATH /data/adb/x
A conf_set SCREEN_POLICY uncap >/dev/null
A conf_set SOFF_INTERVAL 15 >/dev/null
# BOOT_FULL_S：0 合法（= 关），600 是上限。
# 0 必须能写进去——早先一类开关的坑就是 `|| 默认值` 把 0 吃掉了。
ok "$(A conf_set BOOT_FULL_S 0 >/dev/null; grep '^BOOT_FULL_S=' socfreq.conf)" "BOOT_FULL_S=0" "开机满血可以设成 0（关）"
ok "$(A conf_set BOOT_FULL_S 90 >/dev/null; grep '^BOOT_FULL_S=' socfreq.conf)" "BOOT_FULL_S=90" "开机满血窗口可设 90 秒"
reject conf_set BOOT_FULL_S 601
reject conf_set BOOT_FULL_S -1

echo "== 数据目录与模块目录分离 =="
# 要保的就是这条：升级模块 = 模块目录被删掉重建，
# 配置必须在外面。用一个全新的空目录当数据目录跑一遍读写。
SEP="$TMP/sepdata"
mkdir -p "$SEP"
cp "$MOD/socfreq.conf" "$SEP/socfreq.conf"
cp "$MOD/games.txt" "$SEP/games.txt"
SA() { SOCFREQ_DATA_DIR="$SEP" sh webroot/api.sh "$@"; }
ok "$(SA conf_set GPU_PCT 33 >/dev/null; grep '^GPU_PCT=' "$SEP/socfreq.conf")" "GPU_PCT=33" \
   "写入落在数据目录"
ok "$(grep '^GPU_PCT=' socfreq.conf)" "GPU_PCT=45" \
   "模块目录里那份没被碰（升级时被覆盖也不影响用户配置）"
ok "$(SA games_list all | grep -c .)" "$(grep -vc '^[[:space:]]*\(#.*\)\?$' "$SEP/games.txt")" \
   "名单从数据目录读"
# 数据目录为空时（首次装机迁移漏了）：api.sh 自己搬一次，不能报 NO_CONF。
# 必须在**另一个模块目录**里跑：迁移用的是 mv，在当前目录跑会把
# 后面二十几条用例依赖的那份 socfreq.conf 搬走，表现是一堆莫名其妙的红。
FRESH="$TMP/fresh"; mkdir -p "$FRESH"
FR="$TMP/freshroot"; mkdir -p "$FR/webroot"
cp "$MOD/webroot/api.sh" "$FR/webroot/api.sh"
cp "$MOD/socfreq.conf" "$FR/socfreq.conf"
ok "$( (cd "$FR" && SOCFREQ_DATA_DIR="$FRESH" sh webroot/api.sh conf_get 2>&1) | grep -c '^INTERVAL=' )" "1" \
   "数据目录空时自动从模块目录搬一次，而不是报 E/NO_CONF"
ok "$( [ -f "$FRESH/socfreq.conf" ] && echo moved || echo missing )" "moved" \
   "搬完配置确实在数据目录里"

echo "== icon 命令 =="
reject icon 'foo bar'
reject icon '$(id)'
reject icon 'com.x;rm -rf /'
reject icon 'nodots'

# 图标走守护的 icon 子命令。unzip 只做兜底，不做主路
# （部分 ROM 自带 unzip，但不能假定每台机器都有）。
# 下面用桩守护驱动整条链路。
mkdir -p "$TMP/fakebin"
# 守护桩：MIME 跟着 APK 里实际条目的名字走，和线上的 icon.c 同一套判据
# （真守护是看中央目录里的条目名，不是看 apk 文件名）。
# 按 apk 文件名返回的话，"webp 用 image/webp"这条断言测的就只是文件名后缀，
# 而不是 MIME 该不该跟着实际选中的那张图走。
cat > "$TMP/fakebin/socfreqd" <<'EOF'
#!/bin/sh
[ "$1" = "icon" ] || exit 1
apk=$2
[ -r "$apk" ] || exit 1
if grep -q '\.webp' "$apk" 2>/dev/null; then
  printf 'data:image/webp;base64,WEBPPAYLOAD\n'
else
  printf 'data:image/png;base64,PNGPAYLOAD\n'
fi
exit 0
EOF
chmod +x "$TMP/fakebin/socfreqd"

# pm 桩：既要回答 path（icon 用），也要回答 list（games_list 筛已安装用）。
# 少了 list 那半边，games_list 会拿到空集合，于是"只列已装"这条
# 看着通过了，其实是因为谁都匹配不上——假的。
# 另外 stub apk 必须真存在：api.sh 会先判 [ -r "$_apk" ]，
# 文件不存在就直接走"没装"分支，守护根本不会被调用——
# 那样断言照样能绿，但测的完全不是图标那条路。
printf 'STUBAPK' > "$TMP/fake.webp.apk"
cat > "$TMP/fakebin/pm" <<EOF
#!/bin/sh
case "\$1" in
  path) echo "package:$TMP/fake.webp.apk" ;;
  list) echo "package:com.example.game"; echo "package:com.test.game" ;;
esac
exit 0
EOF
# stat 桩：顶掉 -c %Y（PC 的 stat 是别的方言），给固定 mtime
cat > "$TMP/fakebin/stat" <<'EOF'
#!/bin/sh
echo 1700000000
EOF
chmod +x "$TMP/fakebin/pm" "$TMP/fakebin/stat"

# 让 api.sh 找得到桩守护。api.sh 按自己的位置拼 BIN="$ROOT/socfreqd"，
# 所以桩必须放进这套临时目录（$TMP），不是 $MOD——
# 放 $MOD 会把真的守护覆盖掉，而 $TMP 本来就是一次性目录，随便写。
cp "$TMP/fakebin/socfreqd" "$TMP/socfreqd"
chmod +x "$TMP/socfreqd"

run_api() {
  SOCFREQ_ICON_DIR="$TMP/data/local/tmp/sficon" \
  PATH="$TMP/fakebin:$ORIG_PATH" \
  export SOCFREQ_ICON_DIR PATH
  sh webroot/api.sh "$@"
}

OUT=$(run_api icon com.example.game)
ok "$OUT" "data:image/png;base64,PNGPAYLOAD" "icon 返回完整 data URI（自带 MIME）"

# 缓存键必须带 mtime 和取图算法版本：前者让应用换图标后自动失效，
# 后者让"改了选图规则"之后老缓存里选错的那张也自动失效。
IALGO=$(sed -n 's/^ICON_ALGO=//p' "$MOD/webroot/api.sh" | head -n 1)
ok "$(ls "$TMP/data/local/tmp/sficon" 2>/dev/null \
      | grep -c "^com\\.example\\.game\\.1700000000\\.${IALGO}\\.b64$")" \
   "1" "缓存键带 APK mtime 与算法版本（换图标/改算法都会自动失效）"

# 再要一次：应该命中缓存，返回同样的内容
OUT2=$(run_api icon com.example.game)
ok "$OUT2" "$OUT" "第二次命中缓存，返回一致"

# ── 图标来源（.via）：观测入口必须在第二次以后也还有效 ──────────────
# 兜底一旦生效，主路坏没坏就再也看不出来了——页面照常出图、没有任何异常。
# 所以关于页那一行必须说清这张图是谁给的。而"用户点开关于页"这件事
# 几乎必然发生在缓存命中之后：如果命中分支写死裸 OK，
# 这条线索就只在第一次取图时出现过一次，等于没有。
GAME_B64="$TMP/data/local/tmp/sficon/com.example.game.1700000000.${IALGO}.b64"
LAST=$(run_api about 2>/dev/null | sed -n 's/^ICONLAST=//p')
ok "$LAST" "OK=DAEMON" "守护取图成功后关于页记 OK=DAEMON"

# 上一步是缓存命中（第二次调用），来源必须还是 OK=DAEMON 而不是裸 OK
LAST2=$(run_api about 2>/dev/null | sed -n 's/^ICONLAST=//p')
ok "$LAST2" "OK=DAEMON" "缓存命中仍报出原始来源（这里退回裸 OK 就是失去线索）"

# 兜底给的图：.via 内容是 UNZIP
printf 'UNZIP\n' > "$GAME_B64.via"
run_api icon com.example.game >/dev/null
LAST3=$(run_api about 2>/dev/null | sed -n 's/^ICONLAST=//p')
ok "$LAST3" "OK=UNZIP" "兜底给的图，缓存命中后仍报 OK=UNZIP"

# .via 丢了（老缓存/被清过）：报裸 OK，宁可"来源未记录"也不猜 DAEMON。
# 猜错一次的代价是这条线索永久失真——以后看到 OK=DAEMON 也不知道是真是假。
rm -f "$GAME_B64.via"
run_api icon com.example.game >/dev/null
LAST4=$(run_api about 2>/dev/null | sed -n 's/^ICONLAST=//p')
ok "$LAST4" "OK" ".via 缺失时报裸 OK（不猜来源）"

# icon_trim 必须连着删 .via：它 grep 的是 '\.b64$'，孤儿 .via 数不到，
# 不单独清就每淘汰一张图留一个垃圾文件。
rm -rf "$TMP/data/local/tmp/sficon"; mkdir -p "$TMP/data/local/tmp/sficon"
: > "$TMP/data/local/tmp/sficon/orphan.1.b64.via"          # 只有 .via，没有对应 .b64
run_api icon com.example.game >/dev/null
ok "$([ -f "$TMP/data/local/tmp/sficon/orphan.1.b64.via" ] && echo yes || echo no)" \
   "no" "找不到对应 .b64 的孤儿 .via 被清掉"
rm -rf "$TMP/data/local/tmp/sficon"; mkdir -p "$TMP/data/local/tmp/sficon"

# webp 必须给 image/webp，不能一律 png
rm -rf "$TMP/data/local/tmp/sficon"; mkdir -p "$TMP/data/local/tmp/sficon"
printf 'STUBAPK res/mipmap-mdpi-v4/ic_launcher.webp' > "$TMP/fake.webp.apk"
OUT3=$(run_api icon com.example.game)
ok "$OUT3" "data:image/webp;base64,WEBPPAYLOAD" \
   "webp 图标给 image/webp（写死 png 就是破图）"
# 换回 png 的那个，后面用例还要用
printf 'STUBAPK res/mipmap-mdpi-v4/ic_launcher.png' > "$TMP/fake.webp.apk"

# ── 批量取图：一次调用给多行 "包名<TAB>URI" ──────────────────────
# 页面画一屏列表要走这条路；逐包调用就是几十次 root 往返。
# 先清缓存：上面 webp 用例把同一个内存键写成了 WEBPPAYLOAD，不清就比的是旧图。
rm -rf "$TMP/data/local/tmp/sficon"; mkdir -p "$TMP/data/local/tmp/sficon"
BATCH=$(run_api icons com.example.game com.test.game)
ok "$(printf '%s\n' "$BATCH" | grep -c '^com\.example\.game	')" "1" \
   "icons 每个包给一行"
ok "$(printf '%s\n' "$BATCH" | grep -c '^com\.example\.game	data:image/png;base64,PNGPAYLOAD$')" "1" \
   "icons 的图带在包名后面（TAB 分隔）"
ok "$(printf '%s\n' "$BATCH" | grep -c '^com\.test\.game	data:image/png;base64,PNGPAYLOAD$')" "1" \
   "icons 一次取多个包"
# 非法包名只跳过它自己，不把整批打断
BATCH2=$(run_api icons 'bad;pkg' com.example.game)
ok "$(printf '%s\n' "$BATCH2" | grep -c '^bad')" "0" "icons 跳过非法包名"
ok "$(printf '%s\n' "$BATCH2" | grep -c '^com\.example\.game	')" "1" \
   "icons 遇到非法包名后，后面的包照常返回"
# 没图（换一个只会空回的桩；验证完恢复原桩，后面的用例还要它）
cp "$TMP/fakebin/socfreqd" "$TMP/fakebin/socfreqd.batchbak"
cat > "$TMP/fakebin/socfreqd" <<'EOF'
#!/bin/sh
[ "$1" = "icon" ] || exit 1
exit 0
EOF
cp "$TMP/fakebin/socfreqd" "$TMP/socfreqd"
NOICON=$(run_api icons com.example.none)
ok "$NOICON" "com.example.none	" "icons 没图时只给包名和一空列"
cp "$TMP/fakebin/socfreqd.batchbak" "$TMP/fakebin/socfreqd"
rm -f "$TMP/fakebin/socfreqd.batchbak"
cp "$TMP/fakebin/socfreqd" "$TMP/socfreqd"

# 缓存上限：塞超过上限的条目，最旧的要被清掉
rm -rf "$TMP/data/local/tmp/sficon"; mkdir -p "$TMP/data/local/tmp/sficon"
for i in $(seq 1 210); do : > "$TMP/data/local/tmp/sficon/pkg$i.1.b64"; sleep 0.01 2>/dev/null; done
run_api icon com.example.game >/dev/null
ok "$([ "$(ls "$TMP/data/local/tmp/sficon" | grep -c '\.b64$')" -le 201 ] && echo yes)" \
   "yes" "缓存条目数被压在上限附近（<=201）"

# 真实环境里没装该应用时的行为。
# 必须绕开 fakebin 里的 pm 桩，否则桩子照样返回那个假 APK，
# 测的就不是"没装"这条路径了，看着却是绿的。
NOPM=$(PATH="$ORIG_PATH" SOCFREQ_ICON_DIR="$TMP/data/local/tmp/sficon" \
       sh webroot/api.sh icon com.example.notinstalled 2>&1); NOPM_RC=$?
ok "$NOPM_RC" "0" "icon 无 pm 时正常退出（不报错）"
ok "$NOPM" "" "icon 无 pm 时输出空 → 页面退色块"

# 守护取不到图标（比如 APK 损坏、里面根本没有位图）时，
# 不能写一个空文件进缓存——空文件会被下次当成命中，图标就永远出不来。
rm -f "$TMP/data/local/tmp/sficon"/*
cat > "$TMP/fakebin/socfreqd" <<'EOF'
#!/bin/sh
[ "$1" = "icon" ] || exit 1
exit 0
EOF
cp "$TMP/fakebin/socfreqd" "$TMP/socfreqd"; chmod +x "$TMP/socfreqd"
EMPTY=$(run_api icon com.example.broken)
ok "$EMPTY" "" "守护没给出图标时输出空 → 页面退色块"
ok "$(ls "$TMP/data/local/tmp/sficon" 2>/dev/null | grep -c 'com\.example\.broken')" \
   "0" "取不到时不写空缓存（否则下次永远命中空缓存，图标再也不出现）"
cp "$TMP/fakebin/socfreqd" "$TMP/fakebin/socfreqd.cur"
# 恢复正常桩，后面的用例还要用
cat > "$TMP/fakebin/socfreqd" <<'EOF'
#!/bin/sh
[ "$1" = "icon" ] || exit 1
case "$2" in
  *webp*) printf 'data:image/webp;base64,WEBPPAYLOAD\n'; exit 0 ;;
esac
printf 'data:image/png;base64,PNGPAYLOAD\n'
EOF
cp "$TMP/fakebin/socfreqd" "$TMP/socfreqd"; chmod +x "$TMP/socfreqd"

# 缓存目录不能是 /tmp：Android 正常运行的 root shell 里没有 /tmp。
# 只看代码行，注释里提到 /tmp 是解释为什么不能用它，不算违规。
if grep -v '^\s*#' webroot/api.sh | grep -qE '(^|[^a-zA-Z0-9_/.-])/tmp'; then
  echo "  FAIL 还在用 /tmp（Android 上不存在）"; fail=$((fail+1))
else
  ok "yes" "yes" "不再用 /tmp（Android 正常运行下没有这个目录）"
fi
# unzip：**允许出现，但只能是兜底**。
#
# unzip 在部分 ROM 上确实存在（不是"开发机有、目标机没有"那种坑），
# 一刀切禁掉会把唯一的第二条路也砍了，所以只许它当兜底。
#
# 现在要盯的是**两件事**，都仍然成立：
#   1. 取图主路必须还是守护自己解析中央目录，不能退化成依赖 unzip。
#   2. unzip 用之前先探测命令存在，缺了要静默返回（不能报错、不能留垃圾）。
# 这两条才是"最坑的一类依赖"的真正防法：不是禁止出现，
# 而是禁止它出现在**唯一路径**上。
if grep -v '^\s*#' webroot/api.sh | grep -q 'command -v unzip >/dev/null 2>&1 || return 1'; then
  ok "yes" "yes" "unzip 只做兜底，用前先探测命令存在（缺了就静默跳过）"
else
  echo "  FAIL unzip 兜底必须先探测命令存在，不能假定目标机一定有"; fail=$((fail+1))
fi
# 主路没被替换掉：守护的 icon 子命令仍然是第一选择。
if grep -v '^\s*#' webroot/api.sh | grep -q '"\$BIN" icon "\$_apk"'; then
  ok "yes" "yes" "主路仍是守护的 icon 子命令（unzip 没有取而代之）"
else
  echo "  FAIL 取图主路必须还是守护的 icon 子命令"; fail=$((fail+1))
fi
# 兜底不许自带挑图规则：mipmap / ic_launcher 一个都不许出现在 shell 里，
# 否则就是两处规则漂移（改了 C 里的密度权重，兜底还按老的挑）。
if grep -v '^\s*#' webroot/api.sh | grep -qiE 'mipmap-|ic_launcher|xxhdpi'; then
  echo "  FAIL shell 里出现了挑图规则（必须只有 C 里 score_name 一份）"; fail=$((fail+1))
else
  ok "yes" "yes" "shell 里没有挑图规则（挑图统一走守护 score）"
fi
# 兜底必须验内容前缀，否则会把垃圾当图标缓存下来。
if grep -v '^\s*#' webroot/api.sh | grep -q 'iVBOR'; then
  ok "yes" "yes" "unzip 兜底校验 base64 内容前缀（不验就会缓存垃圾）"
else
  echo "  FAIL unzip 兜底必须校验 base64 前缀"; fail=$((fail+1))
fi
# 兜底成功必须记 OK=UNZIP 而不是 OK。
# 记成 OK 的话，"主路坏没坏"这个信息就彻底丢了：页面照常出图，
# 没有任何异常表现，而守护的 zip 解析该修的那件事没人会发现。
if grep -v '^\s*#' webroot/api.sh | grep -q 'icon_fail OK=UNZIP'; then
  ok "yes" "yes" "兜底成功记 OK=UNZIP 而不是 OK（留下主路的诊断入口）"
else
  echo "  FAIL 兜底成功必须记 OK=UNZIP，OK 会把主路故障藏起来"; fail=$((fail+1))
fi
# TOOBIG 要有分派：>4GB 时中央目录偏移会超出 uint32，报 OPEN 会让
# "包太大"和"文件坏了"共用一个码，查的时候会照着查权限和路径。
if grep -v '^\s*#' webroot/api.sh | grep -q 'TOOBIG)  icon_fail TOOBIG'; then
  ok "yes" "yes" "E/TOOBIG 有独立分派（不与 OPEN 混用）"
else
  echo "  FAIL E/TOOBIG 必须有独立分派"; fail=$((fail+1))
fi
# icon_last 的白名单必须容得下 '='。
# 退回"只认大写字母"的话 OK=UNZIP 会被归一化成 UNKNOWN，
# 于是兜底成功了关于页却说"还没取过图标"——凭空造一个假的失败状态。
# 注意不能只 grep "[!A-Z]"：那是 "[!A-Z=]" 的**子串**，会匹配到自己这行。
# 也不要把方括号和星号反斜杠转义——BRE 里 [!A-Z=] 本身就是字符类，
# 转义成 \[!A-Z=\] 反而匹配不到（转义后它表示字面量方括号）。
if grep -v '^\s*#' webroot/api.sh | grep -qF '[!A-Z=]*) _l=UNKNOWN'; then
  ok "yes" "yes" "icon_last 白名单容得下 =（否则 OK=UNZIP 变 UNKNOWN）"
else
  echo "  FAIL icon_last 白名单必须容得下 ="; fail=$((fail+1))
fi
# 反向：不能退回只认大写字母。-F 固定串，"*" 不再当通配，
# 所以 "[!A-Z]*)" 只会匹配字面量那一种写法，不会命中 "[!A-Z=]*)"。
if grep -v '^\s*#' webroot/api.sh | grep -qF '[!A-Z]*) _l=UNKNOWN'; then
  echo "  FAIL icon_last 白名单退回只认大写字母（OK=UNZIP 会被吞成 UNKNOWN）"; fail=$((fail+1))
else
  ok "yes" "yes" "白名单没有退回只认大写字母（反向断言也钉住）"
fi
# 图标写入必须先落同目录临时文件再 rename（原子），
# 临时文件也必须锁在 ICON_DIR 里：写到别处就是跨设备 mv，
# 在 /data 上 rename 失败会退化成 cp，中途断电就留下半个缓存文件，
# 而半个文件下次会被当成命中——图标永远出不来。
if grep -v '^\s*#' webroot/api.sh | grep -q '_out\.tmp\$\$'; then
  ok "yes" "yes" "图标写入走同目录临时文件 + rename"
else
  echo "  FAIL 图标写入没有走同目录临时文件"; fail=$((fail+1))
fi
if grep -v '^\s*#' webroot/api.sh | grep -q 'ICON_DIR/\.\?*installed'; then
  echo "  FAIL 名单筛选又把临时文件写回 ICON_DIR 了（该用 mktemp）"; fail=$((fail+1))
else
  ok "yes" "yes" "名单筛选用 mktemp，不依赖 ICON_DIR 是否可写"
fi

echo "== 文件未被破坏 =="
ok "$(grep -c '^GPU_PCT=' socfreq.conf)" "1" "GPU_PCT 行还在"
ok "$(grep -c '^INTERVAL=' socfreq.conf)" "1" "INTERVAL 行还在"
ok "$(grep -c '^CPU_GOVERNOR=' socfreq.conf)" "1" "CPU_GOVERNOR 行还在"
# 注释行不能被当成配置行改掉
ok "$(grep -c '^#' socfreq.conf)" "$(grep -c '^#' "$MOD/socfreq.conf")" "conf_get/conf_set 没动注释行"

A games_del com.test.game >/dev/null
ok "$(grep -c -x 'com.test.game' games.txt)" "0" "可删除包名"

# 包名里的点是正则元字符：不用 -F 的话 "com.a.b" 会误删别的行
cp games.txt games.keep
printf 'comXaXb\ncom.zzz\n' >> games.txt
A games_del 'com.a.b' >/dev/null
ok "$(grep -c -x 'comXaXb' games.txt)" "1" "删除时点没被当正则（com.a.b 不会误删 comXaXb）"
mv games.keep games.txt

echo "== 名单条数复原 =="
# games_list 现在默认只列"这台机上真装了的"。
# 桩的 pm 只报 com.example.* 那一个，所以只有一条应该留下来。
IN_STUB=$(grep '^com\.example\.' games.txt | head -1)
ok "$(A games_list)" "$IN_STUB" "games_list 只列这台机上真装了的"
ok "$(A games_list all | wc -l)" \
   "$(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' games.txt | tr -d '\r' | wc -l)" \
   "games_list all 返回全量预设名单"

# 顺序是用户自己排的，脚本不许重排（对全量那条）
ok "$(A games_list all | head -3 | md5sum)" \
   "$(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' games.txt | head -3 | tr -d '\r' | md5sum)" \
   "games_list 不重排名单（保持文件原序）"

# 未安装的一律不列：用户要的是"我手机上有什么"，
# 而不是一份 400 条的预设清单（后者看着像功能坏了）。
cat > "$TMP/fakebin/pm" <<'EOF'
#!/bin/sh
[ "$1" = "list" ] && echo "package:com.android.settings"
exit 0
EOF
chmod +x "$TMP/fakebin/pm"
ok "$(A games_list)" "" "一个都没装时列表为空（不是把 400 条预设全倒出来）"

# 装了一个：只该出这一个
cat > "$TMP/fakebin/pm" <<EOF
#!/bin/sh
case "\$1" in
  path) echo "package:$TMP/fake.webp.apk" ;;
  list) echo "package:com.android.settings"; echo "package:com.example.game" ;;
esac
exit 0
EOF
chmod +x "$TMP/fakebin/pm"
ok "$(A games_list)" "com.example.game" "只留真正装了的那个"

# 过滤必须整行精确匹配。两个坑要分别造出"长度刚好对得上"的样本，
# 否则用例是假的（这一点是实测出来的）：
#  * 去掉 -F：已装 com.a.b，正则里的 . 会命中名单里的 comXaXb。
#    两者必须放在**同一份名单**里对比——分开放时，名单里没有那个"像"的行，
#    命中与否都只取决于 com.a.b 自己，去掉 -F 看不出差别。
#    长度也必须一致（都是 7 个字符），否则 . 展开成单字符也匹配不上。
#  * 去掉 -x：已装 com.example.game，前缀匹配会连 com.example.game2 一起收。
printf 'com.a.b\ncomXaXb\ncom.example.game2\n' >> games.txt
cat > "$TMP/fakebin/pm" <<EOF
#!/bin/sh
case "\$1" in
  path) echo "package:$TMP/fake.webp.apk" ;;
  list) echo "package:com.android.settings"
        echo "package:com.example.game"
        echo "package:com.a.b" ;;
esac
exit 0
EOF
chmod +x "$TMP/fakebin/pm"
# 期望：com.example.game 和 com.a.b 都在名单里、也确实装了，要留下；
# comXaXb 不该出（. 被当正则当成通配），
# com.example.game2 也不该出（前缀匹配）。
ok "$(A games_list | tr '\n' ' ')" "com.example.game com.a.b " \
   "过滤是整行精确匹配（. 没被当正则，也不是前缀匹配）"

# ── CRLF 名单 ────────────────────────────────────────────────────
# 症状：名单几百条，页面上"本机已装 0 个"，一条都列不出来，也不报错。
# 原因：CRLF 的行尾带 \r，而 grep 的模式来自 pm（纯 LF），
# -x 要求整行相等 → 永远匹配不上。tr 放在 grep 之后就晚了。
#
# 这条断言必须打在**行为**（命中/删除）上，不能打在"文件里有没有 \r"上：
# MSYS 读文件时会翻译行尾，直接查 CR 在 PC 上一律是 0，
# 查了就等于没测（这也是这条 bug 能一路绿灯到真机的原因）。
# 下面用 printf 显式写 \r\n 造样本，然后要求 games_list 照样命中。
cp games.txt "$TMP/games.keep"
printf '# CRLF 注释\r\ncom.example.game\r\ncom.example.other\r\n' > games.txt
cat > "$TMP/fakebin/pm" <<EOF
#!/bin/sh
case "\$1" in
  path) echo "package:$TMP/fake.webp.apk" ;;
  list) echo "package:com.example.game" ;;
esac
exit 0
EOF
chmod +x "$TMP/fakebin/pm"
ok "$(A games_list | tr -d '\r')" "com.example.game" \
   "CRLF 名单也能命中（匹配前必须规范化行尾）"

# 同一个包重复添加要报 DUP：CRLF 下 -x 判不相等，会误报 OK 并追加第二份。
OUT=$(A games_add com.example.game)
ok "$(printf '%s' "$OUT" | cut -d' ' -f1)" "DUP" \
   "CRLF 名单里判重有效（同一个包不会反复追加）"
ok "$(grep -c -x 'com.example.game' games.txt)" "1" \
   "重复添加后名单里只有一条"

# 删除必须真的删掉：CRLF 下 -x 不匹配，grep -v 反而把那行留下，
# 页面提示"已移除"、刷新一下又回来。
ok "$(A games_del com.example.game | cut -d' ' -f1)" "OK" "删除 CRLF 行报 OK"
ok "$(A games_list | tr -d '\r')" "" "删完确实没了（不会刷新后又回来）"
# list_del 走 write_atomic 重写整份文件，等于顺手把 CRLF 治好了
if grep -q $'\r' games.txt 2>/dev/null; then
  ok "still-crlf" "fixed" "删除后名单被重写成 LF"
else
  ok "fixed" "fixed" "删除后名单被重写成 LF"
fi
cp "$TMP/games.keep" games.txt

echo "== 状态读取与暂停落在数据目录 =="
# 最难查的是"回执正常、行为没变"这一类，这里两处都要盯：
#   st_status 还读 $ROOT/run/status → 首页徽标永远显示"未运行"，
#     而守护其实跑得好好的（pid、各簇频率那些来自 export 的部分照常刷）；
#   cmd_pause 还写 $ROOT/pause     → 页面提示"已暂停"，
#     守护查的是 $DATA/pause，于是继续限频，看门狗也照常拉起。
# 模块目录和数据目录混在一起时这两条全都测不出来（同一个文件读写都对），
# 所以必须放在 SA（数据目录指向 $SEP）这一组里跑。
#
# 换一个会答 export 的守护桩：前面图标那几组把 $TMP/socfreqd 换成了
# "只认 icon、其余 exit 1" 的变体，而 st_status 的头一句就是 `$BIN export`，
# 桩不答它就 die DUMP，下面的状态断言一行都跑不到。
cat > "$TMP/socfreqd" <<'EOF'
#!/bin/sh
[ "$1" = "export" ] && { printf 'ver=test\nalive=1\npct_big=65\nsoc=TESTSOC\n'; exit 0; }
exit 0
EOF
chmod +x "$TMP/socfreqd"
mkdir -p "$SEP/run"
printf 'state=normal\ngame_pkg=-\nreason=-\npid=4242\n' > "$SEP/run/status"
ST=$(SA status)
ok "$(printf '%s\n' "$ST" | sed -n 's/^state=//p')" "normal" \
   "status 读的是数据目录里的 run/status"
ok "$(printf '%s\n' "$ST" | grep -c '^reason=')" "1" \
   "status 透传 reason（游戏/开机满血/熄屏解除三种交还要分得开）"
ok "$(printf '%s\n' "$ST" | grep -c '^pct_big=')" "1" \
   "status 前半段还是守护 export 的硬件数据"

# 数据目录那份不存在时回退读老位置：覆盖安装后老守护可能还在往模块目录写，
# 这时候读老那份至少显示的是真实状态，比一律"未运行"强。
mkdir -p "$TMP/run"
printf 'state=game\ngame_pkg=com.example.game\nreason=game\npid=1\n' > "$TMP/run/status"
mv "$SEP/run/status" "$SEP/status.bak"
ok "$(SA status | sed -n 's/^state=//p')" "game" \
   "数据目录没有时回退读模块目录那份（老装机不白屏）"
# 两处都有时以数据目录为准
mv "$SEP/status.bak" "$SEP/run/status"
ok "$(SA status | sed -n 's/^state=//p')" "normal" \
   "两处都有时以数据目录为准（守护现在只写那儿）"
rm -rf "$TMP/run"
rm -f "$SEP/run/status"
ok "$(SA status | sed -n 's/^state=//p')" "unknown" \
   "两处都没有时给可解析的 state=unknown（空串会让前端白屏）"

SA pause >/dev/null 2>&1
ok "$([ -f "$SEP/pause" ] && echo yes)" "yes" \
   "pause 落在数据目录（守护的 PAUSE_FILE 也在那儿）"
ok "$([ -f "$TMP/pause" ] && echo yes || echo no)" "no" \
   "pause 不再写进模块目录"
SA resume >/dev/null 2>&1
ok "$([ -f "$SEP/pause" ] && echo yes || echo no)" "no" \
   "resume 删掉的是数据目录那份"

echo "== 末行不带换行时的追加分支 =="
# conf_put / list_add 都是"读全量 + 追加一行"。包里那份 socfreq.conf
# 一度以 CPU_GOVERNOR="" 结尾、不带换行，装机拿到的就是它，
# 于是用户第一次保存新键就粘成 CPU_GOVERNOR=""GAME_GRACE_SEC=10。
# 守护的 cfg_get 和 api.sh 的 conf_read 都按行首找键，
# 粘进去的那个键从此读不到——页面显示默认值，用户以为"改了没保存"。
NL="$TMP/nlcase"; mkdir -p "$NL/webroot"
cp "$MOD/webroot/api.sh" "$NL/webroot/api.sh"
printf 'INTERVAL=5\nGPU_PCT=30\nCPU_GOVERNOR=""' > "$NL/socfreq.conf"
printf 'com.a.game\ncom.b.game' > "$NL/games.txt"
NA() { ( cd "$NL" && SOCFREQ_DATA_DIR="$NL" sh webroot/api.sh "$@" ); }
NA conf_set SCREEN_UNCAP 1 >/dev/null 2>&1
ok "$(grep -c '^SCREEN_UNCAP=' "$NL/socfreq.conf")" "1" "conf_set 追加新键没粘到上一行"
ok "$(grep -c '^CPU_GOVERNOR=' "$NL/socfreq.conf")" "1" "末行仍然是独立一行"
ok "$(grep -c '=""[A-Z]' "$NL/socfreq.conf")" "0" "没有 \"\" 紧跟键名的粘行残留"
# 已经存在的键走整行替换，不受影响：确认追加修法没把替换那半边改坏
NA conf_set GPU_PCT 22 >/dev/null 2>&1
ok "$(sed -n 's/^GPU_PCT=//p' "$NL/socfreq.conf")" "22" "已存在的键仍然是整行替换"
NA games_add com.c.game >/dev/null 2>&1
ok "$(grep -c -x 'com.c.game' "$NL/games.txt")" "1" "games_add 追加包名没粘到上一行"
ok "$(grep -c 'gamecom\.' "$NL/games.txt")" "0" "名单里没有粘连的包名"
NA games_add com.c.game >/dev/null 2>&1
ok "$(NA games_add com.c.game | cut -d' ' -f1)" "DUP" "补过换行之后判重仍然有效"

echo "== 分应用档位 modes_* =="
# app_modes.conf 是 pkg=full / pkg=NN / pkg=NN:NN。守护按行首匹配读它，
# 所以这里的重点不是"能不能写进去"，而是"写进去的那条能不能被读到"：
# 同包留两行、包名被当正则、CRLF 残留、末行粘行——四种都会变成
# "页面上设了，实际没生效"。
ok "$(A modes_set com.android.camera full | tr -d '\r')" "OK com.android.camera=full" \
   "modes_set full 回执带 pkg=值"
ok "$(A modes_set com.autonavi.minimap 80 | cut -d' ' -f1)" "OK" "modes_set 数字档"
ok "$(A modes_list | tr -d '\r' | grep -c -x 'com.android.camera=full')" "1" "modes_list 能列出来"
# 越界与坏值
reject modes_set com.x.y 0
reject modes_set com.x.y 101
reject modes_set com.x.y abc
reject modes_set com.x.y '80;echo pwned'
reject modes_set 'com x' full
reject modes_set 'nodots' full
# 同一包换值：必须覆盖成一行，不能追加第二行。
# 守护读的是"后一条赢"，可页面上会列出两个同名条目，
# 而用户删的时候只删得掉一行。
A modes_set com.android.camera 70 >/dev/null
ok "$(grep -c '^com.android.camera=' "$TMP/app_modes.conf")" "1" "同包换值只留一行"
ok "$(A modes_list | tr -d '\r' | grep -x 'com.android.camera=70')" "com.android.camera=70" \
   "覆盖后是新值（不是旧值还在、新值也加了一行）"
ok "$(A modes_set com.android.camera 70 | cut -d' ' -f1)" "DUP" "同包同值报 DUP"
# FULL 归一：手改文件的人写 FULL 也能命中，但存进去要统一成 small，
# 否则同一个包会有 full/FULL 两条互相"看不见"。
A modes_set com.map.FULL FULL >/dev/null
ok "$(A modes_list | tr -d '\r' | grep -x 'com.map.FULL=full')" "com.map.FULL=full" \
   "FULL 归一成 full 再落盘"
# CPU:GPU 双值：两段各判各的；任一段留空表示那一边跟随全局。
ok "$(A modes_set com.dual.both 70:40 | tr -d '\r')" "OK com.dual.both=70:40" \
   "双值写入回执带完整 pkg=cpu:gpu"
ok "$(A modes_set com.dual.cpu 60: | cut -d' ' -f1)" "OK" "60:（GPU 跟随全局）能写"
ok "$(A modes_set com.dual.gpu :40 | cut -d' ' -f1)" "OK" ":40（CPU 跟随全局）能写"
ok "$(A modes_list | tr -d '\r' | grep -x 'com.dual.both=70:40')" "com.dual.both=70:40" \
   "modes_list 原样列出双值（页面自己拆）"
reject modes_set com.x.y :
reject modes_set com.x.y 70:101
reject modes_set com.x.y 0:40
reject modes_set com.x.y 70:abc
reject modes_set com.x.y 70:40:50
reject modes_set com.x.y full:40
A modes_set com.dual.cpu 80: >/dev/null
ok "$(A modes_list | tr -d '\r' | grep -c -x 'com.dual.cpu=80:')" "1" \
   "同包换值仍只留一行，冒号格式保留"
ok "$(A modes_set com.keep.app keep | tr -d '\r')" "OK com.keep.app=keep" \
   "keep（熄屏也封顶）可写"
ok "$(A modes_list | tr -d '\r' | grep -x 'com.keep.app=keep')" "com.keep.app=keep" \
   "modes_list 原样列出 keep"
A modes_set com.keep.app KEEP >/dev/null
ok "$(A modes_list | tr -d '\r' | grep -x 'com.keep.app=keep')" "com.keep.app=keep" \
   "KEEP 归一成 small，不会出现第二条"
# 双值 ↔ 裸数字互换：同一包改值必须覆盖，不能因为"整行不同"就追加。
# DUP 只该在包名和值都相同时出现；否则用户把 70:40 改成 70 会留下两条，
# 守护按后一条赢、页面却列两行。
printf 'com.dual.swap=70:40\n' > "$TMP/app_modes.conf"
A modes_set com.dual.swap 70 >/dev/null
ok "$(grep -c '^com.dual.swap=' "$TMP/app_modes.conf")" "1" "70:40 改成 70 只留一行"
ok "$(grep -x 'com.dual.swap=70' "$TMP/app_modes.conf")" "com.dual.swap=70" "值真的换成裸数字"
A modes_set com.dual.swap 65:35 >/dev/null
ok "$(grep -c '^com.dual.swap=' "$TMP/app_modes.conf")" "1" "裸数字改回双值也只留一行"
ok "$(A modes_set com.dual.swap 65:35 | cut -d' ' -f1)" "DUP" \
   "包名和值都相同才报 DUP"
# 包名不能当正则用：com.a.b 里的点是通配符，
# 用 grep -E "^com\.a\.b=" 去删会连 comXaXb=90 一起删掉。
printf 'com.a.b=50\ncomXaXb=90\n' > "$TMP/app_modes.conf"
ok "$(A modes_del com.a.b | cut -d' ' -f1)" "OK" "删除报 OK"
ok "$(A modes_list | tr -d '\r' | grep -c '^comXaXb=90')" "1" \
   "同位置的 comXaXb 没被顺带删掉（包名当正则会踩这个）"
# 这条判据自己也不能用正则：'^com.a.b=' 里的点同样是通配符，
# 会把 comXaXb 那行数进去（实测就是这样报的"还剩 1 条"）。
# 判"没了"要用固定串整行匹配。
ok "$(A modes_list | tr -d '\r' | grep -c -x 'com.a.b=50')" "0" "目标条目真的没了"
ok "$(A modes_del com.not.there | cut -d' ' -f1)" "MISS" "不存在时给 MISS 而不是假 OK"
# 值也要能按前缀删：页面只给包名，如果按整行相等删，
# 删 "com.x=50" 时文件里写的是 "com.x=60" 就删不掉。
printf 'com.x.y=60\n' > "$TMP/app_modes.conf"
A modes_del com.x.y >/dev/null
ok "$(A modes_list)" "" "按包名前缀删，值不一致也删得掉"
# CRLF 文件：写进去的行守护按 ^pkg= 匹配，带 \r 的值列会读到 "60\r"
printf 'com.crm.a=60\r\ncom.crm.b=full\r\n' > "$TMP/app_modes.conf"
ok "$(A modes_set com.crm.c 50 | cut -d' ' -f1)" "OK" "CRLF 档位文件仍能追加"
# 数 CR 用 tr -cd '\r' | wc -c，不用 grep -c $'\r'：
# MSYS 的 grep 会把"只有 CR"这个模式当成空模式，于是每一行都算命中，
# 输出明明干净却报 3 个（假红，量具坏了不是产品坏了）。
ok "$(A modes_list | tr -cd '\r' | wc -c | tr -d ' ')" "0" "modes_list 输出里没有 CR 残留"
ok "$(A modes_del com.crm.a | cut -d' ' -f1)" "OK" "CRLF 行删得掉"
ok "$(A modes_list | tr -d '\r' | grep -c '^com.crm.a=')" "0" "删完不会刷新又回来"
# 末行不带换行时追加不能粘行（同 conf 那个坑）
printf 'com.nl.a=50' > "$TMP/app_modes.conf"
A modes_set com.nl.b 60 >/dev/null
ok "$(grep -c '50com' "$TMP/app_modes.conf")" "0" "追加没和末行粘在一起"
ok "$(A modes_list | tr -d '\r' | grep -c -x 'com.nl.b=60')" "1" "新条目是独立一行"
# about 要能报出档位条数（用户报障时唯一能一眼看出"设过没有"的入口）
ok "$(A about | grep -c '^NMODE=')" "1" "about 带 NMODE"
ok "$(A about | sed -n 's/^NMODE=//p')" "$(tr -d '\r' < "$TMP/app_modes.conf" | awk '!/^[[:space:]]*$/ && !/^[[:space:]]*#/{n++} END{print n+0}')" \
   "about 的 NMODE 等于文件里的实际条数"
ok "$(A about | sed -n 's/^MODES=//p')" "1" "about 报 app_modes.conf 存在"
rm -f "$TMP/app_modes.conf"
ok "$(A about | sed -n 's/^MODES=//p')" "0" "文件不存在时 MODES=0"
ok "$(A about | sed -n 's/^NMODE=//p')" "0" "不存在时计数是 0，不是空行或 '0 0'"

echo "== 空列表不是执行失败 =="
# 装机模板 app_modes.conf 是"全是注释 + 例子"的一份文件，
# 而 modes_list 的最后一句是 grep -Ev 过滤注释 —— 一条都匹配不上时 grep 退出码 1。
# exec() 只看 errno，非 0 就抛错，前端于是弹一句"执行失败"：
# 用户什么都没做错，档位列表也确实是空的。games.txt 走的是同一条 grep 形状，
# 所以两个都要显式 return 0，并且在这里钉住。
printf '# 只有注释的模板\n#com.android.camera=full\n\n' > "$TMP/app_modes.conf"
sh webroot/api.sh modes_list >/dev/null 2>&1
ok "$?" "0" "只有注释的档位文件：modes_list 退出码 0（不是 1）"
ok "$(sh webroot/api.sh modes_list 2>/dev/null)" "" "同时输出为空（页面显示'还没有专属档位'）"
# 真机那次是这条路径：WebUI 打开配置页 → loadModes → modes_list → errno 1 → toast
ok "$(sh webroot/api.sh modes_list 2>&1 | grep -c '^E/')" "0" "stderr 里没有 E/ 错误码"
# 只留注释的名单（用户把名单清空了但保留表头）也必须是 0
cp games.txt games.keep2
printf '# 名单\n# 一行包名都没有\n' > games.txt
sh webroot/api.sh games_list all >/dev/null 2>&1
ok "$?" "0" "全注释的名单：games_list all 退出码 0"
ok "$(sh webroot/api.sh games_list all)" "" "输出为空而不是报错"
sh webroot/api.sh about >/dev/null 2>&1
ok "$?" "0" "about 不受空名单影响（COUNT=0 也要正常返回）"
ok "$(sh webroot/api.sh about | sed -n 's/^COUNT=//p')" "0" "空名单时 COUNT=0"
cp games.keep2 games.txt

echo "== apps_list（深睡豁免的选择器数据源；档位用 mode_apps）=="
# 只看第三方包：系统包几百个，全列出来没人挑，而且首屏会被撑爆。
AL="$TMP/applist"; mkdir -p "$AL/webroot" "$AL/bin"
cp "$MOD/webroot/api.sh" "$AL/webroot/"
cp "$MOD/socfreq.conf" "$AL/" 2>/dev/null
AA() { ( cd "$AL" && PATH="$AL/bin:$ORIG_PATH" SOCFREQ_DATA_DIR="$AL" sh webroot/api.sh "$@" ); }
cat > "$AL/bin/pm" <<'EOF'
#!/bin/sh
case "$*" in
  *-3*) printf 'package:com.zzz.last\npackage:com.aaa.first\npackage:com.mmm.mid\n' ;;
  *)    printf 'package:com.android.settings\npackage:com.android.providers.media\npackage:com.zzz.last\n' ;;
esac
EOF
chmod +x "$AL/bin/pm"
ok "$(AA apps_list | tr -d '\r' | tr '\n' '|')" "com.aaa.first|com.mmm.mid|com.zzz.last|" \
   "apps_list 默认只列 -3、去掉 package: 前缀并按名字排序"
ok "$(AA apps_list | grep -c 'android.settings')" "0" "默认不含系统包"
ok "$(AA apps_list all | grep -c 'android.settings')" "1" \
   "apps_list all 连系统包一起列（给选择器勾选系统应用）"
# pm 完全不可用（真机上可能是 pm 挂了）：返回空 + 退出码 0，不 die。
# 页面拿到空列表显示"没读到已安装应用"，比弹一个错误码好读。
cat > "$AL/bin/pm" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$AL/bin/pm"
ok "$(AA apps_list)" "" "pm 失败时输出为空"
AA apps_list >/dev/null 2>&1
ok "$?" "0" "pm 失败时退出码仍是 0（不是 die）"
# 一个第三方包都没装：同样是空 + 0
cat > "$AL/bin/pm" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$AL/bin/pm"
AA apps_list >/dev/null 2>&1
ok "$?" "0" "没装第三方包时也是空 + 退出码 0"

echo "== mode_apps（分应用档位：第三方 + 有桌面的系统应用）=="
# 系统包 400+，绝大多数永远不在前台；只并上带 LAUNCHER 活动的那些，
# 并且在 shell 里就把重复项去掉。相机/设置这类包在 ColorOS 的 -3 里没有。
cat > "$AL/bin/pm" <<'EOF'
#!/bin/sh
case "$*" in
  *-3*) printf 'package:com.zzz.last\npackage:com.aaa.first\npackage:com.oplus.camera\n' ;;
  *)    printf 'package:com.android.settings\npackage:com.android.providers.media\npackage:com.oplus.camera\npackage:com.zzz.last\n' ;;
esac
EOF
chmod +x "$AL/bin/pm"
cat > "$AL/bin/cmd" <<'EOF'
#!/bin/sh
cat <<'OUT'
189 activities found:
  Activity #0:
    priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true
    com.android.settings/.Settings
  Activity #1:
    com.oplus.camera/.Camera
OUT
exit 0
EOF
chmod +x "$AL/bin/cmd"
ok "$(AA mode_apps | tr '\n' '|')" \
   "com.aaa.first|com.android.settings|com.oplus.camera|com.zzz.last|" \
   "默认并上桌面入口的系统应用，且和第三方清单去重排序"
ok "$(AA mode_apps all | tr '\n' '|')" \
   "com.aaa.first|com.android.providers.media|com.android.settings|com.oplus.camera|com.zzz.last|" \
   "mode_apps all 连服务/组件一起列（页面「显示系统应用」）"
ok "$(AA apps_list | grep -c 'android.settings')" "0" \
   "apps_list 默认仍不含系统应用"
# 查询失败（老 ROM 没有 cmd / 权限被收）：回退到全部系统包，保证能选到系统应用
cat > "$AL/bin/cmd" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$AL/bin/cmd"
ok "$(AA mode_apps | tr '\n' '|')" \
   "com.aaa.first|com.android.providers.media|com.android.settings|com.oplus.camera|com.zzz.last|" \
   "cmd 不可用时回退到全部系统包（不再只剩第三方）"
AA mode_apps >/dev/null 2>&1
ok "$?" "0" "cmd 不可用时 mode_apps 不 die"

echo "== builtin_white（内置豁免的显示名）=="
mkdir -p "$AL/scripts"
cat > "$AL/scripts/builtin_white.txt" <<'EOF'
# 注释不算
com.tencent.mm          # 微信
com.tencent.wework      # 企业微信
EOF
ok "$(AA builtin_white | tr '\t' '=' | tr '\n' '|')" \
   "com.tencent.mm=微信|com.tencent.wework=企业微信|" \
   "只输出 包名<TAB>中文名，注释行不参与"
cat > "$AL/scripts/builtin_labels.txt" <<'EOF'
# 系统应用名兜底
com.android.settings      # 设置
com.oplus.camera          # 相机
com.miui.gallery          # 相册
EOF
ok "$(AA builtin_labels | tr '\t' '=' | tr '\n' '|')" \
   "com.android.settings=设置|com.oplus.camera=相机|com.miui.gallery=相册|" \
   "系统应用中文名兜底清单按 包名<TAB>名字 输出"
ok "$(AA builtin_labels | grep -c '^#')" "0" "兜底清单的注释行不输出"
rm -rf "$AL/scripts"
ok "$(AA builtin_white)" "" "清单缺失时返回空、不报错"
ok "$(AA builtin_labels)" "" "兜底清单缺失时也返回空"
AA builtin_white >/dev/null 2>&1
ok "$?" "0" "清单缺失时退出码仍是 0"

echo "== 发行商词典 games_suggest =="
# 词典只负责"挑出候选给人确认"，前台匹配永远只认 games.txt。
# 所以这里的重点有两半：命中规则要准（段相等，不是子串），
# 以及**已经在名单里的不能被再列一遍**（列了用户会以为还没加成功）。
VS="$TMP/vs"; mkdir -p "$VS"
cp "$MOD/vendors.txt" "$VS/vendors.txt"
printf '# 名单\ncom.tencent.tmgp.sgame\ncom.kurogames.exist\n' > "$VS/games.txt"
cp "$MOD/socfreq.conf" "$VS/socfreq.conf"
mkdir -p "$VS/webroot"; cp "$MOD/webroot/api.sh" "$VS/webroot/api.sh"
mkdir -p "$VS/bin"
cat > "$VS/bin/pm" <<'EOF'
#!/bin/sh
cat <<'L'
package:com.tencent.tmgp.sgame
package:com.tencent.tmgp.pubgmhd
package:com.miHoYo.Nap
package:com.mihoyo.bbs
package:com.mihoyo.community
package:com.netease.cloudmusic
package:com.tencent.mm
package:com.happyelements.AndroidAnimal
package:com.some.gameshop
package:com.foo.game.center
package:com.kurogames.exist
package:com.kurogames.newone
package:bin.mt.plus
package:com.google.android.apps.nexuslauncher
L
EOF
chmod +x "$VS/bin/pm"
VSRC="$TMP/vsrc"; mkdir -p "$VSRC/run"
VA() { ( cd "$VS" && PATH="$VS/bin:$ORIG_PATH" SOCFREQ_DATA_DIR="$VS" SOCFREQ_ICON_DIR="$VSRC/icon" \
         sh webroot/api.sh "$@" ); }
SUG=$(VA games_suggest | tr -d '\r')
ok "$(printf '%s\n' "$SUG" | grep -c '^com.tencent.tmgp.pubgmhd')" "1" "前缀型词条命中（tmgp 下的手游）"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.tencent.tmgp.sgame')" "0" "已在名单里的不再列一遍"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.miHoYo.Nap')" "1" "大小写不敏感（miHoYo 命中 mihoyo）"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.mihoyo.bbs')" "0" "同公司的社区类被排除表挡掉"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.mihoyo.community')" "0" "community 也在排除表里"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.tencent.mm')" "0" "微信不会被列成游戏候选"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.netease.cloudmusic')" "0" "音乐类不会被通用公司名顺带拉进来"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.some.gameshop')" "0" \
   "段相等判据：gameshop 不算 game（子串包含就会踩这个）"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.foo.game.center')" "0" "游戏中心/渠道客户端被排除"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.kurogames.newone')" "1" "新装的同厂游戏能扫出来"
ok "$(printf '%s\n' "$SUG" | grep -c '^com.kurogames.exist')" "0" "已存在的同样不重复列"
# 候选共 4 条：pubgmhd / Nap / happyelements / kurogames.newone，
# 每条都必须带第二列（没带的话页面上就只有包名，用户没法判断是不是误收）
ok "$(printf '%s\n' "$SUG" | grep -c '	')" "4" "每行都带命中原因（Tab 分隔的第二列）"
ok "$(printf '%s\n' "$SUG" | grep -c .)" "4" "候选恰好这 4 条，不多不少"
# 原因列必须是"命中的那个词"，不是空也不是整行重复：
# 用户就是靠这一列判断是不是误收（gameshop 被当成 game 就是要一眼看出来的）
ok "$(printf '%s\n' "$SUG" | awk -F'\t' '$1=="com.miHoYo.Nap"{print $2}')" "mihoyo" \
   "命中原因列给的是词典里的词"
ok "$(printf '%s\n' "$SUG" | awk -F'\t' '$1=="com.tencent.tmgp.pubgmhd"{print $2}')" \
   "com.tencent.tmgp" "前缀型词条的原因列是那个前缀"
# 点 ＋ 之后必须真的进了名单，并且下次扫描不再出现
VA games_add com.miHoYo.Nap >/dev/null
ok "$(printf '%s\n' "$(VA games_suggest | tr -d '\r')" | grep -c '^com.miHoYo.Nap')" "0" \
   "加进名单后立刻从候选里消失"
# games_list 只列"装了且在名单里"的：桩里 14 个包，名单里 3 条都算装了，
# 所以是三行。判"加进去了没有"要看这一行在不在，不能整串比对
# （整串比的话，任何一条名单变动都会让它红，而报的却是"没加进去"）。
ok "$(VA games_list | tr -d '\r' | grep -c -x 'com.miHoYo.Nap')" "1" \
   "加进去的就是前台会匹配的那份名单"
# 词典文件不存在 = 没有候选，不是错误（守护那边读不到名单文件也是同一个方向）
rm -f "$VS/vendors.txt"
ok "$(VA games_suggest)" "" "词典缺失时安静返回空"
# 上限：候选太多会把页面撑爆，也和 games_list 的保护一致
printf '+game\n' > "$VS/vendors.txt"
cat > "$VS/bin/pm" <<'EOF'
#!/bin/sh
i=1
while [ $i -le 200 ]; do echo "package:com.t$i.game"; i=$((i+1)); done
EOF
ok "$(VA games_suggest | tr -d '\r' | grep -c .)" "80" "候选有条数上限（80 条，首屏不炸）"


echo "== conf_read 会剥掉成对的首尾引号 =="
# 老版本打包的默认配置里写的是 CPU_GOVERNOR=""。守护认不出引号（等价于空），
# 但 WebUI 的下拉框拿到两个引号就匹配不到任何 option，
# 显示成"谁都没选"，而实际生效的默认值恰恰就是"不干预"。
GQ="$TMP/gq"; mkdir -p "$GQ/webroot"
cp "$MOD/webroot/api.sh" "$GQ/webroot/"
sed 's/^CPU_GOVERNOR=.*/CPU_GOVERNOR=""/' "$MOD/socfreq.conf" > "$GQ/socfreq.conf"
cp "$MOD/games.txt" "$GQ/games.txt" 2>/dev/null
AGQ() { ( cd "$GQ" && PATH="$ORIG_PATH" SOCFREQ_DATA_DIR="$GQ" sh webroot/api.sh "$@" ); }
ok "$(AGQ conf_get | sed -n 's/^CPU_GOVERNOR=//p')" "" "老配置里那对引号不会渗到前端"
sed -i 's/^CPU_GOVERNOR=.*/CPU_GOVERNOR=walt/' "$GQ/socfreq.conf"
ok "$(AGQ conf_get | sed -n 's/^CPU_GOVERNOR=//p')" "walt" "正常值原样出来（剥引号不是剥内容）"
# 只剥**成对的**。单个引号说明写的人本来就写错了，悄悄帮他补一个反而把错藏起来
sed -i 's/^CPU_GOVERNOR=.*/CPU_GOVERNOR="walt/' "$GQ/socfreq.conf"
ok "$(AGQ conf_get | sed -n 's/^CPU_GOVERNOR=//p')" '"walt' "落单的一个引号原样留着"
sed -i 's/^GPU_PCT=.*/GPU_PCT="45"/' "$GQ/socfreq.conf"
ok "$(AGQ conf_get | sed -n 's/^GPU_PCT=//p')" "45" "数字键也一律剥（不只处理调速器）"

echo "== labels（应用名批量 + 缓存）=="
# 名字只能从 APK 里读：守护一次读要解 manifest + arsc，
# 所以这一列的协议是"前端点名 → shell 查缓存 → 没缓存的那批一次交守护"。
# 这里验的是这三段的接线和缓存语义；读 APK 本身在 tools/test_appinfo.js 里对着 aapt2 验。
LB="$TMP/lbl"; mkdir -p "$LB/webroot" "$LB/bin"
cp "$MOD/webroot/api.sh" "$LB/webroot/"
cp "$MOD/socfreq.conf" "$LB/socfreq.conf"
cp "$MOD/games.txt" "$LB/games.txt" 2>/dev/null
# 断言里把 TAB 换成 = 再比：源码里留一个裸制表符，
# 下一个人（或下一个编辑器）把它变成空格，测试就红得莫名其妙。
L() { tr -d '\r' | tr '\t' '='; }
# 假 pm：路径里带 ==（真机就是这样），用来钉"按最后一个 = 切"那条
cat > "$LB/bin/pm" <<'EOF'
#!/bin/sh
case "$*" in
  *-f*) printf 'package:/data/app/~~AA==/com.a-bb==/base.apk=com.a.b\n'
        printf 'package:/data/app/~~CC==/com.c-dd==/base.apk=com.c.d\n' ;;
  *)    printf 'package:com.a.b\npackage:com.c.d\n' ;;
esac
EOF
chmod +x "$LB/bin/pm"
# 假守护：按 pkg + apk 回三列，并把每次调用记一笔，好在外面断言"只叫了一次"
cat > "$LB/socfreqd" <<'EOF'
#!/bin/sh
if [ "$1" = labels ]; then
  echo x >> "$SOCFREQ_DATA_DIR/calls"
  while read -r p a; do
    [ -n "$p" ] || continue
    printf '%s\t%s\t名字-%s\n' "$p" "$a" "$p"
  done
  exit 0
fi
exit 0
EOF
chmod +x "$LB/socfreqd"
ALB() { ( cd "$LB" && PATH="$LB/bin:$ORIG_PATH" SOCFREQ_DATA_DIR="$LB" sh webroot/api.sh "$@" ); }
: > "$LB/calls"
ok "$(ALB labels com.a.b com.c.d | L | tr '\n' '|')" \
   "com.a.b=名字-com.a.b|com.c.d=名字-com.c.d|" "点名的两个包都拿到了名字"
ok "$(grep -c . "$LB/calls" 2>/dev/null)" "1" "这一趟守护只被叫了一次（批量，不是逐包 fork）"
# 第二次必须走缓存：一次请求里 fork 守护 + 解两个 APK，
# 页面每刷一次都重来一遍的话，滚列表会把 WebView 拖住
ok "$(ALB labels com.a.b | L)" "com.a.b=名字-com.a.b" "再问同一个包：命中缓存"
ok "$(grep -c . "$LB/calls")" "1" "命中缓存时一次都没再叫守护"
# 装新版本 = APK 路径变（/data/app 下那个随机后缀每次都不同）→ 必须重取
cat > "$LB/bin/pm" <<'EOF'
#!/bin/sh
printf 'package:/data/app/~~ZZ==/com.a-bb==/base.apk=com.a.b\n'
printf 'package:/data/app/~~CC==/com.c-dd==/base.apk=com.c.d\n'
EOF
chmod +x "$LB/bin/pm"
ok "$(ALB labels com.a.b | L)" "com.a.b=名字-com.a.b" "换了包体之后照样有名字"
ok "$(grep -c . "$LB/calls")" "2" "路径变了就真的重取了一次（缓存不是永久的）"
# 不点名时全量给（选择器那一屏要按名字搜）
ok "$(ALB labels | L | grep -c .)" "2" "不点名时返回全部已安装的第三方应用"
# 一个合法包名都没给：空 + 退出码 0。
# 这条必须是绿的：前端刷新时一个都不传不该弹"执行失败"。
ALB labels 'not a pkg;' >/dev/null 2>&1
ok "$?" "0" "非法包名被挡掉之后返回空、退出码仍是 0"
ok "$(ALB labels 'not a pkg;')" "" "非法包名不会当成包名传下去"
# pm 挂了：同样是空 + 0（读不到列表不是错误状态，页面显示包名就行）
cat > "$LB/bin/pm" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$LB/bin/pm"
ALB labels com.a.b >/dev/null 2>&1
ok "$?" "0" "pm 读不到时 labels 不报错"
ok "$(ALB labels com.a.b)" "" "pm 读不到时输出为空"
# 守护不在（没编译 / 路径不对）：缓存里已有的仍然给出去，缺的留空
printf 'com.a.b\t/data/app/~~ZZ==/com.a-bb==/base.apk\t名字-com.a.b\n' > "$LB/labels.tsv"
rm -f "$LB/socfreqd"
cat > "$LB/bin/pm" <<'EOF'
#!/bin/sh
printf 'package:/data/app/~~ZZ==/com.a-bb==/base.apk=com.a.b\n'
EOF
chmod +x "$LB/bin/pm"
ok "$(ALB labels com.a.b | L)" "com.a.b=名字-com.a.b" "守护不在也能用缓存"
# 守护那一趟整个失败（没编出来 / 崩了）时，_cf 是空的：
# 拿空结果覆盖缓存的话，表现就是"昨天还有名字，今天全没了"，而且查不到是谁干的。
printf %s "com.other	/data/app/~~QQ==/com.o-p==/base.apk	名字-com.other
" > "$LB/labels.tsv"
cat > "$LB/socfreqd" <<EOF
#!/bin/sh
exit 1
EOF
chmod +x "$LB/socfreqd"
ALB labels com.a.b >/dev/null 2>&1
ok "$(grep -c com.other "$LB/labels.tsv" 2>/dev/null)" "1" "守护整批失败时不拿空结果覆盖缓存"

# 并发点名：应用页一次会加载名单 / 候选 / 档位，三批 labels 同时进来。
# 临时文件曾共用固定名（labels.cur / labels.need …），互相覆盖的结果是
# "有时候一部分应用没有名字"。现在每个进程一份（$$），这里用慢守护把
# 两次调用真正重叠起来，直接看两边的输出。
cat > "$LB/socfreqd" <<'EOF'
#!/bin/sh
if [ "$1" = labels ]; then
  sleep 0.3
  while read -r p a; do
    [ -n "$p" ] || continue
    printf '%s\t%s\t名字-%s\n' "$p" "$a" "$p"
  done
  exit 0
fi
exit 0
EOF
chmod +x "$LB/socfreqd"
cat > "$LB/bin/pm" <<'EOF'
#!/bin/sh
case "$*" in
  *-f*) printf 'package:/data/app/~~AA==/com.a-bb==/base.apk=com.a.b\n'
        printf 'package:/data/app/~~CC==/com.c-dd==/base.apk=com.c.d\n' ;;
  *)    printf 'package:com.a.b\npackage:com.c.d\n' ;;
esac
EOF
chmod +x "$LB/bin/pm"
rm -f "$LB/labels.tsv" "$LB/o1" "$LB/o2"
ALB labels com.a.b > "$LB/o1" 2>&1 &
_PA=$!
ALB labels com.c.d > "$LB/o2" 2>&1 &
_PB=$!
wait "$_PA" "$_PB" 2>/dev/null
ok "$(L < "$LB/o1")" "com.a.b=名字-com.a.b" "并发两批 labels：第一批拿到自己的名字"
ok "$(L < "$LB/o2")" "com.c.d=名字-com.c.d" "并发两批 labels：第二批没被覆盖"

echo "== 机型适配包（导出 / 导入 / 覆盖）=="
# 探测不到节点的机器只能人工适配。这一段验的是闭环：
# 导出的包有版本标记 → 导入校验后写出 override.conf → 回执说清每条路径
# 在**这台机**上存不存在（导入成功但路径全是别人机器上的，等于没适配）。
PK="$TMP/pkg"
mkdir -p "$PK/webroot" "$PK/data" "$PK/dl/fake/policy0" "$PK/dl/fake/policy6"
cp "$MOD/webroot/api.sh" "$PK/webroot/"
cp "$MOD/socfreq.conf" "$PK/socfreq.conf"
APKG() { ( cd "$PK" && SOCFREQ_DATA_DIR="$PK/data" SOCFREQ_PKG_DIR="$PK/dl" \
           sh webroot/api.sh "$@" ); }

PKGOUT=$(APKG pkg 2>&1); PKGRC=$?
ok "$PKGRC" "0" "导出适配包返回 0"
ok "$(printf '%s\n' "$PKGOUT" | sed -n 's/^OK //p' | head -n 1)" \
   "$(ls -1 "$PK/dl"/socpkg-*.txt 2>/dev/null | head -n 1)" "回执给的就是落盘路径"
ok "$(head -n 1 "$(ls -1 "$PK/dl"/socpkg-*.txt | head -n 1)")" "#SOCPKG 1" \
   "包首行是版本标记（导入端只按它认格式）"
ok "$(grep -c '^=====' "$(ls -1 "$PK/dl"/socpkg-*.txt | head -n 1)")" "10" \
   "十段人读现场全打出来了（含深睡/待机现场；缺一段就说明哪段写挂了）"

cat > "$PK/dl/socpkg-good.txt" <<EOF
#SOCPKG 1
#soc=TESTCHIP
#model=FAKE1
#ovr cpu-dir $PK/dl/fake/policy0 3532800
#ovr cpu-dir $PK/dl/fake/policy6 4320000
#ovr gpu-max $PK/dl/fake/nope/max_freq
#ovr gpu-freqs 1100000000 1050000000
EOF
IMP=$(APKG pkg_import dl/socpkg-good.txt 2>&1)
ok "$(printf '%s\n' "$IMP" | head -n 1)" "OK import TESTCHIP" "导入回执带芯片名"
ok "$(wc -l < "$PK/data/override.conf")" "4" "override.conf 收了四条覆盖"
ok "$(printf '%s\n' "$IMP" | grep -c '在   ')" "2" "两条目录在这台机上确实存在"
ok "$(printf '%s\n' "$IMP" | grep -c '本机没有')" "1" "不存在的那条被点名，不是默默收下"
ok "$(printf '%s\n' "$IMP" | grep -c '频率表 2 个频点')" "1" "频率表按频点个数回执"
ok "$(APKG ovr_show | head -n 1)" "ovr=1" "ovr_show 报出正在使用覆盖"
# 重复导入不能攒成八条：覆盖是整份替换，攒了就读不清楚哪条生效
APKG pkg_import dl/socpkg-good.txt >/dev/null 2>&1
ok "$(wc -l < "$PK/data/override.conf")" "4" "同一份包导入两次不会把覆盖叠两份"

# 坏包一律拒，而且不许留下半截覆盖
printf '#ovr cpu-dir 相对路径 123\n' > "$PK/dl/bad-nohead.txt"
ok "$(APKG pkg_import dl/bad-nohead.txt 2>&1 | head -n 1)" "E/PKG_FMT" "没有版本标记的文件不当包收"
printf '#SOCPKG 2\n#soc=X\n' > "$PK/dl/bad-ver.txt"
ok "$(APKG pkg_import dl/bad-ver.txt 2>&1 | head -n 1)" "E/PKG_FMT" "不认的版本号直接拒"
printf '#SOCPKG 1\n#soc=X\n' > "$PK/dl/bad-noovr.txt"
ok "$(APKG pkg_import dl/bad-noovr.txt 2>&1 | head -n 1)" "E/PKG_NOOVR" "一条覆盖都没有的包不写文件"
printf '#SOCPKG 1\n#soc=X\n#ovr cpu-dir 相对路径 123\n' > "$PK/dl/bad-rel.txt"
ok "$(APKG pkg_import dl/bad-rel.txt 2>&1 | head -n 1)" "E/PKG_OVR" "路径不以 / 开头判为非法"
printf '#SOCPKG 1\n#soc=X\n#ovr gpu-max /ok/path\n#ovr 乱键 1\n' > "$PK/dl/bad-key.txt"
ok "$(APKG pkg_import dl/bad-key.txt 2>&1 | head -n 1)" "E/PKG_OVR" "未知键整包拒（不静默收下看不懂的行）"
printf '#SOCPKG 1\n#soc=X\n#ovr cpu-dir /tmp/a abc\n' > "$PK/dl/bad-hw.txt"
ok "$(APKG pkg_import dl/bad-hw.txt 2>&1 | head -n 1)" "E/PKG_OVR" "硬件上限不是数字就拒"
ok "$(APKG ovr_show | head -n 1)" "ovr=1" "坏包被拒之后原来那份覆盖还在"

# 无参导入取 Download 里最新的那个包
rm -f "$PK/dl"/socpkg-*.txt
printf '#SOCPKG 1\n#soc=OLD\n#ovr cpu-dir %s 100\n' "$PK/dl/fake/policy0" > "$PK/dl/socpkg-old.txt"
touch -t 202001010000 "$PK/dl/socpkg-old.txt"
printf '#SOCPKG 1\n#soc=NEW\n#ovr cpu-dir %s 200\n' "$PK/dl/fake/policy6" > "$PK/dl/socpkg-new.txt"
touch -t 209901010000 "$PK/dl/socpkg-new.txt"
ok "$(APKG pkg_import 2>&1 | head -n 1)" "OK import NEW" "不点名时取最新的那个包"

ok "$(APKG ovr_clear)" "OK ovr_clear" "清除覆盖给回执"
ok "$([ -f "$PK/data/override.conf" ] && echo yes || echo no)" "no" "覆盖文件真的删掉了"
ok "$(APKG ovr_show | head -n 1)" "ovr=0" "清除后 ovr_show 报没在用"

echo "== 全部恢复默认 =="
mkdir -p "$PK/scripts"
cp "$MOD/scripts/deep_sleep.sh" "$PK/scripts/deep_sleep.sh"
cp "$MOD/scripts/builtin_white.txt" "$PK/scripts/builtin_white.txt"
cp "$MOD/app_modes.conf" "$PK/app_modes.conf"
printf '# 用户改过的配置\nCPU_PCT_BIG=99\n' > "$PK/data/socfreq.conf"
printf 'com.foo=full\n' > "$PK/data/app_modes.conf"
printf 'enable=1\nwhite=com.foo\n' > "$PK/data/dsleep.conf"
printf '# 用户改过的适配\n' > "$PK/data/override.conf"
printf 'my own game\n' > "$PK/data/games.txt"
printf 'my vendor\n' > "$PK/data/vendors.txt"
RST=$(APKG reset_all 2>&1)
ok "$RST" "OK reset_all" "恢复默认返回成功"
ok "$(cmp -s "$PK/data/socfreq.conf" "$PK/data/defaults/socfreq.conf" && echo same)" "same" \
   "性能配置回到出厂默认（data/defaults 留的原件）"
ok "$(cmp -s "$PK/data/app_modes.conf" "$PK/app_modes.conf" && echo same)" "same" \
   "分应用档位回到模块自带默认"
ok "$(grep -c '^enable=0$' "$PK/data/dsleep.conf")" "1" "深睡开关回到默认关"
ok "$([ -f "$PK/data/override.conf" ] && echo yes || echo no)" "no" "适配覆盖被撤掉"
ok "$(cat "$PK/data/games.txt" 2>/dev/null)" "my own game" "游戏名单保留（不是配置）"
ok "$(cat "$PK/data/vendors.txt" 2>/dev/null)" "my vendor" "发行商词典保留"
ok "$(grep -c '^white=com.foo$' "$PK/data/dsleep.conf" 2>/dev/null)" "0" \
   "深睡豁免名单按内置默认重建，不保留用户自加的 com.foo"

echo "== 深度睡眠 =="
# api.sh 按 $ROOT/scripts/deep_sleep.sh 找到守护；临时模块目录里也要有一份。
mkdir -p "$TMP/scripts"
cp "$MOD/scripts/deep_sleep.sh" "$TMP/scripts/deep_sleep.sh"
# setsid 换成桩：只记一笔，不真拉起守护。否则这份测试会在台架上留一个
# 每 2 秒跑一圈的 shell 进程，而且它会去调只在手机上有的 dumpsys。
DSSTUB="$TMP/dsstub"; mkdir -p "$DSSTUB"
cat > "$DSSTUB/setsid" <<'EOS'
#!/bin/sh
echo "$@" >> "$DS_STUB_LOG"
exit 0
EOS
chmod +x "$DSSTUB/setsid"
DSA() { PATH="$DSSTUB:$ORIG_PATH" DS_STUB_LOG="$TMP/ds_start.log" \
        SOCFREQ_DATA_DIR="$TMP" sh webroot/api.sh "$@"; }

ok "$(DSA ds_get | sed -n 's/^enable=//p')" "0" "默认是关的"
ok "$(DSA ds_get | sed -n 's/^alive=//p')" "0" "没守护时报未运行"
ok "$(DSA ds_get | sed -n 's/^paused=//p')" "0" "没暂停文件时报未暂停"
ok "$(DSA ds_get | sed -n 's/^dsactive=//p')" "0" "没状态文件时报未在深睡"
ok "$(DSA ds_get | grep -c '^white=')" "0" "默认没有豁免应用"

ok "$(DSA ds_set enable 1 | head -n 1)" "OK enable=1" "打开开关给回执"
ok "$(grep -c '^enable=1' dsleep.conf)" "1" "打开后落盘"
sleep 1
ok "$([ -s "$TMP/ds_start.log" ] && echo yes || echo no)" "yes" \
   "打开开关会拉起守护（有关闭时不会）"
DSA ds_set enable 0 >/dev/null
ok "$(grep -c '^enable=0' dsleep.conf)" "1" "关掉后落盘"

BADENV=$(DSA ds_set enable 2 2>&1); BADRC=$?
ok "$BADRC" "1" "开关只收 0/1"

echo "-- 墓碑冻结开关与后端 --"
ok "$(DSA ds_get | sed -n 's/^freeze=//p')" "1" "冻结默认开"
ok "$(DSA ds_get | sed -n 's/^appops=//p')" "1" "唤醒锁默认开"
ok "$(DSA ds_get | sed -n 's/^fzb=//p')" "off" "没开深睡时后端报 off"
ok "$(DSA ds_set freeze 0 | head -n 1)" "OK freeze=0" "冻结可以单独关"
ok "$(grep -c '^freeze=0' dsleep.conf)" "1" "关冻结落到配置里"
ok "$(DSA ds_get | sed -n 's/^freeze=//p')" "0" "读回来是 0"
DSA ds_set freeze 1 >/dev/null
ok "$(DSA ds_get | sed -n 's/^freeze=//p')" "1" "再打开读回来是 1"
# 开了深睡 + 开了冻结，就该报出实际会用的后端。具体是 v2/v1/sigstop
# 取决于跑台架的机器（Windows 上没有 cgroup，只会到 sigstop），
# 所以只断言"报了一个合法值"，不写死。
DSA ds_set enable 1 >/dev/null
sleep 1
FZB=$(DSA ds_get | sed -n 's/^fzb=//p')
case "$FZB" in
  v2|v1|sigstop) ok y y "开了冻结就报出后端（$FZB）" ;;
  *) ok "$FZB" "v2/v1/sigstop" "开了冻结应报出后端" ;;
esac
DSA ds_set enable 0 >/dev/null

BADFZ=$(DSA ds_set freeze 2 2>&1); BADFZRC=$?
ok "$BADFZRC" "1" "冻结开关也只收 0/1"

BADKEY=$(DSA ds_set foo 1 2>&1)
ok "$(printf '%s\n' "$BADKEY" | head -n 1)" "E/BAD_KEY" "只认 enable / freeze / appops"
reject_rc() { local o; o=$(DSA "$@" 2>&1); echo $?; }
ok "$(reject_rc ds_white_add 'bad pkg')" "1" "豁免名单拒非法包名"
ok "$(reject_rc ds_white_add 'nodots')" "1" "没有点的包名拒收"

ok "$(DSA ds_white_add com.example.sleep | head -n 1)" "OK com.example.sleep" "合法包名可加进豁免"
ok "$(DSA ds_white_add com.example.sleep | head -n 1)" "DUP com.example.sleep" "重复添加报 DUP"
ok "$(DSA ds_get | grep -c '^white=com.example.sleep$')" "1" "ds_get 带出豁免名单"

printf 'enable=1\ndsactive=1\nwcsince=1700000000\ndsw=com.a:3:0,com.b:1:1\ndswc=com.a:9:0\n' > ds_state
ok "$(DSA ds_state | sed -n 's/^dsactive=//p')" "1" "ds_state 读出深睡中"
ok "$(DSA ds_state | grep -c '^dsw=com.a:3:0,com.b:1:1$')" "1" "当次榜原样带出"
ok "$(DSA ds_state | grep -c '^dswc=com.a:9:0$')" "1" "累计榜原样带出"
ok "$(DSA ds_state | grep -c '^white=')" "0" "轮询用的 ds_state 不带白名单（省一次遍历）"
rm -f ds_state

# 被 kill -9 的守护不会再写 state；api 停完要自己把 dsactive 折回 0，
# 否则页面停止后还显示「累计中」。
printf 'enable=1\ndsactive=1\ndsw=com.a:1:0\n' > ds_state
DSA ds_set enable 0 >/dev/null
ok "$(grep '^dsactive=' ds_state)" "dsactive=0" "停止后把 state 的 dsactive 归一成 0"
ok "$(grep '^dsw=' ds_state)" "dsw=com.a:1:0" "归一化只动 dsactive，榜单保留"
rm -f ds_state

ok "$(DSA ds_white_del com.example.sleep | head -n 1)" "OK com.example.sleep" "移除豁免"
ok "$(DSA ds_white_del com.example.sleep | head -n 1)" "MISS com.example.sleep" "重复移除报 MISS"
ok "$(DSA ds_get | grep -c '^white=')" "0" "移除后名单为空"

echo
if [ "$fail" -eq 0 ]; then echo "全部通过 ($pass)"; else echo "$fail 项失败 / $pass 项通过"; fi
exit "$fail"