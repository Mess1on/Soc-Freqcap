#!/system/bin/sh
# Soc智能限频 —— WebUI 后端
#
# 调用方式：sh api.sh <命令> [参数…]（由管理器以 root 身份执行）
# 输出约定：所有内容写到 stdout；出错时 stderr 打一行 "E/<原因码>"，退出码非零。

set -u

# ── 定位模块目录 ────────────────────────────────────────────────
# api.sh 位于 <模块>/webroot/ 下。无路径调用时 ${0%/*} 剥掉的是文件名本身，
# 因此先按最后一个斜杠切出目录，再按 webroot 后缀裁一次。
_self=$0
case "$_self" in
  */*) _d=${_self%/*} ;;
  *)   _d=. ;;
esac
case "$_d" in
  */webroot) _d=${_d%/webroot} ;;
  webroot)   _d=. ;;
  .)         _d=$(pwd) ;;
esac
ROOT=$_d
[ -d "$ROOT" ] || { echo "E/MODDIR" >&2; exit 2; }

# ── 可变状态的家：不在模块目录里 ─────────────────────────────────
# 可变状态不放模块目录：升级时管理器会先删 /data/adb/modules/<id> 再解包，
# 配置和名单放里面会被新包的默认值覆盖；也不放 /sdcard（FUSE/media_rw 权限复杂）。
# 守护侧的同一路径见 src/socfreqd.c 的 SF_DATADIR，两边必须一致。
DATA="${SOCFREQ_DATA_DIR:-/data/adb/soc_freqcap}"
CONF="$DATA/socfreq.conf"
LIST="$DATA/games.txt"
MODES="$DATA/app_modes.conf"
# 发行商词典。只在 games_suggest 里用；守护完全不读它。
VENDORS="$DATA/vendors.txt"
LOG="$DATA/socfreqd.log"
RUN="$DATA/run"
PAUSE="$DATA/pause"
STATUS="$RUN/status"
BIN="$ROOT/socfreqd"          # 二进制仍然在模块目录里（那是解包出来的，不是用户数据）

# ── 深度睡眠 ────────────────────────────────────────────────────
# 实现在 scripts/deep_sleep.sh（独立 shell 守护，service.sh 拉起）。
# 配置和运行状态仍在数据目录；state 单独一份，不跟 socfreqd 的 run/status 混写。
DS_CONF="$DATA/dsleep.conf"
DS_STATE="$DATA/ds_state"
DS_PID="$DATA/run/dsleep.pid"
DS_SH="$ROOT/scripts/deep_sleep.sh"

# 首次（或迁移漏了）时把模块目录里的那份搬进数据目录。
# 搬而不是复制：模块目录里不留副本，升级时才不会拿旧默认值盖掉新数据。
# 正常情况下由 customize.sh 迁移；这里兜底手动解包、其他管理器装机等漏迁移的路径。
if [ ! -d "$DATA" ] || [ ! -f "$CONF" ]; then
  mkdir -p "$DATA" 2>/dev/null
  # 恢复默认要用出厂那份；模块目录里的 socfreq.conf 随后会被搬走/删掉，
  # 先留一份到数据目录（装机脚本也会做，这里兜底手动解包）。
  mkdir -p "$DATA/defaults" 2>/dev/null
  for _f in socfreq.conf app_modes.conf; do
    [ -f "$ROOT/$_f" ] && [ ! -f "$DATA/defaults/$_f" ] \
      && cp -f "$ROOT/$_f" "$DATA/defaults/$_f" 2>/dev/null
  done
  for _f in socfreq.conf games.txt app_modes.conf vendors.txt; do
    [ -f "$ROOT/$_f" ] && [ ! -f "$DATA/$_f" ] \
      && mv -f "$ROOT/$_f" "$DATA/$_f" 2>/dev/null
  done
  [ -f "$ROOT/socfreqd.log" ] && [ ! -f "$LOG" ] \
    && mv -f "$ROOT/socfreqd.log" "$LOG" 2>/dev/null
  chmod 0700 "$DATA" 2>/dev/null
fi
# run/ 由守护写（journal、status），list_installed 也要在此放临时文件，
# 而首次进 WebUI 可能早于守护第一次启动，这里补一次 mkdir；
# 守护侧同样有 mkdir_p(RUN_DIR)，两边重复创建不冲突。
mkdir -p "$RUN" 2>/dev/null
chmod 0700 "$RUN" 2>/dev/null

ICON_DIR="${SOCFREQ_ICON_DIR:-$DATA/icon}"
LABEL_CACHE="$DATA/labels.tsv"
ICON_MAX=200
# 取图算法的版本，进缓存文件名。改了选图规则就 +1，
# 让上一版选错的那些缓存自然失效（而不是等用户手动清）。
ICON_ALGO=2

# ── 输出工具 ────────────────────────────────────────────────────
# 错误只报机器可读的码，不掺进 stdout。
# WebUI 拿 stdout 当数据解析，错误信息混进去会导致整段数据报废。
die() { echo "E/$1" >&2; exit "${2:-1}"; }

# 从 conf 里取一个键的值。只认整行 "KEY=" 开头的，
# 值里的 # 会被当行尾注释切掉。
# 最后剥掉**成对的**首尾引号：默认配置里的 "" 是给人看的"空"，守护视其为空，
# 但 WebUI 下拉框拿到两个引号会匹配不到任何 option，页面显示成"谁都没选"。
# 只剥成对的，单独的 " 或 ' 原样保留——那是写错的输入，静默补全会掩盖错误。
conf_read() {
  [ -f "$CONF" ] || return 1
  sed -n "s/^$1=//p" "$CONF" 2>/dev/null | head -n 1 \
    | sed 's/[[:space:]]*#.*$//; s/^[[:space:]]*//; s/[[:space:]]*$//' \
    | sed "s/^\"\([^\"]*\)\"$/\1/; s/^'\([^']*\)'$/\1/"
}

# 可写的键。区间在 cfg_set 里按键名分支，键增删时这里必须同步。
KEYS='STRATEGY INTERVAL GAME_GRACE_SEC CPU_PCT_BIG CPU_PCT_MID2 CPU_PCT_MID CPU_PCT_LITTLE GPU_PCT GPU_MIN_LOCK GPU_BOOST_OFF BIG_MIN SCREEN_UNCAP SCREEN_POLICY SOFF_INTERVAL SCREEN_STATE_PATH IDLE_CURRENT_MA IDLE_HOLD_CYCLE IDLE_INTERVAL WAKE_DELTA_MA BATT_CURRENT_PATH BATT_CURRENT_UNIT BOOT_FULL_S PSI_ADAPT PSI_GAIN PSI_STEP PSI_HOLD PSI_BACK TH_CAP TH_HIGH TH_LOW TH_STEP TH_MAX TH_BACK CHG_UNCAP CHG_PCT ADAPT_UP_MAX ADAPT_DOWN_MAX BATT_STEP BATT_LOW BATT_LOW_CUT BATT_CRIT BATT_CRIT_CUT'

is_key() {
  for k in $KEYS; do [ "$1" = "$k" ] && return 0; done
  return 1
}

# 只接受十进制整数，位数封顶 5（100000 以上没有意义，顺带挡畸形输入）
is_uint() {
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  [ "${#1}" -gt 5 ] && return 1
  return 0
}

is_switch() { [ "$1" = "0" ] || [ "$1" = "1" ]; }

# 调速器：空 = 不干预。用词表挡，不是词表里的一律拒绝。
# 特意不含 hmbird/scx——它们会把写权限从调度器手里夺走，
# 和本模块"游戏态把写权限交回系统"的设计直接冲突。
is_governor() {
  [ -z "$1" ] && return 0
  case "$1" in
    walt|schedutil|powersave|conservative|performance|ondemand|userspace) return 0 ;;
  esac
  return 1
}

# 包名：Android 包名的实际形态是 段(.段)+，段内允许字母数字下划线。
# 这里比"没有非法字符"更严：必须含点，且不允许首尾点和连续点
# （"a..b" 不是任何真实包名，放进来只会在匹配时永远命中不了）。
is_package() {
  case "$1" in
    ''|*[!A-Za-z0-9._]*) return 1 ;;
    *..*|.*|*.) return 1 ;;
  esac
  case "$1" in *.*) return 0 ;; *) return 1 ;; esac
}

# ── 原子写入 ────────────────────────────────────────────────────
# 参数：目标文件；新内容从 stdin 读。
# 配置读写不走 sed -i：它在 /data 下未必有可用的临时目录，失败时还会
# 把原文件截断。统一走"读全量→内存改→写临时文件→rename"，
# 任何一步失败原文件都不动。
# 临时文件必须和目标同一个目录：rename 只在同一文件系统内是原子的，
# 跨设备会退化成 copy，copy 中断就是一个半截文件。
write_atomic() {
  _t=$1.tmp.$$
  if cat > "$_t"; then
    if mv -f "$_t" "$1" 2>/dev/null; then
      chmod 0644 "$1" 2>/dev/null
      return 0
    fi
  fi
  rm -f "$_t" 2>/dev/null
  return 1
}

# 追加之前补一个换行。
# 末行缺换行时 `cat 文件; echo 新行` 会把新键和末行粘成一行（如 CPU_GOVERNOR=""SCREEN_UNCAP=0）：
# 两个键同时失效，且 grep 每个键都"在"，页面看不出异常，所以必须补。
ensure_final_newline() {
  [ -s "$1" ] || return 0
  # tail -c 1 拿到末字节，命令替换会吃掉结尾的换行，所以还要 tr 一次才判得准：
  # 末字节本来是 \n → 去掉之后是空串 → 不补；末字节是别的 → 补一个。
  if [ -n "$(tail -c 1 "$1" 2>/dev/null | tr -d '\n')" ]; then
    printf '\n' >> "$1" 2>/dev/null
  fi
}

# 按 KEY=VALUE 更新 conf：命中就整行替换，没命中就追加。
# sed 只用来"读"，写一律走 write_atomic——/data 下的文件系统未必
# 支持 sed -i 要的临时目录，而它失败时会把原文件截断成半截。
conf_put() {
  [ -f "$CONF" ] || : > "$CONF" || return 1
  if grep -q -F "$1=" "$CONF" 2>/dev/null; then
    # 用 index() 判断行首，保证只在真正的赋值行上替换，
    # 不会动到注释里提到同名键的那一行。
    awk -v k="$1" -v v="$2" \
      'index($0, k "=") == 1 { print k "=" v; next } { print }' "$CONF" \
      | write_atomic "$CONF"
  else
    ensure_final_newline "$CONF"
    { cat "$CONF"; printf '%s=%s\n' "$1" "$2"; } | write_atomic "$CONF"
  fi
}

list_count() {
  [ -f "$LIST" ] || { echo 0; return; }
  # 用 awk 数，不用 `grep -vc ... || echo 0`：后者在无匹配时退出码为 1，
  # || 分支会再吐一个 0，命令替换收到 "0\n0"，任何对该值的比较都会出错。
  # awk 的 END 必然执行、输出单个整数、退出码恒 0，结果永远是一行一个整数。
  awk '!/^[[:space:]]*(#.*)?$/ { n++ } END { print n + 0 }' "$LIST" 2>/dev/null
}

# ── 状态 ────────────────────────────────────────────────────────

# 两段拼起来：
#   守护的 export  给硬件侧（各簇频率、SoC 型号、封顶值、名单数）
#   run/status     给运行侧（游戏态还是常态、前台包名、pid）
# 两段都是 key=value，直接续写，前端一次 parse 就拿到全部字段。
# 不在这里重新探测 sysfs：两边各探测一次，结论迟早会不一致。
st_status() {
  [ -f "$BIN" ] || die "NO_BIN" 3
  # 用 -f 不用 -x：ELF 丢执行位时 sh 照样 Permission denied，
  # 而 -x 会把"权限不对"报成"程序不存在"，掩盖真正原因。
  _hw=$("$BIN" export 2>/dev/null) || die "DUMP" 3
  [ -n "$_hw" ] || die "DUMP" 3
  printf '%s\n' "$_hw"
  # 状态文件在数据目录下（守护的 RUN_DIR/status）。
  # 兼容读一份 $ROOT/run/status：老装机残留仍在模块目录时，
  # 只认新路径会把正常运行的守护报成"未运行"。
  _st=""
  for _f in "$STATUS" "$ROOT/run/status"; do
    [ -f "$_f" ] || continue
    _st=$(grep -E '^(state|screen|soff|soff_keep|game_pkg|reason|fg|app_mode|app_big|app_little|app_gpu|pid|dlt|batt|bcut|stall|psib|avg10|temp|chg|psi|th|psifrz|strat|smart|tick|fgprobe|gminw)=' "$_f" 2>/dev/null)
    [ -n "$_st" ] && break
  done
  if [ -n "$_st" ]; then
    printf '%s\n' "$_st"
  else
    # 状态文件还没生成时也要给出可解析的字段。
    # 前端拿到缺字段的对象能显示"未运行"，拿到空串只会白屏。
    # 实时偏移那几项给 -1：读不到不等于 0，页面据此不显示"温控降了 0 档"这种假话。
    printf 'state=unknown\nscreen=on\nsoff=15\nsoff_keep=0\ngame_pkg=-\nreason=-\nfg=-\napp_mode=\napp_big=0\napp_little=0\napp_gpu=\npid=\ndlt=-1\nbatt=-1\nbcut=0\nstall=-1\npsib=-1\navg10=-1\ntemp=-1\nchg=-1\npsi=-1\nth=-1\npsifrz=0\n'
  fi
}

st_log() {
  _n=${1:-80}
  is_uint "$_n" || _n=80
  [ "$_n" -gt 400 ] && _n=400
  [ -f "$LOG" ] || return 0
  tail -n "$_n" "$LOG" 2>/dev/null
  return 0
}

# 完整日志页：按来源读守护 / 深睡 / 启动看门狗 / 安装日志。
# 以前的 UI 调的是 logs，但后端只有 log（单份守护日志），所以页面一直
# 显示“读取失败”。这里补齐命令；缺失的文件直接跳过，不报错。
cmd_logs() {
  _src=${1:-all}
  _n=${2:-200}
  _full=0
  [ "$_src" = "export" ] && { _src=all; _full=1; }
  case "$_src" in
    all|daemon|sleep|service|install) ;;
    *) _src=all ;;
  esac
  is_uint "$_n" || _n=200
  [ "$_n" -gt 4000 ] && _n=4000
  # “全部”同时拼三份，单份上限收一收，避免一次吐出上万行。
  [ "$_src" = "all" ] && [ "$_n" -gt 1000 ] && [ "$_full" = "0" ] && _n=1000

  _emit() { # label file
    [ -f "$2" ] || return 0
    printf '===== %s =====\n' "$1"
    if [ "$_full" = "1" ]; then
      cat "$2" 2>/dev/null
    else
      tail -n "$_n" "$2" 2>/dev/null
    fi
    printf '\n'
  }

  case "$_src" in
    daemon)  _emit 守护 "$LOG" ;;
    sleep)   _emit 深睡 "$DATA/dsleep.log" ;;
    service) _emit 启动看门狗 "$DATA/service.log" ;;
    install) _emit 安装 "$DATA/install.log" ;;
    all)
      _emit 守护 "$LOG"
      _emit 深睡 "$DATA/dsleep.log"
      _emit 启动看门狗 "$DATA/service.log"
      _emit 安装 "$DATA/install.log"
      ;;
  esac
  return 0
}

# 日志导出：由后端落到 Download，回执给真实路径。
# 页面里的 <a download> 在多数管理器的 WebView 里没有下载监听，点了没反应，
# 却照样弹"已导出"——导出必须真的落盘，前端只负责显示路径。
cmd_log_export() {
  mkdir -p "$PKG_DIR" 2>/dev/null
  [ -d "$PKG_DIR" ] || die "PKG_DIR" 7
  _b=$(getprop ro.product.brand 2>/dev/null)
  _m=$(getprop ro.product.model 2>/dev/null)
  _fn=$(printf '%s-%s' "$_b" "$_m" | tr -c 'A-Za-z0-9._-' '_')
  _t=$(date +%Y%m%d-%H%M%S 2>/dev/null || echo unknown)
  _f="$PKG_DIR/soclog-$_fn-$_t.txt"
  _tmp="$_f.$$"
  {
    printf '# Soc智能限频 日志导出 %s\n' "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
    cmd_logs export 0
  } > "$_tmp" 2>/dev/null || { rm -f "$_tmp" 2>/dev/null; die "WRITE" 5; }
  mv -f "$_tmp" "$_f" 2>/dev/null || { rm -f "$_tmp" 2>/dev/null; die "WRITE" 5; }
  chmod 0644 "$_f" 2>/dev/null
  printf 'OK %s\n' "$_f"
}

st_about() {
  printf 'MOD=%s\n' "$ROOT"
  printf 'BIN=%s\n' "$([ -f "$BIN" ] && echo 1 || echo 0)"
  printf 'CONF=%s\n' "$([ -f "$CONF" ] && echo 1 || echo 0)"
  printf 'LIST=%s\n' "$([ -f "$LIST" ] && echo 1 || echo 0)"
  printf 'COUNT=%s\n' "$(list_count)"
  # 分应用档位：关于页要能看出"文件在不在、里面几条"。
  # 条目数为 0 和文件不存在要分得开——前者是用户还没设过，
  # 后者是数据目录被清过，两个的处理方式不一样。
  printf 'MODES=%s\n' "$([ -f "$MODES" ] && echo 1 || echo 0)"
  # 计数用 awk，不用 `grep -cv ... || echo 0`：后者在计数为 0 时退出码是 1，
  # || 分支又吐一个 0，命令替换拿到两行（"0\n0"），谁去比都错。
  printf 'NMODE=%s\n' "$([ -f "$MODES" ] && tr -d '\r' < "$MODES" \
      | awk '!/^[[:space:]]*$/ && !/^[[:space:]]*#/{n++} END{print n+0}' || echo 0)"
  printf 'ICON=%s\n' "$ICON_DIR"
  printf 'ICONLAST=%s\n' "$(icon_last)"
  # 分隔标记：前端据此切出后半段 prop；缺失时 indexOf 返回 -1，
  # 切出的对象字段全为 undefined，关于页静默空白。
  # 分隔标记：不要用 printf --，有的 ROM 的 sh 会把 -- 当格式串，
  # 结果只吐两个减号，关于页就永远读不到 module.prop。用 %s 最稳。
  printf '%s\n' '---PROP---'

  cat "$ROOT/module.prop" 2>/dev/null
  return 0
}

# ── 配置 ────────────────────────────────────────────────────────

cfg_get() {
  [ -f "$CONF" ] || die "NO_CONF" 4
  # 一次 awk 读完整份配置。之前每个键起 4 个 sed（四十多个键 = 两百多个
  # 进程），单这条命令就要 0.7 秒，配置页点开明显卡一下。
  # 规则与 conf_read 逐条对齐：整行 KEY=、值里 # 后面当注释、去首尾空白、
  # 只剥**成对**的首尾引号、同名键取第一行。
  CONF="$CONF" KEYS="$KEYS CPU_GOVERNOR" awk '
    BEGIN {
      n = split(ENVIRON["KEYS"], ks, " ")
      for (i = 1; i <= n; i++) want[ks[i]] = 1
    }
    {
      i = index($0, "=")
      if (i <= 1) next
      k = substr($0, 1, i - 1)
      if (!(k in want) || (k in val)) next
      v = substr($0, i + 1)
      sub(/[[:space:]]*#.*$/, "", v)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
      if (v ~ /^"[^"]*"$/ || v ~ /^\047[^\047]*\047$/) v = substr(v, 2, length(v) - 2)
      val[k] = v
    }
    END {
      for (i = 1; i <= n; i++) print ks[i] "=" ((ks[i] in val) ? val[ks[i]] : "")
    }
  ' "$CONF"
  # 荣耀 / 华为内核不切换调速器（守护侧同一判断，见 socfreqd.c 的
  # governor_locked）。页面据此把下拉固定为「不干预」，别让用户以为
  # 选了一个实际不会生效的值。
  case "$(getprop ro.product.brand 2>/dev/null | tr 'a-z' 'A-Z') $(getprop ro.product.manufacturer 2>/dev/null | tr 'a-z' 'A-Z')" in
    *HONOR*|*HUAWEI*|*SAMSUNG*) printf 'gov_locked=1\n' ;;
    *)                          printf 'gov_locked=0\n' ;;
  esac
}

cfg_set() {
  _k=${1:-}; _v=${2:-}
  [ -f "$CONF" ] || die "NO_CONF" 4

  if [ "$_k" = CPU_GOVERNOR ]; then
    is_governor "$_v" || die "BAD_GOVERNOR"
  elif [ "$_k" = STRATEGY ]; then
    case "$_v" in cap|smart) ;; *) die "BAD_STRATEGY" ;; esac
  elif [ "$_k" = SCREEN_POLICY ]; then
    case "$_v" in uncap|keep|cap|cap_off) ;; *) die "BAD_POLICY" ;; esac
  elif [ "$_k" = SCREEN_STATE_PATH ] || [ "$_k" = BATT_CURRENT_PATH ]; then
    case "$_v" in ''|/sys/*) ;; *) die "BAD_PATH" ;; esac
  elif [ "$_k" = BATT_CURRENT_UNIT ]; then
    case "$_v" in ua|ma) ;; *) die "BAD_UNIT" ;; esac
  else
    is_key "$_k" || die "BAD_KEY"
    is_uint "$_v" || die "NOT_NUM"
    # 越界直接报错，不静默夹到边界：
    # 静默改值会让用户以为改了，其实没改。
    case "$_k" in
      INTERVAL)       [ "$_v" -ge 2 ] && [ "$_v" -le 60 ] || die "RANGE" ;;
      GAME_GRACE_SEC) [ "$_v" -ge 2 ] && [ "$_v" -le 300 ] || die "RANGE" ;;
      GPU_MIN_LOCK)   is_switch "$_v" || die "RANGE" ;;
      GPU_BOOST_OFF)  is_switch "$_v" || die "RANGE" ;;
      BIG_MIN)        is_switch "$_v" || die "RANGE" ;;
      SCREEN_UNCAP)   is_switch "$_v" || die "RANGE" ;;
      SOFF_INTERVAL)  [ "$_v" -ge 5 ] && [ "$_v" -le 300 ] || die "RANGE" ;;
      IDLE_CURRENT_MA) [ "$_v" -ge 20 ] && [ "$_v" -le 2000 ] || die "RANGE" ;;
      IDLE_HOLD_CYCLE) [ "$_v" -ge 1 ] && [ "$_v" -le 10 ] || die "RANGE" ;;
      IDLE_INTERVAL)  [ "$_v" -ge 10 ] && [ "$_v" -le 300 ] || die "RANGE" ;;
      WAKE_DELTA_MA)  [ "$_v" -ge 20 ] && [ "$_v" -le 2000 ] || die "RANGE" ;;
      # 开机满血窗口：0=关，上限 600s。再长就失去"开机提速"的意义，
      # 且会盖过厂商开机阶段的热管理。区间和守护的 clampi(0,600) 对齐。
      BOOT_FULL_S)    [ "$_v" -ge 0 ] && [ "$_v" -le 600 ] || die "RANGE" ;;
      # 实时偏移。区间一律和守护 load_cfg 里的 clampi 对齐 ——
      # 两边不一致时，页面能填进一个守护会静默改掉的数。
      PSI_ADAPT|TH_CAP|CHG_UNCAP)  is_switch "$_v" || die "RANGE" ;;
      PSI_GAIN)       [ "$_v" -ge 0 ] && [ "$_v" -le 30 ] || die "RANGE" ;;
      PSI_STEP)       [ "$_v" -ge 1 ] && [ "$_v" -le 10 ] || die "RANGE" ;;
      PSI_HOLD)       [ "$_v" -ge 5 ] && [ "$_v" -le 600 ] || die "RANGE" ;;
      PSI_BACK)       [ "$_v" -ge 5 ] && [ "$_v" -le 600 ] || die "RANGE" ;;
      TH_HIGH)        [ "$_v" -ge 35 ] && [ "$_v" -le 110 ] || die "RANGE" ;;
      TH_LOW)         [ "$_v" -ge 30 ] && [ "$_v" -le 105 ] || die "RANGE" ;;
      TH_STEP)        [ "$_v" -ge 1 ] && [ "$_v" -le 10 ] || die "RANGE" ;;
      TH_MAX)         [ "$_v" -ge 0 ] && [ "$_v" -le 60 ] || die "RANGE" ;;
      TH_BACK)        [ "$_v" -ge 5 ] && [ "$_v" -le 600 ] || die "RANGE" ;;
      CHG_PCT)        [ "$_v" -ge 20 ] && [ "$_v" -le 100 ] || die "RANGE" ;;
      # 合计偏移的总闸。单个来源各自有上限，叠起来仍可能过狠；
      # 0 = 那一侧完全封死（仍保留另一侧）。
      ADAPT_UP_MAX|ADAPT_DOWN_MAX)
                      [ "$_v" -ge 0 ] && [ "$_v" -le 100 ] || die "RANGE" ;;
      BATT_STEP)      is_switch "$_v" || die "RANGE" ;;
      BATT_LOW)       [ "$_v" -ge 5 ] && [ "$_v" -le 95 ] || die "RANGE" ;;
      BATT_LOW_CUT)   [ "$_v" -ge 0 ] && [ "$_v" -le 30 ] || die "RANGE" ;;
      BATT_CRIT)      [ "$_v" -ge 1 ] && [ "$_v" -le 95 ] || die "RANGE" ;;
      BATT_CRIT_CUT)  [ "$_v" -ge 0 ] && [ "$_v" -le 30 ] || die "RANGE" ;;
      *)              [ "$_v" -ge 1 ] && [ "$_v" -le 100 ] || die "RANGE" ;;
    esac
    # 温控的迟滞带：恢复线必须低于触发线，否则温度在阈值附近会逐档来回抖。
    # 守护那边也有同一条兜底，这里先拦住是为了给用户一句人话，
    # 而不是"写成功了但生效值和填的不一样"。
    if [ "$_k" = TH_LOW ]; then
      _hi=$(conf_read TH_HIGH)
      case "$_hi" in ''|*[!0-9]*) : ;; *) [ "$_v" -lt "$_hi" ] || die "TH_ORDER" ;; esac
    fi
    if [ "$_k" = TH_HIGH ]; then
      _lo=$(conf_read TH_LOW)
      case "$_lo" in ''|*[!0-9]*) : ;; *) [ "$_v" -gt "$_lo" ] || die "TH_ORDER" ;; esac
    fi
    # 电量两档必须错开：CRIT 必须低于 LOW，否则两刀会在同一区间叠加。
    if [ "$_k" = BATT_LOW ]; then
      _cr=$(conf_read BATT_CRIT)
      case "$_cr" in ''|*[!0-9]*) : ;; *) [ "$_v" -gt "$_cr" ] || die "BATT_ORDER" ;; esac
    fi
    # 0 = 关；非 0 时必须至少能凑出一整档，否则界面上填 5、实际一档 10。
    if [ "$_k" = PSI_GAIN ] && [ "$_v" -gt 0 ]; then
      _st=$(conf_read PSI_STEP)
      case "$_st" in ''|*[!0-9]*) _st=5 ;; esac
      [ "$_v" -ge "$_st" ] || die "PSI_ORDER"
    fi
    if [ "$_k" = PSI_STEP ]; then
      _g=$(conf_read PSI_GAIN)
      case "$_g" in ''|*[!0-9]*) _g=10 ;; esac
      { [ "$_g" -eq 0 ] || [ "$_v" -le "$_g" ]; } || die "PSI_ORDER"
    fi
    if [ "$_k" = TH_MAX ] && [ "$_v" -gt 0 ]; then
      _ts=$(conf_read TH_STEP)
      case "$_ts" in ''|*[!0-9]*) _ts=5 ;; esac
      [ "$_v" -ge "$_ts" ] || die "TH_ORDER"
    fi
    if [ "$_k" = TH_STEP ]; then
      _tm=$(conf_read TH_MAX)
      case "$_tm" in ''|*[!0-9]*) _tm=25 ;; esac
      { [ "$_tm" -eq 0 ] || [ "$_v" -le "$_tm" ]; } || die "TH_ORDER"
    fi
    if [ "$_k" = BATT_CRIT ]; then
      _lo=$(conf_read BATT_LOW)
      case "$_lo" in ''|*[!0-9]*) : ;; *) [ "$_v" -lt "$_lo" ] || die "BATT_ORDER" ;; esac
    fi
  fi

  conf_put "$_k" "$_v" || die "WRITE" 5
  # 写完立刻读回核对。只信 mv 的返回码不够：
  # 写入"成功"但内容不对的情况要等到下一次重启才暴露，为时已晚。
  [ "$(conf_read "$_k")" = "$_v" ] || die "VERIFY" 6
  # 全局百分比变了：记下方向，给建议做 7 天反向冷却。
  printf 'OK %s=%s\n' "$_k" "$_v"
}

# 一次保存所有键。页面以前逐个 conf_set：三十多个键就是三十多次 root IPC，
# 每次都要重启一遍 sh + 读写整份配置，手机上要十几秒，用户以为没点上就再按。
# 这里一个进程里跑完；值没变的键直接跳过（通常真正要写的只有一两个）。
cmd_conf_set_many() {
  [ "$#" -gt 0 ] || die "BAD_ARGS"
  _n=0
  for _kv in "$@"; do
    case "$_kv" in
      *=*) ;;
      *) die "BAD_KV" ;;
    esac
    _k=${_kv%%=*}
    _v=${_kv#*=}
    [ -n "$_k" ] || die "BAD_KV"
    # 键名必须先验：conf_read 是把键原样拼进 sed 程序的，键里带换行就能
    # 闭合出额外的 sed 命令（w 写文件）。下面那行"值没变就跳过"的快路径
    # 跑在 cfg_set 的校验之前，所以校验只能加在这里。
    is_key "$_k" || [ "$_k" = CPU_GOVERNOR ] || die "BAD_KEY"
    _n=$((_n + 1))
    # 值没变就不动文件：既省写入，也不会因为"原样保存"误改配置。
    [ "$(conf_read "$_k")" = "$_v" ] && continue
    _err=$( ( cfg_set "$_k" "$_v" ) 2>&1 >/dev/null )
    if [ $? -ne 0 ]; then
      printf '%s %s\n' "${_err:-E/WRITE}" "$_k" >&2
      return 7
    fi
  done
  printf 'OK %s\n' "$_n"
}

# ── 芯片推荐预设 ────────────────────────────────────────────────
# 预设只在"全新安装"时自动套用；覆盖安装不会动老配置。这里给页面一个
# 手动入口：按当前 SoC 找 adapt/*.conf，读到什么就应用什么。
# 匹配规则与 customize.sh 完全一致：每条 #match 都要命中，soc 用包含匹配。
_preset_match() {
  PRESET_FILE=
  [ -d "$ROOT/adapt" ] || return 1
  _pb=$(getprop ro.product.brand 2>/dev/null | tr 'a-z' 'A-Z')
  _pm=$(getprop ro.product.model 2>/dev/null | tr 'a-z' 'A-Z')
  _ps=$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')
  [ -z "$_ps" ] && _ps=$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')
  PRESET_SOC=$_ps
  for _f in "$ROOT"/adapt/*.conf; do
    [ -f "$_f" ] || continue
    _ml=$(grep '^#match ' "$_f" 2>/dev/null)
    [ -n "$_ml" ] || continue
    _hit=1
    _oifs=$IFS; IFS='
'
    for _l in $_ml; do
      case "$_l" in
        *model=*) [ "$_pm" = "$(printf '%s' "${_l#*model=}" | tr 'a-z' 'A-Z')" ] || _hit=0 ;;
        *brand=*) [ "$_pb" = "$(printf '%s' "${_l#*brand=}" | tr 'a-z' 'A-Z')" ] || _hit=0 ;;
        *soc=*)   case "$_ps" in *"$(printf '%s' "${_l#*soc=}" | tr 'a-z' 'A-Z')"*) : ;; *) _hit=0 ;; esac ;;
        *) _hit=0 ;;
      esac
    done
    IFS=$_oifs
    if [ "$_hit" = 1 ]; then PRESET_FILE=$_f; return 0; fi
  done
  return 1
}

_preset_val() {
  sed -n "s/^$1=//p" "$PRESET_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t'
}

preset_get() {
  if ! _preset_match; then
    printf 'none=1\nsoc=%s\n' "${PRESET_SOC:-未知}"
    return 0
  fi
  _title=$(sed -n 's/^# *//p' "$PRESET_FILE" | head -n 1)
  printf 'none=0\nsoc=%s\nname=%s\ntitle=%s\n' \
    "${PRESET_SOC:-未知}" "$(basename "$PRESET_FILE" .conf)" "$_title"
  for _pk in big mid2 mid little gpu th_high th_low psi_gain; do
    case "$_pk" in
      big)      _v=$(_preset_val default_cpu_pct_big) ;;
      mid2)     _v=$(_preset_val default_cpu_pct_mid2) ;;
      mid)      _v=$(_preset_val default_cpu_pct_mid) ;;
      little)   _v=$(_preset_val default_cpu_pct_little) ;;
      gpu)      _v=$(_preset_val default_gpu_pct) ;;
      th_high)  _v=$(_preset_val default_th_high) ;;
      th_low)   _v=$(_preset_val default_th_low) ;;
      psi_gain) _v=$(_preset_val default_psi_gain) ;;
    esac
    case "$_v" in ''|*[!0-9]*) continue ;; esac
    printf '%s=%s\n' "$_pk" "$_v"
  done
}

preset_apply() {
  _preset_match || die "NO_PRESET"
  _args=
  _add() { [ -n "$2" ] && _args="$_args $1=$2"; }
  _add CPU_PCT_BIG      "$(_preset_val default_cpu_pct_big)"
  _add CPU_PCT_MID2     "$(_preset_val default_cpu_pct_mid2)"
  _add CPU_PCT_MID      "$(_preset_val default_cpu_pct_mid)"
  _add CPU_PCT_LITTLE   "$(_preset_val default_cpu_pct_little)"
  _add GPU_PCT          "$(_preset_val default_gpu_pct)"
  _add TH_HIGH          "$(_preset_val default_th_high)"
  _add TH_LOW           "$(_preset_val default_th_low)"
  _add PSI_GAIN         "$(_preset_val default_psi_gain)"
  [ -n "$_args" ] || die "NO_VALUES"
  # shellcheck disable=SC2086
  cmd_conf_set_many $_args
}

# ── 名单 ────────────────────────────────────────────────────────

# 名单中已安装在设备上的项。
# 一次 pm list packages 拿全量再自己筛，不逐包调 pm path：
# 数百个包就是数百次进程调用，WebUI 会卡死。
# 这份输出会被当成 grep -F -x 的模式文件用（见 list_installed），
# 所以行尾必须在这里就干净：带 \r 的行 -x 永远判不相等。
_installed_set() {
  pm list packages 2>/dev/null | sed -n 's/^package://p' \
    | awk '{print $1}' | tr -d '\r' | grep -v '^$' | sort
}

# 候选：装了、包名里带发行商词典的段、但还没进名单的应用。
# 只给 WebUI 当"点一下就加进名单"的列表用，**不参与前台匹配**：
# 守护读的永远是 games.txt 的整串包名。词典命中不代表它是游戏，
# 代表"值得你确认一下"。
# 输出每行 "包名<TAB>命中的词"，前端拿第一段当包名、第二段显示原因。
games_suggest() {
  [ -f "$VENDORS" ] || return 0
  # 只看第三方包：系统包里 game 段一堆（预装小游戏、厂商中心），
  # 全列出来就是几百行噪音。
  _inst=$(pm list packages -3 2>/dev/null | sed -n 's/^package://p' \
          | awk '{print $1}' | tr -d '\r' | grep -v '^$' | sort)
  [ -n "$_inst" ] || return 0
  # 排除已在名单中的条目：只呈现未加入的候选。
  # 用临时文件而不是 awk -v：-v 传多行值在 busybox/toybox awk 上行为不一致，
  # 且名单可达四百多条，塞进命令行有长度风险。
  _set="$RUN/.vset.$$"
  tr -d '\r' < "$LIST" 2>/dev/null | grep -Ev '^[[:space:]]*(#|$)' > "$_set" || : > "$_set"
  printf '%s\n' "$_inst" | awk -v vf="$VENDORS" -v sf="$_set" '
    function lw(s) { return tolower(s) }
    # 词判据：带点的按前缀，不带点的按"某一段正好等于它"。
    # 段相等而不是子串包含，是为了 "com.tencent.mm" 不被 mm 之外的
    # 长词顺带命中，也为了 "gameshop" 不被 "game" 命中。
    function hit(w, p,   a, n, i) {
      if (w == "") return 0
      if (index(w, ".") > 0) return (substr(p, 1, length(w)) == w)
      n = split(p, a, ".")
      for (i = 1; i <= n; i++) if (a[i] == w) return 1
      return 0
    }
    BEGIN {
      ni = 0; ne = 0
      while ((getline ln < vf) > 0) {
        sub(/\r$/, "", ln)
        gsub(/^[ \t]+|[ \t]+$/, "", ln)
        if (ln == "" || ln ~ /^#/) continue
        c = substr(ln, 1, 1)
        if (c == "+" || c == "-") { t = c; w = substr(ln, 2) }
        else { t = "+"; w = ln }
        gsub(/^[ \t]+|[ \t]+$/, "", w)
        w = lw(w)
        if (w == "") continue
        if (t == "+") inc[++ni] = w; else exc[++ne] = w
      }
      close(vf)
      ns = 0
      while ((getline ln < sf) > 0) { sub(/\r$/, "", ln); if (ln != "") known[ln] = 1 }
      close(sf)
      shown = 0
    }
    {
      p = $1
      if (p == "") next
      lp = lw(p)
      if (known[p]) next
      m = ""
      for (i = 1; i <= ni; i++) if (hit(inc[i], lp)) { m = inc[i]; break }
      if (m == "") next
      bad = 0
      for (i = 1; i <= ne; i++) if (hit(exc[i], lp)) { bad = 1; break }
      if (bad) next
      # 上限：命中 game/games 这种通用段时可能一抓几十个，
      # 页面全列出来没人看得完，也让首屏变慢。
      if (++shown > 80) exit
      printf "%s\t%s\n", p, m
    }'
  _rc=$?
  rm -f "$_set" 2>/dev/null
  [ "$_rc" -eq 0 ] || die "NO_RUNDIR" 8
}

# 列出已安装应用。默认第三方；all = 连系统包一起（含服务/组件）。
# 深睡豁免选择器也用它：有些厂商短信/电话包不在 `-3` 里，用户要能自己加。
apps_list() {
  if [ "${1:-}" = "all" ]; then
    pm list packages 2>/dev/null | sed -n 's/^package://p' \
      | awk '{print $1}' | tr -d '\r' | grep -v '^$' | sort
  else
    pm list packages -3 2>/dev/null | sed -n 's/^package://p' \
      | awk '{print $1}' | tr -d '\r' | grep -v '^$' | sort
  fi
  # pm 失败或无包：输出为空，这里不 die——
  # 页面拿到空列表显示"没读到已安装应用"，比抛错误码更易读（同 list_all 的处理）。
  return 0
}

# 「分应用档位」的选择器：第三方 + 系统应用。
# 默认只要系统里有 LAUNCHER 入口的（设置/相机/相册…），避免 400+ 服务撑爆列表；
# 但部分 ROM 的 `cmd package query-activities` 被裁掉/不给结果，这时回退到
# `pm list packages -s`（全部系统包），保证"能选到系统应用"。
# 传 all 时直接列全部系统包，给页面上的「显示系统应用」开关用。
mode_apps() {
  _want_all=0
  [ "${1:-}" = "all" ] && _want_all=1
  _sys=""
  if [ "$_want_all" = "0" ]; then
    _sys=$(cmd package query-activities --brief -a android.intent.action.MAIN \
      -c android.intent.category.LAUNCHER 2>/dev/null \
      | sed -n 's/^[[:space:]]*\([A-Za-z0-9_.]*\)\/.*/\1/p')
  fi
  if [ -z "$_sys" ]; then
    _sys=$(pm list packages -s 2>/dev/null | sed -n 's/^package://p' \
      | awk '{print $1}' | tr -d '\r')
  fi
  {
    pm list packages -3 2>/dev/null | sed -n 's/^package://p' | awk '{print $1}' | tr -d '\r'
    printf '%s\n' "$_sys"
  } | grep -v '^$' | sort -u
  # 没有 cmd / 查询失败时至少还有第三方；不 die。
  return 0
}

# 内置深睡豁免清单的显示名：一行 "包名<TAB>中文名"。
# 只给页面显示用（微信的 APK 标签可能是 WeChat，企业微信甚至解析不出来），
# 不参与 Doze / 白名单判断；清单在 <模块>/scripts/builtin_white.txt。
builtin_white() {
  _f="$ROOT/scripts/builtin_white.txt"
  [ -f "$_f" ] || return 0
  awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    {
      h = index($0, "#")
      p = (h > 0) ? substr($0, 1, h - 1) : $0
      n = (h > 0) ? substr($0, h + 1) : ""
      gsub(/^[ \t]+|[ \t]+$/, "", p)
      sub(/[ \t].*$/, "", p)
      gsub(/^[ \t]+|[ \t]+$/, "", n)
      if (p != "" && n != "") printf "%s\t%s\n", p, n
    }' "$_f"
  return 0
}

# 系统应用显示名兜底（APK label 解析不出来时用）。格式同 builtin_white，
# 但只影响显示/搜索，不参与 Doze / 档位匹配。
builtin_labels() {
  _f="$ROOT/scripts/builtin_labels.txt"
  [ -f "$_f" ] || return 0
  awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    {
      h = index($0, "#")
      p = (h > 0) ? substr($0, 1, h - 1) : $0
      n = (h > 0) ? substr($0, h + 1) : ""
      gsub(/^[ \t]+|[ \t]+$/, "", p)
      sub(/[ \t].*$/, "", p)
      gsub(/^[ \t]+|[ \t]+$/, "", n)
      if (p != "" && n != "") printf "%s\t%s\n", p, n
    }' "$_f"
  return 0
}
# 一行一个 "包名<TAB>应用名"，给前端画"第一行应用名、第二行包名"。
# Android 没有现成的应用名来源：pm list packages 无 --show-app-label，
# dumpsys package 也不带标签；名字只存在 APK 里（AndroidManifest 的 android:label，
# 通常是 @string 引用，文本还要去 resources.arsc 字符串池解析），只能由守护读 APK。
# 批量而非逐包：读名字要在 zip 中央目录定位并解出 manifest 和 arsc，
# 逐包调用等于几百次 fork + 几百次重读目录，单进程内循环 IO 相同、进程开销小两个数量级。
# 缓存连 APK 路径一起存：/data/app 的随机后缀每次更新都变，路径变即重取名字；
# 不用时间型 TTL，否则装新应用后要等缓存过期才显示名字。
label_map() {
  # `pm list packages -f` 一行给全 pkg→apk，比每包 fork 一次 pm path 便宜。
  # 取**全部**包而不是只取 -3：分应用档位也列系统里有桌面的应用，
  # 名字同样只能从系统 APK 里读；按需过滤在 cmd_labels 的 _filt 里做。
  # 行形如 package:<apk路径>=<包名>，apk 路径本身含 '='，只能按**最后一个** '=' 切。
  pm list packages -f 2>/dev/null | tr -d '\r' | awk '
    { i = match($0, /=[^=]+$/)
      if (i > 9) {
        p = substr($0, i + 1)
        # "package:" 占 1..8，路径从**第 9 个字符**开始；少算一个会把冒号并进路径，
        # 守护打不开该文件，缓存还会存下同样的错路径（两侧一致地错，输出看不出异常）。
        a = substr($0, 9, i - 9)
        if (p != "" && a != "") print p "\t" a
      } }' | sort
  return 0
}

cmd_labels() {
  # 临时文件带 $$：同一页会同时点名几批（名单/候选/档位），
  # 固定文件名会让后启动的那次把前一次的中间结果覆盖掉，
  # 表现就是"有时候一部分应用没有名字"。
  _cur="$RUN/labels.cur.$$"; _need="$RUN/labels.need.$$"
  _cf="$RUN/labels.new.$$"; _out="$RUN/labels.out.$$"; _wantf="$RUN/labels.want.$$"
  # 缓存不存在时 awk 的第一个文件打不开会整个报错，先把它建出来
  mkdir -p "$RUN" 2>/dev/null
  [ -f "$LABEL_CACHE" ] || : > "$LABEL_CACHE" 2>/dev/null
  : > "$_cf" 2>/dev/null; : > "$_need" 2>/dev/null
  # 带参数时只解"页面此刻要的那几个"包。
  # 按需解析：解一次名字要 manifest + 几 MB 的 arsc，全量解析容易顶到管理器
  # root 调用的超时线；白名单 + 分应用档位合计通常也只有十几个包。
  _filt=0
  if [ "$#" -gt 0 ]; then
    : > "$_wantf" 2>/dev/null
    for _p in "$@"; do is_package "$_p" && printf '%s\n' "$_p" >> "$_wantf"; done
    if [ ! -s "$_wantf" ]; then
      rm -f "$_cur" "$_need" "$_cf" "$_out" "$_wantf" 2>/dev/null
      return 0                      # 一个合法包名都没给：空列表，不是错误
    fi
    _filt=1
  fi
  label_map > "$_cur" 2>/dev/null
  if [ ! -s "$_cur" ]; then
    # pm 没给东西：空列表、退出码 0（处理方式同 list_all）——
    # 此栏非关键路径，页面显示"没读到名字，只显示包名"比抛错误码更温和。
    rm -f "$_cur" "$_need" "$_cf" "$_out" "$_wantf" 2>/dev/null
    return 0
  fi
  # 第一趟：缓存里 APK 路径没变的直接复用，其余进重读名单。
  # 缓存行是 pkg<TAB>apk<TAB>名字 —— 路径完全相等才算同一个包体，
  # 装过新版本的那一列必然不等，于是自动重取。
  #
  # 注意：缓存为空时**不能**走两文件 awk——NR==FNR 惯用法在第一个文件
  # 无记录时会失效（第二个文件首行也满足 NR==FNR），整份当前列表会被读进
  # "缓存"侧、没有包进重读名单。首次装机时缓存正是空的，属常规路径。
  if [ -s "$LABEL_CACHE" ]; then
    awk -F'\t' -v need="$_need" -v cf="$_cf" '
      NR == FNR {
        i = index($0, "\t")
        if (i < 2) next
        k = substr($0, 1, i - 1); rest = substr($0, i + 1)
        j = index(rest, "\t")
        if (j < 2 || k == "") next
        capk[k] = substr(rest, 1, j - 1); clab[k] = substr(rest, j + 1)
        next
      }
      {
        p = $1; a = $2
        if (p == "" || a == "") next
        if (capk[p] == a) printf "%s\t%s\t%s\n", p, a, clab[p] >> cf
        else printf "%s\t%s\n", p, a > need
      }' "$LABEL_CACHE" "$_cur" 2>/dev/null
  else
    cat "$_cur" > "$_need" 2>/dev/null
  fi
  # 第二趟前先看要不要缩小重读名单：缓存里已命中的（上面已进 _cf）不用管，
  # 这里只把"没缓存且本次被点名"的送给守护。
  if [ "$_filt" = 1 ] && [ -s "$_need" ]; then
    awk -F'\t' 'NR == FNR { w[$1] = 1; next } ($1 in w)' "$_wantf" "$_need" > "$_need.f" 2>/dev/null
    mv -f "$_need.f" "$_need" 2>/dev/null
  fi
  # 第二趟：重读那部分交给守护，一个进程循环读全部。
  # 守护按 "pkg<TAB>apk<TAB>名字" 原样回三列（读不到名字的第三列为空），
  # 因此不要在 shell 里按行号对齐拼回：守护少出一行时名字会串到其他包上。
  if [ -s "$_need" ] && [ -f "$BIN" ]; then
    "$BIN" labels < "$_need" >> "$_cf" 2>/dev/null
  fi
  # 响应：只把有名字的送出去，前端自己按包名对上号；没对上的继续显示包名。
  if [ "$_filt" = 1 ]; then
    awk -F'\t' 'NR == FNR { w[$1] = 1; next } (($1 in w) && $3 != "") { printf "%s\t%s\n", $1, $3 }' \
      "$_wantf" "$_cf" > "$_out" 2>/dev/null
  else
    awk -F'\t' '$3 != "" { printf "%s\t%s\n", $1, $3 }' "$_cf" > "$_out" 2>/dev/null
  fi
  # 缓存整体替换：只保留当前仍在机上的包，卸载包不留残行，
  # 这同时就是清理逻辑，不需要单独的 TTL 或条数上限。
  # 仅在 _cf 非空时替换：守护缺失或那一趟整体失败时 _cf 为空，
  # 无条件 mv 会抹掉整份已有缓存，迫使下次全量重解。
  if [ -s "$_cf" ]; then
    mv -f "$_cf" "$LABEL_CACHE" 2>/dev/null
  else
    rm -f "$_cf" 2>/dev/null
  fi
  [ -s "$_out" ] && cat "$_out"
  rm -f "$_cur" "$_need" "$_need.f" "$_out" "$_wantf" 2>/dev/null
  return 0
}

list_all() {
  # 保持文件原样输出，不排序：顺序是用户自己排的。
  # 结尾必须 return 0：grep 无匹配时退出码为 1，而"名单全是注释"是合法的空名单、
  # 不是执行失败；前端 exec() 只看 errno，1 即抛"执行失败"。
  grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$LIST" 2>/dev/null \
    | tr -d '\r'
  return 0
}

# 只列已安装的包。
# 预设名单四百多条、多数未安装，列出未装的没有意义，空图标也会占满页面。
# 顺序按文件原样，不重排：文件顺序即展示顺序。
list_installed() {
  # 临时文件放数据目录的 run/，不放 /tmp：Android root shell 通常没有 /tmp，
  # toybox mktemp 不带模板时默认落向那里，重定向会失败、返回"成功+空输出"，页面显示 0 个。
  # 这里不用 mktemp：它选的目录恰是不可写的那个。
  # 也不用 $ROOT/run：模块目录升级时整目录删掉重解包，可变状态不应放在里面。
  _dir=$RUN
  mkdir -p "$_dir" 2>/dev/null || die "NO_RUNDIR" 8
  _set=$_dir/.set.$$
  _installed_set > "$_set" 2>/dev/null
  # 拿不到包列表必须报错。返回空输出的话，页面显示的是"本机已装 0 个"，
  # 和"名单里真的一条都没装上"长得一模一样，没法区分。
  if [ ! -s "$_set" ]; then rm -f "$_set"; die "NO_PM" 8; fi
  # 名单先规范化成 LF 再整行比对：CRLF 的行尾带 \r，
  # 而模式文件 _set 由 _installed_set 生成（那边已 tr 掉 \r），
  # -x 要求整行相等 -> 任一侧带 \r 就一条都命中不了。
  # tr 放在 grep 之后就晚了，grep 早就按含 \r 的行判过了。
  tr -d '\r' < "$LIST" | grep -F -x -f "$_set"
  rm -f "$_set" 2>/dev/null
  return 0
}

list_add() {
  _p=${1:-}
  is_package "$_p" || die "BAD_PKG"
  [ -f "$LIST" ] || : > "$LIST" || die "WRITE" 5
  # 判重前先规范化：CRLF 的行尾带 \r，-x 判不相等，
  # 于是同一个包能反复追加，每次都报 OK。
  if tr -d '\r' < "$LIST" | grep -q -F -x -- "$_p"; then
    printf 'DUP %s\n' "$_p"
  else
    # 名单末行没有换行时先补一个，否则新包名会和末行粘成 com.a.bcom.c.d：
    # 两个包都匹配不上，而页面报的是"已添加"。
    ensure_final_newline "$LIST"
    printf '%s\n' "$_p" >> "$LIST" || die "WRITE" 5
    printf 'OK %s\n' "$_p"
  fi
}

list_del() {
  _p=${1:-}
  is_package "$_p" || die "BAD_PKG"
  [ -f "$LIST" ] || die "NO_LIST" 4
  # grep -F -x 整行匹配：包名里的点是正则元字符，不加 -F
  # "com.a.b" 会匹配到 "comXaXb" 之类的行并删掉。
  # 先 tr 规范化行尾：CRLF 的行 -x 判不相等，删不掉。
  tr -d '\r' < "$LIST" | grep -v -F -x -- "$_p" | write_atomic "$LIST" || die "WRITE" 5
  printf 'OK %s\n' "$_p"
}

# ── 分应用档位（app_modes.conf）──────────────────────────────────
# 一行一条 pkg=值，值是以下三种：
#   full      CPU 与 GPU 都交还系统
#   80        只封 CPU，GPU 跟随全局（加冒号之前的写法，语义不变）
#   80:40     CPU 80%、GPU 40%；任一段留空表示那一段跟随全局（80: / :40）
# 优先级 应用专属 > 全局百分比。写的一侧有校验、读的一侧（守护）宽松
# 容忍坏行 —— 两边判据要一致，包名沿用上面的 is_package，数字区间同界。
is_mode_val() {
  case "${1:-}" in
    full|FULL|keep|KEEP) return 0 ;;
    */*)
      # big/little[:gpu]：两段 CPU 都必须给，1~100；GPU 段可空。
      _mc=${1%%:*}
      _mg=${1#*:}
      [ "$_mg" = "$1" ] && _mg=""
      _n=0; _r=$_mc
      while [ -n "$_r" ]; do
        case "$_r" in
          */*) _s=${_r%%/*}; _r=${_r#*/} ;;
          *)   _s=$_r; _r="" ;;
        esac
        case "$_s" in ''|*[!0-9]*) return 1 ;; esac
        [ "$_s" -ge 1 ] && [ "$_s" -le 100 ] || return 1
        _n=$((_n + 1))
      done
      [ "$_n" -eq 2 ] || return 1
      if [ -n "$_mg" ]; then
        case "$_mg" in *[!0-9]*) return 1 ;; esac
        [ "$_mg" -ge 1 ] && [ "$_mg" -le 100 ] || return 1
      fi
      return 0
      ;;
    *:*)
      _mc=${1%%:*}
      _mg=${1#*:}
      [ -n "$_mc$_mg" ] || return 1
      if [ -n "$_mc" ]; then
        case "$_mc" in *[!0-9]*) return 1 ;; esac
        [ "$_mc" -ge 1 ] && [ "$_mc" -le 100 ] || return 1
      fi
      if [ -n "$_mg" ]; then
        case "$_mg" in *[!0-9]*) return 1 ;; esac
        [ "$_mg" -ge 1 ] && [ "$_mg" -le 100 ] || return 1
      fi
      return 0
      ;;
  esac
  case "$1" in
    ''|*[!0-9]*) return 1 ;;
  esac
  [ "$1" -ge 1 ] && [ "$1" -le 100 ]
}

# 输出成 "pkg=值" 的纯数据行，供前端渲染；
modes_list() {
  [ -f "$MODES" ] || return 0
  # 同 list_all：包里的 app_modes.conf 是"全是注释 + 例子"的模板，
  # grep 无匹配时退出码为 1，那是"还没设过档位"，不是失败。
  tr -d '\r' < "$MODES" | awk '
    /^[[:space:]]*(#|$)/ { next }
    {
      i = index($0, "=")
      if (i < 2) next
      pkg = substr($0, 1, i - 1)
      rest = substr($0, i + 1)
      sub(/#.*/, "", rest)
      gsub(/^[ \t]+|[ \t]+$/, "", pkg)
      gsub(/[ \t]+$/, "", rest)
      if (!pkg || rest == "") next
      print pkg "=" rest
    }'
  return 0
}

modes_set() {
  _p=${1:-}
  _v=${2:-}
  is_package "$_p" || die "BAD_PKG"
  is_mode_val "$_v" || die "BAD_MODE"
  _v=$(printf '%s' "$_v" | tr 'A-Z' 'a-z')
  [ -f "$MODES" ] || { mkdir -p "$DATA" 2>/dev/null; : > "$MODES" 2>/dev/null; } \
    || die "WRITE" 5
  # 同一应用重复设置要覆盖，不能追加第二行：
  # 守护按"后一条赢"处理，页面上却会列出两行同一个包。
  if tr -d '\r' < "$MODES" | grep -q -F -x -- "$_p=$_v"; then
    printf 'DUP %s=%s\n' "$_p" "$_v"
    return 0
  fi
  # 判断"这个包已经有条目"用 awk 的 index() 前缀比较，不用 grep -E "^$_p="：
  # 包名里的点是正则通配符，"com.a.b" 会连 "comXaXb=90" 一起认成同一个包。
  if tr -d '\r' < "$MODES" | awk -v k="$_p" 'index($0, k "=") == 1 { f = 1 } END { exit !f }'; then
    tr -d '\r' < "$MODES" \
      | awk -v k="$_p" -v v="$_v" 'index($0, k "=") == 1 { print k "=" v; next } { print }' \
      | write_atomic "$MODES" || die "WRITE" 5
    printf 'OK %s=%s\n' "$_p" "$_v"
    return 0
  fi
  ensure_final_newline "$MODES"
  printf '%s=%s\n' "$_p" "$_v" >> "$MODES" || die "WRITE" 5
  printf 'OK %s=%s\n' "$_p" "$_v"
}

modes_del() {
  _p=${1:-}
  is_package "$_p" || die "BAD_PKG"
  [ -f "$MODES" ] || die "NO_MODES" 4
  # 删的是 "pkg=" 前缀那一行（值可能是 full 也可能是 80，只给包名）。
  # 用 awk 前缀匹配而非 grep -F -x：整行相等在值改过之后就删不掉了。
  if ! tr -d '\r' < "$MODES" | awk -v k="$_p" 'index($0, k "=") == 1 { f = 1 } END { exit !f }'; then
    printf 'MISS %s\n' "$_p"
    return 0
  fi
  tr -d '\r' < "$MODES" \
    | awk -v k="$_p" 'index($0, k "=") == 1 { next } { print }' \
    | write_atomic "$MODES" || die "WRITE" 5
  printf 'OK %s\n' "$_p"
}

# ── 图标 ────────────────────────────────────────────────────────
#
# 从设备上已安装的 APK 里读图标，输出完整 data URI。
# 不联网（模块没有网络能力），也不写 /sdcard。
#
# 解包由守护的 icon 子命令完成（src/icon.c 自己读 zip 中央目录），
# 不依赖外部 unzip：Android 的 toybox 通常不带该命令，且各家实现输出格式不一。

icon_trim() {
  # 先清孤儿 .via。这一步必须放在"要不要淘汰"的早退之前：
  # .via 不匹配下面那句 grep '\.b64$' 的计数，且目录只有一两张图时不会触发淘汰，
  # 清理若写在早退之后就永远不会执行。
  ls -1 "$ICON_DIR" 2>/dev/null | grep '\.b64\.via$' \
    | while read -r v; do
        [ -f "$ICON_DIR/${v%.via}" ] || rm -f "$ICON_DIR/$v" 2>/dev/null
      done
  # 缓存条目超过上限就删最旧的。不设上限的话，
  # 用户装几十个游戏就能在数据目录里堆几十 MB。
  _n=$(ls -1 "$ICON_DIR" 2>/dev/null | grep -c '\.b64$')
  [ "${_n:-0}" -le "$ICON_MAX" ] && return 0
  ls -1t "$ICON_DIR" 2>/dev/null | grep '\.b64$' | tail -n +$((ICON_MAX + 1)) \
    | while read -r f; do
        rm -f "$ICON_DIR/$f" "$ICON_DIR/$f.via" 2>/dev/null
      done
  # 淘汰 .b64 时连带删除其 .via，否则每删一张图留下一个孤儿文件。
  # （uninstall.sh 对整个 $ICON_DIR 做 rm -rf，不依赖此处。）
  return 0
}

# 记下最后一次取图的结果，供关于页回显。
# 只留一个码，不记包名：图标是滚动时才按需取的，
# 最后一条对应的包未必就是用户正在看的那个，写包名反而会误导。
# 用单文件而不是每次一个：$ICON_DIR 已经被 uninstall.sh 清过，
# 再多一类文件就要同步改删除逻辑。
icon_fail() {
  mkdir -p "$ICON_DIR" 2>/dev/null || return 0
  printf '%s\n' "${1:-NOICON}" > "$ICON_DIR/.last" 2>/dev/null
  return 0
}

icon_last() {
  _l=$(cat "$ICON_DIR/.last" 2>/dev/null | head -n 1)
  # 白名单必须容下 '='：兜底成功记的是 OK=UNZIP；
  # 若只允许大写字母，OK=UNZIP 会被归一化成 UNKNOWN，显示成"还没取过图标"的假状态。
  case "$_l" in
    ''|*[!A-Z=]*) _l=UNKNOWN ;;
  esac
  printf '%s\n' "$_l"
}

# ── unzip 兜底 ────────────────────────────────────────────────────
# 主路（守护自己解析中央目录）失败时的备用路径。
# 部分 ROM 自带 unzip，但各家列格式与版本有差异，不能作为主路依赖，只作兜底。
#
# 硬性约束：这里不许出现任何挑图规则（mipmap / ic_launcher / xxhdpi 都不写），
# 挑图统一交给守护的 score 子命令，规则只在 icon.c 的 score_name 一份实现；
# 在 shell 里重写一份会造成两处规则漂移，两条路给出不同图标。
#
# 能力边界：兜底只有"按名字挑"这一档。守护还有第二轮——不读名字、直接读图片头
# 按尺寸猜（score_pixels），用于资源名被 AGP 缩短过的包（res/aK.png 这种）。
# score 子命令只吃纯名字，走 unzip 得整包解一遍，不划算。
# 两条路互补：缩短包只有主路能出图，超大/ZIP64 包只有兜底能出图，都不可删。
#
# $1 = apk 路径。成功打 data URI，失败什么都不打并返回非零。
_icon_via_unzip() {
  command -v unzip >/dev/null 2>&1 || return 1
  # sed 剥掉 `unzip -l` 的日期/时间两列，但**保留第一列 Length**，
  # 用 TAB 接名字。守护的 score 会把名字打分后原样带回，
  # 于是可按"同分取字节小的"排序——与主路择优规则一致；
  # 缺 Length 时同档多候选会让两条路给出不同图标。
  # 各家列格式不一样，所以下面还要用内容前缀校验兜底。
  _e=$(unzip -l "$1" 2>/dev/null | grep -iE '\.(png|webp)$' \
        | sed -E 's/^ *([0-9]+) +[^ ]+ +[^ ]+ +/\1\t/' \
       | "$BIN" score 2>/dev/null | sort -k1,1nr -k2,2n | head -n 1 | cut -f3-)
  [ -n "$_e" ] || return 1
  _b=$(unzip -p "$1" "$_e" 2>/dev/null | base64 2>/dev/null | tr -d '\n')
  [ -n "$_b" ] || return 1
  # **必须验内容前缀**：名字剥错（列格式对不上）时 unzip -p 可能输出空或错误文本，
  # base64 后照样非空，不校验就会把垃圾当图标写进缓存并之后每次命中。
  # base64 开头几个字符恰是魔数：iVBORw0KGgo → PNG、UklGR → WebP、/9j/ → JPEG。
  case "$_b" in
    iVBOR*) printf 'data:image/png;base64,%s'  "$_b"; return 0 ;;
    UklGR*) printf 'data:image/webp;base64,%s' "$_b"; return 0 ;;
    /9j/*)  printf 'data:image/jpeg;base64,%s' "$_b"; return 0 ;;
  esac
  return 1
}

cmd_icon() {
  _pkg=${1:-}
  is_package "$_pkg" || die "BAD_PKG"
  [ -f "$BIN" ] || { echo; return 0; }

  _apk=$(pm path "$_pkg" 2>/dev/null | sed -n 's/^package://p' | head -n 1)
  # 没装这个应用。返回空行：名单里大部分包在手机上根本没装，这是常态，
  # 不是错误状态，报错会让整个名单页变成一个错误提示。
  if [ -z "$_apk" ] || [ ! -r "$_apk" ]; then icon_fail NOTINST; echo; return 0; fi

  # 缓存键带上 APK 的 mtime：应用换了图标会自动失效重取，
  # 不用用户手动清缓存。
  # 还要带上取图算法的版本：算法改了（比如声明图标不再受体积预算限制），
  # 老缓存里选错的那张不会自己变对——换键才让它自然失效。
  _mt=$(stat -c %Y "$_apk" 2>/dev/null || echo 0)
  _out=$ICON_DIR/$_pkg.$_mt.$ICON_ALGO.b64
  if [ -s "$_out" ]; then
    # 缓存命中也要报来源：否则关于页只显示"图标正常"，
    # 这张图来自兜底、主路是否损坏的线索就此丢失。
    # .via 缺失（老缓存/被清过）时报裸 OK，不猜来源为 DAEMON。
    _v=$(head -n 1 "$_out.via" 2>/dev/null)
    case "$_v" in
      DAEMON|UNZIP) icon_fail "OK=$_v" ;;
      *)            icon_fail OK ;;
    esac
    cat "$_out"; echo; return 0
  fi

  mkdir -p "$ICON_DIR" 2>/dev/null || { icon_fail NODIR; echo; return 0; }
  icon_trim
  # 解两次：第一次只为判断有没有拿到图，拿到才落缓存；
  # 一次成型会把"失败"也写成空文件，之后永远命中空缓存。
  # 守护的失败原因走 stderr（stdout 必须干净，不能污染 data URI），
  # 存入 $ICON_DIR/.last 供关于页回显。
  _err=$ICON_DIR/.last.$$
  # 先问"这个包自己声明的桌面图标是哪一条"。声明过就照着取，
  # 按名字打分、按尺寸猜那两轮只用于未声明的包：资源名被 AGP 缩短过的包
  # （res/SW.webp 那种）猜出来的经常是游戏内素材，守护"成功"返回错图时
  # 页面看不出异常。这一步多一次 fork，只在缓存未命中的路径上跑。
  _want=$("$BIN" iconpath "$_apk" 2>/dev/null | head -n 1)
  case "$_want" in
    # 不能要求 res/ 前缀：AGP 资源路径缩短后会变成 r/xxx.webp。
    # 空行、报错串、绝对路径、带空格的路径一律当"没有声明"。
    ''|/*|*..*|*[[:space:]]*) _want= ;;
  esac
  if [ -n "$_want" ]; then
    _u=$("$BIN" icon "$_apk" "$_want" 2>"$_err" | head -n 1)
  else
    _u=$("$BIN" icon "$_apk" 2>"$_err" | head -n 1)
  fi
  case "$_u" in
    data:image/*\;base64,*)
      printf '%s\n' "$_u" > "$_out.tmp$$" 2>/dev/null \
        && mv -f "$_out.tmp$$" "$_out" 2>/dev/null
      # 成功也要记来源；不记则关于页长期显示"还没取过图标"，
      # 这种假状态比直接报错更让人怀疑模块坏了。
      printf 'DAEMON\n' > "$_out.via" 2>/dev/null
      icon_fail OK=DAEMON
      cat "$_out"
      ;;
    *)
      _code=$(sed -n 's|^E/||p' "$_err" 2>/dev/null | head -n 1)
      # 主路失败先走兜底，兜不住才落回主路的原始错误码。
      # 顺序不能反：兜底成功也要记 OK=UNZIP 而不是 OK，
      # 保留主路是否损坏的可观测线索。
      if _u=$(_icon_via_unzip "$_apk"); then
        printf '%s\n' "$_u" > "$_out.tmp$$" 2>/dev/null \
          && mv -f "$_out.tmp$$" "$_out" 2>/dev/null
        printf 'UNZIP\n' > "$_out.via" 2>/dev/null
        icon_fail OK=UNZIP
        printf '%s\n' "$_u"
      else
        case "$_code" in
          NOENTRY) icon_fail NOSQUARE ;;   # 一张候选都没挑中：多半是资源名被 AGP 缩短了
          EMIT)    icon_fail BADIMAGE ;;   # 挑中了，但一张都解不出来
          OPEN)    icon_fail OPENFAIL ;;
          TOOBIG)  icon_fail TOOBIG ;;     # >4GB，中央目录偏移会超出 uint32
          *)       icon_fail NOICON ;;
        esac
      fi
      ;;
  esac
  rm -f "$_err" "$_out.tmp$$" 2>/dev/null
  echo
  return 0
}

# 批量取图标：一行 "包名<TAB>data URI"，没图的包只给一行 "包名<TAB>"。
# 页面一次要画几十行，逐包调用等于几十次 root 往返；进程开销也省在
# 同一个 shell 里。单次不封顶：调用方自己按可视区切片。
cmd_icons() {
  [ "$#" -gt 0 ] || return 0
  for _pkg in "$@"; do
    is_package "$_pkg" || continue
    # cmd_icon 末尾固定多打一个空行；这里只取第一行，避免输出里夹空行。
    _u=$(cmd_icon "$_pkg" | head -n 1)
    printf '%s\t%s\n' "$_pkg" "$_u"
  done
}

# ── 机型适配包 ──────────────────────────────────────────────────
# 有些机器的节点名、目录结构和常规不一样，自动探测一个都读不到，
# 装上就是"看不到核心表 / GPU 不限频"。这种机器只能人工适配，
# 而适配得先知道他那台机上到底有什么，所以用户侧只有一个动作：
#   导出：把现场（能读到的节点、频率表、探测结论）打成一个 txt 发过来。
# 改好的节点表由作者写进下一个模块版本，装机时放进数据目录当 override.conf，
# 守护在自动探测扑空时才按它兜底 —— 用户不需要自己导任何东西。
# 不提供导入：用户拿到一份别人机器的表没有意义，做成页面入口等于给他一个
# 把机器钉死在别人节点表上的开关。改好的表由作者随下一个版本发。
# 落点选公共 Download：QQ / 微信 的发送框能直接翻到，不用 root 文件管理器。
PKG_DIR="${SOCFREQ_PKG_DIR:-/storage/emulated/0/Download/Soc智能限频}"
OVR="$DATA/override.conf"

# 机读头一行：值里的换行压成空格，保证"一行一键"，导入端才切得开
pkg_kv() { printf '#%s=%s\n' "$1" "$(printf '%s' "$2" | tr '\n\r' '  ')"; }
pkg_sec() { printf '\n===== %s =====\n' "$1"; }

# 现在这台机上"能用的节点表"，逐条写成 #ovr 行。
# 探测正常的机器导出后这段就是可用的；探测不到的机器这段会短甚至为空，
# 那正是作者要照着后面的人读段落补的东西。
# devfreq 下面不止 GPU：UFS 控制器（ufshci）、总线带宽监控（busmon/bwmon/memlat）、
# dvfsrc 也有 max_freq。只认名字像 GPU 的，黑名单只挡已知项，挡不住新机型。
gpu_node_ok() {
  case "$1" in *mali*|*kgsl*|*gpufreq*|*gpu*|*3d0*) ;; *) return 1 ;; esac
  case "$1" in *ufshci*|*dvfsrc*|*busmon*|*bwmon*|*memlat*) return 1 ;; esac
  return 0
}

pkg_ovr() {
  for d in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$d" ] || continue
    _h=$(cat "$d/cpuinfo_max_freq" 2>/dev/null)
    [ -n "$_h" ] && printf '#ovr cpu-dir %s %s\n' "$d" "$_h"
  done
  # 常规路径都没命中时才有的备用形态：逐核目录里也有 cpuinfo_max_freq
  if ! ls -d /sys/devices/system/cpu/cpufreq/policy* >/dev/null 2>&1; then
    for d in /sys/devices/system/cpu/cpu*/cpufreq; do
      [ -d "$d" ] || continue
      _h=$(cat "$d/cpuinfo_max_freq" 2>/dev/null)
      [ -n "$_h" ] && printf '#ovr cpu-dir %s %s\n' "$d" "$_h"
    done
  fi
  # devfreq 下面不止 GPU 一个设备：总线带宽监控（busmon/bwmon/memlat）也有
  # max_freq 和 available_frequencies。通配列到它的话，导出的 #ovr 会把带宽节点
  # 当成 GPU 上限节点，导入后限的是带宽、核心频率一个字不动 —— 页面还显示"GPU 已接上"。
  # 现在反过来：先按名字白名单只认 GPU 类节点，再用黑名单排掉已知非 GPU。
  for f in /sys/class/devfreq/*/max_freq /sys/class/kgsl/kgsl-3d0/max_gpuclk \
           /sys/class/misc/mali0/device/devfreq/max_freq /sys/kernel/gpu/gpu_max_clock; do
    [ -f "$f" ] || continue
    gpu_node_ok "$f" || continue
    printf '#ovr gpu-max %s\n' "$f"
    _m=$(printf '%s' "$f" | sed 's/max_freq$/min_freq/;s/max_gpuclk$/min_gpuclk/;s/gpu_max_clock$/gpu_min_clock/')
    [ -f "$_m" ] && printf '#ovr gpu-min %s\n' "$_m"
    break
  done
  for f in /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies \
           /sys/class/devfreq/*/available_frequencies \
           /sys/class/misc/mali0/device/devfreq/available_frequencies; do
    [ -f "$f" ] || continue
    gpu_node_ok "$f" || continue
    printf '#ovr gpu-freqs %s\n' "$(cat "$f" 2>/dev/null)"
    break
  done
}

cmd_pkg() {
  mkdir -p "$PKG_DIR" 2>/dev/null
  [ -d "$PKG_DIR" ] || die "PKG_DIR" 7
  _b=$(getprop ro.product.brand 2>/dev/null)
  _m=$(getprop ro.product.model 2>/dev/null)
  _s=$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')
  [ -n "$_s" ] || _s=$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')
  # 文件名里的空格、斜杠一律换成下划线：这种路径经 QQ 传一轮最容易坏
  _fn=$(printf '%s-%s-%s' "$_b" "$_m" "$_s" | tr -c 'A-Za-z0-9._-' '_')
  _t=$(date +%Y%m%d-%H%M 2>/dev/null || echo unknown)
  _f="$PKG_DIR/socpkg-$_fn-$_t.txt"
  {
    printf '#SOCPKG 1\n'
    pkg_kv soc "$_s"
    pkg_kv model "$_m"
    pkg_kv brand "$_b"
    pkg_kv platform "$(getprop ro.board.platform 2>/dev/null)"
    pkg_kv android "$(getprop ro.build.version.release 2>/dev/null) sdk$(getprop ro.build.version.sdk 2>/dev/null)"
    pkg_kv kernel "$(uname -r 2>/dev/null)"
    pkg_kv build "$(getprop ro.build.display.id 2>/dev/null)"
    pkg_kv root "$(for d in /data/adb/ksu /data/adb/ap /data/adb/magisk; do [ -d "$d" ] && printf '%s ' "$d"; done)"
    pkg_ovr

    pkg_sec '守护探测 probe 原样（它自己怎么看这台机）'
    if [ -f "$BIN" ]; then "$BIN" probe 2>&1; else echo '（二进制不在，这份包是装机前导的）'; fi

    pkg_sec '守护 export 原样'
    if [ -f "$BIN" ]; then "$BIN" export 2>&1; else echo '（同上）'; fi

    # 偏移、温度、PSI、亮屏/充电这些"为什么不采样 / 为什么封顶偏低"的证据
    # 都在状态文件里，缺了这段只能靠猜。
    pkg_sec '守护运行状态（run/status）'
    if [ -f "$STATUS" ]; then cat "$STATUS" 2>/dev/null; else echo '（没有状态文件：守护没在跑或还没写出）'; fi

    pkg_sec 'cpufreq 现场逐个 policy'
    for d in /sys/devices/system/cpu/cpufreq/policy*; do
      [ -d "$d" ] || continue
      printf '%s 硬件=%s 当前上限=%s 调速器=%s\n' "${d##*/}" \
        "$(cat "$d/cpuinfo_max_freq" 2>/dev/null)" "$(cat "$d/scaling_max_freq" 2>/dev/null)" \
        "$(cat "$d/scaling_governor" 2>/dev/null)"
      printf '  可用频点=%s\n' "$(cat "$d/scaling_available_frequencies" 2>/dev/null)"
      printf '  可选调速器=%s\n' "$(cat "$d/scaling_available_governors" 2>/dev/null)"
      printf '  核=%s 驻留表=%s\n' "$(cat "$d/related_cpus" 2>/dev/null)" \
        "$([ -f "$d/stats/time_in_state" ] && echo 有 || echo 无)"
    done
    ls -d /sys/devices/system/cpu/cpufreq/policy* >/dev/null 2>&1 || \
      echo '（一个 policy 目录都没有：看看下面这份逐核目录的清单）'
    for d in /sys/devices/system/cpu/cpu0/cpufreq; do
      [ -d "$d" ] && printf '逐核节点 %s 硬件=%s\n' "$d" "$(cat "$d/cpuinfo_max_freq" 2>/dev/null)"
    done

    pkg_sec 'GPU 节点普查'
    ls -1 /sys/class/devfreq/ 2>/dev/null | sed 's/^/  devfreq 下有: /'
    for f in /sys/class/kgsl/kgsl-3d0/max_gpuclk /sys/class/kgsl/kgsl-3d0/min_gpuclk \
             /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies \
             /sys/class/kgsl/kgsl-3d0/gpu_clock_stats /sys/class/kgsl/kgsl-3d0/clock_mhz \
             /sys/kernel/gpu/gpu_max_clock /sys/class/devfreq/*kgsl*/max_freq \
             /sys/class/devfreq/*mali*/max_freq /sys/class/devfreq/*mali*/available_frequencies; do
      [ -e "$f" ] && printf '  在 %s = %s\n' "$f" "$(cat "$f" 2>/dev/null | head -c 100)"
    done
    # devfreq 的设备名可能是 UFS/带宽，光看 max_freq 分不出是不是 GPU；
    # 把每个目录解析到真实设备路径，作者一眼能认出 ufshci / dvfsrc。
    for d in /sys/class/devfreq/*; do
      [ -e "$d" ] && printf '  → %s\n' "$(readlink -f "$d")"
    done
    # MTK 不走 devfreq：频率表和上限在 procfs。这里只读，不写。
    for f in /proc/gpufreq/gpufreq_opp_dump /proc/gpufreq/gpufreq_var_dump \
             /proc/gpufreqv2/stack_working_opp_table /proc/gpufreqv2/gpufreq_status \
             /proc/gpufreqv2/limit_table /proc/gpufreqv2/fix_target_opp_index; do
      [ -e "$f" ] || continue
      printf '  在 %s（%s）:\n' "$f" "$(ls -l "$f" 2>/dev/null | awk '{print $1}')"
      head -20 "$f" 2>/dev/null | sed 's/^/    /'
    done
    for d in /sys/module/ged/parameters /sys/kernel/ged/hal; do
      [ -d "$d" ] || continue
      printf '  目录 %s:\n' "$d"
      ls -1 "$d" 2>/dev/null | head -30 | sed 's/^/    /'
    done
    # GED 的自定义上限/下限是数值节点，单位可能是 kHz（MTK gpufreq 用 kHz）。
    # 把当前值打出来，作者才能判断能不能直接当 max_freq 用。
    for f in /sys/module/ged/parameters/gpu_cust_upbound_freq \
             /sys/module/ged/parameters/gpu_bottom_freq \
             /sys/module/ged/parameters/gpu_cust_boost_freq \
             /sys/kernel/ged/hal/custom_upbound_gpu_freq \
             /sys/kernel/ged/hal/current_freqency; do
      [ -e "$f" ] && printf '  %s = %s\n' "$f" "$(cat "$f" 2>/dev/null)"
    done

    pkg_sec '守护要用的那几个接口读不读得到'
    printf '  dumpsys power 里的 mWakefulness 行数: %s\n' "$(dumpsys power 2>/dev/null | grep -c mWakefulness)"
    printf '  dumpsys window 里的 mCurrentFocus 行数: %s\n' "$(dumpsys window 2>/dev/null | grep -c mCurrentFocus)"
    printf '  dumpsys activity 里的 ResumedActivity 行数: %s\n' "$(dumpsys activity activities 2>/dev/null | grep -c ResumedActivity)"
    printf '  ps -A 行数: %s\n' "$(ps -A 2>/dev/null | wc -l)"
    printf '  /data/system/packages.list: %s\n' "$([ -r /data/system/packages.list ] && echo 可读 || echo 不可读)"

    pkg_sec '当前配置全文'
    cat "$CONF" 2>/dev/null

    # 待机耗电的现场：深睡有没有在跑、名单有多大、系统空闲状态是什么。
    pkg_sec '深度睡眠现场'
    printf '  dsleep.conf:\n'
    sed 's/^/    /' "$DS_CONF" 2>/dev/null
    printf '  ds_state:\n'
    sed 's/^/    /' "$DS_STATE" 2>/dev/null
    printf '  ds_snap（上次未正常退出才有）: %s\n' \
      "$([ -f "$DATA/ds_snap.conf" ] && echo 有 || echo 无)"
    printf '  freeze.saved（冻结账本）: %s\n' \
      "$([ -f "$DATA/run/freeze.saved" ] && echo 有 || echo 无)"
    printf '  freezer 挂载: %s\n' \
      "$(awk '$3=="cgroup" && $4 ~ /(^|,)freezer(,|$)/ {print $2; exit}' /proc/mounts 2>/dev/null)"
    printf '  cgroup.freeze: %s\n' \
      "$([ -r /sys/fs/cgroup/cgroup.controllers ] && grep -qw freezer /sys/fs/cgroup/cgroup.controllers 2>/dev/null && echo 可用 || echo 无)"
    printf '  dsleep.log 最近 40 行:\n'
    tail -n 40 "$DATA/dsleep.log" 2>/dev/null | sed 's/^/    /'
    printf '  dumpsys deviceidle 关键行:\n'
    dumpsys deviceidle 2>/dev/null | grep -E 'mState=|mForceIdle=|mLightState=' \
      | head -6 | sed 's/^/    /'
    printf '  deviceidle whitelist 行数: %s\n' \
      "$(dumpsys deviceidle whitelist 2>/dev/null | grep -c .)"
    printf '  dumpsys power: %s\n' \
      "$(dumpsys power 2>/dev/null | grep -m1 -o 'mWakefulness=[A-Za-z]*')"

    pkg_sec '最近 40 行日志'
    st_log 40

    pkg_sec '这份包缺什么（自动结论，作者先看这段）'
    _np=$(ls -1d /sys/devices/system/cpu/cpufreq/policy* 2>/dev/null | wc -l)
    printf '  policy 目录数 = %s\n' "$_np"
    [ "$_np" = 0 ] && echo '  → CPU 探测只能靠逐核目录或人工给路径（#ovr cpu-dir）'
    _no=0
    for d in /sys/devices/system/cpu/cpufreq/policy*; do
      [ -d "$d" ] || continue
      [ -f "$d/stats/time_in_state" ] || _no=$((_no + 1))
    done
    [ "$_no" -gt 0 ] && printf '  → 有 %s 个 policy 没有 stats/time_in_state：采样只能走粗略档\n' "$_no"
    # 判"有没有可写上限节点"要逐个试，不能把三个候选塞进一条 ls：
    # ls 只要有一个参数打不开就返回非零，高通机器上 mali 那条必然不存在，
    # 于是明明有 3d00000.qcom,kgsl-3d0/max_freq 也会被报成"没找到"。
    _g=0
    for f in /sys/class/devfreq/*/max_freq /sys/class/kgsl/kgsl-3d0/max_gpuclk \
             /sys/class/misc/mali0/device/devfreq/max_freq /sys/kernel/gpu/gpu_max_clock; do
      [ -f "$f" ] || continue
      gpu_node_ok "$f" || continue
      _g=1; break
    done
    [ "$_g" = 0 ] && echo '  → 没找到可写的 GPU 上限节点：GPU 限频在这台机上不会生效'
    [ -d /proc/gpufreqv2 ] && echo '  → 检测到 MTK gpufreqv2：看上面的 limit_table / stack_working_opp_table'
  } > "$_f" 2>/dev/null
  [ -s "$_f" ] || { rm -f "$_f" 2>/dev/null; die "PKG_WRITE" 7; }
  chmod 0644 "$_f" 2>/dev/null
  printf 'OK %s\n' "$_f"
}

# 一行覆盖项在这台机上到底存不存在 —— 导入回执要说清：
# "导入成功"但路径全是别人机器上的，等于什么都没适配。
cmd_pkg_dir() { printf '%s\n' "$PKG_DIR"; }

cmd_ovr_clear() {
  rm -f "$OVR" 2>/dev/null
  [ -f "$BIN" ] && "$BIN" restart >/dev/null 2>&1
  printf 'OK ovr_clear\n'
}


cmd_ovr_show() {
  printf 'ovr=%s\n' "$([ -f "$OVR" ] && echo 1 || echo 0)"
  [ -f "$OVR" ] && cat "$OVR"
  return 0
}

# ── 深度睡眠 ────────────────────────────────────────────────────

_ds_conf_init() {
  mkdir -p "$DATA" "$RUN" 2>/dev/null
  [ -f "$DS_CONF" ] && return 0
  {
    printf '%s\n' '# 深度睡眠配置'
    printf '%s\n' '# enable=1 打开；0 关闭。关闭只停止干预，不会保留任何系统改动。'
    printf '%s\n' 'enable=0'
    printf '%s\n' '# 每行一条 white=包名，名单里的应用深睡时也允许被唤醒（等同系统电量设置里的「无限制」）。'
    printf '%s\n' '# white=com.tencent.mm'
  } | write_atomic "$DS_CONF"
}

_ds_enable() {
  [ -f "$DS_CONF" ] || { echo 0; return 0; }
  awk '
    /^[[:space:]]*enable[[:space:]]*=/ {
      v = $0
      sub(/^[^=]*=/, "", v)
      sub(/#.*/, "", v)
      gsub(/[[:space:]]/, "", v)
      print v
      exit
    }' "$DS_CONF" 2>/dev/null | awk '{print ($1 == "1") ? 1 : 0; f = 1} END {if (!f) print 0}'
}

# 读一个 0/1 的开关键（enable / freeze / appops），缺省用给的默认值。
_ds_switch() {
  _k=${1:-}; _d=${2:-1}
  [ -f "$DS_CONF" ] || { echo "$_d"; return 0; }
  _v=$(awk -v k="$_k" '
    $0 ~ "^[[:space:]]*" k "[[:space:]]*=" {
      v = $0
      sub(/^[^=]*=/, "", v)
      sub(/#.*/, "", v)
      gsub(/[[:space:]]/, "", v)
      print v
      exit
    }' "$DS_CONF" 2>/dev/null)
  case "$_v" in 0|1) echo "$_v" ;; *) echo "$_d" ;; esac
}

# 当前会选哪个冻结后端：v2（原地）/ v1（独立层级）/ sigstop / off。
# 用真脚本探一次，不自己抄一份判断 —— 抄的那份迟早和后端逻辑对不上。

# pid 文件只能当线索：kill -9 后文件会残留，真正判据是 /proc 里的 cmdline。
_ds_alive() {
  _p=$(cat "$DS_PID" 2>/dev/null)
  case "$_p" in ''|*[!0-9]*) echo 0; return 0 ;; esac
  if [ -d "/proc/$_p" ] && tr '\0' ' ' < "/proc/$_p/cmdline" 2>/dev/null \
       | grep -q 'deep_sleep\.sh'; then
    echo 1
  else
    echo 0
  fi
}

_ds_state_val() {
  [ -f "$DS_STATE" ] || { echo ""; return 0; }
  awk -v k="$1" '
    index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }' "$DS_STATE" 2>/dev/null
}

_ds_start() {
  [ -f "$DS_SH" ] || return 0
  # 关着就不常驻：打开开关时 cmd_ds_set 会立刻拉起。
  [ "$(_ds_enable)" = "1" ] || return 0
  [ "$(_ds_alive)" = "1" ] && return 0
  mkdir -p "$RUN" 2>/dev/null
  rm -f "$DS_PID" 2>/dev/null
  if command -v setsid >/dev/null 2>&1; then
    setsid sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
  else
    nohup sh "$DS_SH" >/dev/null 2>&1 < /dev/null &
  fi
  # 等它写出 pid：页面紧接着就会读 alive，差这一下会显示成"守护没在跑"。
  # 200ms 一探：守护写 pid 是启动后立刻做的事，按整秒等会让开关白卡 1 秒。
  _i=0
  while [ "$_i" -lt 10 ] && [ "$(_ds_alive)" != "1" ]; do sleep 0.2; _i=$((_i + 1)); done
  return 0
}

# TERM 正常退出会写 state；被 kill -9 时不会。这里兜底把 dsactive 改成 0，
# 页面停止后不会还显示「累计中」。
_ds_state_active_off() {
  [ -f "$DS_STATE" ] || return 0
  awk 'index($0, "dsactive=") == 1 { print "dsactive=0"; next } { print }' "$DS_STATE" \
    | write_atomic "$DS_STATE"
  return 0
}

# 先发 TERM 让守护自己解冻 + unforce + 撤临时白名单；没来得及退出的按账本/快照兜底。
# source 放在子 shell 里：脚本里的变量和函数不许污染 api.sh。
_ds_stop() {
  _p=$(cat "$DS_PID" 2>/dev/null)
  case "$_p" in
    ''|*[!0-9]*) ;;
    *)
      # 只判数字不够：pid 文件残留几天后这个号可能已经被别的进程回收，
      # 一按"关深睡"就误杀无关进程。和 _ds_alive 用同一个判据。
      if [ -d "/proc/$_p" ] \
         && tr '\0' ' ' < "/proc/$_p/cmdline" 2>/dev/null | grep -q 'deep_sleep\.sh'; then
        kill "$_p" 2>/dev/null
        # 同样按 200ms 探：TERM 后的 unforce 通常几百毫秒内就退完，
        # 按整秒等会让"关开关"白卡一秒多。
        _i=0
        while [ "$_i" -lt 25 ] && [ -d "/proc/$_p" ]; do sleep 0.2; _i=$((_i + 1)); done
        [ -d "/proc/$_p" ] && kill -9 "$_p" 2>/dev/null
      fi
      ;;
  esac
  rm -f "$DS_PID" 2>/dev/null
  [ -f "$DS_SH" ] && ( . "$DS_SH" >/dev/null 2>&1; ds_freeze_restore_if_any; ds_ops_restore_if_any; ds_snap_restore_if_any )
  _ds_state_active_off
  return 0
}

# full=带白名单（页面首次加载）；不带参数只给状态与榜单（2 秒轮询用）。
_ds_dump() {
  _ds_conf_init
  printf 'enable=%s\n' "$(_ds_enable)"
  printf 'alive=%s\n' "$(_ds_alive)"
  printf 'paused=%s\n' "$([ -f "$PAUSE" ] && echo 1 || echo 0)"
  if [ -f "$DS_STATE" ]; then
    grep -E '^(dsactive|wcsince|dsw|dswc)=' "$DS_STATE" 2>/dev/null
    grep -q '^dsactive=' "$DS_STATE" 2>/dev/null || printf 'dsactive=0\n'
  else
    printf 'dsactive=0\nwcsince=0\ndsw=\ndswc=\n'
  fi
  [ "${1:-}" = "full" ] || return 0
  awk '
    /^[[:space:]]*white[[:space:]]*=/ {
      v = $0
      sub(/^[^=]*=/, "", v)
      sub(/#.*/, "", v)
      gsub(/[[:space:]\r]/, "", v)
      if (v != "") printf "white=%s\n", v
    }' "$DS_CONF" 2>/dev/null
  return 0
}

cmd_ds_get() { _ds_dump full; }

cmd_ds_state() { _ds_dump; }

cmd_ds_set() {
  _k=${1:-}; _v=${2:-}
  case "$_k" in
    enable) ;;
    *) die "BAD_KEY" ;;
  esac
  is_switch "$_v" || die "RANGE"
  _ds_conf_init || die "WRITE" 5
  # 三个开关键走同一套写入；只有 enable 需要拉起/停掉守护。
  if grep -q "^[[:space:]]*$_k[[:space:]]*=" "$DS_CONF" 2>/dev/null; then
    awk -v k="$_k" -v v="$_v" '
      $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { print k "=" v; next } { print }' \
      "$DS_CONF" | write_atomic "$DS_CONF" || die "WRITE" 5
  else
    ensure_final_newline "$DS_CONF"
    { cat "$DS_CONF"; printf '%s=%s\n' "$_k" "$_v"; } | write_atomic "$DS_CONF" \
      || die "WRITE" 5
  fi
  [ "$(_ds_switch "$_k")" = "$_v" ] || die "VERIFY" 6
  if [ "$_k" = "enable" ]; then
    # 打开时立即拉起；关闭时直接停掉，避免"刚关又开"时旧的空转进程和新进程抢 pid。
    if [ "$_v" = "1" ]; then
      _ds_start
    else
      _ds_stop
    fi
  fi
  printf 'OK %s=%s\n' "$_k" "$_v"
}

cmd_ds_white_add() {
  _p=${1:-}
  is_package "$_p" || die "BAD_PKG"
  _ds_conf_init || die "WRITE" 5
  if tr -d '\r' < "$DS_CONF" | grep -q -F -x -- "white=$_p"; then
    printf 'DUP %s\n' "$_p"
    return 0
  fi
  ensure_final_newline "$DS_CONF"
  printf 'white=%s\n' "$_p" >> "$DS_CONF" || die "WRITE" 5
  printf 'OK %s\n' "$_p"
}

cmd_ds_white_del() {
  _p=${1:-}
  is_package "$_p" || die "BAD_PKG"
  _ds_conf_init || die "WRITE" 5
  if ! tr -d '\r' < "$DS_CONF" | grep -q -F -x -- "white=$_p"; then
    printf 'MISS %s\n' "$_p"
    return 0
  fi
  tr -d '\r' < "$DS_CONF" | grep -v -F -x -- "white=$_p" \
    | write_atomic "$DS_CONF" || die "WRITE" 5
  # 不在深睡时，系统里的白名单条目不会有人替我们撤，这里补一刀。
  # 深睡中交给守护退出时处理：那时它按当前配置判断这条还算不算用户白名单。
  if [ "$(_ds_state_val dsactive)" != "1" ]; then
    dumpsys deviceidle whitelist "-$_p" >/dev/null 2>&1
  fi
  printf 'OK %s\n' "$_p"
}

# ── 守护控制 ────────────────────────────────────────────────────
# 全部转交守护：拉起/停止/看门狗都在 socfreqd 里，
# 这里不重复实现，免得两边行为不一致。
# 子命令名以 src/socfreqd.c 的 main() 里 strcmp 的那批为准。

cmd_start()   { [ -f "$BIN" ] || die "NO_BIN" 3; "$BIN" restart; _ds_start; }
cmd_stop()    { [ -f "$BIN" ] || die "NO_BIN" 3; "$BIN" stop; _ds_stop; }
cmd_restart() { cmd_start; }
# 暂停是模块级动作：深度睡眠也要先退出并还原，否则暂停期间系统还关在 Doze 里。
cmd_pause()   {
  _ds_stop
  touch "$PAUSE" 2>/dev/null || die "WRITE" 5
  echo "OK paused"
}
cmd_resume()  {
  rm -f "$PAUSE" 2>/dev/null
  # 立刻拉起，不等看门狗。等的话最坏 30 秒，用户会以为没反应。
  # restart 内部也会清 pause，这里先删是双保险。
  cmd_start
}
cmd_ping()    { printf 'PONG\n'; }

cmd_reset_all() {
  # 出厂配置：新装/升级时装机脚本会留一份到 $DATA/defaults；
  # 手动解包/hot-push 没有 defaults 时退回模块目录里的原文件。
  _def_conf="$DATA/defaults/socfreq.conf"
  [ -f "$_def_conf" ] || _def_conf="$ROOT/socfreq.conf"
  [ -f "$_def_conf" ] || die "NO_DEFAULT" 6
  _def_modes="$DATA/defaults/app_modes.conf"
  [ -f "$_def_modes" ] || _def_modes="$ROOT/app_modes.conf"
  _ds_stop
  rm -f "$PAUSE" 2>/dev/null

  # 先写临时文件再 mv：守护每 5 秒按 mtime 重读，直接覆盖可能读到半截配置。
  cp -f "$_def_conf" "$CONF.tmp" 2>/dev/null \
    && mv -f "$CONF.tmp" "$CONF" 2>/dev/null || die "WRITE" 5
  if [ -f "$_def_modes" ]; then
    cp -f "$_def_modes" "$MODES.tmp" 2>/dev/null \
      && mv -f "$MODES.tmp" "$MODES" 2>/dev/null || die "WRITE" 5
  else
    : > "$MODES" 2>/dev/null
  fi

  # 深睡回到默认模板（enable=0），再按本机已安装的包补回内置豁免。
  if [ -f "$DS_SH" ]; then
    ( SOCFREQ_DATA_DIR="$DATA"; export SOCFREQ_DATA_DIR
      . "$DS_SH" >/dev/null 2>&1
      ds_conf_write )
  else
    printf 'enable=0\n' > "$DS_CONF.tmp" 2>/dev/null \
      && mv -f "$DS_CONF.tmp" "$DS_CONF" 2>/dev/null
  fi
  if [ -f "$DS_CONF" ] && [ -f "$ROOT/scripts/builtin_white.txt" ]; then
    pm list packages 2>/dev/null | sed 's/^package://' | tr -d '\r' | sort -u \
      > "$DATA/.rwinst" 2>/dev/null
    sed -n 's/^[[:space:]]*\([A-Za-z0-9_][A-Za-z0-9_.]*\).*/\1/p' \
      "$ROOT/scripts/builtin_white.txt" | sort -u > "$DATA/.rwnew" 2>/dev/null
    while IFS= read -r _p; do
      case "$_p" in ''|*[!A-Za-z0-9._]*) continue ;; esac
      grep -Fqx -- "$_p" "$DATA/.rwinst" 2>/dev/null || continue
      printf 'white=%s\n' "$_p" >> "$DS_CONF" 2>/dev/null
    done < "$DATA/.rwnew"
    rm -f "$DATA/.rwinst" "$DATA/.rwnew" 2>/dev/null
  fi

  rm -f "$OVR" 2>/dev/null
  # 旧版本留下的学习样本；学习模式已移除，恢复默认时一并清掉。
  rm -f "$DATA/learn.tsv" "$DATA/learn_app.tsv" "$DATA/learn.tsv.since" \
        "$DATA/learn_meta.tsv" 2>/dev/null

  [ -f "$BIN" ] && "$BIN" restart >/dev/null 2>&1
  printf 'OK reset_all\n'
}

# ── 分发 ────────────────────────────────────────────────────────
# 命令名与 WebUI 里 api() 的第一个参数一致。

case "${1:-}" in
  ping)       cmd_ping ;;
  status)     st_status ;;
  log)        st_log "${2:-}" ;;
  logs)       shift; cmd_logs "$@" ;;
  log_export) cmd_log_export ;;
  about)      st_about ;;
  conf_get)   cfg_get ;;
  conf_set)   cfg_set "${2:-}" "${3:-}" ;;
  conf_set_many) shift; cmd_conf_set_many "$@" ;;
  # 默认只列"这台机上真装了的"。要用全量预设名单时传 all。
  games_list) [ "${2:-}" = all ] && list_all || list_installed ;;
  games_suggest) games_suggest ;;
  games_add)  list_add "${2:-}" ;;
  games_del)  list_del "${2:-}" ;;
  modes_list) modes_list ;;
  apps_list)  apps_list "${2:-}" ;;
  mode_apps)  mode_apps "${2:-}" ;;
  builtin_white) builtin_white ;;
  builtin_labels) builtin_labels ;;
  labels)     shift; cmd_labels "$@" ;;
  modes_set)  modes_set "${2:-}" "${3:-}" ;;
  modes_del)  modes_del "${2:-}" ;;
  cores)      [ -f "$BIN" ] && "$BIN" cores ;;
  icon)       cmd_icon "${2:-}" ;;
  icons)      shift; cmd_icons "$@" ;;
  reset_all)  cmd_reset_all ;;
  pkg)        cmd_pkg ;;
  pkg_dir)    cmd_pkg_dir ;;
  ovr_show)   cmd_ovr_show ;;
  ovr_clear)  cmd_ovr_clear ;;
  preset_get)   preset_get ;;
  preset_apply) preset_apply ;;
  ds_get)     cmd_ds_get ;;
  ds_state)   cmd_ds_state ;;
  ds_set)     cmd_ds_set "${2:-}" "${3:-}" ;;
  ds_white_add) cmd_ds_white_add "${2:-}" ;;
  ds_white_del) cmd_ds_white_del "${2:-}" ;;
  start)      cmd_start ;;
  stop)       cmd_stop ;;
  restart)    cmd_restart ;;
  pause)      cmd_pause ;;
  resume)     cmd_resume ;;
  *)          echo "E/UNKNOWN" >&2; exit 9 ;;
esac
