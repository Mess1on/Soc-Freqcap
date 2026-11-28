#!/system/bin/sh
# Soc智能限频 · 安装脚本

SKIPUNZIP=0

# ui_print 由管理器提供；缺失时退化为 echo，避免调用报错淹没安装输出。
command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$@"; }

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
  ui_print "  第三方调度总开关处于开启状态"
  CONFLICT=1
fi

[ "$CONFLICT" = "0" ] && ui_print "  未检测到冲突"
ui_print " "

# 环境自检
if [ ! -d /sys/devices/system/cpu/cpufreq ]; then
  ui_print "  未找到 cpufreq 目录，本机内核不支持 CPU 限频"
  abort
fi
if ! ls /sys/devices/system/cpu/cpufreq/policy*/cpuinfo_max_freq >/dev/null 2>&1; then
  ui_print "  未找到任何 policy，本机内核不支持 CPU 限频"
  abort
fi

# 提示
MODEL="$(getprop ro.product.model 2>/dev/null)"
# 芯片代号统一转大写（原厂属性为小写），与机型名的显示风格一致。
_soc_norm() {
  _sv=$(printf '%s' "$1" | tr 'a-z' 'A-Z' | tr -d '\r')
  _sv=${_sv%% *}
  _sv=${_sv%%-*}
  _sv=${_sv%%_*}
  case "$_sv" in
    PINEAPPLE|PINEAPPLEP) _sv=SM8650 ;;
    SM8750P|SM8750S) _sv=SM8750 ;;
    SM8850P|SM8850S) _sv=SM8850 ;;
    SM8650P|SM8650S) _sv=SM8650 ;;
    SM8550P|SM8550S) _sv=SM8550 ;;
    SM8475P|SM8475S) _sv=SM8475 ;;
  esac
  printf '%s' "$_sv"
}
SOC="$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')"
[ -z "$SOC" ] && SOC="$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')"
SOC="$(_soc_norm "$SOC")"

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
ui_print "  安装完成后按最后一行提示操作（重启 / 直接打开 WebUI）"
ui_print "━━━━━━━━━━━━━━━━━━━━"
ui_print " "

# 权限
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/socfreqd"       0 0 0755
set_perm "$MODPATH/service.sh"     0 0 0755
set_perm "$MODPATH/action.sh"      0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh"   0 0 0755
set_perm "$MODPATH/scripts/deep_sleep.sh" 0 0 0755
set_perm "$MODPATH/scripts/hotinstall.sh" 0 0 0755
set_perm "$MODPATH/tools/fix_metamodule.sh" 0 0 0755
set_perm "$MODPATH/tools/safemode.sh"       0 0 0755
set_perm "$MODPATH/tools/collect_crash.sh"  0 0 0755

# 配置文件用户可写，方便自己改
set_perm "$MODPATH/socfreq.conf" 0 0 0644
set_perm "$MODPATH/games.txt"    0 0 0644

# WebUI：index.html 只需读；api.sh 必须可执行。
# 否则不同管理器的 WebUI 直接 ./api.sh 调用会 permission denied。
set_perm "$MODPATH/webroot/index.html" 0 0 0644
set_perm "$MODPATH/webroot/api.sh"     0 0 0755

# 名单统一成 LF：CRLF 行尾的 \r 会让 api.sh 的 grep -x 整行比对全部失配且不报错。
if [ -f "$MODPATH/games.txt" ]; then
  if tr -d '\r' < "$MODPATH/games.txt" > "$MODPATH/games.txt.lf" 2>/dev/null; then
    mv -f "$MODPATH/games.txt.lf" "$MODPATH/games.txt"
    chmod 0644 "$MODPATH/games.txt"
  else
    rm -f "$MODPATH/games.txt.lf"
  fi
fi

# 可变状态搬到模块目录之外
# 配置和名单留在模块目录里会在每次升级时被包内默认值覆盖。
# /data/adb/soc_freqcap 与之同级但在升级范围之外，是 root 私有的普通目录。
DATA=/data/adb/soc_freqcap
# 同名文件 / 悬空软链会让 mkdir 静默失败，直到写标记文件时才报
if [ -L "$DATA" ] || { [ -e "$DATA" ] && [ ! -d "$DATA" ]; }; then
  mv -f "$DATA" "$DATA.bad.$$" 2>/dev/null \
    && ui_print "  数据目录位置被占用，已移到 $DATA.bad.$$"
fi
if [ -e "$DATA/run" ] && [ ! -d "$DATA/run" ]; then
  mv -f "$DATA/run" "$DATA/run.bad.$$" 2>/dev/null
fi
mkdir -p "$DATA/run" 2>/dev/null
if [ ! -d "$DATA/run" ]; then
  /system/bin/mkdir -p "$DATA/run" 2>/dev/null
fi
if [ ! -d "$DATA/run" ]; then
  ui_print "  !! 无法创建 $DATA（检查 /data 空间、同名文件或 SELinux 权限）"
  abort
fi
ui_print "  配置目录：$DATA"

# 按芯片套预设档位（单一来源：adapt/*.conf 的 default_*）
# 芯片默认值和节点适配表放在同一份 adapt/<soc>.conf 里，装机脚本不再写死第二套。
# 只对全新安装生效；升级一律不碰用户配置（用户值可能刚好等于默认值，
# 不能用"值等于默认"的启发式去覆盖）。
# 内核 present 是 0-7 / 0-5,7 这类串；按它数“可能存在的核”，
_cpu_count() {
  _pc=$(cat /sys/devices/system/cpu/present 2>/dev/null)
  if [ -z "$_pc" ]; then
    ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | wc -l
    return
  fi
  _pn=0
  for _pseg in $(printf '%s' "$_pc" | tr ',' ' '); do
    case "$_pseg" in
      *-*)
        _pa=${_pseg%-*}; _pb=${_pseg#*-}
        case "$_pa$_pb" in ''|*[!0-9]*) continue ;; esac
        _pn=$((_pn + _pb - _pa + 1))
        ;;
      *)
        case "$_pseg" in ''|*[!0-9]*) continue ;; esac
        _pn=$((_pn + 1))
        ;;
    esac
  done
  [ "$_pn" -gt 0 ] && printf '%s' "$_pn" || \
    ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | wc -l
}

# 匹配规则与节点适配表一致：文件里每条 #match 都命中才算命中，
_adapt_match() {
  ADAPT_FILE=
  [ -d "$MODPATH/adapt" ] || return 0
  _b=$(getprop ro.product.brand 2>/dev/null | tr 'a-z' 'A-Z')
  _m=$(getprop ro.product.model 2>/dev/null | tr 'a-z' 'A-Z')
  _s=$(getprop ro.soc.model 2>/dev/null | tr 'a-z' 'A-Z')
  [ -z "$_s" ] && _s=$(getprop ro.board.platform 2>/dev/null | tr 'a-z' 'A-Z')
  _s=$(_soc_norm "$_s")
  _cores=${SF_TEST_CORES:-$(_cpu_count)}
  _best=
  _bestn=0
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
        *soc=*)   _e=$(_soc_norm "${_l#*soc=}"); case "$_s" in *"$_e"*) : ;; *) _hit=0 ;; esac ;;
        *cores=*) [ "$_cores" = "${_l#*cores=}" ] || _hit=0 ;;
        *) _hit=0 ;;
      esac
    done
    IFS=$_oifs
    [ "$_hit" = 1 ] || continue
    _n=$(printf '%s\n' "$_ml" | grep -c '^#match ')
    if [ "$_n" -gt "$_bestn" ]; then
      _best="$_f"
      _bestn=$_n
    fi
  done
  ADAPT_FILE=$_best
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
      default_cpu_pct_mid2)   _key=CPU_PCT_MID2;   _lo=1;  _hi=100 ;;
      default_cpu_pct_mid)    _key=CPU_PCT_MID;    _lo=1;  _hi=100 ;;
      default_cpu_pct_little) _key=CPU_PCT_LITTLE; _lo=1;  _hi=100 ;;
      default_gpu_pct)        _key=GPU_PCT;        _lo=1;  _hi=100 ;;
      default_th_high)        _key=TH_HIGH;        _lo=35; _hi=110 ;;
      default_th_low)         _key=TH_LOW;         _lo=30; _hi=105 ;;
      default_psi_gain)       _key=PSI_GAIN;       _lo=0;  _hi=30 ;;
      default_cpu_floor_m)    _key=CPU_FLOOR_M;    _lo=0;  _hi=10000000 ;;
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

# 应用分类默认值按芯片省电预设生成：
_cat_pct() {
  _v=$(($1 + $2))
  [ "$_v" -lt 1 ] && _v=1
  [ "$_v" -gt 100 ] && _v=100
  printf '%s' "$_v"
}

_cat_make_block() {
  _sav_big=$(sed -n 's/^preset_sav_big=//p' "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
  _sav_mid2=$(sed -n 's/^preset_sav_mid2=//p' "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
  _sav_mid=$(sed -n 's/^preset_sav_mid=//p' "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
  _sav_little=$(sed -n 's/^preset_sav_little=//p' "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
  case "$_sav_big" in ''|*[!0-9]*) return 1 ;; esac
  # 同档预设没写时用相邻簇兜底，避免 4 簇机少一档就没默认值
  [ -n "$_sav_mid2" ] || _sav_mid2=$_sav_mid
  [ -n "$_sav_mid" ] || _sav_mid=$_sav_mid2
  [ -n "$_sav_little" ] || _sav_little=$_sav_mid
  _cat_block="$DATA/.cat_defaults.$$"
  _cat_old_block="$DATA/.cat_defaults_old.$$"
  : > "$_cat_block"
  : > "$_cat_old_block"
  for _c in video social reading; do
    case "$_c" in
      video)   _ba=-5; _oa=0;  _oba=-5; _ooa=-8 ;;
      social)  _ba=-5; _oa=10; _oba=-5; _ooa=7 ;;
      reading) _ba=-7; _oa=-10; _oba=-7; _ooa=-11 ;;
    esac
    _ov=$(_cat_pct "$_sav_big" "$_oba")
    printf '%s_cpu_big=%s\n' "$_c" "$_ov" >> "$_cat_old_block"
    for _spec in "mid2:$_sav_mid2" "mid:$_sav_mid" "little:$_sav_little"; do
      _t=${_spec%%:*}; _b=${_spec#*:}
      case "$_b" in ''|*[!0-9]*) continue ;; esac
      _ov=$(_cat_pct "$_b" "$_ooa")
      printf '%s_cpu_%s=%s\n' "$_c" "$_t" "$_ov" >> "$_cat_old_block"
    done

    _cb=$(sed -n "s/^cat_${_c}_big=//p" "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
    case "$_cb" in ''|*[!0-9]*) _cb= ;; esac
    if [ -n "$_cb" ]; then
      _v=$(_cat_pct "$_cb" 0)
      printf '%s_cpu_big=%s\n' "$_c" "$_v" >> "$_cat_block"
      for _t in mid2 mid little; do
        _cv=$(sed -n "s/^cat_${_c}_${_t}=//p" "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
        case "$_cv" in ''|*[!0-9]*) continue ;; esac
        _v=$(_cat_pct "$_cv" 0)
        printf '%s_cpu_%s=%s\n' "$_c" "$_t" "$_v" >> "$_cat_block"
      done
      _cg=$(sed -n "s/^cat_${_c}_gpu=//p" "$ADAPT_FILE" 2>/dev/null | head -n 1 | tr -d ' \r\t')
      case "$_cg" in ''|*[!0-9]*) _cg=0 ;; esac
      printf '%s_gpu=%s\n' "$_c" "$_cg" >> "$_cat_block"
      continue
    fi

    _v=$(_cat_pct "$_sav_big" "$_ba")
    printf '%s_cpu_big=%s\n' "$_c" "$_v" >> "$_cat_block"
    for _spec in "mid2:$_sav_mid2" "mid:$_sav_mid" "little:$_sav_little"; do
      _t=${_spec%%:*}; _b=${_spec#*:}
      case "$_b" in ''|*[!0-9]*) continue ;; esac
      _v=$(_cat_pct "$_b" "$_oa")
      printf '%s_cpu_%s=%s\n' "$_c" "$_t" "$_v" >> "$_cat_block"
    done
    printf '%s_gpu=0\n' "$_c" >> "$_cat_block"
  done
}

_cat_old_defaults() {
  _f=$1
  [ -f "$_f" ] || return 1
  _ok=1
  for _spec in video:44 social:55 reading:50; do
    _c=${_spec%%:*}; _d=${_spec#*:}
    _v=$(sed -n "s/^${_c}_cpu=//p" "$_f" 2>/dev/null | head -n 1 | tr -d ' \r\t')
    [ -n "$_v" ] && [ "$_v" != "$_d" ] && _ok=0
    for _t in big mid2 mid little; do
      _v=$(sed -n "s/^${_c}_cpu_${_t}=//p" "$_f" 2>/dev/null | head -n 1 | tr -d ' \r\t')
      [ -n "$_v" ] && [ "$_v" != "$_d" ] && _ok=0
    done
  done
  [ "$_ok" = 1 ]
}

# 旧公式生成过、但用户没改 CPU 的文件也迁移到新公式；改过任意一档就保留。
_cat_old_generated() {
  _f=$1
  [ -f "$_f" ] && [ -f "$_cat_old_block" ] || return 1
  _hit=0
  while IFS='=' read -r _k _v; do
    case "$_k" in
      *_cpu_big|*_cpu_mid2|*_cpu_mid|*_cpu_little) ;;
      *) continue ;;
    esac
    _cur=$(sed -n "s/^${_k}=//p" "$_f" 2>/dev/null | head -n 1 | tr -d ' \r\t')
    [ "$_cur" = "$_v" ] || return 1
    _hit=1
  done < "$_cat_old_block"
  [ "$_hit" = 1 ]
}

_cat_rewrite() {
  _f=$1
  [ -f "$_f" ] || return 0
  _block=$_cat_block
  if [ "${2:-0}" = 1 ]; then
    _block="$_cat_block.keep"
    cp -f "$_cat_block" "$_block" 2>/dev/null
    for _c in video social reading; do
      _g=$(sed -n "s/^${_c}_gpu=//p" "$_f" 2>/dev/null | head -n 1 | tr -d ' \r\t')
      case "$_g" in ''|*[!0-9]*) _g=0 ;; esac
      sed "s/^${_c}_gpu=.*/${_c}_gpu=$_g/" "$_block" > "$_block.n" 2>/dev/null \
        && mv -f "$_block.n" "$_block"
    done
  fi
  {
    printf '# 应用分类：按场景覆盖当前封顶。0 = 不设置/跟随全局。\n'
    cat "$_block" 2>/dev/null
    awk '
      /^[ \t]*(video|social|reading)_(cpu|cpu_big|cpu_mid2|cpu_mid|cpu_little|gpu)[ \t]*=/ { next }
      /^[ \t]*# 应用分类/ { next }
      { print }
    ' "$_f" 2>/dev/null
  } > "$_f.cat.$$" 2>/dev/null && mv -f "$_f.cat.$$" "$_f"
  chmod 0644 "$_f" 2>/dev/null
  [ "$_block" = "$_cat_block" ] || rm -f "$_block"
}

_cat_soc_defaults() {
  [ -n "$ADAPT_FILE" ] && [ -f "$ADAPT_FILE" ] || return 0
  _cat_make_block || return 0
  _cat_rewrite "$DATA/defaults/app_categories.conf" 0
  _force_mark="$DATA/.cat_scene_v3_4"
  _force=0
  [ -f "$_force_mark" ] || _force=1
  if [ "$_force" = 1 ]; then
    _cat_rewrite "$DATA/app_categories.conf" 0
    : > "$_force_mark"
  elif [ "$_FRESH_CONF" = 1 ] || _cat_old_defaults "$DATA/app_categories.conf" \
     || _cat_old_generated "$DATA/app_categories.conf"; then
    _cat_rewrite "$DATA/app_categories.conf" 1
  fi
  rm -f "$_cat_block" "$_cat_old_block" "$_cat_block.keep" "$_cat_block.keep.n" 2>/dev/null
}

# 出厂默认留一份：模块目录里的 socfreq.conf 随后会被搬到数据目录/删掉，
# 页面上的「恢复全部默认」需要一份不会被用户改到的原件。
mkdir -p "$DATA/defaults" 2>/dev/null
for _f in socfreq.conf app_modes.conf app_categories.conf; do
  [ -f "$MODPATH/$_f" ] && cp -f "$MODPATH/$_f" "$DATA/defaults/$_f" 2>/dev/null
done
chmod 0644 "$DATA/defaults"/*.conf 2>/dev/null

# 数据目录已有配置视为升级
_FRESH_CONF=0
[ -f "$DATA/socfreq.conf" ] || _FRESH_CONF=1

for _f in socfreq.conf app_modes.conf app_categories.conf vendors.txt socfreqd.log; do
  if [ -f "$MODPATH/$_f" ] && [ ! -f "$DATA/$_f" ]; then
    mv -f "$MODPATH/$_f" "$DATA/$_f" 2>/dev/null
  fi
done
_adapt_match
_cat_soc_defaults
if [ "$_FRESH_CONF" = 1 ]; then
  _soc_defaults "$DATA/socfreq.conf"
fi

# 机型适配表（adapt/）
# 有些机器的节点长得不在预期路径上：一个 policy* 目录都读不到、
# 或找不到可写的 GPU 上限节点。这种机器由作者照着用户发来的适配包改好一张表，
_soc_adapt() {
  [ -d "$MODPATH/adapt" ] || return 0
  if [ -n "$ADAPT_FILE" ] && [ -f "$ADAPT_FILE" ]; then
    # 守护运行时要按 #match（brand/model/soc/cores）选中的那份读 cpuset/gov，
    # 不能只按 SoC 文件名猜；把命中文件整份落到数据目录，守护优先读它。
    { printf '#from adapt/%s\n' "${ADAPT_FILE##*/}"; cat "$ADAPT_FILE"; } \
      > "$DATA/adapt.conf" 2>/dev/null
    chmod 0644 "$DATA/adapt.conf" 2>/dev/null
    _body=$(grep -E '^(cpu-dir|gpu-max|gpu-min|gpu-freqs) ' "$ADAPT_FILE" 2>/dev/null)
    if [ -n "$_body" ]; then
      _n=$(printf '%s\n' "$_body" | grep -c .)
      { printf '#from adapt/%s\n' "${ADAPT_FILE##*/}"; printf '%s\n' "$_body"; } \
        > "$DATA/override.conf" 2>/dev/null
      chmod 0644 "$DATA/override.conf" 2>/dev/null
      ui_print "  命中适配表 ${ADAPT_FILE##*/}：$_n 条节点已装进数据目录（重启或热安装后生效）"
      return 0
    fi
    ui_print "  命中 ${ADAPT_FILE##*/}：只有芯片默认/运行参数，没有节点表"
  else
    # 没命中：只撤 adapt 自己装过的那份运行时表，别动用户手工文件
    if head -n 1 "$DATA/adapt.conf" 2>/dev/null | grep -q '^#from adapt/'; then
      rm -f "$DATA/adapt.conf" 2>/dev/null
    fi
  fi
  # 没命中 / 命中文件没有节点表：只撤 adapt 自己装过的那份
  if head -n 1 "$DATA/override.conf" 2>/dev/null | grep -q '^#from adapt/'; then
    rm -f "$DATA/override.conf" 2>/dev/null
    ui_print "  本次包里没有匹配这台机的节点表，已撤掉上一版装进去的那份（回到自动探测）"
  fi
}
_soc_adapt

# 追加写入前保证目标文件以换行结尾：末行无换行时 >> 会把新行与末行粘成一行
ensure_final_newline() {
  [ -s "$1" ] || return 0
  if [ -n "$(tail -c 1 "$1" 2>/dev/null | tr -d '\n')" ]; then
    printf '\n' >> "$1" 2>/dev/null
  fi
}

# 数据目录已有 conf 时不整体覆盖，避免用户改过的参数被打回默认。
if [ -f "$DATA/socfreq.conf" ] && [ -f "$MODPATH/socfreq.conf" ]; then
  _added=0
  ensure_final_newline "$DATA/socfreq.conf"
  # 分簇限速：旧版只有三簇统一的 GOV_UP_US / GOV_DOWN_US。升级时先把
  # 旧值摊到三个新键，再走通用补键循环，免得用户调过的值被打回默认。
  for _gk in UP DOWN; do
    _old=$(sed -n "s/^[[:space:]]*GOV_${_gk}_US[[:space:]]*=//p" "$DATA/socfreq.conf" 2>/dev/null \
      | head -n 1 | sed 's/#.*//' | tr -d ' \r\t')
    case "$_old" in ''|*[!0-9]*) continue ;; esac
    [ "${#_old}" -gt 5 ] && continue
    [ "$_old" -lt 100 ] || [ "$_old" -gt 50000 ] && continue
    # 旧默认就是 1000；没动过的话让它落新默认（超大核 3000 / 主簇 1000），
    [ "$_old" = "1000" ] && continue
    grep -q "^GOV_${_gk}_BIG=" "$DATA/socfreq.conf" 2>/dev/null && continue
    printf 'GOV_%s_BIG=%s\nGOV_%s_MID=%s\nGOV_%s_LITTLE=%s\n' \
      "$_gk" "$_old" "$_gk" "$_old" "$_gk" "$_old" >> "$DATA/socfreq.conf" 2>/dev/null \
      && _added=$((_added + 1))
  done
  ensure_final_newline "$DATA/socfreq.conf"
  while read -r _line; do
    case "$_line" in ''|\#*) continue ;; esac
    case "$_line" in *=*) ;; *) continue ;; esac
    _k=${_line%%=*}
    # 熄屏策略是行为开关：通用补键循环不碰它，下面按旧值单独迁移。
    [ "$_k" = SCREEN_POLICY ] && continue
    grep -q "^${_k}=" "$DATA/socfreq.conf" 2>/dev/null && continue
    printf '%s\n' "$_line" >> "$DATA/socfreq.conf" 2>/dev/null && _added=$((_added + 1))
  done < "$MODPATH/socfreq.conf"
  # 熄屏轻巡检改为常开：存量用户不管原来选的是交还系统还是 keep，覆盖
  ensure_final_newline "$DATA/socfreq.conf"
  _CAP_MARK="$DATA/.screen_cap_default"
  if [ ! -f "$_CAP_MARK" ]; then
    if grep -qE '^[[:space:]]*SCREEN_POLICY[[:space:]]*=' "$DATA/socfreq.conf" 2>/dev/null; then
      sed -i 's/^[[:space:]]*SCREEN_POLICY[[:space:]]*=.*/SCREEN_POLICY=cap/' \
        "$DATA/socfreq.conf" 2>/dev/null
    else
      printf 'SCREEN_POLICY=cap\n' >> "$DATA/socfreq.conf" 2>/dev/null
    fi
    # 旧键同步成 0，避免以后看配置时误以为还在熄屏交还。
    if grep -qE '^[[:space:]]*SCREEN_UNCAP[[:space:]]*=' "$DATA/socfreq.conf" 2>/dev/null; then
      sed -i 's/^[[:space:]]*SCREEN_UNCAP[[:space:]]*=.*/SCREEN_UNCAP=0/' \
        "$DATA/socfreq.conf" 2>/dev/null
    fi
    : > "$_CAP_MARK" 2>/dev/null
    _added=$((_added + 1))
    ui_print "  熄屏轻巡检已设为常开（SCREEN_POLICY=cap）"
  fi
  [ "$_added" -gt 0 ] && ui_print "  配置补齐新增项：$_added 条"
  # V2.5.9：切前台预热默认关（ASB/Kin 没有这套逻辑）。旧默认 2 一次性迁到 0。
  _TIER_MARK="$DATA/.app_switch_tier_default0"
  if [ ! -f "$_TIER_MARK" ]; then
    if grep -qE '^[[:space:]]*APP_SWITCH_TIER[[:space:]]*=[[:space:]]*2[[:space:]]*$' "$DATA/socfreq.conf" 2>/dev/null; then
      sed -i 's/^[[:space:]]*APP_SWITCH_TIER[[:space:]]*=.*/APP_SWITCH_TIER=0/' "$DATA/socfreq.conf" 2>/dev/null
      ui_print "  APP_SWITCH_TIER: 2 -> 0（切前台预热已默认关闭，与 ASB/Kin 一致）"
    fi
    : > "$_TIER_MARK" 2>/dev/null
  fi
  # 删掉模块目录内原件：并存两份配置时改模块目录那份不会有任何效果
  rm -f "$MODPATH/socfreq.conf" 2>/dev/null
fi

# 荣耀 / 华为 / 三星：清掉旧默认写入的 conservative
# 内核切到通用 conservative 后直接 kernel panic：装完约一分钟（开机满血
# 新版默认已改成"不干预"；这里把存量机器上由旧默认写进去的那一行
_GOV_MARK="$DATA/.gov_default_fix"
if [ ! -f "$_GOV_MARK" ]; then
  _br=$(getprop ro.product.brand 2>/dev/null | tr 'a-z' 'A-Z')
  _mf=$(getprop ro.product.manufacturer 2>/dev/null | tr 'a-z' 'A-Z')
  case "$_br $_mf" in
    *HONOR*|*HUAWEI*|*SAMSUNG*)
      if [ -f "$DATA/socfreq.conf" ]; then
        awk -F= '
          $1 == "CPU_GOVERNOR" {
            v = $2
            sub(/[[:space:]]*#.*/, "", v)
            gsub(/[[:space:]]/, "", v)
            if (v == "conservative") { print "CPU_GOVERNOR="; next }
          }
          { print }' "$DATA/socfreq.conf" > "$DATA/socfreq.conf.gov" 2>/dev/null \
          && mv -f "$DATA/socfreq.conf.gov" "$DATA/socfreq.conf" 2>/dev/null \
          && chmod 0644 "$DATA/socfreq.conf" 2>/dev/null \
          && ui_print "  荣耀/华为机型：调速器改回「不干预」（旧默认会触发内核硬重启）"
      fi
      ;;
  esac
  : > "$_GOV_MARK" 2>/dev/null
fi

# 名单：升级时把精选名单的新条目合进来
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

# 内置满血清单：相机类
B_NEW="$MODPATH/scripts/builtin_modes.txt"
B_BASE="$DATA/builtin_modes.base.txt"
if [ -f "$B_NEW" ] && [ -f "$DATA/app_modes.conf" ]; then
  # pm 拿不到已安装列表时整段跳过，并且**不推进基准**：
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

# 内置深睡永久豁免：常用通讯 / 运动健康
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

# 安装期自检：确认守护可以执行
# 权限设完后试一次 exec。loader 的拒绝发生在 exec 之前（PT_TLS 对齐、
CHK=$("$MODPATH/socfreqd" version 2>&1)
RC=$?
# 成败要按退出码判，不能按"输出为空"判：loader 的拒绝信息（CANNOT LINK
if [ "$RC" != 0 ] || [ -z "$CHK" ]; then
  ui_print "!  守护无法执行（退出码 $RC），loader 原话："
  ui_print "   $CHK"
  ui_print "!  常见原因是 ELF 头部对齐不满足本机要求（TLS / 页对齐）。"
  ui_print "!  请把这段报错反馈给作者。"
else
  ui_print "  自检通过：守护 $CHK"
fi

# 申请 KernelSU 热安装
export MODULE_HOT_INSTALL_REQUEST=true
export MODULE_HOT_RUN_SCRIPT=scripts/hotinstall.sh
case "${KSU_HAS_HOT_INSTALL:-}" in
  true|1)
    ui_print "  安装完成：已热安装，直接打开 WebUI 即可，无需重启"
    ui_print "  （想立刻让深睡/开机脚本也走新版本时，再重启一次也可以）"
    ;;
  *)
    ui_print "  安装完成：请重启手机后再使用"
    ui_print "  （当前安装器未提供热安装；KernelSU 稳定版 3.3.1 尚未包含该功能）"
    ui_print "  （SukiSU Ultra 需 v4.2.1-r2+；KernelSU 需含 2026-10-15 热安装提交的构建）"
    ;;
esac