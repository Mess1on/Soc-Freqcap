#!/system/bin/sh
# api.sh — 自适配动态限频 WebUI 后端
# 由 WebUI 经 kernelsu.exec 以 root 身份调用：api.sh <cmd> [args...]
# 约定：成功输出数据（JSON / 纯文本 / TSV），失败输出 "ERR: 原因" 到 stderr 并 exit 1

MODDIR=${0%/*}
MODDIR=${MODDIR%/webroot}
CONF="$MODDIR/powerd.conf"
GAMES="$MODDIR/games.txt"
RUN_DIR="$MODDIR/run"
LOG_FILE=$(sed -n 's/^LOG_FILE=//p' "$CONF" 2>/dev/null | head -1)
[ -z "$LOG_FILE" ] && LOG_FILE="$MODDIR/powerd.log"

err() { echo "ERR: $1" >&2; exit 1; }

jesc() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\n\t' '  '
}

valid_pkg() {
  case "$1" in ''|*[!a-zA-Z0-9._]*) return 1 ;; esac
  return 0
}

# 从单包 dumpsys/pm dump 里提取应用名称
get_label() {
  local p="$1" line lab
  line=$(dumpsys package "$p" 2>/dev/null | grep -m1 -i 'application-label')
  if [ -z "$line" ]; then
    line=$(pm dump "$p" 2>/dev/null | grep -m1 -i 'application-label')
  fi
  lab=$(printf '%s' "$line" | sed "s/.*application-label['\" :=]*//" | sed "s/^['\"]//; s/['\"]\$//")
  [ -z "$lab" ] && lab="$p"
  printf '%s' "$lab" | tr -d '\t\n\r' | cut -c1-60
}

# 探测 CPU 簇分级与 GPU（供 WebUI 配置页按实际架构显示/隐藏）
# 分级逻辑与 powerd.sh 保持一致（去重后档位数 n：1→big；2→big+little；
# 3→big+mid+little；>=4→big+mid2+mid+little）
# 输出：{"cpu":[{"key":"big","max":4326000},...],"gpu":{"max":940}|null}
# cpu.max 单位 kHz；gpu.max 单位 MHz（已归一化）；探测不到为 null/空数组
cmd_clusters() {
  local CPUFREQ=/sys/devices/system/cpu/cpufreq
  local p maxf freqs="" f n=0
  for p in "$CPUFREQ"/policy*/cpuinfo_max_freq; do
    [ -f "$p" ] || continue
    maxf=$(cat "$p" 2>/dev/null)
    case "$maxf" in ''|*[!0-9]*) continue ;; esac
    freqs="$freqs $maxf"
  done
  freqs=$(printf '%s\n' $freqs | sort -nu | tr '\n' ' ')
  for f in $freqs; do n=$((n+1)); done

  local cpu_json=""
  if [ "$n" -ge 1 ]; then
    set -- $freqs  # $1..$n 升序
    local f_big f_little f_mid2 f_mid
    eval "f_big=\${$n}"
    f_little=$1
    [ "$n" -ge 4 ] && eval "f_mid2=\${$((n-1))}"
    if [ "$n" -ge 4 ]; then
      eval "f_mid=\${$((n-2))}"
    elif [ "$n" -eq 3 ]; then
      f_mid=$2
    fi
    cpu_json="{\"key\":\"big\",\"max\":$f_big}"
    [ "$n" -ge 4 ] && cpu_json="$cpu_json,{\"key\":\"mid2\",\"max\":$f_mid2}"
    [ "$n" -ge 3 ] && cpu_json="$cpu_json,{\"key\":\"mid\",\"max\":$f_mid}"
    [ "$n" -ge 2 ] && cpu_json="$cpu_json,{\"key\":\"little\",\"max\":$f_little}"
  fi

  local gpu_json="null" node v mhz
  for node in \
    /sys/class/devfreq/3d00000.qcom,kgsl-3d0/max_freq \
    /sys/class/devfreq/5000000.qcom,kgsl-3d0/max_freq \
    /sys/class/devfreq/2c00000.qcom,kgsl-3d0/max_freq \
    /sys/class/kgsl/kgsl-3d0/devfreq/max_freq \
    /sys/class/kgsl/kgsl-3d0/max_gpuclk \
    /sys/class/kgsl/kgsl-3d0/max_clock_mhz \
    /sys/class/devfreq/13000000.mali/max_freq \
    /sys/class/devfreq/mtk-mali/max_freq \
    /sys/class/devfreq/mali0/max_freq \
    /sys/class/devfreq/gpufreq/max_freq \
    /sys/class/misc/mali0/device/devfreq/max_freq \
    /sys/devices/platform/soc/13000000.mali/devfreq/max_freq \
    /sys/devices/platform/13000000.mali/devfreq/max_freq \
    /sys/class/devfreq/48000000.mali/max_freq \
    /sys/kernel/gpu/gpu_max_clock; do
    [ -f "$node" ] || continue
    v=$(cat "$node" 2>/dev/null)
    case "$v" in ''|*[!0-9]*) continue ;; esac
    if [ "$v" -ge 100000000 ]; then mhz=$((v/1000000))
    elif [ "$v" -ge 100000 ]; then mhz=$((v/1000))
    else mhz=$v
    fi
    gpu_json="{\"max\":$mhz}"
    break
  done
  printf '{"cpu":[%s],"gpu":%s}\n' "$cpu_json" "$gpu_json"
}

cmd_ping() { echo OK; }

cmd_status() {
  local state pid alive=0 paused=0 game_pkg gpu last soc
  state=$(cat "$MODDIR/state" 2>/dev/null)
  [ -z "$state" ] && state=unknown
  pid=$(cat "$MODDIR/powerd.lock/pid" 2>/dev/null)
  case "$pid" in ''|*[!0-9]*) pid="" ;; *) [ -d "/proc/$pid" ] && alive=1 ;; esac
  [ -f "$MODDIR/pause" ] && paused=1
  if [ -f "$RUN_DIR/status" ]; then
    game_pkg=$(sed -n 's/^game_pkg=//p' "$RUN_DIR/status" 2>/dev/null | head -1)
    gpu=$(sed -n 's/^gpu_result=//p' "$RUN_DIR/status" 2>/dev/null | head -1)
    last=$(sed -n 's/^last_seen=//p' "$RUN_DIR/status" 2>/dev/null | head -1)
  fi
  soc=$(getprop ro.soc.model 2>/dev/null)
  [ -z "$soc" ] && soc=$(getprop ro.board.platform 2>/dev/null)
  printf '{"state":"%s","alive":%d,"paused":%d,"pid":"%s","game_pkg":"%s","gpu_result":"%s","last_seen":"%s","soc":"%s"}\n' \
    "$(jesc "$state")" "$alive" "$paused" "$(jesc "$pid")" "$(jesc "$game_pkg")" \
    "$(jesc "$gpu")" "$(jesc "$last")" "$(jesc "$soc")"
}

cmd_log() {
  local n="$1" lf="$LOG_FILE"
  case "$n" in ''|*[!0-9]*) n=200 ;; esac
  [ "$n" -gt 1000 ] && n=1000
  [ -f "$lf" ] || lf="$MODDIR/powerd.log"
  [ -f "$lf" ] && tail -n "$n" "$lf" 2>/dev/null
  return 0
}

CONF_KEYS="INTERVAL GAME_EXIT_GRACE_SEC FENGCHI_DETECT_SEC CPU_PCT_BIG CPU_PCT_MID2 CPU_PCT_MID CPU_PCT_LITTLE GPU_PCT GPU_BOOST_CONTROL CPU_GOVERNOR LOG_FILE"

# 配置自愈：补全缺失的 key（默认值与 powerd.sh 一致）。
# 老版本升级上来的 powerd.conf 可能缺少新 key（如 FENGCHI_DETECT_SEC），
# 守护进程只在内存里给默认值，不会写回，导致 WebUI 读出空值。
ensure_conf_keys() {
  [ -f "$CONF" ] || return 0
  local k v
  for k in $CONF_KEYS; do
    grep -q "^$k=" "$CONF" 2>/dev/null && continue
    case "$k" in
      INTERVAL) v=5 ;;
      GAME_EXIT_GRACE_SEC) v=30 ;;
      FENGCHI_DETECT_SEC) v=1 ;;
      CPU_PCT_BIG) v=58 ;;
      CPU_PCT_MID2) v=60 ;;
      CPU_PCT_MID) v=62 ;;
      CPU_PCT_LITTLE) v=65 ;;
      GPU_PCT) v=60 ;;
      GPU_BOOST_CONTROL) v=0 ;;
      CPU_GOVERNOR) v= ;;
      LOG_FILE) v="$MODDIR/powerd.log" ;;
    esac
    printf '%s=%s\n' "$k" "$v" >> "$CONF"
  done
  chmod 644 "$CONF" 2>/dev/null
}

cmd_conf_get() {
  ensure_conf_keys
  local k v first=1
  printf '{'
  for k in $CONF_KEYS; do
    v=$(sed -n "s/^$k=//p" "$CONF" 2>/dev/null | head -1)
    v=$(printf '%s' "$v" | sed 's/[[:space:]]*#.*$//')  # 去行尾注释
    case "$v" in '"'*'"') v=$(printf '%s' "$v" | sed 's/^"//; s/"$//') ;; esac
    [ "$first" = 1 ] || printf ','
    first=0
    printf '"%s":"%s"' "$k" "$(jesc "$v")"
  done
  printf '}\n'
}

cmd_conf_set() {
  ensure_conf_keys
  local k="$1" v="$2" num=0 min=0 max=0
  case "$k" in
    INTERVAL) num=1; min=2; max=60 ;;
    GAME_EXIT_GRACE_SEC) num=1; min=5; max=300 ;;
    FENGCHI_DETECT_SEC) num=1; min=0; max=10 ;;
    CPU_PCT_BIG|CPU_PCT_MID2|CPU_PCT_MID|CPU_PCT_LITTLE|GPU_PCT) num=1; min=1; max=100 ;;
    GPU_BOOST_CONTROL) case "$v" in 0|1) ;; *) err "值非法（0/1）" ;; esac ;;
    CPU_GOVERNOR) ;;
    LOG_FILE) case "$v" in /*) ;; *) err "路径必须以 / 开头" ;; esac ;;
    *) err "未知参数" ;;
  esac
  if [ "$num" = "1" ]; then
    case "$v" in ''|*[!0-9]*) err "需要数字" ;; esac
    [ "$v" -lt "$min" ] && v=$min
    [ "$v" -gt "$max" ] && v=$max
  fi
  v=$(printf '%s' "$v" | tr -d '|&\\' | tr -d '\n' | tr -d "'" | tr -d '"')
  [ -f "$CONF" ] || err "配置文件不存在"
  if [ "$k" = "CPU_GOVERNOR" ]; then
    sed -i "s|^$k=.*|$k=\"$v\"|" "$CONF"
  else
    sed -i "s|^$k=.*|$k=$v|" "$CONF"
  fi
  grep -q "^$k=" "$CONF" 2>/dev/null || err "写入失败"
  echo OK
}

cmd_games_list() {
  [ -f "$GAMES" ] || return 0
  grep -v -e '^#' -e '^[[:space:]]*$' "$GAMES" 2>/dev/null | tr -d '\r' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | sort -u
  return 0
}

cmd_games_add() {
  valid_pkg "$1" || err "包名非法"
  touch "$GAMES" 2>/dev/null || err "无法写入名单"
  grep -q -F -x -- "$1" "$GAMES" 2>/dev/null || printf '%s\n' "$1" >> "$GAMES"
  echo OK
}

cmd_games_del() {
  valid_pkg "$1" || err "包名非法"
  if [ -f "$GAMES" ]; then
    grep -v -F -x -- "$1" "$GAMES" > "$GAMES.tmp" 2>/dev/null && mv "$GAMES.tmp" "$GAMES"
  fi
  echo OK
}

# 一次 dumpsys 全量解析所有应用 包名 -> 名称（TSV），前端缓存后做即时搜索
# 无名称的应用以包名作为名称输出
cmd_apps_all() {
  dumpsys package 2>/dev/null | awk '
    /Package \[/ {
      if (pkg != "") print pkg "\t" lastlab;
      pkg=$0; sub(/^.*Package \[/, "", pkg); sub(/\].*$/, "", pkg);
      lastlab=pkg; next
    }
    tolower($0) ~ /application-label/ {
      lab=$0; sub(/^.*[Aa]pplication-[Ll]abel['"'"' :=]*/, "", lab);
      gsub(/^['"'"']|['"'"']$/, "", lab);
      gsub(/\t/, " ", lab);
      if (lab != "") lastlab=lab;
    }
    END { if (pkg != "") print pkg "\t" lastlab }'
  return 0
}

# 取单个应用图标（APK 内最高密度 launcher 光栅图标），输出 data URI，取不到输出空
cmd_app_icon() {
  local p="$1" apk entry dens mime b64 listing
  valid_pkg "$p" || return 1
  command -v unzip >/dev/null 2>&1 || return 1
  apk=$(pm path "$p" 2>/dev/null | head -1 | sed 's/^package://')
  [ -n "$apk" ] && [ -f "$apk" ] || return 1
  # APK 内全部光栅图片条目（条目名可能含空格，取第 4 列之后全部）
  listing=$(unzip -l "$apk" 2>/dev/null | grep -iE '\.(png|webp|jpg|jpeg)$' | sed -E 's/^ *[^ ]+ +[^ ]+ +[^ ]+ +//')
  [ -n "$listing" ] || return 1
  # 1) 最高密度的 mipmap launcher 图标（优先非 round）
  for dens in xxxhdpi xxhdpi xhdpi hdpi mdpi; do
    entry=$(printf '%s\n' "$listing" | grep -i "mipmap-${dens}" | grep -iE 'launcher|icon' | grep -vi 'round' | head -1)
    [ -n "$entry" ] && break
    entry=$(printf '%s\n' "$listing" | grep -i "mipmap-${dens}" | grep -iE 'launcher|icon' | head -1)
    [ -n "$entry" ] && break
  done
  # 2) 任意 mipmap/drawable 下的 launcher/icon
  if [ -z "$entry" ]; then
    entry=$(printf '%s\n' "$listing" | grep -iE 'mipmap|drawable' | grep -iE 'launcher|icon' | grep -vi 'round' | head -1)
  fi
  if [ -z "$entry" ]; then
    entry=$(printf '%s\n' "$listing" | grep -iE 'mipmap|drawable' | grep -iE 'launcher|icon' | head -1)
  fi
  # 3) 自适应图标兜底：只有 anydpi-v26 XML 时，取约定命名的背景光栅
  if [ -z "$entry" ]; then
    entry=$(printf '%s\n' "$listing" | grep -iE 'ic_launcher_background\.(png|webp|jpg|jpeg)$' | head -1)
  fi
  [ -n "$entry" ] || return 1
  case "$entry" in
    *.webp|*.WEBP) mime="image/webp" ;;
    *.jpg|*.jpeg|*.JPG|*.JPEG) mime="image/jpeg" ;;
    *) mime="image/png" ;;
  esac
  b64=$(unzip -p "$apk" "$entry" 2>/dev/null | base64 2>/dev/null | tr -d '\n')
  [ -n "$b64" ] && printf 'data:%s;base64,%s' "$mime" "$b64"
  return 0
}

# ─────────────────────────────────────────────────────────────
# 关于 cmd_app_icon 的取舍（本项目不采用他的做法，理由记在这里）
#
# Ktwo 的选图顺序（api.sh:244-281）：
#   unzip -l 列出全部光栅条目
#   → 按 mipmap-密度从 xxxhdpi 往下试 launcher|icon，排除 round
#   → 退回任意 mipmap|drawable 下的 launcher|icon
#   → 再退回 ic_launcher_background.(png|webp|jpg|jpeg)
#   → unzip -p 抽出来，base64
#
# 为什么本项目不照抄：
# 1) **Android 的 toybox 里没有 unzip**。
#    Ktwo 自己第 248 行就写了 `command -v unzip || return 1`——
#    也就是"没有 unzip 就直接放弃"。设备上装了 busybox 才有，
#    没装就是静默留色块。所以这不是可靠路径，只是他那边恰好能跑。
# 2) 名字匹配对**资源路径缩短**的正式包完全失效：
#    res/mipmap-xxhdpi/ic_launcher.png 变成 res/aK.png，
#    上面四条规则一条都不成立。他没有兜底档，那些包他也是色块。
# 3) unzip -l | grep | sed 是几次进程调用 + 两次全量文本扫描，
#    名单滚动时每个包都跑一遍。守护自己读中央目录只 pread 尾部。
#
# 本项目：守护自己解 zip 中央目录（src/icon.c），
# 先按名字打 35~45 分（密度 + launcher/icon 加权 - round/背景减权）；
# 一张都选不出来时，才用"只看 res/ 下的正方形图、边长最接近 192"
# 这个分数 1~9 的兜底档——它压在按名字那档之下，
# 保证名字还在的时候永远不会被兜底顶掉。
#
# 对同一个"资源名没被缩短"的 APK 实测，两边选中的是同一张：
#   res/mipmap-xxhdpi/ic_launcher.png
# 差别只在名字被缩短的包：本项目有兜底，Ktwo 直接 return 1。
# ─────────────────────────────────────────────────────────────

cmd_restart() {
  local pid alive=0
  rm -f "$MODDIR/pause"
  pid=$(cat "$MODDIR/powerd.lock/pid" 2>/dev/null)
  case "$pid" in ''|*[!0-9]*) ;; *) [ -d "/proc/$pid" ] && alive=1 ;; esac
  [ "$alive" = "1" ] && kill "$pid" 2>/dev/null
  sleep 2
  # 看门狗 30s 内也会拉起；这里立即拉起一次（acquire_lock 防多实例）
  nohup /system/bin/sh "$MODDIR/powerd.sh" start >/dev/null 2>&1 &
  echo OK
}

cmd_about() {
  echo "---PROP---"
  cat "$MODDIR/module.prop" 2>/dev/null
  echo "---CHANGELOG---"
  cat "$MODDIR/updatelog.txt" 2>/dev/null
  return 0
}

case "$1" in
  ping) cmd_ping ;;
  status) cmd_status ;;
  log) cmd_log "$2" ;;
  conf_get) cmd_conf_get ;;
  conf_set) cmd_conf_set "$2" "$3" ;;
  games_list) cmd_games_list ;;
  games_add) cmd_games_add "$2" ;;
  games_del) cmd_games_del "$2" ;;
  apps_all) cmd_apps_all ;;
  app_icon) cmd_app_icon "$2" ;;
  clusters) cmd_clusters ;;
  restart) cmd_restart ;;
  about) cmd_about ;;
  *) echo "ERR: 未知命令" >&2; exit 1 ;;
esac
