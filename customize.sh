#!/system/bin/sh
# Soc智能限频 · 安装脚本

SKIPUNZIP=0

# ui_print 由管理器提供；缺失时退化为 echo，避免调用报错淹没安装输出。
command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$@"; }

# ── 冲突检测 ────────────────────────────────────────────────────
# 同时启用多个改 CPU 频率/调速器的模块会互相覆盖来回拉扯。
# 这里只检测并提示，不修改、不删除、不标记任何其他模块，是否共存由用户决定。

conflict_found() {
  local name="$1"
  if [ -d "/data/adb/modules/$name" ] || [ -d "/data/adb/modules_update/$name" ]; then
    # 输出不带前缀标记，标记由管理器统一添加。
    ui_print "  检测到 $name"
    return 0
  fi
  return 1
}

ui_print " "
ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print "  冲突检测"
ui_print "━━━━━━━━━━━━━━━━━━━━"
CONFLICT=0
for M in Scene_Config_replace Scene5 modules_thermal scene; do
  conflict_found "$M" && {
    ui_print "  本模块与它同时改 CPU 频率，请先卸载其中一个"
    CONFLICT=1
  }
done

# 第三方调度框架的总开关记录在应用私有配置里，模块目录改名也查不到，需单独检测
SCENE_PREFS="/data/data/com.omarea.vtools/shared_prefs/global.xml"
if [ -f "$SCENE_PREFS" ] && grep -q '"dynamic_control" value="true"' "$SCENE_PREFS" 2>/dev/null; then
  ui_print "  Scene 调度总开关处于开启状态"
  CONFLICT=1
fi

[ "$CONFLICT" = "0" ] && ui_print "  未检测到冲突"
ui_print " "

# ── 环境自检 ────────────────────────────────────────────────────
if [ ! -d /sys/devices/system/cpu/cpufreq ]; then
  ui_print "  未找到 cpufreq 目录，本机内核不支持 CPU 限频"
  abort
fi
if ! ls /sys/devices/system/cpu/cpufreq/policy*/cpuinfo_max_freq >/dev/null 2>&1; then
  ui_print "  未找到任何 policy，本机内核不支持 CPU 限频"
  abort
fi

# ── 提示 ────────────────────────────────────────────────────────
MODEL="$(getprop ro.product.model 2>/dev/null)"
# 芯片代号统一转大写（原厂属性为小写），与机型名的显示风格一致。
SOC="$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')"
[ -z "$SOC" ] && SOC="$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')"

ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print "  Soc智能限频"
ui_print "  $MODEL · $SOC"
ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print ""
ui_print "  常态：按 socfreq.conf 里的百分比封顶各簇与 GPU"
ui_print "  游戏：检测到前台游戏自动放开并交还系统调度"
ui_print ""
ui_print "  配置：/data/adb/soc_freqcap/socfreq.conf"
ui_print "  名单：/data/adb/soc_freqcap/games.txt"
ui_print "  日志：/data/adb/soc_freqcap/socfreqd.log"
ui_print "  深度睡眠默认关闭，在 WebUI 的「睡眠」页打开"
ui_print "  （都在模块目录之外，升级模块不会覆盖你的改动）"
ui_print ""
ui_print "  重启后自动生效"
ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print " "

# ── 权限 ────────────────────────────────────────────────────────
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/socfreqd"       0 0 0755
set_perm "$MODPATH/service.sh"     0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh"   0 0 0755
set_perm "$MODPATH/scripts/deep_sleep.sh" 0 0 0755
set_perm "$MODPATH/scripts/hotinstall.sh" 0 0 0755

# 配置文件用户可写，方便自己改
set_perm "$MODPATH/socfreq.conf" 0 0 0644
set_perm "$MODPATH/games.txt"    0 0 0644

# WebUI：index.html 只需读；api.sh 必须可执行。
# set_perm_recursive 已经把整个目录刷成 0644，这里要显式补回 0755，
# 否则不同管理器的 WebUI 直接 ./api.sh 调用会 permission denied。
set_perm "$MODPATH/webroot/index.html" 0 0 0644
set_perm "$MODPATH/webroot/api.sh"     0 0 0755

# 名单统一成 LF：CRLF 行尾的 \r 会让 api.sh 的 grep -x 整行比对全部失配且不报错。
# 包内名单是 LF，但用户可能用 Windows 编辑器改过再推回来。
if [ -f "$MODPATH/games.txt" ]; then
  if tr -d '\r' < "$MODPATH/games.txt" > "$MODPATH/games.txt.lf" 2>/dev/null; then
    mv -f "$MODPATH/games.txt.lf" "$MODPATH/games.txt"
    chmod 0644 "$MODPATH/games.txt"
  else
    rm -f "$MODPATH/games.txt.lf"
  fi
fi

# ── 可变状态搬到模块目录之外 ────────────────────────────────────
# 管理器升级模块的做法是"先删 /data/adb/modules/<id> 再解包"，
# 配置和名单留在模块目录里会在每次升级时被包内默认值覆盖。
# /data/adb/soc_freqcap 与之同级但在升级范围之外，是 root 私有的普通目录。
#
# 只在新位置不存在时才搬运，避免覆盖用户已改过的配置。
DATA=/data/adb/soc_freqcap
mkdir -p "$DATA/run" 2>/dev/null && ui_print "  配置目录：$DATA"

# ── 按芯片套预设档位（单一来源：adapt/*.conf 的 default_*）──────
# 芯片默认值和节点适配表放在同一份 adapt/<soc>.conf 里，装机脚本不再写死第二套。
# 只对全新安装生效；升级一律不碰用户配置（用户值可能刚好等于默认值，
# 不能用"值等于默认"的启发式去覆盖）。
#
# 匹配规则与节点适配表一致：文件里每条 #match 都命中才算命中，
# brand= / model= / soc= 三种键；没有 #match 的文件不参与。
_adapt_match() {
  ADAPT_FILE=
  [ -d "$MODPATH/adapt" ] || return 0
  _b=$(getprop ro.product.brand 2>/dev/null | tr 'a-z' 'A-Z')
  _m=$(getprop ro.product.model 2>/dev/null | tr 'a-z' 'A-Z')
  _s=$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')
  [ -z "$_s" ] && _s=$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')
  for _f in "$MODPATH"/adapt/*.conf; do
    [ -f "$_f" ] || continue
    _ml=$(grep '^#match ' "$_f" 2>/dev/null)
    [ -n "$_ml" ] || continue
    _hit=1
    _oifs=$IFS; IFS='
'
    for _l in $_ml; do
      case "$_l" in
        *model=*) [ "$_m" = "$(printf '%s' "${_l#*model=}" | tr 'a-z' 'A-Z')" ] || _hit=0 ;;
        *brand=*) [ "$_b" = "$(printf '%s' "${_l#*brand=}" | tr 'a-z' 'A-Z')" ] || _hit=0 ;;
        *soc=*)   case "$_s" in *"$(printf '%s' "${_l#*soc=}" | tr 'a-z' 'A-Z')"*) : ;; *) _hit=0 ;; esac ;;
        *) _hit=0 ;;
      esac
    done
    IFS=$_oifs
    [ "$_hit" = 1 ] || continue
    ADAPT_FILE="$_f"
    return 0
  done
}

# 全新安装时把命中文件里的 default_* 套进用户配置。只认白名单键，
# 数字/范围都校验；文件里打错一个字符最多是"这条不套"，不会写坏配置。
_soc_defaults() {
  [ -n "$ADAPT_FILE" ] && [ -f "$ADAPT_FILE" ] || return 0
  [ -f "$1" ] || return 0
  _sed=""; _n=0; _msg=""
  while IFS='=' read -r _k _v; do
    case "$_k" in default_*) ;; *) continue ;; esac
    case "$_v" in ''|*[!0-9]*) continue ;; esac
    case "$_k" in
      default_cpu_pct_big)    _key=CPU_PCT_BIG;    _lo=1;  _hi=100 ;;
      default_cpu_pct_little) _key=CPU_PCT_LITTLE; _lo=1;  _hi=100 ;;
      default_gpu_pct)        _key=GPU_PCT;        _lo=1;  _hi=100 ;;
      default_th_high)        _key=TH_HIGH;        _lo=35; _hi=110 ;;
      default_th_low)         _key=TH_LOW;         _lo=30; _hi=105 ;;
      default_psi_gain)       _key=PSI_GAIN;       _lo=0;  _hi=30 ;;
      *) continue ;;
    esac
    [ "$_v" -ge "$_lo" ] && [ "$_v" -le "$_hi" ] || continue
    grep -q "^$_key=" "$1" 2>/dev/null || continue
    _sed="$_sed -e s/^$_key=.*/$_key=$_v/"
    _n=$((_n + 1))
    _msg="$_msg $_key=$_v"
  done < "$ADAPT_FILE"
  [ "$_n" -gt 0 ] || return 0
  if eval "sed $_sed \"\$1\"" > "$1.pz" 2>/dev/null; then
    mv -f "$1.pz" "$1" 2>/dev/null && chmod 0644 "$1" 2>/dev/null
    ui_print "  芯片默认（${ADAPT_FILE##*/}）：$_msg"
  else
    rm -f "$1.pz" 2>/dev/null
  fi
}

# 出厂默认留一份：模块目录里的 socfreq.conf 随后会被搬到数据目录/删掉，
# 页面上的「恢复全部默认」需要一份不会被用户改到的原件。
mkdir -p "$DATA/defaults" 2>/dev/null
for _f in socfreq.conf app_modes.conf; do
  [ -f "$MODPATH/$_f" ] && cp -f "$MODPATH/$_f" "$DATA/defaults/$_f" 2>/dev/null
done
chmod 0644 "$DATA/defaults"/*.conf 2>/dev/null

# 数据目录已有配置视为升级
_FRESH_CONF=0
[ -f "$DATA/socfreq.conf" ] || _FRESH_CONF=1

for _f in socfreq.conf app_modes.conf vendors.txt socfreqd.log; do
  if [ -f "$MODPATH/$_f" ] && [ ! -f "$DATA/$_f" ]; then
    mv -f "$MODPATH/$_f" "$DATA/$_f" 2>/dev/null
  fi
done
_adapt_match
if [ "$_FRESH_CONF" = 1 ]; then
  _soc_defaults "$DATA/socfreq.conf"
fi

# ── 机型适配表（adapt/）────────────────────────────────────────
# 有些机器的节点长得不在预期路径上：一个 policy* 目录都读不到、
# 或找不到可写的 GPU 上限节点。这种机器由作者照着用户发来的适配包改好一张表，
# 放进包里的 adapt/ 随版本发布；装机时按 #match 认机器，命中就装进数据目录
# 当 override.conf。守护只在自动探测扑空时用它，所以正常机器完全不受影响。
#
# 三条规矩：
#   1) 文件里每条 #match 都要命中才算命中（brand= / model= / soc= 三种键）；
#      一条 #match 都没有的文件不装 —— 没有条件就等于对所有机器适用，太危险。
#   2) 只认这四种键开头的行，别的一律不带进数据目录（打错前缀的行不会被静默装上）。
#   3) 没命中任何文件时，只撤掉"上一版由 adapt 装进去的那份"（认 #from adapt/ 标记）。
#      作者把某台机的适配下架了，用户升级后就该回到自动探测；
#      而用户自己用命令行导进去的表没有这个标记，不该被装机删掉。
_soc_adapt() {
  [ -d "$MODPATH/adapt" ] || return 0
  if [ -n "$ADAPT_FILE" ] && [ -f "$ADAPT_FILE" ]; then
    _body=$(grep -E '^(cpu-dir|gpu-max|gpu-min|gpu-freqs) ' "$ADAPT_FILE" 2>/dev/null)
    if [ -n "$_body" ]; then
      _n=$(printf '%s\n' "$_body" | grep -c .)
      { printf '#from adapt/%s\n' "${ADAPT_FILE##*/}"; printf '%s\n' "$_body"; } \
        > "$DATA/override.conf" 2>/dev/null
      chmod 0644 "$DATA/override.conf" 2>/dev/null
      ui_print "  命中适配表 ${ADAPT_FILE##*/}：$_n 条已装进数据目录（重启后生效）"
      return 0
    fi
    ui_print "  命中 ${ADAPT_FILE##*/}：只有芯片默认，没有节点表"
  fi
  # 没命中 / 命中文件没有节点表：只撤 adapt 自己装过的那份
  if head -n 1 "$DATA/override.conf" 2>/dev/null | grep -q '^#from adapt/'; then
    rm -f "$DATA/override.conf" 2>/dev/null
    ui_print "  本次包里没有匹配这台机的节点表，已撤掉上一版装进去的那份（回到自动探测）"
  fi
}
_soc_adapt

# 追加写入前保证目标文件以换行结尾：末行无换行时 >> 会把新行与末行粘成一行
# （如 CPU_GOVERNOR=""SCREEN_UNCAP=0、com.a.gamecom.b.game），
# 键和包名看似都在但 grep 一条都匹配不上，所以所有追加写入前都过一遍这个函数。
ensure_final_newline() {
  [ -s "$1" ] || return 0
  if [ -n "$(tail -c 1 "$1" 2>/dev/null | tr -d '\n')" ]; then
    printf '\n' >> "$1" 2>/dev/null
  fi
}

# ── 配置：补入新增的键 ──────────────────────────────────────────
# 数据目录已有 conf 时不整体覆盖，避免用户改过的参数被打回默认。
# 包内新增的键若用户那份里没有，页面上会多出没有对应行的开关，
# 因此只追加用户那份里不存在的键整行，已存在的键一个都不碰。
if [ -f "$DATA/socfreq.conf" ] && [ -f "$MODPATH/socfreq.conf" ]; then
  _added=0
  ensure_final_newline "$DATA/socfreq.conf"
  while read -r _line; do
    case "$_line" in ''|\#*) continue ;; esac
    case "$_line" in *=*) ;; *) continue ;; esac
    _k=${_line%%=*}
    # 熄屏策略是行为开关：老用户没有这个键时应保持旧行为，
    # 不能靠升级补成 uncap。新装用户拿包内 conf，默认就是 uncap。
    [ "$_k" = SCREEN_POLICY ] && continue
    grep -q "^${_k}=" "$DATA/socfreq.conf" 2>/dev/null && continue
    printf '%s\n' "$_line" >> "$DATA/socfreq.conf" 2>/dev/null && _added=$((_added + 1))
  done < "$MODPATH/socfreq.conf"
  [ "$_added" -gt 0 ] && ui_print "  配置补齐新增项：$_added 条"
  # 删掉模块目录内原件：并存两份配置时改模块目录那份不会有任何效果
  rm -f "$MODPATH/socfreq.conf" 2>/dev/null
fi

# ── 名单：升级时把精选名单的新条目合进来 ────────────────────────
# 三方合并，基准是上次装机时包内的名单（games.base.txt）：
#   包内比基准多的 → 补进用户名单
#   基准比包内多的 → 从用户名单移除（上游下架的条目）
#   用户自己加的（基准和包内都没有）→ 不在两个差集里，不动
#   用户自己删的（包内和基准都还有）→ 不算新增，不会被塞回去
# 没有基准的旧装机只补新增、不删条目：此时无法区分用户新增和上游删除。
G_NEW="$MODPATH/games.txt"
G_USER="$DATA/games.txt"
G_BASE="$DATA/games.base.txt"
if [ -f "$G_NEW" ]; then
  if [ ! -f "$G_USER" ]; then
    mv -f "$G_NEW" "$G_USER" 2>/dev/null
    cp -f "$G_USER" "$G_BASE" 2>/dev/null
  else
    # 注释行和空行不参与比对：它们不是包名，进差集只会把整段注释重复追加。
    _pk() { grep -Ev '^[[:space:]]*(#|$)' "$1" 2>/dev/null; }
    _add="$DATA/.gadd" _del="$DATA/.gdel" _tmp="$DATA/.gtmp"
    if [ -f "$G_BASE" ]; then
      _pk "$G_BASE" > "$DATA/.gb"
      _pk "$G_NEW"  > "$DATA/.gn"
      grep -Fxv -f "$DATA/.gb" "$DATA/.gn" > "$_add" 2>/dev/null
      grep -Fxv -f "$DATA/.gn" "$DATA/.gb" > "$_del" 2>/dev/null
    else
      _pk "$G_USER" > "$DATA/.gu"
      _pk "$G_NEW"  > "$DATA/.gn"
      grep -Fxv -f "$DATA/.gu" "$DATA/.gn" > "$_add" 2>/dev/null
      : > "$_del"
      ui_print "  名单没有升级基准，本次只补新增、不删条目"
    fi
    _na=0 _nd=0
    ensure_final_newline "$G_USER"
    while read -r _p; do
      case "$_p" in ''|*[!-a-zA-Z0-9._]*) continue ;; esac
      grep -Fxq -- "$_p" "$G_USER" 2>/dev/null && continue
      printf '%s\n' "$_p" >> "$G_USER" 2>/dev/null && _na=$((_na + 1))
    done < "$_add"
    while read -r _p; do
      case "$_p" in ''|*[!-a-zA-Z0-9._]*) continue ;; esac
      grep -Fxv -- "$_p" "$G_USER" > "$_tmp" 2>/dev/null && mv -f "$_tmp" "$G_USER" 2>/dev/null
      _nd=$((_nd + 1))
    done < "$_del"
    # 基准更新为本轮包内名单，下次升级按它做差。
    cp -f "$G_NEW" "$G_BASE" 2>/dev/null
    rm -f "$G_NEW" "$_add" "$_del" "$DATA/.gb" "$DATA/.gn" "$DATA/.gu" 2>/dev/null
    [ "$_na" -gt 0 ] || [ "$_nd" -gt 0 ] \
      && ui_print "  名单已合并：补入 $_na 条、移除 $_nd 条"
    chmod 0644 "$G_USER" "$G_BASE" 2>/dev/null
  fi
fi

# ── 内置满血清单：相机类 ────────────────────────────────────────
# 只给**本机已安装**的包追加 `pkg=full  #builtin`，用户已有任意一行就跳过。
# 升级只补"上一版清单里没有"的包：基准存 builtin_modes.base.txt，
# 用户删掉的内置项在下次升级不会被塞回来（它在基准里，不属于新增）。
B_NEW="$MODPATH/scripts/builtin_modes.txt"
B_BASE="$DATA/builtin_modes.base.txt"
if [ -f "$B_NEW" ] && [ -f "$DATA/app_modes.conf" ]; then
  # pm 拿不到已安装列表时整段跳过，并且**不推进基准**：
  # 宁可这次不加，也别让这些相机以后永远补不进来。
  pm list packages 2>/dev/null | sed 's/^package://' | tr -d '\r' \
    | grep -v '^$' | sort -u > "$DATA/.binst" 2>/dev/null
  if [ -s "$DATA/.binst" ]; then
    sed -n 's/^[[:space:]]*\([A-Za-z0-9_.]*\)[[:space:]]*=.*/\1/p' "$B_NEW" \
      | grep -v '^$' | sort -u > "$DATA/.bnew" 2>/dev/null
    if [ -f "$B_BASE" ]; then
      sed -n 's/^[[:space:]]*\([A-Za-z0-9_.]*\)[[:space:]]*=.*/\1/p' "$B_BASE" \
        | grep -v '^$' | sort -u > "$DATA/.bold" 2>/dev/null
    else
      : > "$DATA/.bold"
    fi
    grep -Fxv -f "$DATA/.bold" "$DATA/.bnew" > "$DATA/.badd" 2>/dev/null || : > "$DATA/.badd"
    _nb=0
    ensure_final_newline "$DATA/app_modes.conf"
    while read -r _p; do
      case "$_p" in ''|*[!A-Za-z0-9._]*) continue ;; esac
      grep -Fxq -- "$_p" "$DATA/.binst" 2>/dev/null || continue
      # 用户已经给这个包设过任何值（甚至手写过 80）就不要动
      awk -v k="$_p" 'index($0, k "=") == 1 { f = 1 } END { exit !f }' \
        "$DATA/app_modes.conf" && continue
      printf '%s=full  #builtin\n' "$_p" >> "$DATA/app_modes.conf" 2>/dev/null \
        && _nb=$((_nb + 1))
    done < "$DATA/.badd"
    # 基准 = 本轮整份清单；下次升级按它算"新增了哪些相机包"。
    cp -f "$B_NEW" "$B_BASE" 2>/dev/null
    [ "$_nb" -gt 0 ] && ui_print "  相机类应用已设为满血：$_nb 个（页面里可改/删）"
  fi
  rm -f "$DATA/.binst" "$DATA/.bnew" "$DATA/.bold" "$DATA/.badd" 2>/dev/null
fi

# ── 内置深睡永久豁免：常用通讯 / 运动健康 ───────────────────────
# 深睡会强制进 Doze；通讯和健康应用被压住就是"锁屏收不到消息/步数不动"。
# 只给本机已安装的包写 white=；用户删掉的不会被下次升级塞回来
# （基准 builtin_white.base.txt 只补"新版清单新增"的包）。
W_NEW="$MODPATH/scripts/builtin_white.txt"
W_BASE="$DATA/builtin_white.base.txt"
W_CONF="$DATA/dsleep.conf"
if [ -f "$W_NEW" ]; then
  # 没有 dsleep.conf 时先按深睡脚本自己的模板建一份，别在装机脚本里另抄格式。
  if [ ! -f "$W_CONF" ] && [ -f "$MODPATH/scripts/deep_sleep.sh" ]; then
    ( SOCFREQ_DATA_DIR="$DATA"; export SOCFREQ_DATA_DIR
      . "$MODPATH/scripts/deep_sleep.sh"
      ds_conf_write ) >/dev/null 2>&1
  fi
  if [ -f "$W_CONF" ]; then
    pm list packages 2>/dev/null | sed 's/^package://' | tr -d '\r' \
      | grep -v '^$' | sort -u > "$DATA/.winst" 2>/dev/null
    if [ -s "$DATA/.winst" ]; then
      sed -n 's/^[[:space:]]*\([A-Za-z0-9_][A-Za-z0-9_.]*\).*/\1/p' "$W_NEW" \
        | grep -v '^$' | sort -u > "$DATA/.wnew" 2>/dev/null
      if [ -f "$W_BASE" ]; then
        sed -n 's/^[[:space:]]*\([A-Za-z0-9_][A-Za-z0-9_.]*\).*/\1/p' "$W_BASE" \
          | grep -v '^$' | sort -u > "$DATA/.wold" 2>/dev/null
      else
        : > "$DATA/.wold"
      fi
      grep -Fxv -f "$DATA/.wold" "$DATA/.wnew" > "$DATA/.wadd" 2>/dev/null || : > "$DATA/.wadd"
      # 上游清单删掉的包：只撤 white= 整行（用户自己填的其它值不碰）。
      grep -Fxv -f "$DATA/.wnew" "$DATA/.wold" > "$DATA/.wdel" 2>/dev/null || : > "$DATA/.wdel"
      _nr=0
      while read -r _p; do
        case "$_p" in ''|*[!A-Za-z0-9._]*) continue ;; esac
        grep -Fxq -- "white=$_p" "$W_CONF" 2>/dev/null || continue
        grep -Fxv -- "white=$_p" "$W_CONF" > "$W_CONF.rm" 2>/dev/null \
          && mv -f "$W_CONF.rm" "$W_CONF" 2>/dev/null && _nr=$((_nr + 1))
      done < "$DATA/.wdel"
      [ "$_nr" -gt 0 ] && ui_print "  深睡豁免已撤下：$_nr 个（新版清单移除）"
      _nw=0
      ensure_final_newline "$W_CONF"
      while read -r _p; do
        case "$_p" in ''|*[!A-Za-z0-9._]*) continue ;; esac
        grep -Fxq -- "$_p" "$DATA/.winst" 2>/dev/null || continue
        awk -v k="white=$_p" 'index($0, k) == 1 { f = 1 } END { exit !f }' \
          "$W_CONF" && continue
        printf 'white=%s\n' "$_p" >> "$W_CONF" 2>/dev/null && _nw=$((_nw + 1))
      done < "$DATA/.wadd"
      cp -f "$W_NEW" "$W_BASE" 2>/dev/null
      [ "$_nw" -gt 0 ] && ui_print "  深睡永久豁免已加入：$_nw 个（通讯/健康，可删）"
    fi
    rm -f "$DATA/.winst" "$DATA/.wnew" "$DATA/.wold" "$DATA/.wadd" "$DATA/.wdel" "$W_CONF.rm" 2>/dev/null
  fi
fi

chmod 0700 "$DATA" "$DATA/run" 2>/dev/null
chmod 0644 "$DATA/socfreq.conf" "$DATA/games.txt" 2>/dev/null
# 不往模块目录回填：并存两份配置时改模块目录那份无效，且比没有配置更难排查。

# ── 安装期自检：确认守护可以执行 ──────────────────────────────────
# 权限设完后试一次 exec。loader 的拒绝发生在 exec 之前（PT_TLS 对齐、
# 目标架构、ELF 头损坏），此时守护根本没启动，日志里一行都不会有。
# version 是最轻的子命令：不碰 sysfs、不写文件，失败也不留残留。
CHK=$("$MODPATH/socfreqd" version 2>&1)
if [ -z "$CHK" ]; then
  ui_print "!  守护无法执行，loader 原话："
  ui_print "   $CHK"
  ui_print "!  常见原因是 ELF 头部对齐不满足本机要求（TLS / 页对齐）。"
  ui_print "!  请把这段报错反馈给作者。"
else
  ui_print "  自检通过：守护 $CHK"
fi

# ── 申请 KernelSU 热安装 ─────────────────────────────────────────
# 本模块不写 system/、也没有只能在开机阶段做的操作，装完就能用：
# KernelSU 会用新目录替换模块，并执行 scripts/hotinstall.sh（停旧守护 → 起新版）。
# 支持热安装的管理器不会留 update 标记，也不会再提示重启；
# 不支持的管理器会忽略这两个变量，回到"重启后生效"的老路，不会更糟。
export MODULE_HOT_INSTALL_REQUEST=true
export MODULE_HOT_RUN_SCRIPT=scripts/hotinstall.sh
case "${KSU_HAS_HOT_INSTALL:-}" in
  true|1) ui_print "  已申请热安装：装完可直接打开 WebUI，无需重启" ;;
  *)      ui_print "!  当前管理器不支持热安装（KernelSU / SukiSU 需 v4.2.1+），本次仍需重启" ;;
esac