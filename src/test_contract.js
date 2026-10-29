// 契约检查：socfreqd export 输出的键，必须和 index.html 实际读取的键对得上。
// 做法：扫 index.html 里所有 <标识符>.<小写键> 与 <标识符>.<大写键> 的属性访问，
// 再和 export 的输出集合、api.sh 的键集合交叉比对。
const fs = require('fs');

// 前 7 个参数是本仓库里的文件，缺一个就跑不了。
// 最后一个是"别人的 api.sh"，只用于相似度对比——它不随本包发布，
// 外部用户手上根本没有。强制要求它，等于把自检锁在自己这儿。
const ARGS = [
  ['守护源码',      'src/socfreqd.c'],
  ['WebUI 页面',    'webroot/index.html'],
  ['配置文件',      'socfreq.conf'],
  ['接口脚本',      'webroot/api.sh'],
  ['README',        'README.md'],
  ['游戏名单',      'games.txt'],
  ['图标取图源码',  'src/icon.c'],
];
const missing = [];
for (let i = 0; i < ARGS.length; i++) {
  const p = process.argv[i + 2];
  if (!p || !fs.existsSync(p)) missing.push(`  argv[${i + 2}] ${ARGS[i][0]}  ${p || '(未传)'} — 应为 ${ARGS[i][1]}`);
}
if (missing.length) {
  console.error(`参数不对（需要 ${ARGS.length} 个，现有 ${process.argv.length - 2} 个）：\n` + missing.join('\n'));
  console.error('\n用法:\n  node src/test_contract.js \\\n' +
    '     src/socfreqd.c webroot/index.html socfreq.conf webroot/api.sh \\\n' +
    '     README.md games.txt src/icon.c [比对基准脚本]');
  process.exit(2);
}

// 比对基准可选：没传或路径不对就跳过相似度那两条，其余检查照跑。
// 不能因为少了参照物就整个退出——那样外部用户根本没法自证。
const refPath = process.argv[9];
const haveRef = !!(refPath && fs.existsSync(refPath));
if (process.argv[9] && !haveRef) {
  console.log(`  ..   跳过相似度对比：找不到 ${process.argv[9]}`);
}

const csrc = fs.readFileSync(process.argv[2], 'utf8');
const html = fs.readFileSync(process.argv[3], 'utf8');
const conf = fs.readFileSync(process.argv[4], 'utf8');
const apiSrc = fs.readFileSync(process.argv[5], 'utf8');
const iconSrc = fs.readFileSync(process.argv[8], 'utf8');

const MOD0 = require('path').join(__dirname, '..'); // 路径里有中文，别用 file:// URL（会被百分号编码）

// README 已压缩：细节拆到 docs/。文档类断言统一读 README + docs/*.md，
// 免得把"文档里写了"绑死在某一个文件上。
const DOC = (() => {
  let all = fs.readFileSync(process.argv[6], 'utf8');
  const dir = MOD0 + '/docs';
  if (fs.existsSync(dir)) {
    for (const f of fs.readdirSync(dir).sort()) {
      if (/\.md$/.test(f)) all += '\n' + fs.readFileSync(dir + '/' + f, 'utf8');
    }
  }
  return all;
})();

let fail = 0, pass = 0;
function ok(c, n, e){
  if(c){ console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}
let m;

/* 去注释：分 shell 和 C 两种，混用会吃掉真代码。
   shell 版只剥行注释，且引号里的 # 不算注释 —— 对整行做 s/#.*$// 会把
   printf '#%s=%s\n' 从中间切断，剩下的半个引号把后面几十行吞进字符串里。
   C 版才剥块注释 —— shell 里的 ${x%/*} 也含 /*，用块注释规则会把代码吃掉。 */
function stripSh(src) {
  return src.split('\n').map(l => {
    let out = '', s1 = false, s2 = false, prev = '';
    for (const c of l) {
      if (c === '\'' && !s2) s1 = !s1;
      else if (c === '"' && !s1) s2 = !s2;
      else if (c === '#' && !s1 && !s2 && prev !== '$') break;
      out += c; prev = c;
    }
    return out;
  }).join('\n');
}

/* 引号里的 /* 不是注释起始（"http://…" 这类字符串常量到处都是）。 */
function stripC(src) {
  const out = [];
  let s1 = false, s2 = false, blk = false, line = '';
  for (let i = 0; i < src.length; i++) {
    const c = src[i], n = src[i + 1];
    if (blk) {
      if (c === '*' && n === '/') { blk = false; i++; }
      else if (c === '\n') { out.push(line.replace(/\s+$/, '')); line = ''; }
      continue;
    }
    if (!s1 && !s2) {
      if (c === '/' && n === '*') { blk = true; i++; continue; }
      if (c === '/' && n === '/') {
        while (i < src.length && src[i] !== '\n') i++;
        out.push(line.replace(/\s+$/, '')); line = '';
        continue;
      }
    }
    if (c === '"' && !s1) s2 = !s2;
    else if (c === '\'' && !s2) s1 = !s1;
    if (c === '\n') { out.push(line); line = ''; continue; }
    line += c;
  }
  out.push(line);
  return out.join('\n');
}


/* 首页徽标和分应用档位都靠 status 那几行键。
 * 少 grep 一个键的话前端拿到 undefined：reason 缺 → 开机满血显示"游戏中"；
 * fg/app_mode 缺 → "设为该档"永远点不动。
 * 这里逐个键判，不写死整条 grep 的样子——加一个键（比如 app_mode）
 * 会让写死的断言红，而红的原因明明是"新键加上了"。 */
{
  const g = (apiSrc.match(/grep -E '\^\(([^']*)\)='/g) || []).join(' ');
  const keys = new Set((apiSrc.match(/grep -E '\^\(([^']*)\)='/) || ['', ''])[1]
                       .split('|'));
  for (const k of ['state', 'screen', 'soff', 'game_pkg', 'reason', 'fg', 'app_mode', 'app_gpu', 'pid', 'batt', 'bcut']) {
    ok(keys.has(k), 'api.sh 读状态文件时带上 ' + k, g.slice(0, 140));
  }
  ok(/printf 'state=unknown[^\n]*app_mode=[^\n]*app_gpu=[^\n]*pid=/.test(apiSrc),
     '状态文件不存在时的兜底也给全这几个键（前端不会拿到 undefined）');
}
ok(/s\.reason/.test(html), 'WebUI 会读 status 的 reason');
/* 每个 reason 都要有自己的徽标文案。少一个就掉进 else，
 * 而 else 是"游戏中 · 已解锁"——相机设成满血时前台根本没有游戏，
 * 用户看到的就是一句假话。 */
{
  const badge = (html.match(/why === 'boot'[\s\S]{0,400}?;/) || [''])[0];
  ok(!!badge, '徽标的 reason 分支抠得到（锚点：why === \'boot\'）',
     badge ? '分支长度 ' + badge.length : '没找到');
  for (const [k, txt] of [['boot', '开机满血'], ['screen', '熄屏解除'], ['app', '应用满血']]) {
    ok(badge.includes("'" + k + "'") && badge.includes(txt),
       'reason=' + k + ' 有专属文案（' + txt + '）',
       (badge.match(new RegExp(k + "[^:\\n]*:[^\\n]*")) || ['没找到'])[0]);
  }
  ok(/: '游戏中 · 已解锁';/.test(badge),
     '只有 game（和没有 reason 的老状态文件）才显示"游戏中"');
}
ok(/snprintf\(g_reason, sizeof g_reason, "%s", bwhy\)/.test(csrc)
   && /snprintf\(g_reason, sizeof g_reason, "game"\)/.test(csrc)
   && /snprintf\(g_reason, sizeof g_reason, "app"\)/.test(csrc),
   '守护会写 game / boot|screen / app 这几类 reason');
ok(/fprintf\(f, "state=%s\\nscreen=%s\\nsoff=%d\\n/.test(csrc) &&
   /reason=%s/.test(csrc),
   '守护发布状态里带 screen/soff/reason（前端区分不开的根因在这儿）');

ok(/DATA=\/data\/adb\/soc_freqcap/.test(fs.readFileSync(MOD0 + '/service.sh', 'utf8')),
   'service.sh 用的是同一个数据目录');
ok(/mv -f "\$MODPATH\/\$_f" "\$DATA\/\$_f"/.test(fs.readFileSync(MOD0 + '/customize.sh', 'utf8')),
   '装机时把老位置的配置搬过去，而不是复制一份留下歧义');
/* 回填等于同时存在两份配置：用户照说明去改模块目录那份，
   改完什么都不发生。这条防的是"好心补原件"把问题又绕回去。 */
{
  const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
  ok(!/cp "\$DATA\/\$_f" "\$MODPATH\/\$_f"/.test(cz),
     '不往模块目录回填配置（两份配置必然有一份是死的）');
}
ok(/rm -rf \/data\/adb\/soc_freqcap/.test(fs.readFileSync(MOD0 + '/uninstall.sh', 'utf8')),
   '卸载清掉数据目录（不留 root 私有的孤儿目录）');
/* uninstall.sh 的兜底：三个 restore_* 函数内部拼的都是 "$1/run/xxx.saved"，
   所以调用点必须传**父目录**。这一版之前就踩了：循环变量存的是
   /data/adb/soc_freqcap/run，函数再拼一次 run/ 变成 .../run/run/nodes.saved，
   文件永远找不到，而函数开头是 `[ -f ... ] || return 0`——静默返回。
   后果是 sysfs 上的封顶一个都没还原，卸载末尾照样打
   "已恢复启动前状态"。这条兜底存在的唯一场景正是守护跑不起来的时候，
   静默失效最要命，所以钉成断言而不是靠读代码。

   断言按**值**判，不按字面量判：把循环列表写成 "$DATA/run"、
   或者给 DATA 的定义加个 /run 尾巴，都是同一个 bug，
   而只匹配 `for BASE in "$DATA" "$MODDIR"` 这句话的话，
   换个变量名就绕过去了（实测把 for 改成 "$DATA/run" 时，
   检查调用点的 `/\run"?$/` 反而还是绿的——因为调用点写的确实是 "$BASE"，
   脏的是 BASE 的值）。所以先把变量表建出来、把值代入，再看落不散到 /run。 */
{
  const un = fs.readFileSync(MOD0 + '/uninstall.sh', 'utf8');
  const unCode = un.split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).join('\n');

  // 顶层 NAME=value 表（值去掉外层引号）。${0%/*} 这类解不开，留原样。
  const vars = {};
  for (const m of unCode.matchAll(/^([A-Z][A-Z0-9_]*)=(?:"([^"]*)"|'([^']*)'|([^\s#]+))/gm))
    vars[m[1]] = m[2] !== undefined ? m[2] : (m[3] !== undefined ? m[3] : m[4]);

  // 取 for 列表 → 逐个解析成真实路径。
  const loop = unCode.match(/for\s+(\w+)\s+in\s+([^;]+);/);
  ok(!!loop, '兜底是个 for 循环（新增第三处家时也要走同一个循环）');
  const items = loop ? loop[2].trim().split(/\s+/) : [];
  /* 一层代入就够：只把 "$NAME"/"${NAME}" 换成顶层赋值的**字面绝对路径**。
   * MODDIR=${0%/*} 这种参数展开解不开（再套一层正则会把它咬成
   * "%/*}" 之类的碎渣，比不解还糟），留成不透明标记 p.opaque=true。
   * 不透明不代表不能查——"值以 /run 结尾"这件事对字面量同样成立。 */
  const items2 = items.map((tok) => {
    const raw = tok.replace(/^"|"$/g, '');
    const m = raw.match(/^\$\{?([A-Z][A-Z0-9_]*)\}?$/);
    const v = m && vars[m[1]];
    if (v && v.startsWith('/')) return { tok: raw, path: v, opaque: false };
    return { tok: raw, path: raw, opaque: true };
  });
  ok(items2.length >= 2, `循环列表 ${items2.length} 项（新位置 + 老位置至少两处）`,
     items2.map(x => x.tok).join(' , '));
  ok(!items2.some(x => /\/run$/.test(x.path)),
     '传给 restore_* 的是父目录，不是 .../run（函数内会再拼一次 run/）',
     items2.map(x => x.tok + '→' + x.path).join(' , '));
  const home = items2.filter(x => !x.opaque && /\/soc_freqcap$/.test(x.path));
  ok(home.length >= 1, '其中一处就是数据目录（搬家后的家）',
     items2.map(x => x.tok + '→' + x.path).join(' , '));
  /* 老位置那一项必须是模块目录本身，而且脚本里要真的解得出这个变量——
   * 写成 ${MODDIR} 却从没赋值（或拼错变量名）时，展开是空串，
   * 函数里的 [ -f "$1/run/..." ] 一样静默失败，和 /run/run 同一个症状。 */
  const legacy = items2.filter(x => x.opaque && /^\$\{?MODDIR\}?$/.test(x.tok));
  ok(legacy.length >= 1 && vars.MODDIR !== undefined,
     '另一处是老位置(模块目录)，且 MODDIR 在脚本里确实有赋值',
     'MODDIR=' + (vars.MODDIR === undefined ? '(未定义)' : vars.MODDIR));

  const calls = (unCode.match(/^\s*restore_(?:from|perms|govs) \S+/gm) || []);
  ok(calls.length === 3,
     '三个 restore_* 都被调用（少一个就是少兜一本账）',
     calls.join(' | '));
  ok(calls.every(c => /^\s*restore_(?:from|perms|govs) "\$\{?BASE\}?"$/.test(c)),
     '三本账都在循环里、都传同一个循环变量（写在循环外就只兜了一处家）',
     calls.join(' | '));
  ['restore_from', 'restore_perms', 'restore_govs'].forEach((fn) => {
    const body = (un.match(new RegExp(fn + '\\(\\)\\s*\\{[\\s\\S]*?\\n\\}')) || [''])[0];
    ok(/\$1\/run\/[a-z]+\.saved/.test(body),
       fn + ' 内部按 "$1/run/xxx.saved" 拼路径（和调用点约定一致）');
  });
  // 账本在数据目录里，rm -rf 也在。删早了后面就无账可兜。
  ok(unCode.indexOf('for ') < unCode.indexOf('rm -rf /data/adb/soc_freqcap'),
     '先按 journal 还原，再删数据目录（顺序反了等于把账本先烧了）');
}

/* ── 追加写入必须先补换行 ────────────────────────────────────────
   末行不带换行的文件被 `>>` 或 `cat 文件; echo 新行` 追加时，
   新行会和末行粘成一行：
     CPU_GOVERNOR=""GAME_GRACE_SEC=10
     com.a.gamecom.b.game
   守护的 cfg_get 用 sscanf("%[^=]=%s")、api.sh 的 conf_read 用 sed "s/^KEY=//"，
   两边都按行首找键 —— 粘进去的那个键永远读不到，而回执是 OK、
   页面显示的是默认值，看起来像"改了没保存"。
   装机台架能抓到它（把防护禁掉就会红），所以钉成静态断言。 */
console.log('\n== 追加写入的换行防护 ==');
{
  const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
  ok(/^[^#\n]*\bensure_final_newline\s*\(\)\s*\{/m.test(apiSrc),
     'api.sh 定义了 ensure_final_newline');
  ok(/^[^#\n]*\bensure_final_newline\s*\(\)\s*\{/m.test(cz),
     'customize.sh 也定义了（装机脚本不能只靠 api.sh 那份）');
  // 判据要"每个追加点都被保护"，不是"函数被调用过至少一次"：
  // 只查出现次数的话，给 conf 补了防护、名单那处漏掉，照样是绿的。
  const guarded = (src) => {
    const lines = src.split('\n').filter(l => l.trim() && !l.trim().startsWith('#'));
    const bad = [];
    lines.forEach((l, i) => {
      // 命中"追加重定向"：>> 目标文件（排除 heredoc、排除 printf 到 stdout）
      const app = l.match(/>>\s*"?\$(CONF|LIST|MODES|G_USER|DATA\/[a-z.]+)"?/);
      if (!app) return;
      // 往上看 10 行内有没有对同一个目标补过换行（循环里会加条件分支）
      const ctx = lines.slice(Math.max(0, i - 10), i + 1).join('\n');
      if (!/ensure_final_newline/.test(ctx)) bad.push(l.trim());
    });
    return bad;
  };
  const b1 = guarded(apiSrc), b2 = guarded(cz);
  ok(b1.length === 0, 'api.sh 每个追加点之前都补过换行', b1.join(' ;; '));
  ok(b2.length === 0, 'customize.sh 每个追加点之前都补过换行', b2.join(' ;; '));
  // 包里那份原件必须以换行结尾：它是所有追加的起点，
  // 不带换行就等于给每台机器的第一份用户配置埋了这颗雷。
  ok(/\n$/.test(conf), 'socfreq.conf 以换行结尾（新装机的第一份配置不埋粘行雷）',
     '末字节是 ' + JSON.stringify(conf.slice(-1)));
  ok(/\n$/.test(fs.readFileSync(MOD0 + '/games.txt', 'utf8')),
     'games.txt 同样以换行结尾');
}

/* ── 名单要能跟着版本升级（三方合并）────────────────────────────
   "游戏列表不能实时更新"有两半：白名单覆盖不全，
   以及升级时"别覆盖用户配置"这条规则连带把包里的新条目也挡了。
   解法是留一份 games.base.txt 做基准做差集，这里钉它的存在与语义。 */
console.log('\n== 名单升级通道（games.base.txt 三方合并）==');
{
  const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
  ok(/G_BASE="\$DATA\/games\.base\.txt"/.test(cz), '装机时留下升级基准');
  ok(/grep -Fxv -f "\$DATA\/\.gb" "\$DATA\/\.gn"/.test(cz),
     '新增 = 包里比基准多的（整行精确比对，不是子串）');
  ok(/grep -Fxv -f "\$DATA\/\.gn" "\$DATA\/\.gb"/.test(cz),
     '移除 = 基准比包里多的（上游下架的条目）');
  ok(/: > "\$_del"/.test(cz),
     '没有基准时不做删除（分不清"用户加的"和"上游删的"，宁可多留）');
  ok(/cp -f "\$G_NEW" "\$G_BASE"/.test(cz), '合并完把基准更新成本次包里那份');
  // 注释行不是包名，不能进差集
  ok(/_pk\(\)\s*\{ grep -Ev '\^\[\[:space:\]\]\*\(#\|\$\)'/.test(cz),
     '比对前先滤掉注释与空行（否则整段注释会被当包名追加）');
  // 三个临时文件都必须在 $DATA 里：Android 正常的 root shell 里没有 /tmp，
  // 写过去就是"重定向失败但函数返回成功"那一类静默损坏。
  ok(!/\$\{?TMPDIR\}?|\/tmp\//.test(cz), '装机段不使用 /tmp');
  // 基准也要被卸载清掉（它在数据目录里，跟着整个目录一起删）
  ok(/rm -rf \/data\/adb\/soc_freqcap/.test(fs.readFileSync(MOD0 + '/uninstall.sh', 'utf8')),
     '卸载连基准文件一起删（在数据目录里，整目录删掉即可）');
  // 这条通道要有台架：只有真跑三种装机路径才看得出合并结果对不对
  ok(fs.existsSync(MOD0 + '/src/test_install_merge.sh'),
     '存在 src/test_install_merge.sh（迁移段的台架）');
}

/* ── ① 熄屏策略：新装 uncap、老用户保持 keep ─────────────────── */
console.log('\n== 熄屏轻巡检 / 开机满血 ==');
ok(/^SCREEN_POLICY=uncap$/m.test(conf), '包内 conf 给全新安装写 uncap');
ok(/^SCREEN_UNCAP=0$/m.test(conf), '旧键 SCREEN_UNCAP 仍写 0（只做兼容映射）');
ok(/cfg\.screen_policy = SCREEN_KEEP;/.test(csrc) &&
   /cfg\.soff_interval = 15;/.test(csrc),
   '守护兜底是 keep / 15s：升级用户没有新键时行为不变');
ok(/!uncap_seen \? SCREEN_UNCAP/.test(csrc) &&
   /cfg\.screen_uncap \? SCREEN_UNCAP : SCREEN_KEEP/.test(csrc),
   '没有 SCREEN_POLICY 时旧 SCREEN_UNCAP 照旧映射；两个键都没有才按新装默认 uncap');
ok(/if \(cfg_get\("SCREEN_POLICY", v\) == 0\)[\s\S]{0,220}?uncap[\s\S]{0,120}?keep/.test(csrc),
   'SCREEN_POLICY 只认 uncap / keep（坏值回落旧映射）');
ok(/\[ "\$_k" = SCREEN_POLICY \] && continue/.test(fs.readFileSync(MOD0 + '/customize.sh', 'utf8')),
   '升级不把 SCREEN_POLICY 补进老用户配置（否则老用户会被改成 uncap）');
/* 判断失败必须当"亮屏"：读不到就放开，等于在最不可靠的时候取消所有保护 */
ok(/if \(!got \|\| rc != 0\) return 1;/.test(csrc),
   'screen_awake 读不到时按亮屏处理（宁可照常限频，不要悄悄放开）');
ok(/else if \(cfg\.screen_policy == SCREEN_KEEP && cfg\.screen_uncap &&/.test(csrc),
   '旧 SCREEN_UNCAP 的 boost 分支只在 keep 策略下运行（不和新状态机双跑）');

/* 亮屏信号：实测 fb0/blank 不存在、mSleeping 在锁屏仍为 true，不能当判据 */
ok(/int screen_value_on\(long v, int invert\)/.test(csrc) &&
   /return invert \? \(v == 0\) : \(v > 0\);/.test(csrc),
   'blank 0=亮 / brightness >0=亮 的取值规则是纯函数');
ok(/if \(!g_scr_path\[0\] && !access\("\/sys\/class\/graphics\/fb0\/blank"/.test(csrc) &&
   /opendir\("\/sys\/class\/backlight"\)/.test(csrc),
   '自动挑信号：fb0/blank 优先，其次任意 backlight/brightness');
ok(/mSleeping/.test(csrc) === false,
   '不拿 dumpsys window 的 mSleeping 当熄屏判据（这份 ROM 锁屏亮屏时仍是 true）');
ok(/screen_is_off_now\(verify\)/.test(csrc) && /screen_wake_seen\(\)/.test(csrc),
   'ACTIVE 用 sysfs 提前判熄屏；SCREEN_OFF 用同一信号判亮屏');
ok(/static void enter_screen_off\(void\)/.test(csrc) &&
   /!strcmp\(g_state, "normal"\)[\s\S]{0,200}?hand_back_full\("熄屏解除限频/.test(csrc) &&
   /snprintf\(g_state, sizeof g_state, "full"\)/.test(csrc),
   '熄屏只从常态交还，并标记 state=full');
ok(/static void leave_screen_off\(void\)/.test(csrc) &&
   /!strcmp\(g_state, "full"\)[\s\S]{0,120}?apply_normal\(1\)/.test(csrc),
   '亮屏只把熄屏交还的那一支压回；游戏态保持解锁');
ok(/strcmp\(g_state, "game"\) != 0 && strcmp\(g_state, "full"\) != 0/.test(csrc),
   'reclaim_caps_allowed 同时排除 game/full（熄屏热重载不会抢回节点）');
ok(/cfg\.screen_policy == SCREEN_UNCAP\) \{[\s\S]{0,260}?g_screen_off/.test(csrc),
   '熄屏轻巡检分支在主循环里，早于 adapt_tick/learn_tick/foreground_pkg');
ok(/fprintf\(f, "state=%s\\nscreen=%s\\nsoff=%d\\n/.test(csrc) &&
   /screen=on\\nsoff=15/.test(apiSrc),
   '状态里发布 screen/soff，页面能区分 ACTIVE 与熄屏');
ok(/st === 'full'/.test(html) && /s\.soff/.test(html),
   '页面能渲染 state=full（熄屏轻巡检），不会落到"未运行"');
// 开关落到两个键：新键给守护，旧键留给回退/兼容
ok(/pairs\.push\(\['SCREEN_POLICY', suOn \? 'uncap' : 'keep'\]\)/.test(html) &&
   /pairs\.push\(\['SCREEN_UNCAP', suOn \? '1' : '0'\]\)/.test(html),
   '页面开关同时写 SCREEN_POLICY 和兼容的 SCREEN_UNCAP');
ok(/c\.SCREEN_POLICY === 'uncap'/.test(html) &&
   /c\.SCREEN_POLICY === undefined &&/.test(html) &&
   /c\.SCREEN_UNCAP === undefined \|\| c\.SCREEN_UNCAP === '1'/.test(html),
   '页面读新键；老配置按旧开关显示；两个键都没有时按默认打开');

/* ── P0b：深度空闲 / keep ─────────────────────────────────────── */
console.log('\n== P0b 深度空闲 / 分应用 keep ==');
{
  const P0B = ['IDLE_CURRENT_MA', 'IDLE_HOLD_CYCLE', 'IDLE_INTERVAL',
               'WAKE_DELTA_MA', 'BATT_CURRENT_PATH', 'BATT_CURRENT_UNIT'];
  ok(P0B.every(k => new RegExp('^' + k + '=', 'm').test(conf)), 'conf 写明 6 个 P0b 键');
  ok(P0B.every(k => csrc.includes('"' + k + '"')), '守护逐个读这些键');
  ok(P0B.every(k => apiSrc.includes(k)), 'api.sh 白名单放行这些键');
  ok(/long batt_current_ma\(long raw, int unit_ma\)/.test(csrc) &&
     /long a = raw < 0 \? -raw : raw;/.test(csrc) &&
     /return unit_ma \? a : a \/ 1000;/.test(csrc),
     '电流归一化：先取绝对值，再按 ma/ua 换算');
  ok(/static int read_signed\(const char \*path, long \*out\)/.test(csrc) &&
     /int valid = read_signed\(cfg\.batt_current_path, &raw\);/.test(csrc),
     '电流用有符号读取：充电负值（-404）不能被当成读失败');
  ok(/int idle_step\(long \*ema, int \*hold, long ma, int valid,/.test(csrc) &&
     /if \(!valid\) \{ \*hold = 0; return 0; \}/.test(csrc) &&
     /if \(wake\) \{ \*hold = 0; return 2; \}/.test(csrc),
     '深睡判据是纯函数：读不到不改基线，突增只给唤醒提示');
  ok(/cfg\.idle_current_ma = 250;/.test(csrc) &&
     /cfg\.idle_hold_cycle = 2;/.test(csrc) &&
     /cfg\.idle_interval = 30;/.test(csrc) &&
     /cfg\.wake_delta_ma = 150;/.test(csrc),
     'P0b 默认值 250mA / 2 轮 / 30s / 150mA');
  ok(/g_deep \? cfg\.idle_interval : cfg\.soff_interval/.test(csrc) &&
     /进入深度空闲/.test(csrc),
     '深度空闲切换到 IDLE_INTERVAL，亮屏信号仍每轮读');
  ok(/MODE_KEEP/.test(csrc) && /!strcmp\(val, "keep"\)/.test(csrc),
     '守护能把 pkg=keep 与数字档位区分开');
  ok(/g_soff_keep/.test(csrc) && /mode_lookup\(g_fg, &mpct, NULL\) && mpct == MODE_KEEP/.test(csrc),
     'keep 只认熄屏瞬间的前台包，不交还');
  ok(/if \(has_mode && mpct == MODE_KEEP\) has_mode = 0;/.test(csrc),
     '亮屏时 keep 不改变全局分档（只影响熄屏）');
  ok(/if \(g_soff_keep\) \{[\s\S]{0,120}?caps_patrol\(0\);[\s\S]{0,80}?gpu_patrol\(\)/.test(csrc),
     'keep 熄屏只巡检频率节点，不跑 dumpsys/PSI/学习');
  ok(/soff_keep=%d/.test(csrc) && /screen=deep|g_deep \? "deep"/.test(csrc),
     '状态发布三态与 keep 标记');
  ok(/screen === 'deep'/.test(html) && /熄屏 \? 保持封顶|熄屏 · 保持封顶/.test(html),
     '页面显示深度空闲/熄屏保持封顶');
  ok(/v == "keep" \|\| v == "KEEP"\) \{ skipd\[p\] = 1/.test(apiSrc),
     'advice_app 跳过 keep 条目（学习只写数字）');
  ok(/这条是熄屏保持封顶，学习不改/.test(apiSrc),
     'modes_learn 对 keep 条目报 SKIP');
  ok(/\['keep','熄屏也封顶'\]/.test(html),
     '分应用档位下拉里有「熄屏也封顶」');
}

/* ── P0.5：温控 zone 缓存 / 心跳看门狗 ───────────────────────── */
console.log('\n== P0.5 巡检瘦身 ==');
{
  const adaptSrc = fs.readFileSync(MOD0 + '/src/adapt.c', 'utf8');
  const svc = fs.readFileSync(MOD0 + '/service.sh', 'utf8');
  ok(/#define MAX_THERMAL 128/.test(adaptSrc) &&
     /static char g_thm_paths\[MAX_THERMAL\]\[256\]/.test(adaptSrc) &&
     /static int\s+g_thm_scanned/.test(adaptSrc),
     '温控 zone 路径有缓存数组和扫描标记');
  ok(/static int thermal_scan\(void\)[\s\S]{0,220}?opendir\(g_thm_dir\)/.test(adaptSrc),
     '缓存扫描只在启动/失效时 opendir 一次');
  ok(/if \(!g_thm_scanned\) thermal_scan\(\);/.test(adaptSrc) &&
     /if \(failed\) \{[\s\S]{0,160}?thermal_scan\(\);/.test(adaptSrc),
     '缓存未建或某 zone 读失败才重扫，不每轮扫 87 个 zone');
  ok(/int thermal_type_ok\(const char \*type\)/.test(adaptSrc) &&
     /strstr\(low, "cpu"\) \|\| strstr\(low, "gpu"\) \|\| strstr\(low, "soc"\)/.test(adaptSrc),
     'zone 过滤判据仍是 cpu/gpu/soc，并抽成可测纯函数');
  ok(/strstr\(low, "trip"\)\) return 0/.test(adaptSrc)
     && /thermal_type_ok\("cpu-hw-trip-0"\)/.test(fs.readFileSync(MOD0 + '/src/test_real.c', 'utf8')),
     'trip 阈值节点（cpu-hw-trip-*，恒 95000）被过滤，不当成当前温度');
  ok(/#define HEARTBEAT_FILE RUN_DIR "\/heartbeat"/.test(csrc) &&
     /static void heartbeat\(void\)/.test(csrc) &&
     /heartbeat\(\);/.test(csrc) &&
     /unlink\(HEARTBEAT_FILE\)/.test(csrc),
     '守护每轮写心跳，退出时删除');
  ok(/HB="\$DATA\/run\/heartbeat"/.test(svc) &&
     /stat -c %Y "\$HB"/.test(svc) &&
     /\[ "\$\(\(_now - _mt\)\)" -gt 90 \]/.test(svc),
     '看门狗按心跳 mtime 判 90s 过期（不只看 pid）');
  ok(/sleep 60/.test(svc) && /kill -9 "\$PID"/.test(svc),
     '看门狗 60s 一轮；心跳过期但进程还在时先杀掉再拉起');
}

/* ── P2：电量阶梯 ─────────────────────────────────────────────── */
console.log('\n== P2 电量阶梯 ==');
{
  const adaptC = fs.readFileSync(MOD0 + '/src/adapt.c', 'utf8');
  const P2 = ['BATT_STEP', 'BATT_LOW', 'BATT_LOW_CUT', 'BATT_CRIT', 'BATT_CRIT_CUT'];
  ok(P2.every(k => new RegExp('^' + k + '=', 'm').test(conf)), 'conf 写明 5 个电量阶梯键');
  ok(P2.every(k => csrc.includes('"' + k + '"')), '守护逐个读这些键');
  ok(P2.every(k => apiSrc.includes(k)), 'api.sh 白名单放行这些键');
  ok(/int batt_cut_pct\(int capacity, int charging, const struct adapt_cfg \*c\)/.test(adaptC) &&
     /if \(!c->batt \|\| charging \|\| capacity < 0\) return 0;/.test(adaptC) &&
     /if \(capacity < c->batt_low\) cut \+= c->batt_low_cut;/.test(adaptC) &&
     /if \(capacity < c->batt_crit\) cut \+= c->batt_crit_cut;/.test(adaptC),
     '电量阶梯纯函数：低电/临界两刀、插电/读不到/关闭都不收');
  ok(/a->batt_cut = batt_cut_pct\(capacity, charging, c\);/.test(adaptC) &&
     /d -= a->batt_cut;/.test(adaptC) &&
     /if \(d < -c->down_max\) d = -c->down_max;/.test(adaptC),
     '电量阶梯并入 delta，并共用 ADAPT_DOWN_MAX 总闸');
  ok(/if \(per_app\) d \+= a\.batt_cut;/.test(csrc),
     '分应用 CPU 档不吃电量阶梯（和不吃充电地板同一个口径）');
  ok(/if \(g_app_gpu > 0\) d \+= adapt_batt_cut\(\);/.test(csrc),
     '分应用 GPU 档也不吃电量阶梯');
  ok(/batt=%d\\nbcut=%d/.test(csrc) && /batt=-1\\nbcut=0/.test(apiSrc),
     '状态发布容量与电量收紧量，兜底字段齐全');
  ok(/pairs\.push\(\['BATT_STEP'/.test(html) &&
     /getElementById\('batt_step'\)\.checked = c\.BATT_STEP === '1'/.test(html) &&
     /num\('batt_low',\s*'BATT_LOW',\s*20\)/.test(html),
     '功能页电量开关/阈值可读可存');
  /* 出厂默认关：conf、守护缺键兜底、页面显示三处必须同一口径。
     只改一处会出现"页面关着、守护还在压"或反过来。 */
  ok(/^BATT_STEP=0$/m.test(conf), 'socfreq.conf 里电量阶梯默认 0');
  ok(/cfg\.ad\.batt = 0;/.test(csrc) &&
     /cfg\.ad\.batt = \(!strcmp\(v, "1"\) \|\| !strcasecmp\(v, "true"\)\) \? 1 : 0;/.test(csrc),
     '守护缺键时按关处理（只有显式 1/true 才开）');
  ok(/BATT_LOW\)[\s\S]{0,120}?BATT_ORDER/.test(apiSrc) &&
     /BATT_CRIT\)[\s\S]{0,120}?BATT_ORDER/.test(apiSrc),
     'API 双向拦住 BATT_CRIT >= BATT_LOW');
}

/* ── 出厂默认的五项行为 ───────────────────────────────────────── */
console.log('\n== 出厂默认（熄屏交还 / 充电不封顶 / 电量阶梯关 / 温控适中 / PSI 适中） ==');
{
  ok(/^SCREEN_POLICY=uncap$/m.test(conf), '熄屏交还系统：新装 conf 是 uncap');
  ok(/!uncap_seen \? SCREEN_UNCAP/.test(csrc) &&
     /\(c\.SCREEN_UNCAP === undefined \|\| c\.SCREEN_UNCAP === '1'\)/.test(html),
     '两个熄屏键都没有时，守护和页面都按 uncap（显式 keep 不受影响）');
  ok(/^CHG_UNCAP=1$/m.test(conf) && /^CHG_PCT=100$/m.test(conf),
     '充电态默认不封顶');
  ok(/cfg\.ad\.chg = 1;\s+cfg\.ad\.chg_pct = 100;/.test(csrc),
     '守护缺键兜底也是充电不封顶');
  ok(/^TH_CAP=1$/m.test(conf) && /^TH_HIGH=75$/m.test(conf) &&
     /^TH_LOW=68$/m.test(conf) && /^TH_MAX=25$/m.test(conf),
     '降速时机默认「适中」（75/68/压 25）');
  ok(/^PSI_ADAPT=1$/m.test(conf) && /^PSI_GAIN=10$/m.test(conf),
     '真的卡的时候默认「适中」（借 10）');
  for (const f of ['adapt/sm8750.conf', 'adapt/sm8850.conf']) {
    const a = fs.readFileSync(MOD0 + '/' + f, 'utf8');
    ok(/^default_th_high=75$/m.test(a) && /^default_th_low=68$/m.test(a) &&
       /^default_psi_gain=10$/m.test(a),
       f.split('/')[1] + ' 新装默认也落在「适中」这一档');
  }
}

/* ── ④ 开机满血：窗口由守护判定，不由 service.sh 写 sysfs ─────── */
ok(/^BOOT_FULL_S=60$/m.test(conf), 'socfreq.conf 里 BOOT_FULL_S 默认 60');
ok(/clampi\(atoi\(v\), 0, 600, 60\)/.test(csrc), 'BOOT_FULL_S 区间 0~600，兜底 60');
ok(/BOOT_BOOST_FILE RUN_DIR "\/boot_boost_until"/.test(csrc),
   '满血窗口的截止时间戳在 run/ 下');
{
  const sv = fs.readFileSync(MOD0 + '/service.sh', 'utf8');
  ok(/boot_boost_until/.test(sv) && /\/proc\/uptime/.test(sv),
     'service.sh 写的是 uptime 口径的截止时间（不用墙钟，开机阶段会跳）');
  /* service.sh 跑得很早，cpufreq 节点未必就绪；它直接写 sysfs 失败还是静默的。
     所以它只许留时间戳，交权和压回都在守护里做。 */
  ok(!/scaling_max_freq/.test(sv), 'service.sh 不直接写 sysfs（节点可能还没就绪）');
  ok(/if \(cfg\.boot_full_s > 0 && boot_boost_active\(\)\)/.test(csrc),
     '守护在窗口内交权；到点由 boot_boost_active 自己删时间戳');
}
ok(/strcmp\(g_reason, "game"\)/.test(csrc),
   '满血窗口结束直接压回，不吃游戏那套退出宽限');
ok(/reason=%s/.test(csrc) && /snprintf\(g_reason/.test(csrc),
   '交权原因单独发布（state 仍只有 normal|game，前端不用改判断链）');

/* ---- CPU 簇键格式 ---- */
console.log('\n== CPU 各簇键 ==');
ok(/printf\("cpu_%s=%ld\\n", p\[i\]\.name \+ 6/.test(csrc), 'cpu_%s 去掉 policy 前缀');
ok(/printf\("cpucur_%s=/.test(csrc), 'cpucur_%s 键名一致');
ok(/\^cpu_\\d\+\$/.test(html), 'WebUI 用 cpu_数字 过滤');
ok(html.includes("s['cpucur_'"), 'WebUI 用 cpucur_ 前缀取当前值');

/* ---- 主循环巡检路径：稳态必须真的会压回 CPU 上限 ---- */
console.log('\n== 稳态巡检可达性 ==');
// apply_normal 的调用点只有重载/启动/游戏复位三个时，
// 稳态分支就只巡检 GPU，厂商把 scaling_max_freq 顶回去后没人管。
// 这类"编译得过但执行不到"的路径光看代码看不出来，
// 必须显式断言调用点存在。
const mainLoop = (csrc.match(/for \(; !g_stop;\) \{[\s\S]*\n\}/) || [''])[0];
ok(mainLoop.length > 0, '找到主循环');
/* 稳态巡检现在传的是 caps_changed || adapt_changed（换档、或实时偏移变了
 * 那一轮为 1，其余为 0）。
 * 这里不能钉字面量 apply_normal(0)：它换成变量之后只会报"调用点没了"，
 * 而真正要保证的是两件事——巡检这一路仍在、且主循环里不许再写死 0。
 * 写死 0 的话，从 70% 的应用切到 80% 的应用时 cur<=cap 直接跳过，
 * 节点永远停在旧应用的下限上。 */
ok(/missing_since = 0;[\s\S]{0,900}apply_normal\(caps_changed \|\| adapt_changed\)/.test(mainLoop),
   '稳态分支仍在巡检 CPU 上限（厂商顶回来会被压回）');
ok(csrc.includes('int caps_changed = (want_pct != g_app_pct);'),
   'force 的来源是"档位换没换"，不是写死的 0 或 1');
ok(!/apply_normal\(0\)/.test(mainLoop), '主循环没有残留的写死 apply_normal(0)',
   (mainLoop.match(/apply_normal\([^)]*\)/g) || []).join(' , '));
const calls = (csrc.match(/apply_normal\([^)]*\)/g) || []);
ok(calls.length >= 4, `apply_normal 至少 4 个调用点 (实际 ${calls.length})`);
ok(calls.includes('apply_normal(1)'), '存在 apply_normal(1)（启动/重载/复位）');

// perms_restore 必须取代 journal_restore(PERM_JOURNAL)：
// 权限 journal 的值列是占位 0，走数值还原会往节点写 0
ok(!/journal_restore\(PERM_JOURNAL\)/.test(csrc),
   '不再用 journal_restore 还原权限 journal');
ok(csrc.includes('static int perms_restore'), '存在 perms_restore');
const permsCalls = (csrc.match(/perms_restore\(\)/g) || []).length;
ok(permsCalls >= 3, `perms_restore 三处调用（常态/cleanup/启动）实际 ${permsCalls}`);

// pause 自查：主循环必须自己看 pause，不能只靠看门狗
ok(/for \(; !g_stop;\) \{[\s\S]{0,300}access\(PAUSE_FILE/.test(csrc),
   '主循环自查 pause 文件');

// dumpsys 必须套 timeout，否则卡住时整轮停摆
ok(csrc.includes('timeout 2 dumpsys'), 'dumpsys 套了 timeout');

// log_open 不能截断日志（uninstall 也会调它）。
// 先剥掉注释再判，否则注释里提到 O_TRUNC 会被当成命中。
const logOpenFn = (csrc.match(/static void log_open[\s\S]*?\n\}/)?.[0] || '')
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/\/\/[^\n]*/g, '');
ok(!logOpenFn.includes('O_TRUNC'), 'log_open 不使用 O_TRUNC');
ok(/O_APPEND/.test(logOpenFn), 'log_open 使用 O_APPEND');

/* ---- 安装期权限：api.sh 必须可执行 ---- */
console.log('\n== 安装期权限 ==');
const path = require('path');
const MODDIR = path.join(__dirname, '..'); // 路径里有中文，别用 file:// URL（会被百分号编码）
const SMOKE = path.join(MODDIR, 'tools', 'smoke_ondevice.sh');
const cust = fs.readFileSync(path.join(MODDIR, 'customize.sh'), 'utf8');
// set_perm_recursive 会把整棵树刷成 0644，WebUI 若直接 ./api.sh 调用就会 permission denied，
// 表现为"无法连接守护"。所以 api.sh 必须被显式改回 0755。
ok(/set_perm\s+"\$MODPATH\/webroot\/api\.sh"\s+0 0 0755/.test(cust),
   'api.sh 被显式设为 0755（set_perm_recursive 会刷成 0644）');

/* ---- KernelSU 热安装：装完不用重启，直接打开 WebUI ---- */
{
  const hot = fs.readFileSync(path.join(MODDIR, 'scripts', 'hotinstall.sh'), 'utf8');
  ok(/export MODULE_HOT_INSTALL_REQUEST=true/.test(cust) &&
     /export MODULE_HOT_RUN_SCRIPT=scripts\/hotinstall\.sh/.test(cust),
     'customize.sh 申请 KernelSU 热安装（模块文件/WebUI 立即生效）');
  ok(/set_perm\s+"\$MODPATH\/scripts\/hotinstall\.sh"\s+0 0 0755/.test(cust),
     '热安装脚本显式 0755（安装后由 KernelSU 执行）');
  ok(/"\$BIN" restart/.test(hot) && /"\$BIN" stop/.test(hot) &&
     /if \[ -f "\$PAUSE" \]/.test(hot) && /deep_sleep\.sh/.test(hot),
     '热安装脚本：暂停保持、否则 restart 主守护并按开关重启深睡');
  ok(/\[ -f "\$MODDIR\/remove" \] && exit 0/.test(fs.readFileSync(MOD0 + '/service.sh', 'utf8')),
     'service.sh 看到 remove 标记就不启动（卸载到重启之间不再写频率）');
  ok(/\[ -f "\$MODDIR\/remove" \]/.test(hot) && /跳过启动/.test(hot),
     '热安装脚本也尊重 remove 标记');
}
// api.sh 里用 -f 而不是 -x：少了执行位时 sh 自己会带解释器跑，
// 用 -x 会在守护明明存在时直接报"守护程序不存在"。
const apiBody = fs.readFileSync(path.join(MODDIR, 'webroot', 'api.sh'), 'utf8');
ok(!/\[\s*-x\s+"\$DAEMON"\s*\]/.test(apiBody), 'api.sh 不用 -x 判断守护存在');

/* ---- action.sh 不能在无交互环境里阻塞按键读取 ---- */
console.log('\n== action.sh 非交互安全 ==');

/* ---- service.sh 必须后台跑并立刻退出 ---- */
console.log('\n== service.sh 不阻塞后续模块 ==');
const svc = fs.readFileSync(path.join(MODDIR, 'service.sh'), 'utf8');
// Magisk/KernelSU 同步执行 service.sh。前台 while true 会把排在后面的
// 所有模块 service 一起卡死，表现为"装完就触发救砖模块"。
ok(/^\s*\(\s*$/m.test(svc) || /\(\s*\n\s*while true; do/.test(svc), '看门狗放在子 shell 里');
ok(/while true; do[\s\S]*?\n\s*done\s*\n\)\s*&/.test(svc), '看门狗后台运行（ & ）');
ok(/\n\s*exit 0\s*$/.test(svc.trimEnd()), 'service.sh 以 exit 0 结束');
// 剥掉注释再判，否则注释里提到 while 会被误判成裸 while。
// 子 shell 里的 while 前面有缩进且上一行是单独的 "("，裸的则顶格。
const svcCode = stripSh(svc);
// 只把 while true 挪进子 shell 是不够的：前面那段等开机
// （boot_completed 轮询，最长 150×2+5 秒）留在前台照样占住 runner。
// 子 shell 之前不允许出现任何前台等待。
const svcPre = svcCode.split(/^\(\s*$/m)[0] || '';
ok(!/^\s*(while|sleep|read)\b/m.test(svcPre),
   '子 shell 之前没有前台等待/阻塞',
   svcPre.trim().split('\n').slice(-2).join(' | '));
const sub = (svcCode.match(/^\(\s*([\s\S]*)\)\s*&/m) || [])[1] || '';
ok(/boot_completed/.test(sub), '开机等待已挪进子 shell');
ok(/while true/.test(sub), '看门狗也在同一个子 shell 里');

/* ---- action.sh 的音量键交互 ----
 * 这里钉的不是"action.sh 不能读 getevent"，而是四道护栏：
 * 管理器点「执行」时 stdin 根本不是终端，音量键是唯一输入；
 * 要防的从来不是"读按键"，而是"读不到按键时还在等"。
 * 缺一条护栏就是"点执行手机卡住"。 */
console.log('\n== action.sh 输入方式（音量键）==');
const act = fs.readFileSync(path.join(MODDIR, 'action.sh'), 'utf8');
const actCode = stripSh(act);
{
  // 1) 每一次 getevent 读数都必须在 timeout 里
  const bare = (actCode.match(/^[^\n]*getevent[^\n]*$/gm) || [])
    .filter(l => /getevent\s+-lqc/.test(l) && !/timeout/.test(l));
  ok(bare.length === 0, '每一次按键读数都套了 timeout（没事件时不会永远阻塞）',
     bare.join(' ;; '));

  // 2) 进循环之前只查"命令在不在"，**不许拿列表选项当闸门**。
  //    真机（一加 PJZ110）上 getevent 没有 -e：`getevent -le` 打印 usage 后失败，
  //    据此判"这台机读不到音量键"，会让一台音量键好好的机器
  //    永远只能看到那一句提示。防卡死靠的是每次读数套 timeout，
  //    不是靠探测——探测本身也会坏。
  {
    const body = (act.match(/can_use_keys\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(!!body, 'can_use_keys 函数体找得到（摘不出来就没法逐项判）',
       body ? '' : '正则没匹配到顶格的 } 收尾');
    for (const [k, why] of [
      ['command -v getevent', '命令本身要在'],
      ['command -v timeout', '没有 timeout 就等于没有超时保护'],
      ['-r "$API"', 'api.sh 读不到就别进循环（后面每一步都要调它）'],
    ]) ok(body.includes(k), `can_use_keys 检查了 ${why}`, k);
    ok(!/getevent\s+-[a-z]*e/.test(body),
       'can_use_keys 不再用 getevent 的列表选项判能力（toybox 没有 -e）',
       (body.match(/getevent[^\n]*/g) || []).join(' , '));
    ok(!/KEY_VOLUME/.test(body),
       '能力检查里不出现按键名探测（那种探测一失败就把功能整个关掉）');
  }
  ok(/if ! can_use_keys; then[\s\S]{0,900}?exit 0/.test(actCode),
     '探不到按键时打印等价命令后立刻退出，不进循环');
  ok(!/你这台机.*没有音量键|读不到音量键/.test(actCode),
     '回退文案不能断言"机器没有音量键"（我们只是没探测成功）',
     (actCode.match(/[^\n]*音量键[^\n]*/) || [''])[0].trim());
  // 打印出来的命令必须是能照抄执行的（含数据目录路径），
  // 否则这一支只是"我不支持"，用户还是什么都没得到
  ok(/conf_set/.test(actCode) && /\$DATA\/pause/.test(actCode),
     '回退分支给的是能照抄的完整命令（conf_set / pause 路径）');

  // 3) 两个等待点都要有超时，且确认阶段的默认方向是"取消"
  const waits = (actCode.match(/key_wait\s+\S+/g) || []);
  ok(waits.length >= 2, `菜单和确认两处都在等按键（实到 ${waits.length} 处）`);
  ok(/case "\$C" in[\s\S]*?up.*取消[\s\S]*?\*\).*取消/s.test(actCode)
     || /timeout\s*取消|超时取消/.test(act),
     '二次确认超时的默认方向是取消（不是执行）');

  // 4) 松手（UP）不能算第二次按键
  ok(!/KEY_VOLUME[UD]P\*?\)?\s*\*\s*UP/.test(actCode)
     && /KEY_VOLUMEUP\*DOWN\*\)/.test(actCode) && /KEY_VOLUMEDOWN\*DOWN\*\)/.test(actCode),
     '只认 DOWN，UP 事件被丢掉（一次按下算一回合）',
     (actCode.match(/\*KEY_VOLUME\S*\)/g) || []).join(' , '));

  /* 模块目录必须按"最后一个斜杠"切，并且处理 $0 不带斜杠的情况。
   * 真机撞到的：`cd 到模块目录; sh action.sh` 时 $0="action.sh"，
   * ${0%/*} 什么都剥不掉 → MODDIR="action.sh" → api.sh 判成读不到 →
   * 按键能力检查失败 → 屏幕上留下一句"这台机缺少 getevent"的假话。
   * api.sh 里有这一条归一化，action.sh 必须有同一条。 */
  ok(/_self=\$0\s*case "\$_self" in[\s\S]{0,160}\*\)\s+MODDIR=\$\(pwd\)/.test(actCode),
     'action.sh 归一化 $0（不带斜杠时用 pwd），MODDIR 不会变成文件名本身',
     (actCode.match(/MODDIR=[^\n]*/) || [''])[0]);
  ok(!/^MODDIR=\$\{0%\/\*\}\s*$/m.test(actCode),
     '不能是裸 ${0%/*}（那条会报出假话）');

  // 5) 改动类操作走 api.sh，不在命令行里自己写 conf
  ok(!/>+\s*"\$?(CONF|DATA\/socfreq\.conf)"/.test(actCode),
     'action.sh 不直接写配置文件（校验只有 api.sh 那一份实现）');
  ok(/api conf_set/.test(actCode) && /api pause/.test(actCode) && /api resume/.test(actCode),
     '加减封顶 / 暂停 / 恢复都转给 api.sh');

  // 6) 只有 action.sh 可以读按键：装机与开机路径一次都不行
  for (const [n, src] of [['customize.sh', cust],
                          ['service.sh', svc],
                          ['post-fs-data.sh',
                           fs.readFileSync(path.join(MODDIR, 'post-fs-data.sh'), 'utf8')]]) {
    ok(!/getevent/.test(stripSh(src)), n + ' 不读按键（阻塞会拖死 runner）');
  }
  /* 空闲窗口要够长，而且"任何按键"都算活动。
   * 用户实测反馈："我按了按钮，它还说是未检测到操作就退出了" ——
   * 只让音量键重置计时的话，先按电源键亮屏、或误按别的键都被丢掉，
   * 时钟照走，慢慢看完八行菜单就"超时退出"。 */
  ok(/K=\$\(key_wait 90\)/.test(actCode) && /C=\$\(key_wait 20\)/.test(actCode),
     '菜单 90 秒、二次确认 20 秒（30/10 秒在手机上读八行菜单根本不够）',
     (actCode.match(/key_wait \d+/g) || []).join(' , '));
  ok(/\*EV_KEY\*\)\s+echo "other";\s+return/.test(actCode),
     '别的实体键算"有人在动"，返回 other 让调用处重新计时');
  ok(/other\)\s+continue/.test(actCode), '菜单阶段收到 other 是重新等，不是退出');
  ok(/\[ "\$C" = "other" \] && continue/.test(actCode),
     '确认阶段收到 other 重新问一次（仍然只有音量－ 会真的执行）');
  ok(!/EV_ABS|ABS_MT/.test((actCode.match(/case "\$_ev" in[\s\S]*?esac/) || [''])[0]),
     '触摸屏事件不算活动（口袋误触会让这个界面永远不退出）');
  ok(/90 秒无按键自动退出/.test(actCode) && /20 秒不按键 = 取消/.test(actCode),
     '屏幕上写的等待时长和代码里的一致（写 30 秒实际等 90 秒也是骗人）');

  ok(fs.existsSync(path.join(MODDIR, 'src/test_action_keys.sh')),
     '音量键状态机有台架（假 getevent 喂剧本）');

  /* 管理器那块「执行」输出是只能往下追加的文本区，不是终端。
   * 每次按键都重打一遍八项菜单的话：按八下就是八屏重复菜单，
   * 选到哪一行了反而要往下翻。现在钉成：
   *   完整菜单只在进循环前打一次；
   *   每次移动只补一行当前项；
   *   真终端（[ -t 1 ]）才走 ANSI 整屏重画。 */
  {
    // print_menu 和 while 之间夹着注释和一行自证日志（atrace "菜单已画出…"），
    // 而这里的 actCode 早就把注释剥掉了 —— 剩下的就是空行和那行 atrace。
    // 用"找位置"而不是"一条巨型正则"：锚点排不掉新代码，
    // 锚点不能写死成 print_menu\nwhile，结果 atrace 一加进来，
    // 两条断言同时变成"没匹配到"，看着像功能坏了。
    const pm = actCode.indexOf('print_menu\n');
    const ws = actCode.indexOf('while :; do', pm);
    const ds = actCode.indexOf('\ndone', ws);
    const loop = (pm >= 0 && ws > pm && ds > ws) ? actCode.slice(ws, ds + 5) : '';
    ok(!!loop, '菜单循环的锚点找得到（print_menu 之后进 while）',
       loop ? '循环长度 ' + loop.length : '没匹配到');
    ok(!/while \[ "\$_i" -le "\$NITEMS" \][\s\S]*?done\n\n?\s*K=\$\(key_wait/.test(actCode),
       '循环体里不再整屏重打菜单');
    ok(/say "->?\s?\$CUR \$\(label "\$CUR"\)"|say "→ \$CUR \$\(label "\$CUR"\)"/.test(loop),
       '每次移动只补一行当前项',
       (loop.match(/say "[^"]*CUR[^"]*"/g) || []).join(' , '));
    // 用 includes 而不是正则：源码里就是 \033 这四个字符，
    // 正则里再写 \\033 变成"反斜杠+033"，永远匹配不上（只会假红）
    ok(actCode.includes("if [ -t 1 ]; then") &&
       actCode.includes("printf '\\033[H\\033[2J'; print_menu"),
       '只有真终端才 ANSI 整屏重画（管理器的文本区不认转义序列）',
       (actCode.match(/printf [^\n]*33[^\n]*/) || []).join(' , '));
    ok((actCode.match(/print_menu\n/g) || []).length <= 2,
       'print_menu 不在每次按键的通用路径上被调');
  }
}

/* ---- customize.sh 不对不存在的路径 set_perm ---- */
console.log('\n== 安装脚本路径 ==');
ok(!/set_perm\s+"\$MODPATH\/run"/.test(cust), '不对包里不存在的 run/ set_perm');

/* ---- WebUI 两处实测 bug ---- */
console.log('\n== WebUI 交互 ==');
const ui = fs.readFileSync(path.join(MODDIR, 'webroot', 'index.html'), 'utf8');
ok(/\^CPU_PCT_\/\.test\(id\)/.test(ui) || /\^CPU_PCT_/.test(ui),
   'CPU 滑杆按 % 显示（gpu_pct 之外的 CPU 簇不能显示成秒）');
ok(!/onclick="delGame\('\s*\+/.test(ui), '删除按钮不拼 onclick（双引号会截断属性）');
ok(/dataset\.pkg/.test(ui) && /querySelectorAll\('button\.rm'\)/.test(ui),
   '删除按钮改用 data-pkg + 事件绑定');
ok(ui.includes('kernelsu'), '支持新版 KernelSU 的 kernelsu 桥');
ok(ui.includes('window.kernelsu') || /typeof kernelsu/.test(ui), 'kernelsu 在 ksu 之前探测');

/* ---- 桥接失败时的常驻提示 ----
 * 用户提的要求是原话："桥接探测失败时，WebUI 顶部直接显示
 * 当前管理器不支持 WebUI，请用 action.sh 命令行操作"。
 * 措辞是指定的，所以这里逐字比；行为上还要"藏标签页 + 停轮询"，
 * 以及和"桥通但 api.sh 找不到"分两种文案 —— 处置方式完全相反。 */
console.log('\n== WebUI 桥接失败提示 ==');
{
  ok(/function setDead\(kind, detail\)\{/.test(ui), '有 setDead() 这个统一出口');
  ok(ui.includes('当前管理器不支持 WebUI，请用 action.sh 命令行操作'),
     '措辞与要求逐字一致');
  ok(/setDead\('nobridge'\)/.test(ui), 'exec() 四个桥都没有时置位 nobridge');
  ok(/setDead\('noapi'\)/.test(ui),  '探不到 api.sh 时置位 noapi（两种失败要分开）');
  ok(/root 接口正常，但找不到模块的 api\.sh/.test(ui),
     'noapi 的文案说的是路径问题');
  ok(/getElementById\('tabbar'\)\.style\.display = 'none'/.test(ui),
     '失败后把标签页藏起来（底下全是拿不到数据的空卡片）');
  ok(/timers\.push\(setInterval/.test(ui) && /timers\.forEach\(id=>clearInterval\(id\)\)/.test(ui),
     '轮询句柄登记在 timers 里，失败时统一停掉');
  ok(/if\(DEAD\) return;/.test(ui),
     'toast() 在已判定不可用时不再刷屏（否则每 5 秒跳一条同样的错）');

  /* 同一个 id 出现两次是静默 bug：getElementById 只认第一处，
   * 后一处永远停在 HTML 里写死的初值。版本号在副标题和
   * 首页底部各有一个 span，都叫 ver，于是守护升到别的版本，
   * 底部那行还是 V1.0。 */
  const ids = (ui.match(/\sid="[\w-]+"/g) || []).map(s => s.slice(5, -1));
  const dup = ids.filter((v, i) => ids.indexOf(v) !== i);
  ok(dup.length === 0, 'index.html 里没有重复的 id', [...new Set(dup)].join(','));
}

console.log('\n== 默认值 ==');
// 三处必须一致：守护的 fallback、conf 文件、WebUI 滑杆默认值。
// 只改一处的话，删掉 conf 或写坏一个键，行为会静默变成另一个数，
// 而且用户看不出"我装的是 55 还是 66"。
{
  const DF = {CPU_PCT_BIG:65, CPU_PCT_MID2:64, CPU_PCT_MID:66, CPU_PCT_LITTLE:65,
              GPU_PCT:45, INTERVAL:5, GAME_GRACE_SEC:10};
  for (const [k,v] of Object.entries(DF)) {
    ok(new RegExp('^' + k + '=' + v + '\\s*$', 'm').test(conf), 'conf 里 ' + k + '=' + v);
    if (/^CPU_PCT_/.test(k)) {
      ok(new RegExp("k:'" + k + "'.{0,40}d:" + v).test(html),
         'WebUI 滑杆默认 ' + k + '=' + v);
    }
  }
  // 守护侧的 fallback 整体验证：四个整数按 BIG,MID2,MID,LITTLE 排
  ok(/dflt\[4\]\s*=\s*\{65,\s*64,\s*66,\s*65\}/.test(csrc),
     '守护四档 fallback {65,64,66,65}',
     (csrc.match(/dflt\[4\]\s*=\s*\{[^}]*\}/) || [''])[0]);
  ok(/cfg\.gpu_pct\s*=\s*45;/.test(csrc), '守护 GPU fallback 45');
  ok(/cfg\.interval\s*=\s*5;/.test(csrc), '守护 INTERVAL fallback 5');
  ok(/cfg\.grace\s*=\s*10;/.test(csrc), '守护 GAME_GRACE fallback 10');

  /* ── 滑杆的手势与热区（新拟物改版时最容易顺手改回去）────────────
     竖着滑列表时误改封顶值，根因是 range 的触摸热区太厚、
     又没声明手势方向。touch-action:pan-y 才是真修法：
     竖向位移交给页面滚动，横向才改值。只把滑杆调细是治标。 */
  {
    const css = (html.match(/<style>([\s\S]*?)<\/style>/) || ['', ''])[1];
    ok(/input\[type=range\]\{[^}]*touch-action:\s*pan-y/.test(css),
       '滑杆声明 touch-action:pan-y（竖滑滚页面、横滑才改值）');
    const hz = (css.match(/input\[type=range\]\{[^}]*?height:\s*(\d+)px/) || [])[1];
    ok(!!hz && +hz <= 22, '滑杆热区高度收到 22px 以内', hz ? hz + 'px' : '没找到 height');
    /* 新拟物的判据不是"好不好看"，而是这几条硬约束：
       卡片和背景同色、靠阴影塑形、不用硬描边。
       哪天有人加回 border:1px 描边，卡片就会从"浮在板上"变成"贴了个框"。 */
    ok(!/\.card\{[^}]*border:\s*1px/.test(css), '.card 不用硬描边（新拟物靠阴影塑形）');
    ok(/box-shadow:var\(--up\)/.test(css), '卡片用 --up 双层软阴影（左上光源）');
    ok(/--lt:[\s\S]*?--sh:/.test(css), '亮/暗两层阴影色都定义了');

    /* 滑杆的"误按"防线。原生 range 是"手指落在哪，值跳到哪"，
       在手机上等于滚个页面就把封顶改了，而且没有任何提示。
       pan-y 只管滚动方向，管不了误按，所以另有两道：
       preventDefault + 记下原值在 input 里回滚。 */
    ok(/const GRAB_PX = \d+;/.test(html), '滑杆有明确的抓取容差（GRAB_PX）');
    ok(/matches\('input\[type=range\]'\)/.test(html), '误按拦截只针对 range，不拦别的控件');
    ok(/e\.pointerType === 'mouse'\) return/.test(html),
       '鼠标不拦（桌面调试要点轨道跳转，而且指针精度够）');
    ok(/el\._reject = el\.value;/.test(html) && /el\.value = el\._reject;/.test(html),
       '两道保险：preventDefault 之外还记原值、input 里回滚（各家 WebView 对 range 默认行为不一致）');
    ok(/delete el\._reject;\s*\n\s*pv\(el\.id\)/.test(html),
       '回滚后把显示值一起改回来（只回滚 input 不回滚显示，数字停在错的上面）');
  }
  // README 里那几行示例是用户第一眼看到的对照。
  // 示例数字和实际显示不一致的话，用户按 README 对不上号，
  // 会以为是算错了而不是文档过期。
  {
    const rd = DOC;
    ok(/1901 MHz\s*→\s*1292 MHz/.test(rd), 'README 示例：1901 → 1292（不是 1293）');
    ok(/3187 MHz\s*→\s*2103 MHz/.test(rd), 'README 示例：3187 → 2103（不是 2104）');
    ok(!/→\s*1293 MHz/.test(rd), 'README 没有向上溢出的封顶值');
    // README 里给的 test_contract 命令要真能跑。
    // 少一个参数的话，用户照着敲会直接 ERR_INVALID_ARG_TYPE 崩掉。
    // 命令在 README 里折了两行，正则要跨行数，别只取第一行。
    const cmd = (rd.match(/node src\/test_contract\.js[\s\S]*?games\.txt/) || [])[0] || '';
    const argc = (cmd.match(/src\/socfreqd\.c|webroot\/index\.html|socfreq\.conf|webroot\/api\.sh|README\.md|games\.txt/g) || []).length;
    ok(argc >= 6, 'README 的 test_contract 命令带齐 6 个参数（少一个就崩）',
       '只写了 ' + argc + ' 个');
    // README 说的参数个数，要和 ARGS 里真正必填的个数一致。
    // 曾经对不上：README 写"要八个"，实际只有七个必填，
    // 外部用户照着敲，第一步就 exit 2。
    //
    // 不去解析中文数字——栽过：[一二三四五六七八九十] 这个类里
    // 并不含量词"个"，而 README 写的是"七个"，正则一次都没命中过，
    // 断言看着在生效，其实是个永远为假的空壳。
    // 改成直接盯住真正的症状：有没有把第 8 个说成必填。
    // 匹配式要容得下量词：README 写的是"八个"，不是"八"。
    // 少写那个 (个)? 会让这条断言匹配不上真实文本，
    // 于是变异照样"存活"——看起来是没覆盖，其实是写错了。
    ok(!/要\*\*八(个)?\*\*参数/.test(rd),
       'README 没把比对基准算进必填参数',
       'README 声称要八个，但必填的只有 ' + ARGS.length + ' 个');
    // 末尾的比对基准必须是可选的：它不随本包发布，
    // 写成必填就等于把自检锁在作者自己机器上。
    ok(/\[比对基准脚本\]|<比对基准脚本>/.test(rd), 'README 把比对基准标成可选参数');

    // 冒烟示例必须带 sh。adb push 推过去的是 0644，没有执行位，
    // 直接 su -c '/data/local/tmp/smoke_ondevice.sh' 会被内核拒掉，
    // 报 Permission denied——而且是在第一条检查之前，
    // 看着像脚本坏了，实际一条都没跑到。
    // 只认代码块里的命令行，散文里提到这个写法的（正是在警告别这么用）不算。
    const smokeCmds = [...rd.matchAll(/^\s*su -c '([^']*smoke_ondevice[^']*)'/gm)].map(m => m[1]);
    ok(smokeCmds.length > 0, 'README 里有冒烟示例',
       '没找到 su -c ... smoke_ondevice');
    for (const c of smokeCmds) {
      ok(/^sh\s+\/data\/local\/tmp\/smoke_ondevice\.sh/.test(c),
         '冒烟示例用 sh 解释执行（push 过去没有执行位）',
         '这句是: su -c \'' + c + '\'');
    }

    /* smoke_ondevice.sh 的一致性检查。
     * 冒烟脚本不在参数表里（它不在 src/），路径从 __dirname 推。 */
    // 中文数字表要一直往后加：冒烟脚本每加一节，头注释和 README 那两个
    // "只查 N 件事"就得跟着变，而表里缺了"七"的时候断言读到的是 0，
    // 报出来像"文档没跟上"，其实坏的是这张表自己。
    const CN = { '一': 1, '二': 2, '三': 3, '四': 4, '五': 5, '六': 6,
                 '七': 7, '八': 8, '九': 9, '十': 10 };
    const smokeTxt = fs.readFileSync(SMOKE, 'utf8');
    const smokeCode = smokeTxt.split('\n')
      .filter(l => l.trim() && !l.trim().startsWith('#')).join('\n');
    const secs = [...smokeTxt.matchAll(/^echo "== \d+\./gm)].length;

    /* 冒烟脚本里凡是调 socfreqd 的地方，必须直接执行、不能套 sh。
     * shell 对"脚本操作数"是读进来当命令解析，不是 execve，
     * 拿 ELF 去解析只会得到 cannot execute binary file，stdout 一行都没有。
     * 症状是后面几条判据恒空——代码全对也报 FAIL，看起来像坏了，
     * 实际是从没执行过。这个错在 PC 上测不出来，只能查调用形式。
     * api.sh 是真脚本，那几处 sh "$API" 保留是对的，不能一起改掉。 */
    {
      ok(!/(?:^|[|;&(]|\$\()\s*(?:sh|bash|dash|ash)\s+["']?[^\s"']*socfreqd/m.test(smokeCode),
         '冒烟脚本没有用 sh 去跑 socfreqd（ELF 要直接执行）',
         (smokeCode.match(/(?:^|[|;&(]|\$\()\s*(?:sh|bash|dash|ash)\s+[^\n]*socfreqd/m) || [''])[0]);
    }
    {
      const n = (smokeCode.match(/"\$D\/socfreqd"\s+(export|status|version|icon|uninstall)/g) || []).length;
      ok(n >= 4, `socfreqd 直接执行 ${n} 处（至少 4）`);
      ok((smokeCode.match(/sh\s+"\$API"/g) || []).length >= 5,
         'api.sh 仍用 sh 解释执行（那是脚本，保留才对）');
      // 头注释里"几件事"必须等于实际节数，不然读者按注释数会漏看
      const says = smokeTxt.match(/只查(\S+?)件事/);
      ok(says && CN[says[1]] === secs,
         `头注释的"只查${says ? says[1] : '?'}件事"与实际 ${secs} 节一致`);
    }

    /* README 里"只查几件事"那句必须跟着脚本一起变。
     * 这类数字每加一节就腐烂一次：脚本加了第 4 节（逐核频率表），
     * 脚本头和 README 两处却还写着"四件事"，读者按注释数会以为查漏了。
     * 不比对具体措辞，只比两边的节数——以后加到第七节也不会再失效。 */
    {
      const dSecs = [...rd.matchAll(/只查(\S+?)件事/g)].map(x => CN[x[1]] || 0);
      ok(dSecs.length >= 1, 'README 说明了"只查几件事"');
      ok(dSecs.length > 0 && dSecs.every(n => n === secs),
         `README 的"只查 N 件事"与脚本实际 ${secs} 节一致`,
         'README 写 ' + dSecs.join('/') + '，脚本是 ' + secs);
    }

    /* 冒烟脚本自己也得跟着搬家。
     * 它读的是守护那份配置和名单：路径还指着模块目录时，
     * 装机后模块目录里的副本已经被 customize.sh 用 mv 搬走了，
     * 于是"名单为空""COUNT 对不上"一起报出来——坏的是脚本，不是模块。
     * 更要防的是反向漂移：守护改了 SF_DATADIR、脚本没跟上，
     * 真机冒烟就永远在验一个不存在的路径。所以两边路径直接比字符串。 */
    {
      const dd = (csrc.match(/#define\s+SF_DATADIR\s+"([^"]+)"/) || ['', ''])[1];
      ok(!!dd, '守护里有 SF_DATADIR 字面量（比对基准本身要在）');
      ok(smokeCode.includes('DATA=' + dd),
         `冒烟脚本的数据目录与守护一致（${dd}）`,
         (smokeCode.match(/DATA=\S+/g) || [''])[0]);
      // 名单路径必须在每次读之前重新解析：api.sh 第一次被调用时会做迁移，
      // 开头算好的旧路径等真去数行时文件已经被 mv 走了。
      ok(/pick_list\(\)\s*\{/.test(smokeCode), '冒烟脚本用 pick_list 现取名单路径');
      /* 模块目录那份是 pick_list 里的老位置回退，只允许出现在函数体内。
       * 函数体外再出现一处，就说明某个读取点没走 pick_list，
       * 而它读的可能是已被 mv 走的文件——症状是"名单是空的"。
       * 所以先把函数体摘掉再查剩下的。 */
      {
        const body = (smokeTxt.match(/pick_list\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
        const rest = smokeCode.replace(body, '');
        ok(!!body, 'pick_list 函数体找得到（摘不出来就没法区分回退分支和漏改的点）',
           body ? '体长 ' + body.length : '正则没匹配到 \n} 收尾');
        ok(!/"\$D\/games\.txt"/.test(rest),
           'pick_list 之外没有别的地方直接读模块目录里的 games.txt',
           (rest.match(/.{0,40}\$D\/games\.txt/g) || [''])[0]);
      }
      ok(/screen_uncap=/.test(smokeCode) && /boot_full=/.test(smokeCode),
         '冒烟脚本会验两个新键有没有出现在 export 里');
    }

    /* bad / say 不能混用：bad 才计入 BAD 并让退出码非零。
     * 有一次一个变异把 bad "games.txt 里还有 $CR 个 CR" 换成了
     * say "（跳过 CR 检查）"——CR 残留照样打印，退出码却是 0。
     * 那种损坏最难发现：输出看着齐全，结论却是"通过"。
     * 不去数 bad/say 各几条（加一条检查就变，数字必然过期），
     * 而是逐个 else 分支看：else 一定是坏消息，必须是 bad。 */
    {
      // 每个 else 后第一条输出调用
      const branches = [...smokeCode.matchAll(/else\s*\n\s*(bad|say)\s/g)].map(m => m[1]);
      ok(branches.length >= 4, `else 分支 ${branches.length} 个（至少 4）`);
      const notBad = branches.filter(k => k !== 'bad');
      ok(notBad.length === 0,
         '每个 else 分支都是 bad（失败消息不能走 say，否则退出码永远是 0）',
         '走 say 的有 ' + notBad.length + ' 处');
      ok(!/say\s+"[（(]?\s*(跳过|忽略|不做)/.test(smokeCode),
         '没有把失败分支写成 say（那会让退出码永远是 0）');
      // CR 检查这一支必须是 bad：清单有残留 CR 就是不合格，不能只提示
      ok(/if \[ "\$CR" -eq 0 \]; then[\s\S]{0,200}?else\s*\n\s*bad /.test(smokeCode),
         'games.txt 有残留 CR 时走的是 bad（失败），不是 say');
      // 反过来，say 只该出现在"正常"分支上：它不带引号，说明是行首新起一条
      ok(/(^|[\n;&|])\s*say\s/g.test(smokeCode), 'say 仍被正常使用（报"这条过了"）');
    }
  }
}

/* ---- 游戏名单 ---- */
{
  const gtxt = fs.readFileSync(process.argv[7], 'utf8');
  const pkgs = gtxt.split('\n').map(s=>s.trim()).filter(s=>s && s[0] !== '#');
  ok(pkgs.length > 400, `名单条目数 > 400 (${pkgs.length})`);
  ok(new Set(pkgs).size === pkgs.length, '名单无重复');
  /* 名单按发行商分节，组内字母序。
     分节本身就是可读的证据，组内有序才好查、好增删。
     所以要查的是两件更要紧的事：
       1) 每个包名仍只出现一次（分节时最容易出的错是同一包落进两节）
       2) 分节标题都以 # 开头，不会被当成包名（守护与 api.sh 都跳过 # 行）
     顺序与匹配无关：守护是整串精确比对。 */
  const sectRe = /^#\s*──\s*(.+?)\s*──\s*$/;
  const sects = [];
  let cur = null;
  for (const raw of gtxt.split('\n')) {
    const s = raw.trim();
    if (!s) continue;
    if (s[0] === '#') {
      const m = sectRe.exec(s);
      if (m) {
        // 标题形如「腾讯（61）」：名字和条数都包在这一个分组里
        const cm = /^(.*?)\s*（(\d+)）$/.exec(m[1]);
        cur = { name: cm ? cm[1] : m[1], hdr: cm ? +cm[2] : NaN, count: 0 };
        sects.push(cur);
      }
      continue;
    }
    if (cur) cur.count++;
  }
  ok(sects.length >= 8, `名单按发行商分了节 (${sects.length} 节)`);
  {
    const bad = sects.filter(s => !Number.isFinite(s.hdr) || s.hdr !== s.count);
    ok(bad.length === 0, '每节标题标注的条数与实际一致',
       bad.map(s => `${s.name} 标 ${s.hdr} 实 ${s.count}`).join('；'));
  }
  {
    // 组内必须有序，否则"分节"只做了一半
    const order = {};
    let name = null;
    for (const raw of gtxt.split('\n')) {
      const s = raw.trim();
      if (!s) continue;
      if (s[0] === '#') {
        const m = sectRe.exec(s);
        name = m ? m[1] : null;
        continue;
      }
      const arr = order[name] || (order[name] = []);
      arr.push(s);
    }
    const unsorted = Object.entries(order).filter(([, a]) => a.some((v, i) => i && a[i-1].toLowerCase() > v.toLowerCase()));
    ok(unsorted.length === 0, '每节内部按包名排好序',
       unsorted.map(([k, a]) => k).join('、'));
  }
  ok(/static int is_game[\s\S]*?strcmp\(g_pkgs\[i\], pkg\)/.test(csrc),
     'is_game 是整串精确比对（顺序无关，分节安全）');
  ok(new RegExp('define MAX_PKG ' + Math.max(512, pkgs.length + 16)).test(csrc),
     'MAX_PKG 容得下当前名单', (csrc.match(/#define MAX_PKG \d+/) || [''])[0]);
  // 用户那份清单里的包必须都在
  for (const p of ['com.tencent.tmgp.sgame','com.tencent.tmgp.pubgmhd',
                   'com.netease.dwrgba','com.miHoYo.hkrpg','com.kurogame.mingchao',
                   'com.papegames.lysk','com.lilithgame.wgame','com.supercell.clashofclans',
                   'com.playrix.gardenscapes','com.aniplex.fategrandorder',
                   'com.bilibili.game.fgo','com.hypergryph.arknights.bilibili']) {
    ok(pkgs.includes(p), '含 ' + p);
  }
}

/* ---- WebUI 图标 ---- */
{
  ok(/function makeIconEl/.test(html), 'WebUI 有图标渲染');
  ok(/api\('icon'/.test(html), 'WebUI 调用 icon 接口');
  ok(/IntersectionObserver/.test(html), '图标懒加载（400+ 条不能一次全请求）');
  // 前端不自己拼 data URI（选图允许 .webp，拼错 MIME 就是破图），
  // 只校验后端回来的那一串。钉的是校验正则本身。
  ok(!html.includes("'data:image/png;base64,' +") &&
     html.includes('/^data:image\\/(png|webp|jpeg|gif);base64,/'),
     '图标用后端回来的 data URI，前端不自己拼 MIME、也不写文件到共享存储');
  // 取不到图必须退色块，不能留破图
  ok(/className = 'ico ph'/.test(html), '图标位默认是色块占位');
  ok(!/ph\.src\s*=/.test(html), '占位元素不会被塞 src（破图）');
  // 后端返回完整 data URI（自带 MIME），前端直接用，不能再自己拼。
  // 写死 image/png 的话，webp 的自适应图标解不开就是破图。
  // 只查"拼接赋值"那一处，别把下面校验用的正则本身也算进来。
  ok(!/img\.src\s*=\s*['"]data:image\//.test(html), '前端不再自己拼 data:image/（webp 会解不开）');
  // 拿到的东西必须验过是 data URI 才敢用，空/杂讯一律退回色块
  ok(/\^data:image\\\/\(png\|webp\|jpeg\|gif\);base64,/.test(html),
     '前端校验 MIME 后才用（空输出/杂讯退回色块）');
  ok(/img\.onerror/.test(html), '图标解不开时 onerror 退色块，不留破图');
  // 懒加载必须有真实边界：范围拉到无穷就等于一次性全请求
  const rm = (html.match(/rootMargin:\s*'([^']*)'/) || [])[1];
  ok(rm && /^\d{1,4}px$/.test(rm), '懒加载有明确的像素边界（不是全量）', rm);
  ok(rm && parseInt(rm) <= 500, '懒加载边界 ≤500px（列表很长时不会被一次性拉满）', rm);
  ok(/icon\)\s*cmd_icon/.test(apiSrc), 'icon 子命令已接入分发');
  /* ---- 错误协议：前后端必须说同一种话 ---- */
  // api.sh 的 die() 打的是 E/<码>，前端要是还按 ERR: 判，
  // 所有失败都会被当成成功：徽标显示"未运行"，而用户什么都没做错。
  const dieFmt = (apiSrc.match(/die\(\)\s*\{\s*echo\s+"([^"]+)"/) || [])[1] || '';
  ok(dieFmt.startsWith('E/'), 'api.sh 的 die 打的是 E/<码>', dieFmt);
  /* 前端判成败的那一处现在叫 mmBad()：MM 那类桥没有退出码，只能从输出猜。
   * 断言跟着改成检查 mmBad 的函数体，而不是检查调用点——
   * 调用点只剩一个函数名，再钉表达式就只会假红。 */
  const mmBadBody = (html.match(/function mmBad\(s, cmd\)\{[\s\S]*?\n\}/) || [''])[0];
  ok(!!mmBadBody, 'mmBad() 函数体抠得到', mmBadBody ? '' : '锚点变了');
  // 源码里是正则字面量 `/(^|\n)E\//`，斜杠前面带反斜杠——
  // 断言里漏了这个转义就永远匹配不上（只会假红）
  ok(/\(\^\|\\n\)E\\\//.test(mmBadBody) || /\^\|\\n\\\)E/.test(mmBadBody),
     'MM 那路按 E/ 判失败（不是 ERR:）', (mmBadBody.match(/[^\n]*E[\\/][^\n]*/) || [''])[0]);
  ok(!/ERR:/.test(mmBadBody), '不再认 api.sh 根本不产出的 ERR: 前缀');
  // 空输出仍然算失败——但只对"必然有话说"的命令；
  // 列表类命令的空是合法状态（没设过档位 / 没候选），当成失败就会
  // 在打开配置页时弹一句"执行失败"。
  ok(/s\.trim\(\)/.test(mmBadBody), '空输出这一半的判据还在');
  ok(/modes_list\|modes_del\|games_list\|games_suggest\|labels\|icon/.test(mmBadBody),
     '空列表命令在"可空"白名单里',
     (mmBadBody.match(/\(\s*\/[^\n]*\)\/\.test\(cmd\)/) || [''])[0]);
  const callBad = (html.match(/const bad = mmBad\(s, cmd\);/g) || []);
  ok(callBad.length === 2, 'MM 与 magisk 两条无退出码的桥都走 mmBad（实到 '
     + callBad.length + ' 处）');
  ok(!/(^|\n)ERR:/.test(html), 'index.html 里没有残留的 ERR: 判据');
  ok(/\.replace\(\/\^E\\\/\\S\*\\s\*\/,\s*''\)/.test(html),
     'api() 剥的是 E/<码> 前缀（否则用户直接看到 "E/NO_CONF"）');
  /* ---- about 子命令：标记必须真的打出来 ---- */
  // 前端靠 ---PROP--- 切分。少打的话 indexOf 返回 -1，
  // 切片仍得到一个"对象"、所有字段 undefined，关于页静默空白且不报错。
  const aboutBody = (apiSrc.match(/st_about\(\)\s*\{[\s\S]*?\n\}/) || [])[0] || '';
  ok(/---PROP---/.test(aboutBody), 'st_about 会打 ---PROP--- 标记');
  ok(/printf '%s\\n' '---PROP---'/.test(aboutBody) && !/printf -- /.test(aboutBody),
     '标记用 %s 打，不依赖 printf --（有的 ROM 的 sh 会把 -- 当格式串）');
  ok(/module\.prop/.test(aboutBody), 'st_about 会 cat module.prop');
  ok(/indexOf\(\s*['"]---PROP---['"]\)/.test(html), '前端按 ---PROP--- 切分');
  ok(/i\s*>=\s*0/.test(html) && /\(\?:id\|name\|version\|versionCode\|author\|description\)=/.test(html)
     && /replace\(\/\^-\+\(\?=/.test(html),
     '标记缺失时从 module.prop 行恢复（首行被吃掉 -- 也能剥回来）');

  // module.prop 里不能留空的 updateJson=。
  // KernelSU Next / MM 这类管理器会把它当 URL 去拉更新，
  // 空值会让它们弹一个"检查更新失败"。
  // 这行原本还缺结尾换行，POSIX 下最后一行照样能读到，
  // 于是"没有结尾换行"和"值为空"是两个独立的坑，两个都堵。
  const mp = fs.readFileSync(MODDIR + '/module.prop', 'utf8');
  ok(!/^\s*updateJson\s*=\s*$/m.test(mp),
     'module.prop 没有空的 updateJson=',
     (mp.match(/^\s*updateJson.*$/m) || [''])[0]);
  const mpRaw = fs.readFileSync(MODDIR + '/module.prop');
  ok(mpRaw[mpRaw.length - 1] === 0x0a,
     'module.prop 以换行结尾',
     '末字节是 0x' + mpRaw[mpRaw.length - 1].toString(16));
  // 用字节判，不能用 split('\n') 的最后一段：
  // 没有结尾换行时最后一段是内容本身（非空），
  // 有结尾换行时最后一段是空串。两种情况都存在，
  // 只有比字节才分得清。
  // id 必须和 README、smoke 脚本默认路径对得上（目录名不是显示名）
  const mpid = (mp.match(/^id=(.+)$/m) || [])[1] || '';
  ok(mpid === 'soc_freqcap',
     'module.prop 的 id=soc_freqcap',
     '实际是 id=' + mpid);
  // 守护侧的 MIME 必须和前端校验用的那四种对上。
  // 上面的断言只查了前端的正则，守护把 webp 标成 png 一样能全绿——
  // 而 webp 恰恰是 Android 8+ 自适应图标的主力格式，
  // 标错的直接后果是整列图标退色块，只有这一种格式坏，最难猜。
  const iconBody = (iconSrc.match(/mime_of\([^)]*\)\s*\{[\s\S]*?\r?\n\}/) || [''])[0];
  // 样式的 \)+ ：if (!strcmp(dot+1,"webp")) 里 strcmp 和 if 各收一个括号，
// 按单个 \) 匹配永远不中——写断言时把这里数成一层就会踩。
ok(/"webp"\)\s*\)?\s*return\s*"image\/webp"/.test(iconBody), '守护给 webp 发 image/webp（不是 image/png）',
     iconBody.slice(0, 60));
  ok(/"gif"\)\s*\)?\s*return\s*"image\/gif"/.test(iconBody), '守护给 gif 发 image/gif');
  ok(/return\s*"image\/jpeg"/.test(iconBody), '守护给 jpg/jpeg 发 image/jpeg');
  // 前端认的这几种，守护必须都发得出，否则就是"解不开"→ 退色块。
  // 两边都只取 MIME 尾缀再比集合，别把 "data:image/" 的前缀噪声算进去。
  const htmlMimes = (html.match(/data:image\\\/\(([^)]*)\)/) || [, ''])[1].split('|').filter(Boolean).sort().join(',');
  const srcMimes = [...new Set((iconSrc.match(/"image\/([a-z0-9+]+)"/g) || [])
    .map(s => s.replace(/["\/]/g, '').replace(/^image/, '')))].sort().join(',');
  ok(htmlMimes && htmlMimes === srcMimes,
     '前端认的 MIME 集合 = 守护会发的集合', `前端 ${htmlMimes} / 守护 ${srcMimes}`);
  // 前端探测模块根目录时认的回执，必须和 api.sh 实际吐的一模一样。
  // 认错了的话每台设备都显示"找不到 api.sh"，明明装好了却像没装。
  const pingOut = ((apiSrc.match(/cmd_ping\(\)\s*\{\s*printf\s+'([^']*)'/) || [])[1] || '').replace(/\\n/g, '');
  // 取 ping 调用之后那个 if 的条件——那才是"认回执"的地方。
  // 中间隔着两行注释，所以窗口不能太小；
  // 但也不能写成惰性 {0,300}?：那会匹配 0 个字符，探针只拿到 exec 那一行，
  // 于是 PONG 明明在下一行也判不出来。
  // 取探根循环里认 ping 回执的那一句。现在探根和目标命令合并在一次 exec 里，
  // 但"认的是 PONG 本身"这条不能丢：只看退出码会把装坏的 api.sh 当成能用的。
  const probe = (html.match(/const probe = 'for r in '[\s\S]{0,800}?exit 127'/) || [''])[0] || '';
  ok(!!pingOut, 'api.sh 有固定的 ping 回执');
  ok(probe.length > 0, '前端有模块根探测调用点（与首条命令合并成一次 exec）');
  ok(probe.includes('" = "' + pingOut + '"'),
     `前端探测认的回执和后端一致（后端回 ${JSON.stringify(pingOut)}）`,
     `探测处比对的是: ${(probe.match(/" = "([A-Za-z]+)"/) || [])[1]}`);
  ok(/\[ "\$\(sh "\$p" ping 2>\/dev\/null\)" = "PONG" \]/.test(html),
     '探测要求 ping 的输出正好是 PONG（退出码 0 但回执不对也不算找到）');
}

/* ---- 加载期：守护必须真的能跑 ---- */
{
  // Bionic 拒载发生在 exec 之前，守护一行都不写，
  // 页面只显示"无法连接守护"，用户看不出是二进制本身起不来。
  const cust = fs.readFileSync(path.join(MODDIR, 'customize.sh'), 'utf8');
  ok(/socfreqd["']?\s+version/.test(cust), '安装期会 exec 一次守护（不是只看文件在不在）');
  ok(/ui_print[^\n]*\$CHK/.test(cust), '守护跑不起来时把 loader 原话显示给用户');
  // 试的必须是 version：它是唯一不碰 sysfs、不写文件的子命令。
  ok(/!\s*strcmp\(cmd,\s*"version"\)/.test(csrc) && /printf\("socfreqd %s/.test(csrc),
     'version 子命令存在且只打印一行就返回');
  // 自检失败不该中止安装：跑不起来也要让用户能进 WebUI 看日志，
  // 一旦 abort 就变成"装了但完全进不去"，比现在更难查。
  // if/else/fi 整段取出后必须按 else 切开：
  // 直接拿整段去查"自检通过"，查到的是成功分支那半句，
  // 于是这个断言永远红——看着像代码坏了，其实是切错了边。
  const ifBlock = ((cust.match(/if \[ -z "\$CHK" \][\s\S]*?\nfi/) || [])[0] || '');
  const failBlock = ifBlock.split(/\nelse\b/)[0];
  const passBlock = ifBlock.split(/\nelse\b/)[1] || '';
  ok(failBlock && !/\bexit\b|\babort_after_this/.test(failBlock),
     '自检失败只告警，不中止安装');
  ok(failBlock && !failBlock.includes('自检通过'),
     '自检失败时不打印"自检通过"', failBlock.slice(0, 50).replace(/\s+/g, ' '));
  ok(failBlock && /ui_print/.test(failBlock) && /loader|无法执行|ELF/.test(failBlock),
     '自检失败会给出可读原因');
  ok(passBlock.includes('自检通过'), '自检成功时会说一声通过（否则用户不知道装好没）');
}

/* ---- 与比对基准的相似度 ---- */
{
  // 相似度没有"安全线"这种东西，只能盯着最硬的指标：
  // 剔除 shell 语法糖后，双方还有几行是完全一样的。
  // 语法糖（fi/done/}/esac）不算，那不是谁写的。
  if (!haveRef) {
    console.log('  skip 相似度对比（未提供基准脚本）');
  } else {
  const refSrc = fs.readFileSync(refPath, 'utf8');
  const BOILER = /^\s*(fi|done|esac|else|then|\{|\}|;;|return\s+[01]|case\s+"?\$\{?1\}?"?\s+in)\s*;?\s*$/;
  const codeLines = s => s.split(/\r?\n/)
    .map(x => x.trim())
    .filter(x => x && !x.startsWith('#') && !BOILER.test(x) && x !== 'fi');
  const mine = codeLines(apiSrc);
  const others = new Set(codeLines(refSrc));
  const same = mine.filter(x => others.has(x));
  const pct = same.length / mine.length * 100;
  ok(same.length <= 3, `api.sh 与基准实质相同 ≤3 行（实测 ${same.length} 行 / ${pct.toFixed(1)}%）`,
     same.join(' | '));
  // 最长公共连续片段超过 20 字符，基本可以判定是整段照搬
  const a = apiSrc.replace(/\r/g, ''), b = refSrc.replace(/\r/g, '');
  let best = 0;
  const prev = new Int32Array(b.length + 1), cur = new Int32Array(b.length + 1);
  for (let i = 1; i <= a.length; i++) {
    for (let j = 1; j <= b.length; j++) cur[j] = a[i-1] === b[j-1] ? prev[j-1] + 1 : 0;
    prev.set(cur);
  }
  for (let j = 0; j <= b.length; j++) if (prev[j] > best) best = prev[j];
  ok(best <= 20, `api.sh 与基准最长公共片段 ≤20 字符（实测 ${best}）`);
  } // if(!haveRef)
  // 也不能只靠改函数名蒙混过关：文件名和目录结构得是本模块自己的。
  // 这条不看基准，没传基准时也必须查。
  ok(/Soc智能限频/.test(apiSrc), 'api.sh 里带本模块名，不是通用模板');
}

/* ---- 前端读的字段，后端必须真的产出 ---- */
{
  // 前端 parse(status) 之后会取这些字段。改了 api.sh 的输出格式
  // 却没同步前端，表现是页面上大片"—"和空白，很难看出是接口对不上。
  const need = ['state', 'game_pkg', 'pid', 'paused', 'npkg', 'soc'];
  const stFn = (apiSrc.match(/st_status\(\)\s*\{([\s\S]*?)\n\}/) || [])[1] || '';
  // run/status 存在时，实际把它读出来的字段必须覆盖前端的运行侧字段。
  // 不能只搜 "state="：兜底分支里也有 state=unknown，
  // 那样这个断言在 grep 整个坏掉的情况下照样通过。
  const reads = (stFn.match(/grep\s+-E\s+'([^']+)'/) || [])[1] || '';
  // 只剥掉 ^ $ ( ) 这些结构字符，别连 = 一起剥：
  // '^(state|game_pkg|pid)=' 拆出来必须是三个干净的字段名。
  const readFields = reads.replace(/[\^$()]/g, '').split('|')
    .map(s => s.replace(/=$/, '')).filter(Boolean);
  for (const f of need) {
    const ok1 = readFields.includes(f) ||
                new RegExp('printf\\("' + f + '=[%ds]', 'i').test(csrc);
    ok(ok1, `status 提供 ${f}（前端会读）`, `run/status 读到 [${readFields.join(',')}]`);
  }
  // run/status 不存在时也必须有兜底输出，否则前端拿到空对象白屏
  ok(/state=unknown/.test(stFn), '没有 run/status 时也给出可解析的兜底字段');
}

/* ---- 用户报过的两个问题：图标和名单，各钉一条不许退回 ---- */
{
  // 路径要取自 argv，不是 csrc 的内容——csrc 是文件内容，
  // 拿它去 dirname 会把整份源码当路径，直接 ENAMETOOLONG。
  const iconSrc = fs.readFileSync(process.argv[8], 'utf8');
  const iconFn = (csrc.match(/if \(!strcmp\(cmd, "icon"\)\)\s*\{([\s\S]*?)\n  \}/) || [])[1] || '';
  ok(iconFn.length > 0, '守护有 icon 子命令');
  ok(/icon_from_apk\(argv\[2\],/.test(iconFn),
     'icon 子命令真的调了取图函数（不是空壳子命令）');
  // 取不到图标时必须静默返回，不能报错：名单里大部分包本机就没装，
  // 报整个错会让名单页变成一个错误提示。
  ok(!/stderr/.test(iconFn), '取不到图标时不往 stderr 写东西（页面据此退色块）');

  // windowBits 必须是 -15：15 是 zlib 头、32 是 gzip 自动识别，
  // zip 里的 deflate 流（RFC 1951）三者都不匹配，实测一律 Z_DATA_ERROR，
  // 而 store 条目照样正常，所以少量测试样本看不出来。
  const wbits = (iconSrc.match(/inflateInit2\(&z,\s*([^)]+)\)/) || [])[1] || '';
  ok(wbits.trim() === '-15',
     `deflate 用 raw 窗口（inflateInit2(&z, ${wbits.trim()}））`);

  // 图标来源必须是 APK 字节，不能依赖外部解压工具。
  // 注释必须一起剥掉，否则必然误判：api.sh 的行注释和 icon.c 的块注释里
  // 都正面写着"toybox 里没有 unzip"来解释这个坑，搜全文一定命中。
  // 两个文件都要查：取图逻辑在 icon.c 里，只查 api.sh 漏得掉。
  const apiCode = stripSh(apiSrc);
  const iconCode = stripC(iconSrc);
  /* icon.c 仍然不许依赖 unzip——守护那条自己解析中央目录的路
     是主路，不能因为"有些机器有 unzip"就把主路拆掉。
     api.sh 则相反：允许 unzip 出现，但只能是兜底——
     部分 ROM 确实自带 unzip，不是那种"开发机上没有"的坑。
     但也不能因此把它提成唯一路径。 */
  ok(!/\bunzip\b/.test(iconCode), 'icon.c 的代码里不出现 unzip（主路自己读 zip，不依赖外部工具）');
  ok(/\bunzip\b/.test(apiCode),
     'api.sh 里有 unzip 兜底（有 unzip 的机器上它能救主路）');
  ok(/command -v unzip >\/dev\/null 2>&1 \|\| return 1/.test(apiCode),
     'unzip 兜底先探测命令存在（没有就直接返回，不能报错）');
  ok(/\$BIN" icon "\$_apk/.test(apiCode),
     'api.sh 仍然走守护的 icon 子命令作主路（兜底不是主路）');
}

/* ---- icon.c 的条目识别 ---- */
{
  const stripBlk = s => s.replace(/\/\*[\s\S]*?\*\//g, '');
  const ic = stripBlk(iconSrc);

  // 打分必须作用在**有 NUL 结尾**的名字上。
  // 中央目录里没有 NUL，文件名后面紧挨下一个条目头的 PK\x01\x02；
  // 直接把裸指针交给 strrchr/strstr，扩展名判断对每个条目都不成立，
  // 结果是一个图标都认不出来，页面只剩色块，而且不报错、不崩。
  // （真 APK 夹具实测：11 个条目，修前认出 0 个。）
  ok(!/score_name\s*\(\s*\(const char\s*\*\)\s*\(\s*buf\s*\+\s*\w+\s*\+\s*46\s*\)/.test(ic),
     '打分前先把文件名拷到带 NUL 的缓冲区',
     'icon.c 还在把中央目录里的裸指针交给 score_name');
  ok(/memcpy\s*\(\s*nm\s*,\s*buf\s*\+\s*\w+\s*\+\s*46\s*,\s*nlen\s*\)/.test(ic)
     && /nm\s*\[\s*nlen\s*\]\s*=\s*0/.test(ic),
     '拷贝后补 NUL 结尾');
  // 拷出来的那份要留下来，别只用来打分。
  ok(/memcpy\s*\(\s*ents\s*\[\s*\w+\s*\]\.name\s*,\s*nm\s*,/.test(ic),
     '条目名字用拷贝后的那份（而不是重新从 buf 取）');

  // 密度打分：按"离槽位像素多近"排，中间那档（xxhdpi）最高。
  //
  // 这条翻过两次车，两次都是注释、代码、测试三方各说各话：
  //   第一次 代码是 35+i（越高越大），注释写"越大越好"，
  //           而更早的注释写着"故意不先取 xxxhdpi"；
  //   第二次 上一轮按注释把它改成 39-i（mdpi 最高），
  //           理由写成"mdpi 在 46px 小图标上够看"——
  //           可 46 是 **CSS px**、槽位是 CSS 38px，
  //           dpr 3~3.5 之后实际要 114~133 **物理像素**，
  //           mdpi 的 48px 源图要塞进 114px 是放大 2.4 倍，糊。
  // 现在按物理像素定：xxhdpi(144) 最高，mdpi(48)/xxxhdpi(192) 两端低。
  //
  // 所以这里不再断言某个写死的分数公式（写死的公式正是上次拦住正确方向的东西），
  // 改为断言**槽位像素表存在**、**分值由像素偏差算出来**，
  // 具体算出来的分数交给 t_icon_parse.py 用真字节跑。
  // densFlat 折行归一：那条三元是折行写的，逐行正则匹配不上，
  // 会一直红，而红的原因只是排版。
  const densFlat = ic.replace(/\s+/g, ' ');
  ok(/DENS_PX\[\]\s*=\s*\{[^}]*144/.test(densFlat),
     '密度像素表在（按物理像素定档，不是按名字顺序）');
  const slotM = ic.match(/#define\s+ICON_SLOT_PX\s+(\d+)/);
  ok(!!slotM, '槽位像素有定义，不是散在注释里的魔数');
  // 这两条盯的是"密度分值只能由像素偏差算出来"。
  // 断言不能钉 /return\s+39\s*-\s*i/，可实际那行是 `int sc = 39 - i;`，
  // 前面根本没有 return —— 正则永远匹配不上，于是"别改回小图优先"
  // 这条一直是**空的**：改坏了也不会红。删掉 return 前缀才对得上。
  // 变异脚本 v_icondensity.ps1 就是靠这两条证明它们真能失败。
  ok(!/=\s*39\s*-\s*i\s*;/.test(densFlat),
     '不要再出现 39 - i（那是"小图优先"，忽略了 dpr）');
  ok(!/=\s*35\s*\+\s*i\s*;/.test(densFlat), '不要再出现 35 + i（那是无脑选最大的图）');
  // 正向：分值必须真的用到像素偏差，删掉就等于退回固定分。
  // 用 densFlat（上面已折行归一），否则三元折行就匹配不上。
  ok(/DENS_PX\[i\]\s*>\s*ICON_SLOT_PX\s*\?\s*DENS_PX\[i\]\s*-\s*ICON_SLOT_PX\s*:\s*ICON_SLOT_PX\s*-\s*DENS_PX\[i\];/.test(densFlat),
     '分值按"离槽位多少像素"算（不是按 DENSITY 下标，也不是常量）');
  ok(/int sc = 39 - \(d \* 9\) \/ 144;/.test(densFlat),
     '偏差换算成分数的那一行在');
  // round：按整词排除第一轮。子串匹配会误伤 background。
  ok(/has_word\(n,\s*"round"\)\)\s*return\s+-1;/.test(ic),
     'round 按整词排除（strstr 会把 back-g-roun-d 也当成 round）');
  ok(/if\s*\(has_word\(n,\s*"round"\)\)/.test(ic) && !/strstr\(n,\s*"round"\)/.test(ic),
     '不得用 strstr 匹配 round');
  ok(/ic_launcher_background\./.test(ic),
     '认得 ic_launcher_background（自适应图标只剩光栅图时的兜底）');
  ok(/return\s+12;/.test(ic), 'background 给 12 分（压在 mipmap 下、drawable 上）');

  /* ── 按文件名分主次（真机回归：取到的是背景层）───────────────────
     自适应图标的包里 mipmap-xxhdpi/ 同时有 ic_launcher.png、
     ic_launcher_foreground.png、ic_launcher_background.png。
     只要路径含 mipmap-<密度> 就给同一个分，再叠加"同分取字节小的"，
     于是稳定选中字节最小的背景层 —— 页面上是一个纯色方块。
     这一组断言盯的是"密度分之外还要按文件名分主次"这件事本身。 */
  ok(/static const char \*base_of\(/.test(ic), '有 base_of：分主次看文件名，不看整条路径');
  ok(/base_of\(n\)/.test(ic), 'score_name 里真的用了 basename（整条路径里本来就含 launcher 这个词）');
  // 顺序是关键：foreground/background 必须**先于** launcher 判。
  // 反过来的话 ic_launcher_background.png 会因为含 "launcher" 拿到 +12，
  // 又变回选背景层 —— 按分数判之后连"同分取小"都不需要，分更高必赢。
  {
    const iFg = ic.search(/has_word\(b,\s*"foreground"\)\s*\|\|\s*has_word\(b,\s*"background"\)/);
    const iL  = ic.search(/else if\s*\(has_word\(b,\s*"launcher"\)/);
    ok(iFg >= 0 && iL >= 0 && iFg < iL,
       '前景/背景层必须在 launcher 之前判（否则 ic_launcher_background 会因含 launcher 加分）',
       `fg@${iFg} launcher@${iL}`);
  }
  ok(/sc\s*-=\s*20;/.test(ic), '前景/背景层扣分（只作最后备选，不直接否掉）');
  ok(/sc\s*\+=\s*12;/.test(ic), '文件名含 launcher 加分（决定性的一档）');
  // 背景层不能否掉：整包只剩分层素材时，否掉就掉进第二轮按尺寸瞎猜，
  // 那比拿到底图更容易取到游戏内素材。具体大小关系在 test_real 里跑真值。
  ok(/has_word\(b,\s*"icon"\)\s*\|\|\s*has_word\(b,\s*"logo"\)/.test(ic),
     'icon/logo 是次一级的加分项');
  // 体积闸：按字节设限，不靠密度猜大小
  ok(/#define\s+ICON_BUDGET\s+\(?\s*24u?\s*\*\s*1024/.test(ic),
     '有单张体积预算（选密度不该顺带选中几十 KB 的美术图）');
  ok(/pick_next\(ents,\s*n,\s*1\)/.test(ic) && /pick_next\(ents,\s*n,\s*0\)/.test(ic),
     '预算内挑一遍、全都没有才放开（两趟，不是直接不要大的）');

  /* ── strrchr 的返回值必须判空 ──
     score_name 里 dot 没判空就直接 strcmp(dot + 1, "png")，
     碰到没有点号的条目名（"LICENSES"、"META-INF/services/xxx"、
     "assets/channel"）就是 strcmp(NULL + 1, ...)，读地址 0x1，
     Bionic 直接 SIGSEGV。
     而扫描循环对中央目录里**每个**条目都调 score_name、没有前置过滤，
     所以一条这样的名字就让整个命令崩、页面全色块——
     不是"少一张图"。这三个包实测都有：coolapk 5913 条里 7 条、
     base 3227 条里 5 条、Kin 105 条里 2 条，且位置靠前。 */
  {
    // 函数体一律按 \r?\n\} 收尾：这个仓库是 CRLF 行尾，
    // 写死 \n\} 会**匹配不到任何东西**（\n 后面紧跟的是 \r）。
    // 空串对 .test() 一律返回 false，看起来像"断言红了"，
    // 其实是正则压根没匹配上——这两种要分清，所以下面把体长打出来。
    //
    // 锚点不能写成 `static [\s\S]*?score_name(`。
    // 那段 [\s\S]*? 不换行也不限字符，会从前面某个 static 一路伸到
    // **下一个**出现 score_name( 的地方（或者更远），取出来的"函数体"
    // 其实是别的函数——断言于是对着错的代码红，或者更糟：对错的代码绿过。
    // 现在限制 static 和函数名之间不能跨行、不能跨大括号。
    const bodyOf = (name) => {
      // 这行正则里 { } \ 都要在 JS 字符串里写成 \{ \} \\，
      // 否则 [\s\S] 之类会被字符串层先吃掉一层，反则根本不成立。
      const re = new RegExp('static [^;{}\\n]*?\\b' + name +
                            '\\([^)]*\\)\\s*\\{[\\s\\S]*?\\r?\\n\\}');
      return (ic.match(re) || [''])[0];
    };
    const snBody = bodyOf('score_name');
    const sflat  = snBody.replace(/\s+/g, ' ');
    ok(snBody.length > 0, 'score_name 的函数体能被取到（正则没匹配上空串）',
       `体长 ${snBody.length}`);
    ok(/if \(!dot\) return -1;/.test(sflat),
       'score_name 判空 strrchr 的结果（漏了就是读地址 0x1 直接崩）');
    // 允许中间夹着"目录条目"那一行，但 strrchr 之后**不许**出现别的
    // 变量声明或多于一行跳转：补空指针守卫时补错对象（补到隔壁
    // 函数的 dot 上）比不补更坏——不补会崩，补错会静默选错图。
    const afterDot = sflat.split("strrchr(n, '.')")[1] || '';
    ok(/^\s*;?\s*(?:if \([^;]*\)\s*return -1;\s*)*if \(!dot\) return -1;/.test(afterDot),
       '判空紧跟在 strrchr 后面（只允许夹目录跳过，不许补到别的对象上）');
    ok(/n\[0\] && n\[strlen\(n\) - 1\] == '\/'\) return -1;/.test(sflat),
       '目录条目跳过（以 / 结尾且无点号，白算一趟）');
    // 同类函数里都有这道守卫，唯独 score_name 漏过——所以逐个点名，
    // 免得"别处有就等于这里也有"。注意 mime_of 的兜底不是 -1 而是
    // image/png（取图路径上它只对已选中的条目调用，判据不同）。
    for (const fn of ['score_pixels', 'mime_of']) {
      const fbody = bodyOf(fn).replace(/\s+/g, ' ');
      ok(/if \(!dot\) return/.test(fbody),
         `${fn} 同样要判空（漏了这里就崩）`);
    }
    // 自检壳：test_real.c 里那组用例靠它才调得到 static 的 score_name。
    ok(/int sf_test_score_name\(const char \*name\)/.test(ic),
       '有 sf_test_score_name 壳（否则那组回归用例连不上，验不到等于没验）');
    // 两轮目标同一个：第二轮不许再写死 192。
    ok(/int d = w > ICON_SLOT_PX \? w - ICON_SLOT_PX : ICON_SLOT_PX - w;/.test(densFlat),
       '第二轮（按尺寸）也以 ICON_SLOT_PX 为目标，不另写 192');
    ok(!/w > 192 \? w - 192 : 192 - w;/.test(ic),
       '不要再出现第二轮的 192 目标（两处目标不同就会漂移）');
  }

  /* ── 缩短资源名：按名字认不出来时的兜底 ── */
  // 关键前提：**名字还在的时候必须选对**。兜底档只在名字选不出时才跑，
  // 如果第一轮就选错（或者压根选不出），再好的兜底也救不回来。
  // 之前所有测试都只测了"缩短名能不能兜住"，
  // 从没测过"正常包还能不能选对"——那才是回归的方向。
  ok(/int sc = pass == 0 \? score_name\(nm\) : -1;/.test(ic),
     '第一轮按名字打分（名字还在时靠它）');
  // 中央目录必须按 EOCD 给的偏移走，不能从尾部倒着推：
  // EOCD 里的 cdsize 不含 EOCD 自身那 12 字节，倒推会差 12 字节、
  // 落在数据区里，第一条签名就不匹配，一个候选都出不来。
  ok(/uint32_t cdsz = rd32\(buf \+ eocd \+ 12\);/.test(ic)
     && /uint32_t cdoff = rd32\(buf \+ eocd \+ 16\);/.test(ic),
     '中央目录偏移取自 EOCD 字段（不是从尾部倒推）');
  ok(/cdoff >= base && \(size_t\)cdoff \+ cdsz <= \(size_t\)sz/.test(ic),
     '中央目录必须完整落在已读窗口内（否则换更大窗口重来）');
  ok(/static int score_pixels\s*\(/.test(ic), '有按图片自身特征打分的兜底档');
  ok(/static int under_res\s*\(/.test(ic), '兜底档只认 res/ 下的图');
  // 只接受 res/，绝不放宽到 assets/：assets 里 661 张多是动画帧和纹理，
  // 挑出来的多半是错的图——那比留色块更糟。
  // 断言要同时看两处：
  //   函数体  —— 挡住"under_res 里改成 res/ 或 assets/"
  //   调用处  —— 挡住"调用处再 or 一次 assets/"
  // 只查函数体的话，把调用处改成 !under_res(n) && !strstr(n,"assets/")
  // 照样放行：函数体一个字没改。
  ok(/static int under_res\s*\(\s*const char \*n\s*\)\s*\{\s*return\s*!strncmp\(n,\s*"res\/",\s*4\)\s*;\s*\}/.test(ic),
     'under_res 的函数体只认 res/，没有第二个分支');
  ok(/if\s*\(\s*!under_res\(name\)\s*\)\s*return -1;/.test(ic),
     '兜底调用处只按 under_res 过滤',
     '调用处放宽了目录范围（多半是 or 上了 assets/）');
  ok(!/under_res\([^)]*\)\s*(&&|\|\|)/.test(ic),
     'under_res 的调用没有被 && 或 || 改写');
  // IHDR 的宽高是**大端**，必须用 rd32be；用 zip 那个小端 rd32 的话，
  // 192x192 被读成 3221225472，被 w>512 判掉——所有 PNG 全军覆没，
  // 页面只剩色块、不报错也不崩。所以这条既要认大端，也要禁止小端回潮。
  ok(/static int png_dim\s*\(/.test(ic) && /rd32be\(p\s*\+\s*16\)/.test(ic)
     && /rd32be\(p\s*\+\s*20\)/.test(ic),
     'PNG 宽高从 IHDR 明文按大端读（偏移 16/20，不用 inflate）');
  {
    const pd = (ic.match(/static int png_dim[\s\S]*?\n\}/) || [''])[0];
    ok(!/rd32\(p \+ (16|20)\)/.test(pd),
       'png_dim 里不许再用小端 rd32 读 IHDR（会全灭）',
       '退回 rd32 了：192x192 会被读成 32 亿');
  }
  ok(/memcmp\(p\s*\+\s*12,\s*"IHDR",\s*4\)/.test(ic), 'PNG 先确认是 IHDR');
  // 上面三条都是"看源码写了什么"。这里**执行字节**：
  // 把 IHDR 的大端布局真拼出来，跑一遍解出来的宽高。
  // 光看源码永远抓不到"拼装顺序写反了但函数名还在"这种错。
  {
    const be = (p, o) => ((p[o] << 24) | (p[o+1] << 16) | (p[o+2] << 8) | p[o+3]) >>> 0;
    const le = (p, o) => ((p[o] | (p[o+1] << 8) | (p[o+2] << 16) | (p[o+3] << 24)) >>> 0);
    const mk = (w, h) => {
      const b = [0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,
                 0,0,0,13, 0x49,0x48,0x44,0x52,
                 (w>>>24)&255,(w>>>16)&255,(w>>>8)&255,w&255,
                 (h>>>24)&255,(h>>>16)&255,(h>>>8)&255,h&255];
      return b;
    };
    // 抄 C 的签名：rd32be(p) 里的 p 已经指到要读的 4 个字节上，
    // 所以调用得先切片。上一次我写成单参却按双参调用，
    // 读到的是偏移 0 的文件头，5 条全错——测试自己骗了自己。
    const rd32be = p => (((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) >>> 0);
    for (const [w, h] of [[192,192],[64,64],[132,132],[512,512],[48,48]]) {
      const b = mk(w, h);
      const gw = rd32be(b.slice(16, 20)), gh = rd32be(b.slice(20, 24));
      ok(gw === w && gh === h,
         `PNG IHDR 按大端读出 ${w}x${h}`,
         `解出 ${gw}x${gh}`);
    }
    // 小端读法必须明显错——大端小端混了就是整片取不到图
    {
      const b = mk(192, 192);
      const lw = le(b, 16);
      ok(lw !== 192 && lw > 512,
         '小端读同一份字节会得到 >512（所以会被边长上限判掉）',
         `小端读出 ${lw}，说明这个反例不再成立，测试本身要更新`);
    }
    // 不变量：大端解出的值必须落在 48..512 的合法图标区间
    {
      const b = mk(192, 192);
      const gw = be(b, 16);
      ok(gw >= 48 && gw <= 512, '大端读出的值落在方形筛选区间内（192 合格）');
    }
  }
  // WebP 三种容器都要认：有损 VP8、无损 VP8L、扩展 VP8X
  ok(/"VP8L"/.test(ic) && /"VP8X"/.test(ic) && /"VP8 "/.test(ic),
     'WebP 三种容器（VP8/VP8L/VP8X）都读宽高');
  ok(/if\s*\(w\s*!=\s*h\)\s*return -1;/.test(ic), '必须正方形');
  ok(/w\s*<\s*48\s*\|\|\s*w\s*>\s*512/.test(ic), '边长限制在 48~512');
  // 兜底档的分数必须低于所有按名字识别的档（drawable=10）
  const pxRet = (ic.match(/int q = [^;]+;\s*\n\s*int s = ([^;]+);/) || [])[1] || '';
  const pxS = pxRet.match(/9\s*-\s*q/);
  ok(!!pxS, '兜底档按"接近 192"打分，上限不超过 9', pxRet);
  ok(!/int\s+s\s*=\s*(1[0-9]|[2-9][0-9])/.test(ic),
     '兜底档分数不得超过 10（不能盖过 drawable）');

  /* ── 两轮扫描：按名字没选出才轮到兜底 ── */
  ok(/for\s*\(\s*int pass = 0; pass < 2/.test(ic), '分两轮扫描');
  ok(/pass\s*==\s*0\s*\?\s*score_name\(nm\)/.test(ic),
     '第 1 轮只按名字，第 2 轮才读图片头');
  // 兜底必须锁在第 2 轮里。放到第 1 轮的话，每遇到一个非 png/未知扩展名
  // 就 open+pread 一次，正式包里几千个条目能跑到上千次。
  // 条件里允许再加守卫（比如 && fdpeek >= 0），但 pass==1 这个前提不能丢。
  ok(/if\s*\(sc\s*<\s*0\s*&&\s*pass\s*==\s*1/.test(ic),
     '读图片头只在第 2 轮做（第 1 轮每条目一次 pread 太贵）');

  /* ── 候选上限：保分数最高的 64 个，不是先到先得 ── */
  ok(/#define ENTS_MAX 64/.test(ic), '候选上限有名字（ENTS_MAX）');
  ok(/int worst = 0;[\s\S]{0,200}?ents\[worst\]\.score >= sc[\s\S]{0,80}?slot = worst/.test(ic),
     '满了挤掉分数最低的那个（一满就停的话，前 64 个可能全不是图标）');
  ok(!/while\s*\(\s*p\s*\+\s*46\s*<=\s*end\s*&&\s*n\s*<\s*64\s*\)/.test(ic),
     '不能再用 n<64 当循环条件（先到先得）');

  /* ── 选择逻辑：不能读 ents[-1] ──
     用空白归一后再比，否则加个同分比较（换小文件）就把断言弄红了——
     断言盯的是"有没有守卫"，不是某一行的排版。

     择优体本身已经搬进 pick_next()（预算闸要在同一个判断里跑两遍，
     留在循环里就得再加一个 flag，两处逻辑容易只改一处），
     所以这里跟着改成盯 pick_next 内部，守卫一个都不能少。 */
  {
    const flat = ic.replace(/\s+/g, ' ');
    const pickFn = (ic.match(/static int pick_next[\s\S]*?\n\}/) || [''])[0];
    ok(/static int pick_next\s*\(struct entry\s*\*ents,\s*int n,\s*int skip_over_budget\)/.test(flat),
       '择优判断在 pick_next 里（预算闸要跑两遍，不能散在循环中）');
    ok(/if \(ents\[j\]\.score < 0\) continue;/.test(pickFn),
       '择优排除 score=-1 的失败条目');
    ok(/if \(best < 0 \|\|/.test(pickFn),
       'pick_next 里 best<0 时不先解引用 ents[best]');
    ok(!/ents\[j\]\.score > ents\[best\]\.score\) best/.test(flat)
       || /best < 0 \|\|/.test(flat),
       '不存在无条件 ents[best] 解引用（best 可能还是 -1）');
  }
  ok(!/ents\[j\]\.score > ents\[best\]\.score\s*\)\s*best = j;/.test(ic),
     '不能出现 ents[best] 在 best 还是 -1 时的读取（越界）');
  ok(/for\s*\(\s*int tried = 0; tried < n; tried\+\+\s*\)/.test(ic),
     '外层按"试过的次数"计数，不是靠 best<0 退出');
}

/* ── 逐核路径必须是 cpuN/cpufreq/，不是 cpufreq/cpuN/ ──
   这几条是补 P0 那个洞的：之前新增的 21 条断言查的全是
   printf("corehz_%d=..." 这类 key 名在不在，一条都没查 sysfs 路径，
   于是 300 条全绿、功能恒空。 */
{
  // 自己声明一份：下面那块里的 cs 是块作用域，这里用不到它。
  const cs = csrc.replace(/\/\*[\s\S]*?\*\//g, '');
  const ic = iconSrc.replace(/\/\*[\s\S]*?\*\//g, '');
  ok(/CPUBASE\s+"\/cpu%d\/cpufreq\/%s"/.test(cs),
     '逐核路径模板是 CPUBASE "/cpu%d/cpufreq/%s"');
  ok(/#define CPUBASE "\/sys\/devices\/system\/cpu"/.test(cs),
     'CPUBASE 指向核心容器目录');
  // 显式禁止旧错法：CPUFREQ 后面直接跟 /cpu%d/
  const bad = /CPUFREQ\s*"\/cpu%d\//.exec(cs);
  ok(!bad, '禁止 CPUFREQ "/cpu%d/..."（那是 policy 目录，里面没有 cpuN）',
     bad ? '出现了 ' + bad[0] : '');
  ok(/static int core_path[\s\S]{0,300}?CPUBASE "\/cpu%d\/cpufreq\/%s"/.test(cs),
     'core_path 自己拼的是 CPUBASE + cpuN/cpufreq');
  ok(/static int core_exists[\s\S]{0,300}?core_path\(b, sizeof b, c, "cpuinfo_max_freq"\)/.test(cs),
     'core_exists 用 core_path（和读取路径同源）');
  /* P1：corepol 必须是 policy 号，不是数组下标。
     detect_policies 按 hw_max 去重后，下标 ≠ policy 号。 */
  ok(/printf\("corepol_%d=%d\\n", c, core_policy_of\(c\)\)/.test(cs),
     'corepol 报 policy 号（和 cpukey 同源），不是数组下标');
  ok(!/int pi = -1/.test(cs), '不再拿数组下标当 policy 号（去重后不相等）');
  /* P1.5：归属靠 readlink，不解析 related_cpus。
     related_cpus 可能是十六进制掩码，按十进制解析会全成 0。 */
  ok(/static int core_policy_of[\s\S]{0,700}?readlink\(/.test(cs),
     'policy 归属靠 readlink 符号链接');
  /* readlink 的路径不能带尾斜杠。
     core_path 的模板结尾是 "/cpu%d/cpufreq/%s"，传空 leaf 拼出来
     是 ".../cpu3/cpufreq/"，按 POSIX 路径解析，结尾带斜杠时最后一个分量
     会被**穿过**符号链接去解析，路径就不再"命名一个符号链接"，
     readlink 失败（EINVAL）→ 每颗核都 -1 → corepol_* 全是 -1 →
     表画得出来但每行都没有档位名，和 P1 一模一样，还不报错。
     所以这一处不能走 core_path，要写死不带斜杠的完整路径。 */
  {
    const fn = cs.match(/static int core_policy_of\b[\s\S]*?\n\}/);
    ok(!!fn, '找得到 core_policy_of 函数体');
    const body = fn ? fn[0] : '';
    ok(!/core_path\([^)]*,\s*""\s*\)/.test(body),
       'core_policy_of 不再用 core_path 传空 leaf（会拼出尾斜杠）');
    ok(/snprintf\(b, sizeof b, CPUBASE "\/cpu%d\/cpufreq", c\)/.test(body),
       'readlink 的路径不带尾斜杠');
  }
  ok(!/related_cpus/.test(cs),
     '不再解析 related_cpus（可能是十六进制掩码，按十进制解会全成 0）');
  /* P2：ncores 是真计数，不是最大下标+1 */
  ok(/int cores\[MAX_CORES\];/.test(cs), '存在的核号收进数组');
  ok(/if \(core_exists\(c\)\) cores\[nc\+\+\] = c;/.test(cs),
     'nc 按实际存在计数（有核掉线时不会多报）');
  ok(!/nc = c \+ 1/.test(cs), '不再用 nc = c + 1（那是最大下标+1，不是核数）');
  ok(/for \(int i = 0; i < nc; i\+\+\) \{\s*const int c = cores\[i\];/.test(cs),
     '按 cores[] 遍历（核号掉线后不连续，不能按 0..nc 走）');
  /* P3：三档原因码都得真产出，不能只有两档 */
  ok(/fprintf\(stderr, "E\/OPEN\\n"\)/.test(ic), 'E/OPEN 真有产出（不是空档）');
  ok(!/if \(fd < 0\) return 0;\s*\n\s*off_t sz/.test(ic),
     '打不开 APK 要报 OPEN，不能静默返回');
  /* 顺序：!saw_cd 必须在 !n 之前，否则是死代码。
     saw_cd 只在中央目录块里置 1，n 也只在同一块里自增，
     于是 saw_cd==0 蕴含 n==0。先判 !n 的话 !saw_cd 那条永远走不到，
     会把"目录没读到"报成"包里没有方图"，两种病因不能混。
     只断言两条字符串都在，查不出顺序，那条死分支就是这么全绿通过的。 */
  {
    const iOpen = ic.search(/if \(!saw_cd\)\s*\{\s*fprintf\(stderr, "E\/OPEN/);
    const iNone = ic.search(/if \(!n\)\s*\{\s*fprintf\(stderr, "E\/NOENTRY/);
    ok(iOpen >= 0 && iNone >= 0 && iOpen < iNone,
       '!saw_cd 判在前、!n 判在后（否则 saw_cd 分支不可达）',
       iOpen < 0 ? '没找到 !saw_cd' : iNone < 0 ? '没找到 !n'
                : `saw_cd@${iOpen} noentry@${iNone}`);
  }
}

/* ---- 图标是猜的，这话必须写在用户看得见的地方 ---- */
{
  // 不写清楚，用户只会以为图标功能坏了。
  // 正式的包过了 AGP 资源名缩短，只能靠"res/ 下的正方形图"去挑，
  // 挑到的未必是桌面图标——这是设计上的边界，不是故障。
  const rd2 = DOC;
  ok(/resources\.arsc/.test(rd2), 'README 说清要准确得解析 resources.arsc');
  ok(/不保证是桌面图标/.test(rd2), 'README 直说图标不保证是桌面图标');
  ok(/ph\.title = p \+ '\\n还没取到桌面图标/.test(html),
     '色块长按有提示（说明是猜的）');
  ok(/img\.title = pkg \+ '\\n这是从 APK 里挑出的一张方图/.test(html),
     '取到图后仍说明它可能不是桌面图标');
}

/* ---- 每个核心的频率必须逐核上报 ---- */
{
  const cs = csrc.replace(/\/\*[\s\S]*?\*\//g, '');
  // 只按 policy 报的话，页面上就只有 CPU0 / CPU6 两行，
  // 中间那几颗核跑到哪完全看不见。必须逐核报三组数。
  ok(/#define MAX_CORES 16/.test(cs), '核心数有上限');
  ok(/static int core_exists\s*\(/.test(cs), '有"这个核心在不在"的判断');
  // 用 cpuinfo_max_freq 判存在：它保证每个真核心都有，
  // 而 cpufreq 子目录未必（被裁掉的簇只有 cpuN/）。
  ok(/static int core_exists[\s\S]{0,200}?cpuinfo_max_freq/.test(cs),
     'core_exists 用 cpuinfo_max_freq 判（不是 cpufreq 目录）');
  ok(/printf\("ncores=%d\\n"/.test(cs), '导出里带上核心总数');
  // 有一条现成的通用断言（"WebUI 读的小写键全部有来源"）只查守护
  // **有没有 printf** 这个键，不查这个键**值**对不对——有 printf 就算过。
  // 所以 ncores 还得单独查：它要是恒 0 就画不出表，
  // 要是直接报 MAX_CORES 就会凭空多出 8 行 0 MHz。
  ok(/if \(core_exists\(c\)\) cores\[nc\+\+\] = c;/.test(cs),
     'ncores 由 core_exists 实数得出（不是常量、不是 MAX_CORES）');
  ok(/printf\("corehz_%d=%ld\\n"/.test(cs), '逐核报当前实际频率');
  ok(/printf\("coretgt_%d=%ld\\n"/.test(cs), '逐核报封顶值');
  ok(/printf\("coremax_%d=%ld\\n"/.test(cs), '逐核报硬件上限');
  ok(/printf\("coremin_%d=%ld\\n"/.test(cs), '逐核报硬件下限（柱子下面那行的范围）');
  /* 每核历史：/proc/stat 的 total/idle 差值算占用，频率和占用各存 8 个样本。 */
  ok(/"\/proc\/stat"/.test(cs), '逐核占用读 /proc/stat');
  ok(/printf\("coreload_%d=%d\\n"/.test(cs), '逐核报最新占用百分比');
  ok(/printf\("corefreqs_%d=/.test(cs) && /printf\("coreloads_%d=/.test(cs),
     '逐核报 8 个频率/占用历史样本');
  ok(/printf\("coreobsmin_%d=%ld\\n"/.test(cs) &&
     /printf\("coreobsmax_%d=%ld\\n"/.test(cs),
     '逐核报最近 5 秒观测范围（柱子下面那行随负载变）');
  ok(/if \(!strcmp\(cmd, "cores"\)\)/.test(cs), '守护有 cores 子命令');
  ok(/cores\)      \[ -f "\$BIN" \] && "\$BIN" cores/.test(apiSrc),
     'api.sh 暴露 cores');
  /* 关键：每颗核归哪个 policy 必须由守护报，不能让前端自己猜。
   * 前端手里 cpukey_N 的 N 是 policy 号不是核心号，
   * 拿核心号去查只能命中 cpu0/cpu6，cpu1~cpu3 全查不到。 */
  ok(/printf\("corepol_%d=%d\\n"/.test(cs), '逐核报归属的 policy');
  ok(/readlink/.test(cs), '归属靠 cpuN/cpufreq 这个符号链接（内核建的，指向 policyM）');
  ok(/renderCores/.test(html), '前端按核心画明细');
  ok(/corepol_/.test(html), '前端用 corepol_ 查档位，不用核心号去猜');
  // 读不到的值显示 —，不是 0 MHz：0 会被读成"核心停了"
  ok(/val > 0 \? fmtHz\(val, false\) : '—'/.test(html),
     '读不到的频率显示破折号，不填 0 MHz');
  /* 柱形图：格子 + 8 根柱 + 当前档高亮；数据来自 corehist/corecurbin。 */
  ok(/\.core-grid\{[^}]*repeat\(4,/.test(html) && /\.core-bar\{/.test(html),
     '每核用四列小卡 + 柱形图');
  ok(/s\['corefreqs_'\+i\]/.test(html) && /s\['coreloads_'\+i\]/.test(html),
     '前端按频率/占用历史画柱');
  ok(/loads\.slice\(-8\)/.test(html) && /b >= off/.test(html),
     '最新样本在最右，旧样本左移，不足 8 个右对齐');
  ok(/maxLoad > 0 && v === maxLoad/.test(html),
     '占用最高的一根柱标黄');
  ok(/Math\.max\(4, v\) \+ '%'/.test(html),
     '柱高直接等于占用百分比');
  ok(/async function loadCores/.test(html) &&
     /setInterval\(\(\)=>\{[\s\S]{0,80}?loadCores\(\)/.test(html) &&
     /\}, 1000\)/.test(html),
     '配置页每秒拉一次 cores（不是等 5 秒的 loadStatus）');
  /* 同步 exec 会卡输入：焦点在文本框/数字框、或正在触摸拖动时暂停轮询；
     搜索框防抖重画；拖滑杆合并 paintHw。 */
  ok(/function formBusy\(\)/.test(html) &&
     /!formBusy\(\)\) loadCores\(\)/.test(html) &&
     /!formBusy\(\)\) loadStatus\(\)/.test(html),
     '输入/拖动时暂停后台轮询（不让同步 root 调用卡按键）');
  ok(/function queuePicker\(\)/.test(html) &&
     /oninput="queuePicker\(\)"/.test(html) &&
     /function queueDsPicker\(\)/.test(html) &&
     /oninput="queueDsPicker\(\)"/.test(html),
     '两个搜索框都防抖 150ms 再重画（不再每敲一个键重建 400 行）');
  ok(/function queuePaintHw\(\)/.test(html) &&
     /queuePaintHw\(\)/.test(html),
     '拖滑杆时合并 paintHw，不按每个像素重排');
  ok(/pointerup[\s\S]{0,300}t\.blur\(\)/.test(html),
     '滑杆松手后主动失焦，实时曲线恢复');
  ok(/CORE_UI\.grid/.test(html) && /gridOnly/.test(html),
     '每秒只更新核卡，不重建整页');
  /* paintHw 每 5 秒清空 #hw-table；原地更新前必须确认 grid 还在文档里，
     否则只改脱离文档的旧节点，卡片直接消失。 */
  ok(/gridOnly && CORE_UI\.grid && CORE_UI\.grid\.isConnected/.test(html),
     '原地更新前检查核卡 grid 仍挂在文档上');
  ok(/s\['coreobsmin_'\+i\]/.test(html) && /s\['coreobsmax_'\+i\]/.test(html),
     '前端范围优先用观测值');
  /* 核卡百分比不能复用滑杆的 .pct：后者带 min-width:46px，
     在四列小卡里会把 cpuN 挤成 0 宽，只剩省略号。 */
  ok(!/\.core-top \.pct\{/.test(html) && /\.core-top \.cpct\{/.test(html) &&
     /pc\.className = 'cpct'/.test(html),
     '核卡百分比用独立类名 cpct，避开滑杆 .pct 的固定宽度');
}

/* ---- 关于页：赞赏码 ---- */
{
  ok(/<img src="reward-wechat\.jpg"[^>]*alt="微信赞赏码"/.test(html),
     '关于页有微信赞赏码');
  ok(/<img src="reward-alipay\.png"[^>]*alt="支付宝赞赏码"/.test(html),
     '关于页有支付宝赞赏码');
  ok(html.includes('一直都是免费更新 无收费 悬赏一下给作者动力更新！'),
     '赞赏码文案和约定一致');
  ok(fs.existsSync(MOD0 + '/webroot/reward-wechat.jpg') &&
     fs.existsSync(MOD0 + '/webroot/reward-alipay.png'),
     '两张赞赏码真的随包发布（不是只写了 <img>）');
  ok(/\.reward-grid\{[^}]*grid-template-columns:1fr 1fr/.test(html),
     '赞赏码两列并排');
  ok(/\.reward figcaption\{[^}]*color:#5f6b7c/.test(html),
     '赞赏码白卡的说明字用固定深灰（深色主题下不能跟着变浅）');
}

/* ---- 取图失败必须可诊断 ---- */
{
  const ic2 = iconSrc.replace(/\/\*[\s\S]*?\*\//g, '');
  // 静默留色块时两边都只能猜：是没装、是资源名被缩短了、还是图解不开，
  // 长得一模一样。守望去 stderr 打原因码，api.sh 存下来，关于页回显。
  ok(/if\s*\(!n\)\s*\{\s*fprintf\(stderr,\s*"E\/NOENTRY\\n"\);/.test(ic2),
     '一张候选都没有时打 E/NOENTRY');
  ok(/fprintf\(stderr,\s*"E\/EMIT\\n"\);/.test(ic2), '挑中了但解不出时打 E/EMIT');
  // 原因必须走 stderr：stdout 只有 data URI，混进去前端就废了
  ok(!/printf\("E\//.test(ic2), '原因码不许出现在 stdout（会污染 data URI）');

  ok(/^icon_fail\(\)/m.test(apiSrc), 'api.sh 有记录取图状态的地方');
  ok(/2>"\$_err"/.test(apiSrc), '守护的 stderr 被接住（不能 2>/dev/null 丢掉）');
  ok(/NOENTRY\)\s*icon_fail NOSQUARE/.test(apiSrc), 'NOENTRY -> NOSQUARE');
  ok(/EMIT\)\s*icon_fail BADIMAGE/.test(apiSrc), 'EMIT -> BADIMAGE');
  // .last 要能挡住脏值：它是写进 /data/local/tmp 的普通文件。
  // 白名单含 '='：兜底成功记的是 OK=UNZIP，只认大写字母时会把它
  // 归一化成 UNKNOWN —— 兜底成功了关于页却说"还没取过图标"，
  // 凭空造一个假的失败状态出来。
  /* 这两条必须查**去注释后**的源码，不能查全文。
   /* 这两条必须查**去注释后**的源码，不能查全文：
      解释"某种坏写法为什么会坏"的注释里就带着那个被禁的写法，
      全文搜索会匹配到自己的注释，断言恒红——
      而红的原因只是注释提到了它，不是代码又退回去了。
      同理，去掉注释之后正向那条也只可能在真正的代码里命中。 */
  const apiNoComment = stripSh(apiSrc);
  ok(/\*\[!A-Z=\]\*\)/.test(apiNoComment),
     'icon_last 只认大写字母和 =，脏值退回 UNKNOWN');
  // 不能退回"只认大写字母"。注意 [!A-Z] 是 [!A-Z=] 的**子串**，
  // 所以光搜 [!A-Z] 会匹配到 [!A-Z=] 自己，永远为真；
  // 必须带上紧跟的 '*' 才区分得开两种写法。
  ok(!/\*\[!A-Z\]\*\)/.test(apiNoComment),
     'icon_last 的白名单不能退回"只认大写字母"（会吃掉 OK=UNZIP）');
  // 缓存命中也必须刷新状态：命中时不写，关于页就一直停在
  // 上一次那个包的结果——很可能是别的应用留下的 NOTINST，
  // 于是图标明明正常，关于页却说没取到。
  // （现在这一笔带上了来源，写成 OK=<DAEMON|UNZIP>，所以两种都认。）
  ok(/if\s*\[\s*-s\s+"\$_out"\s*\]\s*;\s*then[\s\S]{0,400}?icon_fail ("OK=\$_v"|OK)\s*;?/.test(apiSrc),
     '命中缓存时也把状态刷成 OK（带来源后缀）');
  // 中间可能夹着注释行，所以只要求"取到图的分支里有 icon_fail OK"，
  // 靠前后文定位而不是靠相邻两行——注释位置一变断言就假失败。
  ok(/data:image\/[^\n]*base64,[\s\S]{0,400}?icon_fail OK[\s\S]{0,80}?cat "\$_out"/.test(apiSrc),
     '新取到图时也把状态刷成 OK');
  ok(/icon_fail NOTINST;\s*echo;\s*return 0/.test(apiSrc),
     '没装也要记一笔（关于页据此区分"没装"和"取不到图"）');

  ok(/printf 'ICONLAST=%s\\n'/.test(apiSrc), 'about 输出带上最近一次的取图状态');
  ok(/ICONLAST/.test(html), '关于页把取图状态显示出来');
  ok(/NOSQUARE:\s*\['bad'/.test(html) && /资源名缩短|res\/aK\.png/.test(html),
     '关于页说清"资源名被缩短"是怎么回事');

  // api.sh 能吐出的每一个图标状态码，前端都必须有对应的文案。
  // 之前就是漏了 NODIR：api.sh:341 写了 NODIR，前端 ICON_STATE 没有这个键，
  // 于是关于页显示"未知状态 NODIR"——用户看到的是一句看不懂的话，
  // 而这条路径恰恰只在"缓存目录建不起来"这种已经出问题时才命中，
  // 等于在最需要说明的时候什么也没说明。
  // 不写死一份清单，而是从 api.sh 里现抽：抽到什么就得覆盖什么，
  // 以后加码忘了加文案，这里立刻红。
  const emitted = [...apiSrc.matchAll(/icon_fail ([A-Z]+)/g)].map(m => m[1]);
  const uniq = [...new Set(emitted)].filter(x => x !== 'NOICON' || true);
  for(const code of uniq){
    ok(new RegExp('\\b' + code + ':\\s*\\[').test(html),
       `api.sh 会吐 ${code}，前端 ICON_STATE 里有对应文案`);
  }
  ok(uniq.length >= 7, `api.sh 至少能吐 7 种图标状态，实到 ${uniq.length} 种`);
  // 兜底不能把码吞掉：真出现没预期的码时，那一行是唯一的现场。
  ok(/'未预期的图标状态：'/.test(html),
     '未知码的兜底文案里带上原码（唯一现场，别丢）');
}

/* ---- unzip 兜底：规则只有一份，且必须验内容 ---- */
{
  const ic3 = iconSrc.replace(/\/\*[\s\S]*?\*\//g, '');
  const apiCode = apiSrc.replace(/^\s*#.*$/gm, '');   // 去注释行

  /* ① 挑图规则只留一份。
     兜底在 shell 里，如果它自己写一套 mipmap/xxhdpi/ic_launcher 规则，
     就是第二轮"两处规则漂移"的起点：改了 C 里的密度档位，兜底还按老的挑，
     两条路给出不同图标，页面显示哪个取决于哪条先成功——
     而且没有任何报错，因为两套规则都能"正常"工作。
     只许调守护的 score 子命令。
     category.LAUNCHER 是枚举桌面应用用的，不是挑图规则，先剔掉再查。 */
  ok(!/mipmap-|ic_launcher|launcher|xxhdpi/i.test(apiCode.replace(/category\.LAUNCHER/g, '')),
     'shell 里不许出现挑图规则（只有 C 里 score_name 那一份）');
  ok(/_icon_via_unzip\(\)/.test(apiSrc), 'api.sh 有 unzip 兜底函数');
  // 兜底必须真的走 score：条目名喂给守护，打分由 C 做
  ok(/unzip -l[\s\S]{0,400}?\|\s*"\$BIN" score/.test(apiSrc),
     'unzip -l 的条目名喂给守护 score（不在 shell 里重写规则）');
  ok(/!\s*strcmp\(cmd, "score"\)/.test(csrc) && /icon_score_stdin/.test(csrc),
     '守护有 score 子命令并调用 icon_score_stdin');
  // 壳必须放在 icon.c 里：score_name 是 static，socfreqd.c 调不到
  ok(/int icon_score_stdin\(void\)/.test(ic3), 'icon_score_stdin 定义在 icon.c');
  ok(/icon_score_stdin[\s\S]*?score_name\(/.test(ic3),
     'score 壳直接调 score_name（同一份实现）');

  /* ② 兜底必须验内容前缀。
     `unzip -l` 的列格式各家不一样，名字剥错时 `unzip -p` 输出空
     或者一段错误文本，base64 之后照样非空。不验就是把垃圾当图标写进缓存，
     而且缓存键是 包名+mtime，之后每次都命中那份垃圾——
     用户看不出任何异常，只会觉得"图标是花的"。 */
  ok(/iVBOR/.test(apiSrc) && /UklGR/.test(apiSrc),
     'unzip 兜底校验 base64 内容前缀（PNG / WebP）');
  ok(/\/9j\//.test(apiSrc), 'unzip 兜底也认 JPEG 前缀');

  /* ③ 4GB 上限和 32 位偏移绑在一起。
     cdsz/cdoff 是 uint32_t，抬过 4GB 偏移会被截断，截断后
     (cdoff + cdsz <= sz) 反而更容易判成"装得下"——静默读错数据，
     比直接报错糟得多。字段没改成 uint64_t 之前，上限就是 4GB 硬边界。 */
  ok(/sz > 4LL \* 1024 \* 1024 \* 1024/.test(ic3), '上限抬到 4GB（原本 512MB）');
  ok(!/sz > \d+L?L? \* 1024 \* 1024 \* 1024 \* 1024/.test(ic3),
     '不许出现 >4GB 的上限（cdsz/cdoff 还是 uint32_t，会截断后误判装得下）');
  ok(/uint32_t cdsz = rd32/.test(ic3) && /uint32_t cdoff = rd32/.test(ic3),
     'cdsz/cdoff 仍是 uint32_t（这条和上面的 4GB 上限是一对，别只改一半）');

  /* ④ E/TOOBIG 必须三段打通：守护产出 → api.sh 分派 → 前端文案。
     断一节的症状都是"关于页显示未知状态"，
     而这只在已经出问题时才命中，等于最需要说明的时候什么也没说明。 */
  ok(/fprintf\(stderr,\s*"E\/TOOBIG\\n"\)/.test(ic3), '守护超大包时打 E/TOOBIG');
  ok(/TOOBIG\)\s*icon_fail TOOBIG/.test(apiSrc), 'api.sh 有 TOOBIG 分派');
  ok(/TOOBIG:\s*\['bad'/.test(html), '前端 ICON_STATE 有 TOOBIG 文案');
  // 4GB 和"不是普通文件"必须分开报：合成一个之后，
  // "包太大"和"文件坏了"共用 OPENFAIL，查的时候会照着查权限和路径，
  // 而真正的原因是体积——王者荣耀那种 2GB+ 的游戏包就一直查不出来。
  ok(/sz <= 0\)/.test(ic3), 'sz<=0 单独走 E/OPEN');
  ok(/OPEN\)\s*icon_fail OPENFAIL/.test(apiSrc) && /TOOBIG\)\s*icon_fail TOOBIG/.test(apiSrc),
     'OPEN 与 TOOBIG 分派到不同的码');

  /* ⑤ 兜底成功不能记成 OK。
     兜底一旦生效，主路（守护解析中央目录）坏没坏就再也看不出来了——
     页面照常出图，没有任何异常表现。OK=UNZIP 是留给以后的诊断入口。 */
  ok(/icon_fail OK=UNZIP/.test(apiSrc), '兜底成功记 OK=UNZIP 而不是 OK');
  ok(/'OK=UNZIP':/.test(html), '前端认得 OK=UNZIP');

  /* ⑥ 来源要活过缓存。
     上面那条只在"第一次取图"时成立：缓存命中走的是另一个分支，
     那里写死 icon_fail OK 的话，第二次以后关于页永远是光秃秃的
     "图标正常"——而用户点开关于页看这行时几乎必然已经是第二次以后。
     观测入口在最该说话的时候沉默，等于没有观测入口。
     所以两条成功路径都要落一个 .via，缓存命中时把它读回来。 */
  ok(/printf 'DAEMON\\n' > "\$_out\.via"/.test(apiSrc),
     '主路成功把来源写进 .via（缓存命中时才有得读）');
  ok(/printf 'UNZIP\\n' > "\$_out\.via"/.test(apiSrc),
     '兜底成功把来源写进 .via');
  ok(/head -n 1 "\$_out\.via"/.test(apiSrc) && /icon_fail "OK=\$_v"/.test(apiSrc),
     '缓存命中读 .via 并报 OK=<来源>');
  // .via 缺失时报裸 OK，不许猜 DAEMON：猜错一次，这条线索就永久失真。
  ok(/icon_fail OK ;;/.test(apiSrc),
     '.via 不存在时报裸 OK（宁可"来源未记录"也不猜一个 DAEMON）');
  ok(/'OK=DAEMON':/.test(html), '前端认得 OK=DAEMON');
  ok(/来源未记录/.test(html), '裸 OK 的文案说清"来源没记上"，不冒充守护');
  // .via 结尾是 .via，icon_trim 那句 grep '\.b64$' 数不到它：
  // 不跟着删就是每清一张图留一个孤儿文件。
  ok(/grep '\\\.b64\$' \| tail[\s\S]{0,200}\$ICON_DIR\/\$f\.via/.test(apiSrc),
     'icon_trim 删 .b64 时连着删它的 .via');
  ok(/\[ -f "\$ICON_DIR\/\$\{v%\.via\}" \] \|\| rm -f/.test(apiSrc),
     'icon_trim 另外清掉找不到对应 .b64 的孤儿 .via');

  /* ⑦ 同分要有第二判据，且两条路用同一把尺子。
     主路是"同分取 csize 小的"；兜底只有 sort -rn 的话，
     同分顺序取决于 unzip -l 的枚举顺序 —— 于是同一档多个候选时
     两条路会给出不同的图，漂的是排序不是打分，更难发现。
     现在 shell 保留 Length 列、score 原样带回、按 (分数降序, 字节升序) 排。 */
  ok(/printf\("%d\\t%s\\t%s\\n", sc, len, name\)/.test(ic3),
     'score 输出三列：分数<TAB>字节数<TAB>名字');
  ok(apiSrc.includes('\\1\\t'),
     'shell 保留 unzip -l 的 Length 列（同分才有得比）');
  ok(/sort -k1,1nr -k2,2n/.test(apiSrc),
     '排序是分数降序 + 字节升序（对齐主路的"同分取小"）');
  ok(/cut -f3-/.test(apiSrc), '取名字用 cut -f3-（名字里可以有空格）');
  // 只有第一段全是数字才当长度，否则整行还是名字：
  // zip 条目名理论上可以含 TAB，不能见 TAB 就切。
  ok(/for \(k = 0; k < \(size_t\)\(tab - nm\); k\+\+\)/.test(ic3),
     'score 只在第一段全是数字时才当它是长度列');

  /* ⑧ 两条路能力互补，这条边界不许被 README 说反。
     兜底只有"按名字挑"，没有第二轮（按图片尺寸猜）——
     缩短过的包（res/aK.png）喂给 score 是 0 条候选，只有主路能出图。
     谁以为"有兜底就万无一失"，就会把主路的第二轮当冗余删掉。 */
  const readme = DOC;
  // 判据用"命令表里只有 zcat/gzip/gunzip"那半句：它只在把"没有 unzip"
  // 当成事实陈述时才会出现。README 里保留了一句带引号的历史说法
  // （"早先这里写的是…这条前提是错的"），那是纠正不是断言，
  // 用"没有 unzip"当关键词会把这句合法的纠正一起误杀。
  ok(!/命令表里只有 zcat/.test(readme),
     'README 不再把"Android 没有 unzip"当事实断言（已被真机证伪）');
  ok(/这条前提是错的|已被.*证伪/.test(readme),
     'README 明确写出这条前提被纠正过，别让下一个人再照它做决定');
  ok(/互补/.test(readme), 'README 写明两条路是互补不是包含');
  ok(/兜底只有"按名字挑"这一档|只有主路能出图/.test(apiSrc + readme),
     '兜底覆盖不到缩短包这件事写在代码或文档里');
}

/* ---- 安装/执行脚本的输出 ---- */
const actionSrc = fs.readFileSync(MODDIR + '/action.sh', 'utf8');
const customSrc = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
const uninSrc  = fs.readFileSync(MOD0 + '/uninstall.sh', 'utf8');

/* ---- uninstall.sh 的兜底必须三本账都读 ---- */
{
  // 兜底存在的唯一场景，是守护二进制已经跑不了（被 loader 拒掉、或被
  // 管理器删了）。那时候只剩 run/*.saved。数值那本早就有了，
  // 但 perms 和 govs 没读：节点权限留在 666、调速器留在我们改的那个上，
  // 而且一句日志都不会有——比数值不还原还难查。
  ok(/perms\.saved/.test(uninSrc), 'uninstall.sh 兜底读了 perms.saved');
  ok(/govs\.saved/.test(uninSrc),  'uninstall.sh 兜底读了 govs.saved');
  /* 账本被清/守护从未跑起来时，至少把 CPU 上限放回硬件最高；
     深睡脚本缺失时，按快照直接还原 Doze 常量与临时白名单。 */
  ok(/_had_journal=0/.test(uninSrc) && /cpuinfo_max_freq/.test(uninSrc) &&
     /ds_snap\.conf/.test(uninSrc) &&
     /settings put global device_idle_constants/.test(uninSrc),
     '卸载兜底：无账本也恢复 CPU 上限；深睡快照不依赖脚本也能还原');
  ok(/rm -rf \/data\/adb\/modules_update\/soc_freqcap/.test(uninSrc),
     '卸载时清掉同模块的待更新目录（防止重启后又装回来）');

  // perms.saved 的第二列恒为 0（只是占位，perms_restore 也只取第三列）。
  // 兜底里绝不能把 VAL 写回去：max_pwrlevel 这种 0 是合法索引值，
  // 写进去等于把节点清零。
  //
  // 断言要盯住"VAL 出现在任何写入位置"，不能只查 `> "$VAL"`：
  // 真要写坏它的人会写成 `echo "$VAL" > "$NODE"`——
  // 节点名换一个，上面那个前提就不成立了。实测就是这样：
  // 变异写了进去，断言却因为只认一种写法而放行。
  const pBlock = (uninSrc.match(/restore_perms\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
  ok(pBlock && !/echo\s+"\$VAL"|>\s*"\$VAL"|printf[^\n]*"\$VAL"/.test(pBlock),
     'perms.saved 兜底只 chmod、不回写 VAL',
     '兜底里把 $VAL 写进了节点: ' + ((pBlock.match(/.*\$VAL.*/) || [''])[0] || '').trim());
  ok(pBlock && /chmod\s+"\$MODE"\s+"\$NODE"/.test(pBlock),
     'perms.saved 兜底按快照里的 MODE 收回权限',
     (pBlock.match(/chmod.*/) || [''])[0]);

  // govs.saved 每个节点两行：journal_add 写的三列权限行，
  // 和 fprintf 写的两列名字行。只认后者。
  const gBlock = (uninSrc.match(/restore_govs\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
  // 注意两件事：
  //  1. match(/case.*/) 会命中文件里第一条 case（nodes.saved 那条用 $VAL），
  //     不是 govs 这条。要在 gBlock 里找，且认的是 $B。
  //  2. 这里的模式是 *[0-9]*，不是 *[!0-9]*——第二个字段不是数字才当名字。
  //     写成 [!0-9] 会把方向搞反：带数字的名字会被当成权限行丢掉。
  //     我自己在这上面栽过一次：正则里写 *\[\^0-9\]* 去找文件里的 *[0-9]*，
  //     永远不中，差点以为 shell 写错了。
  ok(gBlock && /case\s+"\$B" in ''\|\*\[\^?0-9\]\*\)/.test(gBlock),
     'govs.saved 兜底只取带名字的那行（第二列非数字）',
     (gBlock.match(/case\s+"\$B".*/) || [''])[0] || 'gBlock 里没有 case "$B"');
  ok(gBlock && /echo\s+"\$B"\s*>\s*"\$A"/.test(gBlock),
     'govs.saved 兜底把名字写回 scaling_governor');
}

{
  // 守护自己写两行、也读这两行，读的那行必须就是写入的那行：
  // sscanf("%s %s %o") 只在三列时返回 3，于是它取到 journal_add
  // 写的权限行，把字面量 "0" 写进 scaling_governor，
  // 而真正的名字那行只有两列、返回 2，被挡掉了。
  // 恢复完的调速器是个 "0"，内核多半拒收。
  const csrcAll = fs.readFileSync(process.argv[2], 'utf8');
  const govFn = (csrcAll.match(/static void restore_governors[\s\S]*?\n\}/) || [''])[0];
  ok(govFn && /sscanf\(line,\s*"%511s\s+%63s"\s*,\s*node,\s*gov\)\s*!=\s*2/.test(govFn),
     'restore_governors 认得两列的名字行',
     '只按三列解析的话会读到字面量 0');
  ok(govFn && /strspn\(gov,\s*"01234567"\)/.test(govFn),
     'restore_governors 跳过纯数字（权限行），不把 0 当名字写回');

  // 两列分支写完名字后必须把权限还回去，而且还得是"还原当时的"，
  // 不是某个循环里预置的字面量。
  // 原先是 chmod(node, mode)，而 mode 每轮开头被重置成 644：
  // %o 只在匹配成功时赋值，两列那条路它根本不更新。
  // 两行的顺序又是"权限行在前、名字行在后"，于是权限行刚把 0664
  // 恢复好，名字行又压回 0644——perfd 就再也换不动调速器了，
  // 而且一句日志都没有。
  const twoCol = (govFn.match(/sscanf\(line,\s*"%511s\s+%63s"\s*,\s*node,\s*gov\)\s*!=\s*2[\s\S]*?\n  \}/) || [''])[0];
  ok(twoCol && /int\s+before\s*=\s*read_mode\(node\)/.test(twoCol),
     '两列分支写之前先 read_mode 记下当前权限',
     twoCol ? '没看到 read_mode' : '没截到两列分支');
  ok(twoCol && /chmod\(node,\s*before\)/.test(twoCol),
     '写完把权限还给记录值（不是写死 0644）',
     twoCol ? '末尾不是 chmod(node, before)' : '');
  // 也不能再有 mode = 644 这种预置：那是病根，
  // 留着会让人以为还有"保底权限"这回事。
  ok(govFn && !/mode\s*=\s*644\s*;/.test(govFn),
     '循环里不再预置 mode = 644（%o 失败时不赋值，它会变成字面量）',
     (govFn.match(/.*mode\s*=\s*644.*/) || [''])[0]);
}

{
  // ui_print 是管理器的函数，不保证存在。
  // 没有它时脚本里每一行都变成 "command not found" 打到 stderr，
  // KSU 的执行界面就整页空白——用户完全看不出是哪里坏了。
  ok(/command -v ui_print[\s\S]{0,120}ui_print\(\) \{/.test(actionSrc),
     'action.sh 在 ui_print 缺失时有兜底，否则界面全空白');
  ok(/command -v ui_print[\s\S]{0,120}ui_print\(\) \{/.test(customSrc),
     'customize.sh 同样兜底 ui_print');

  // 前面不加 v / x 小标记：管理器自己会加一套，再加一次就变成
  // "v 未检测到冲突"，看着像多了个字。
  const marks = [];
  for (const m of customSrc.matchAll(/ui_print\s+"(\s+)[vx]\s/g))
    marks.push(m[0].trim());
  for (const m of actionSrc.matchAll(/say\s+"(\s+)[vx]\s/g))
    marks.push(m[0].trim());
  ok(marks.length === 0, '输出里没有 v / x 前缀', marks.join(' | '));

  // 芯片代号统一大写。原厂属性是小写（ro.soc.model = sm8750），
  // 直接打出来跟旁边大写的机型名不齐。
  ok(/tr 'a-z' 'A-Z'/.test(customSrc), '安装时芯片代号转大写');
  ok(!/tr 'A-Z' 'a-z'/.test(customSrc), '安装时不再转小写');
  ok(/tr 'a-z' 'A-Z'/.test(actionSrc), '执行脚本里芯片代号也转大写');
  ok(!/tr 'A-Z' 'a-z'/.test(actionSrc), '执行脚本里不再转小写');
  // WebUI 端也要转：守护换取值来源也不会退回小写
  ok(/toUpperCase\(\)/.test(html), 'WebUI 显示的芯片代号也是大写');

  // 标题独占一行，不夹在横杠中间。
  // 挤在一行时，部分管理器会把横杠两端的文字当行首标记解析，
  // 刷机日志最上方就会出现奇怪的竖线/方框（用户图三）。
  for (const [n, src] of [['customize.sh', customSrc], ['action.sh', actionSrc]]) {
    const bad = [...src.matchAll(/ui_print\s+"━+[^"━]+━+"/g)].map(x => x[0]);
    ok(bad.length === 0, n + ' 的标题不夹在横杠中间', bad.join(' | '));
  }
  // 所有分隔线等宽，长短不齐会让刷机日志看起来是坏的
  for (const [n, src] of [['customize.sh', customSrc], ['action.sh', actionSrc]]) {
    const w = [...src.matchAll(/ui_print\s+"(━+)"/g)].map(x => x[1].length);
    ok(w.length > 0 && new Set(w).size === 1,
       n + ' 的分隔线等宽', '宽度: ' + [...new Set(w)].join('/'));
  }
}

/* ── ⑦ 分应用档位（app_modes.conf）─────────────────────────────
 * 这一层最容易出的三个错，都在"静默不生效"这一类：
 *   1) 优先级顺序写反 → 熄屏解除 / 开机满血把用户给相机设的满血盖掉，
 *      或反过来让专属档位永久压住满血窗口；
 *   2) 换档位时没有 force 写一遍 → 巡检的判断是 cur<=cap 就跳过，
 *      从 70% 的应用切到 80% 的应用，节点永远停在 70%；
 *   3) 改完全局配置忘了把生效中的应用档位盖回去 → 页面点一次"保存"，
 *      相机的满血就静默变回 65%。
 * 所以这三条都钉住，而不是只查"有没有读文件"。 */
console.log('\n== 分应用档位 ==');
{
  ok(/#define MODE_FILE\s+SF_DATADIR "\/app_modes\.conf"/.test(csrc),
     '守护从数据目录读 app_modes.conf');
  ok(/MODES="\$DATA\/app_modes\.conf"/.test(apiSrc),
     'api.sh 的 MODES 与守护同一路径');

  /* 优先级：命中判断必须在 game 与 boost 都排除之后。
   * 用 has_mode = (!game && !boost && ...) 这个式子当锚点——
   * 顺序一旦改回去，这行就变了，断言才会红。 */
  ok(/int has_mode = \(!game && !boost && fg\[0\]\) \? mode_lookup\(fg, &mpct, &mgpu\) : 0;/.test(csrc),
     '优先级 游戏名单 > 满血窗口 > 应用专属（判断式是这条的唯一锚点）',
     (csrc.match(/int has_mode[^\n]*/) || ['没找到'])[0]);
  ok(csrc.indexOf('int game = foreground_pkg') < csrc.indexOf('int has_mode ='),
     '先算游戏再算应用档位（顺序反了应用档位会永久压住游戏名单）');

  // 换档必须 force：巡检的 cur<=cap 跳过逻辑对"升档"方向是坏的
  ok(/int caps_changed = \(want_pct != g_app_pct\);/.test(csrc) &&
     /apply_normal\(caps_changed \|\| adapt_changed\)/.test(csrc),
     '档位变了就强制重写（含稳态巡检那一处）',
     (csrc.match(/apply_normal\([^)]*\)/g) || []).join(' , '));
  ok((csrc.match(/apply_normal\(0\)/g) || []).length === 0,
     '没有残留的 apply_normal(0)（换档必须能强制生效）');

  // 改完全局配置要把生效中的档位盖回去（施加点已从 reload_if_changed 挪到主循环，
  // 因为生效值还要叠加实时偏移，必须等本轮传感器算完）
  {
    const body = (csrc.match(/if \(cfg_changed \|\| adapt_changed\) \{[\s\S]*?\n    \}/) || [''])[0];
    ok(/if \(g_app_pct > 0\) set_caps\(g_app_pct,/.test(body),
       '重算块里 assign_pct 之后重新套上生效中的应用档位', body ? '' : '重算块没抠到');
    ok(body.indexOf('assign_pct') < body.indexOf('set_caps(g_app_pct'),
       '顺序对：先按全局重算，再用应用档位盖上去');
    ok(/apply_normal\(1\)/.test(body), '重算块里带 force（升档方向不吃 cur<=cap 跳过）');
  }

  // 数字档：CPU 段和 GPU 段分开取；两段都可以缺省（0 = 跟随全局）
  ok(/int want_pct = \(has_mode && !is_full && mpct > 0\) \? mpct : 0;/.test(csrc) &&
     /int want_gpu = \(has_mode && !is_full\) \? mgpu : 0;/.test(csrc),
     'full 不参与百分比档位（它走交还系统那条路）；数字档按 CPU/GPU 两段取');
  ok(/cfg\.pct\[g_pol\[i\]\.tier\]/.test(csrc),
     '全局档沿用 assign_pct 算好的档位归属，不各档各算');

  // 读的一侧要容忍坏行，但不能把坏行当"命中"
  ok(/if \(!strcmp\(val, "full"\)|!strcmp\(val, "FULL"\)\)/.test(csrc) ||
     /strcmp\(val, "full"\) \|\| !strcmp\(val, "FULL"\)/.test(csrc),
     '守护认 full（大小写都收，和 api.sh 一致）');
  ok(/if \(pct < 1 \|\| pct > 100\) continue;/.test(csrc),
     '越界数字整条丢掉（写的一侧本来就该挡，读的一侧不能拿它当档位）');
  ok(/g_modes\[i\]\.pct = pct;/.test(csrc) && /g_modes\[i\]\.gpu = gpuv;/.test(csrc)
     && /dup = 1;/.test(csrc),
     '同一包重复出现取后一条，不追加第二行（CPU/GPU 两段一起覆盖）');

  /* api.sh 侧：包名不能拼进正则。
   * 包名里的点是通配符，grep -E "^com.a.b=" 会连 "comXaXb=90" 一起匹配上
   * ——删一个应用的档位顺带删了别人的。
   * 判据必须先剥掉注释：解释"为什么不能用 grep -E"的那行注释本身就写着
   * grep -E "^$_p="，不剥的话这条断言永远红，而代码是对的。 */
  const apiCode = apiSrc.split('\n').filter(l => !l.trim().startsWith('#')).join('\n');
  ok(!/grep[^|;\n]*["']\^\$_p=/.test(apiCode),
     'modes_del / modes_set 没把包名拼进正则（点会被当通配符）',
     (apiCode.match(/grep[^\n]*\^\$_p=/g) || [''])[0]);
  ok(/awk -v k="\$_p" 'index\(\$0, k "="\) == 1/.test(apiSrc),
     '按前缀判等用 awk 的 index()，是固定串比较');
  ok(/modes_list\) modes_list ;;/.test(apiSrc) && /modes_set\)  modes_set/.test(apiSrc) &&
     /modes_del\)  modes_del/.test(apiSrc),
     '三个 modes_* 都接进了分发');

  /* 内置相机满血清单：装机只给已装的包追加，且用户改过/删掉的都不动。 */
  {
    const b = fs.readFileSync(MOD0 + '/scripts/builtin_modes.txt', 'utf8');
    ok(['com.android.camera', 'com.android.camera2', 'com.oplus.camera',
        'com.sec.android.app.camera', 'com.vivo.camera', 'com.huawei.camera',
        'com.google.android.GoogleCamera'].every(p =>
          new RegExp('^' + p.replace(/\./g, '\\.') + '=full$', 'm').test(b)),
       '内置清单覆盖主要厂商的相机包名');
    const pk = b.split('\n').filter(l => /^[A-Za-z]/.test(l)).map(l => l.split('=')[0]);
    ok(pk.length >= 15 && new Set(pk).size === pk.length,
       '内置清单没有重复包名', pk.length + ' 条');
    const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
    ok(/builtin_modes\.txt/.test(cz) && /builtin_modes\.base\.txt/.test(cz) &&
       /#builtin/.test(cz) && /grep -Fxq -- "\$_p" "\$DATA\/\.binst"/.test(cz) &&
       /grep -Fxv -f "\$DATA\/\.bold" "\$DATA\/\.bnew"/.test(cz),
       '装机只给已安装的包追加（#builtin），并按基准只补新增项');
    ok(/相机类应用.set/.test(html) || /相机类应用装机时/.test(html),
       '页面上说明相机类默认满血、可改可删');
  }

  /* 内置深睡永久豁免：通讯/运动健康默认加白，只给已安装的包写。 */
  {
    const w = fs.readFileSync(MOD0 + '/scripts/builtin_white.txt', 'utf8');
    const need = ['com.tencent.mm', 'com.tencent.mobileqq', 'com.alibaba.android.rimet',
                  'com.tencent.wework', 'com.mi.health'];
    ok(need.every(p => new RegExp('^' + p.replace(/\./g, '\\.') + '\\b', 'm').test(w)),
       '内置豁免覆盖微信 / QQ / 钉钉 / 企业微信 / 小米运动健康');
    const pk = w.split('\n').filter(l => /^[A-Za-z]/.test(l)).map(l => l.trim().split(/\s+/)[0]);
    ok(pk.length >= 30 && new Set(pk).size === pk.length,
       '豁免清单去重且覆盖通讯/音乐/健康/系统应用', pk.length + ' 条');
    ok(['com.netease.cloudmusic', 'com.tencent.qqmusic', 'com.luna.music',
        'com.kugou.android', 'cn.kuwo.player'].every(p =>
          new RegExp('^' + p.replace(/\./g, '\\.') + '\\b', 'm').test(w)),
       '主流音乐应用默认豁免（网易云/QQ音乐/汽水/酷狗/酷我）');
    ok(['com.android.deskclock', 'com.coloros.alarmclock', 'com.android.server.telecom',
        'com.android.mms', 'com.android.calendar', 'com.android.contacts'].every(p =>
          new RegExp('^' + p.replace(/\./g, '\\.') + '\\b', 'm').test(w)),
       '时钟/电话/信息/日历等系统关键应用默认豁免');
    ok(['com.ss.android.lark', 'com.larksuite.suite', 'com.whatsapp',
        'org.telegram.messenger', 'org.thoughtcrime.securesms', 'jp.naver.line.android']
        .every(p => !new RegExp('^' + p.replace(/\./g, '\\.') + '\\b', 'm').test(w)),
       '飞书/Lark/WhatsApp/Telegram/Signal/LINE 不在清单里');
    const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
    ok(/builtin_white\.txt/.test(cz) && /builtin_white\.base\.txt/.test(cz) &&
       /grep -Fxq -- "\$_p" "\$DATA\/\.winst"/.test(cz) &&
       /ds_conf_write/.test(cz) && /grep -Fxv -f "\$DATA\/\.wold"/.test(cz) &&
       /grep -Fxv -f "\$DATA\/\.wnew"/.test(cz),
       '装机只给已安装的包写 white=；只补新增、撤下上游移除的项');
    const ds2 = fs.readFileSync(MOD0 + '/scripts/deep_sleep.sh', 'utf8');
    ok(/ALARM_GUARD_SEC=1800/.test(ds2) && /ds_alarm_soon\(\)/.test(ds2) &&
       /origWhen/.test(ds2) && /暂不进入深睡/.test(ds2) &&
       /暂不进入系统 Doze/.test(ds2),
       '闹钟前 30 分钟不进入，7:30 收口也让路（防止早上闹钟不响）');
    ok(/微信、QQ、钉钉/.test(html), '睡眠页说明内置豁免默认加入、可移除');
    ok(/builtin_white\) builtin_white ;;/.test(apiSrc) &&
       /builtin_white\(\)\s*\{[\s\S]{0,600}?scripts\/builtin_white\.txt/.test(apiSrc),
       '内置豁免的中文名有后端命令（清单只读，不参与匹配）');
    ok(/async function loadBuiltinNames/.test(html) &&
       /const BUILTIN_LABELS = \{\}/.test(html) &&
       /hasOwnProperty\.call\(BUILTIN_LABELS, p\)/.test(html),
       '内置中文名晚到也能盖回（BUILTIN_LABELS 覆盖，不再依赖启动顺序）');
    ok(/requestAnimationFrame\(\(\)=>setTimeout\(async \(\)=>\{\s*await loadStatus\(\);[\s\S]{0,600}?loadBuiltinNames\(\);/.test(html)
       && /await loadStatus\(\);[\s\S]{0,120}?requestAnimationFrame\(\(\)=>setTimeout\(\(\)=>\{\s*loadLog\(true\);/.test(html),
       '启动先等状态回来并让出一帧画徽标，再发日志/应用名（exec 同步，不能紧跟着发）');
    ok(/@@ROOT=/.test(html) && /@@NOAPI/.test(html) && /probeOwner/.test(html)
       && /if\(probeOwner === key\) return apiOut\(r\)/.test(html),
       '探模块根与目标命令合成同一条 shell：徽标只付一次 exec，且不重复执行首条命令');
    ok(/for\(const wait of \[300, 900, 2000\]\)\{[\s\S]{0,220}?r = await exec\(probe\);/.test(html),
       '首次 exec 未就绪时按 300/900/2000ms 退避重试（不把偶发失败拖成 5 秒后的下一拍）');
    ok(/statusFail === 1 && !DEAD/.test(html) && /setTimeout\(\(\)=>loadStatus\(\), 400\)/.test(html)
       && /!b\.classList\.contains\('stale'\)/.test(html),
       '首屏第一次状态失败先保持"连接中"并在 400ms 后自试；已有状态时保留上次值');
    ok(/const BADGE_KEY = 'soc_badge_v1'/.test(html)
       && /function paintCachedBadge\(\)/.test(html) && /paintCachedBadge\(\);/.test(html)
       && /saveBadge\(\);/.test(html),
       '徽标缓存上次状态：首屏先淡着画，新状态到了再覆盖（不再每次先黑着连接中）');
    ok(/\.status\.stale\{opacity:/.test(html),
       '缓存态有单独的淡化样式，不和故障态混');
  }
  // 应用选择器的数据源
  ok(/apps_list\)\s+apps_list "\$\{2:-\}" ;;/.test(apiSrc)
     && /mode_apps\)\s+mode_apps "\$\{2:-\}" ;;/.test(apiSrc),
     'apps_list / mode_apps 都接进了分发，并且接受 all');
  {
    const body = (apiSrc.match(/apps_list\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    const code = body.split('\n').filter(l => !l.trim().startsWith('#')).join('\n');
    ok(!!body, 'apps_list 函数体抠得到', body ? '' : '收尾不是顶格 }');
    ok(/pm list packages -3/.test(code) && /pm list packages 2>\/dev\/null/.test(code)
       && /"all"/.test(body),
       'apps_list 默认只列第三方，all 才连系统包一起列');
    // 判"有没有 die"必须先剥注释：函数里的注释正好写着"这里不 die"，
    // 不剥就把自己解释这句话的注释当成调用点，报一个反方向的红。
    ok(!/\bdie\b/.test(code) && /return 0/.test(code),
       'apps_list 读不到时返回空而不是 die（页面显示"没读到"比弹错误码好读）',
       (code.match(/\bdie[^\n]*/) || [''])[0]);
  }
  {
    /* 分应用档位要能选系统应用（相机、设置…），默认只并上"有 LAUNCHER 活动"的；
       查询被 ROM 裁掉时回退到全部系统包，勾选「显示系统应用」也列全部。 */
    const body = (apiSrc.match(/mode_apps\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    const code = body.split('\n').filter(l => !l.trim().startsWith('#')).join('\n');
    ok(/pm list packages -3/.test(code) &&
       /query-activities[\s\S]{0,100}LAUNCHER/.test(code) &&
       /pm list packages -s/.test(code) && /"all"/.test(body) &&
       /sort -u/.test(code) && !/\bdie\b/.test(code),
       'mode_apps = 第三方 ∪ 桌面系统应用；cmd 不可用回退全部系统包，all 列全部');
  }
  ok(/\[ "\$1" -ge 1 \] && \[ "\$1" -le 100 \]/.test(apiSrc),
     '数字档区间 1~100（和 conf 里 CPU_PCT_* 同界）');
  ok(/_v=\$\(printf '%s' "\$_v" \| tr 'A-Z' 'a-z'\)/.test(apiSrc),
     'FULL 归一成 small，免得同一包写出 full/FULL 两条互相覆盖');

  /* 页面入口必须是"从已安装列表里挑"。
   * 这里不能做"把当前前台设为满血"：这一页是在管理器里打开的，
   * 那一刻守护抓到的前台永远是管理器，那个按钮唯一能设的对象就是管理器。
   * 所以现在钉的是：有应用列表入口、并且没有任何地方拿 s.fg 去写 modes_set。 */
  ok(/api\('apps_list'\)/.test(ui) && /api\('mode_apps'\)/.test(ui) &&
     /function renderPicker/.test(ui),
     '分应用档位走"已安装应用选择器"（含系统应用那份清单）');
  ok(/function setAppsAll/.test(ui) && /id="pk-all"/.test(ui) && /id="ds-all"/.test(ui)
     && /api\('apps_list', 'all'\)/.test(ui) && /api\('mode_apps', 'all'\)/.test(ui),
     '选择器有「显示系统应用」开关：勾选后拉全部系统包（含服务/组件）');
  ok(/builtin_labels\) builtin_labels ;;/.test(apiSrc)
     && /builtin_labels\(\) \{[\s\S]{0,400}?builtin_labels\.txt/.test(apiSrc)
     && fs.existsSync(MOD0 + '/scripts/builtin_labels.txt'),
     '系统应用名兜底有后端命令和清单文件');
  ok(/const FALLBACK_LABELS = \{\}/.test(ui) && /async function loadBuiltinLabels/.test(ui)
     && /typeof FALLBACK_LABELS === 'undefined'/.test(ui),
     '前端名字兜底：APK label 为空时用 builtin_labels，搜索/显示都认');
  ok(/let PK_LIMIT = 80/.test(ui) && /DS\.limit = 80/.test(ui)
     && /PK_LIMIT \+= 80/.test(ui) && /DS\.limit \+= 80/.test(ui)
     && /显示更多/.test(ui),
     '选择器分页：一次 80 行，点「显示更多」再追加（不再一次画 400 行）');
  ok(/function markScroll/.test(ui) && /scrollStamp/.test(ui)
     && /Date\.now\(\) - scrollStamp < 200/.test(ui),
     '滑动时暂停图标批量取，停手后再发 root 调用');
  ok(/APPS_MODE === null/.test(ui) && /APPS === null/.test(ui) &&
     /renderPicker[\s\S]{0,900}?APPS_MODE\.filter/.test(ui) &&
     /renderDsPicker[\s\S]{0,900}?APPS\.filter/.test(ui),
     '两份清单分开：档位用 APPS_MODE（含系统），深睡豁免仍用 APPS（第三方）');
  /* 系统应用的名字常是英文（Camera），用户搜中文要能命中。 */
  ok(/const SEARCH_ALIAS = \{[\s\S]{0,600}'相机': 'camera'/.test(ui) &&
     /function searchHit\(p, terms\)/.test(ui) &&
     /searchHit\(p, terms\)/.test(ui),
     '中文搜索别名（相机 → camera），且只影响搜索不改显示名');
  ok(!/modes_set[^;\n]*s\.fg/.test(ui) && !/setFgMode/.test(ui),
     '不再有"把当前前台设为该档"那种入口（前台在管理器里永远是管理器）',
     (ui.match(/[^\n]*setFgMode[^\n]*/) || [''])[0].trim());
  // 用户明确要求把首页那一格删掉（"前台 / 档位 不用显示"）：
  // 状态徽标已经说了处于哪一种档，具体是哪个应用在应用页那一列里看得更全，
  // 而那一格只会把一长串包名折成两行塞在小格子里。
  // 守护那边照常发布 fg= / app_mode=（action.sh 的 status 在读），所以钉的是
  // "前端不渲染它"，不是"协议里没这两个键"。
  ok(!/m-mode/.test(ui), '首页不再有"前台 / 档位"那一格');
  ok(/why === 'app'\s*\?/.test((ui.match(/why === 'boot'[\s\S]{0,400}?;/) || [''])[0]),
     '应用满血仍然从 reason=app 分出显示（删格子的同时不能把状态也删没了）');
  ok(/api\('modes_set', pkg, val\)/.test(ui) && /api\('modes_del', pkg\)/.test(ui),
     '页面按钮真的打到 modes_set / modes_del');
  ok(/setMode\(p, v\)/.test(ui), '选择器里的 ＋/改 也走同一个 setMode');
  // 用户手写的值（比如 75）必须能在下拉里出现，否则一点就被改成预设值
  ok(/MODE_OPTS\.some\(o => o\[0\] === cv\)/.test(ui) &&
     /GPU_MODE_OPTS\.some\(o => o\[0\] === gv\)/.test(ui),
     'CPU/GPU 当前值不在预设里时会临时插进下拉（不会把 75% 悄悄改成 90%）');
  ok(/SEARCH_LABELS_ARMED/.test(ui) && /SEARCH_LABELS_DS_ARMED/.test(ui)
     && /ensureLabels\(APPS_MODE\)/.test(ui) && /ensureLabels\(APPS\)/.test(ui),
     '一输入搜索就后台补全全部应用名（分页只画 80 行，搜索仍匹配全量）');
  ok(/shown\.slice\(0, PK_LIMIT\)/.test(ui) && /shown\.slice\(0, DS\.limit\)/.test(ui)
     && /APPS_MODE\.filter\(p => !MODED\[p\]/.test(ui),
     '列表分页 80 行，且已设过档位的不再列进候选栏');
  ok(/api\('games_add', b\.dataset\.pkg\)/.test(ui),
     '候选栏的 ＋ 打到 games_add（点一下才进名单）');

  // CPU:GPU 双值：两段各判各的，空段表示那一边跟随全局
  ok(/char \*colon = strchr\(val, ':'\);/.test(csrc) &&
     /strspn\(cp, "0123456789"\)/.test(csrc) &&
     /strspn\(gp, "0123456789"\)/.test(csrc),
     '守护把 70:40 拆成两段分别校验');
  ok(/if \(!cp\[0\] && !gp\[0\]\) continue;/.test(csrc), '两段都空的行整条丢掉');
  ok(/if \(colon\) \{[\s\S]{0,600}?\} else if \(!strcmp\(val, "full"\)/.test(csrc),
     '冒号判断在先：full:40 不会被当成满血');
  ok(/_mc=\$\{1%%:\*\}/.test(apiSrc) && /_mg=\$\{1#\*:\}/.test(apiSrc),
     'api.sh 的 is_mode_val 拆两段各自校验（70: / :40 也收）');
  ok(/带 GPU 设置，学习不改/.test(apiSrc) && /index\(v, ":"\)/.test(apiSrc),
     'modes_learn 遇到带冒号的条目直接 SKIP（只懂 CPU，重写会吞掉 GPU 段）');
  ok(/skipd\[p\] = 1/.test(apiSrc) && /p in skipd/.test(apiSrc),
     'advice_app 不给带 GPU 段的条目出建议（按钮点下去只会 SKIP）');
  ok(/if \(g_app_gpu > 0\) snprintf\(gmode/.test(csrc) &&
     /app_mode=%s\\napp_gpu=%s/.test(csrc),
     '状态里 app_gpu 是独立字段，空 = 跟随全局（前端不用拆 70:40）');
  ok(/function parseModeVal\(/.test(ui) && /function joinModeVal\(/.test(ui) &&
     /function modeGpuSelect\(/.test(ui) && /function modeText\(/.test(ui),
     '页面能拆/拼 CPU:GPU，档位行和选择器都有 GPU 控件');
  {
    const body = (ui.match(/function pkVal\(\)\{[\s\S]*?\n\}/) || [''])[0];
    ok(/joinModeVal\(cv, gv\)/.test(body) &&
       /\(m === 'full' \|\| m === 'keep'\) return m;/.test(body),
         '选择器把 CPU/GPU 拼成一个值；full/keep 不参与拼接');
  }
}

/* ── ⑥ 发行商词典：只做候选，绝不做前台匹配 ────────────────────
 * 这条边界一旦糊掉，后果是"微信被当成游戏 → 前台一切到微信就解除限频"，
 * 也就是这个模块最核心的承诺被破坏。所以这里钉的是"守护根本读不到它"，
 * 而不是"守护读的时候有过滤"。 */
console.log('\n== 发行商词典（只作候选）==');
{
  const vend = fs.readFileSync(path.join(MOD0, 'vendors.txt'), 'utf8');
  ok(!/vendors/.test(csrc),
     '守护源码里不出现 vendors —— 前台匹配只认 games.txt 的整串包名',
     (csrc.match(/.{0,40}vendors.{0,40}/) || [''])[0]);
  ok(!/#define\s+VENDOR/.test(csrc), '守护没有词典路径宏');
  ok(/VENDORS="\$DATA\/vendors\.txt"/.test(apiSrc), 'api.sh 从数据目录读词典');
  ok(/games_suggest\) games_suggest ;;/.test(apiSrc), 'games_suggest 已接进分发');

  // games_suggest 绝不能写名单：写了一键确认就成了"自动加游戏"
  {
    const body = (apiSrc.match(/games_suggest\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(!!body, 'games_suggest 函数体抠得到', body ? '' : '收尾不是顶格 }');
    ok(!/>>?\s*"\$LIST"|write_atomic "\$LIST"/.test(body),
       'games_suggest 不写 games.txt（加进名单必须是用户的动作）',
       (body.match(/[^\n]*(\$LIST)[^\n]*/g) || []).join(' ;; '));
    ok(/pm list packages -3/.test(body),
       '只扫第三方包：系统包里的 game 段一大堆，全列出来就是噪音');
    ok(/shown > 80/.test(body), '候选有条数上限');
    // 段相等判据（带点的才按前缀）：子串包含会把 gameshop 也算成 game
    ok(/if \(index\(w, "\."\) > 0\) return \(substr\(p, 1, length\(w\)\) == w\)/.test(body) &&
       /for \(i = 1; i <= n; i\+\+\) if \(a\[i\] == w\) return 1/.test(body),
       '命中判据是"整段相等"，带点词才按前缀');
  }

  // 词典本身：命中表必须都是"游戏专用段"，通用公司名不能当命中项
  const inc = (vend.split('\n')
    .map(l => l.trim())
    .filter(l => l && !l.startsWith('#'))
    .map(l => (l[0] === '+' || l[0] === '-') ? { t: l[0], w: l.slice(1) } : { t: '+', w: l }))
    .filter(x => x.t === '+' && x.w);
  const words = inc.map(x => x.w.toLowerCase());
  ok(words.length >= 12, '命中项不至于太少（太少这栏就形同虚设）', String(words.length));
  for (const banned of ['tencent', 'netease', 'bilibili', 'alibaba', 'huawei', 'xiaomi',
                        'mi', '360', 'bytedance']) {
    ok(!words.includes(banned),
       '通用公司名 "' + banned + '" 没被当命中项（会顺带拉进社交/音乐/工具）');
  }
  // 排除表要比命中表长：同一家既有游戏又有非游戏，靠的是排除
  const excN = (vend.split('\n').filter(l => /^\s*-\S/.test(l)) || []).length;
  ok(excN >= 15, '排除表够长（命中之后还要过一遍排除）', '排除 ' + excN + ' 条');
  for (const need of ['cloudmusic', 'bbs', 'community', 'gamecenter', 'launcher', 'emulator']) {
    ok(vend.includes('-' + need), '排除表里有 ' + need + '（同公司的非游戏那一侧）');
  }
  ok(vend.includes('不参与前台匹配'), '词典文件自己就写明"不参与前台匹配"');

  // 页面：候选栏必须显式说明"点 ＋ 才生效"，且不会自己写名单
  ok(/games_suggest/.test(ui) && /不会自动生效/.test(ui),
     '候选栏写明"不会自动生效，点 ＋ 才加进名单"');
  ok(!/games_add[\s\S]{0,200}forEach\(b=>\{\s*api\('games_add'\)/.test(ui),
     '页面没有自动批量加候选（只有点按钮才加）');
}

/* ── 前端接线完整性 ─────────────────────────────────────────────
 * 这类错不报异常、不白屏，只是"点了没反应"：
 *   onclick 指向一个不存在的函数；
 *   JS 里 getElementById 取一个 markup 没有的 id（改了 DOM 忘了改 JS，
 *   或反过来）—— 取到 null 之后 .textContent 抛错，被外层 catch 咽掉，
 *   表现是那一块永远停在初始值。
 * 删掉一行展示要顺手查有没有留下 fg-name/fg-set 这类悬空引用。 */
console.log('\n== 前端接线 ==');
{
  const js = (ui.match(/<script>([\s\S]*?)<\/script>/) || ['', ''])[1];
  const ids = new Set((ui.match(/\sid="[\w-]+"/g) || []).map(s => s.slice(5, -1)));
  const handlerNames = (attr) =>
    [...new Set([...ui.matchAll(new RegExp(attr + '="([A-Za-z_$][\\w$]*)\\(', 'g'))].map(m => m[1]))];
  const defined = (f) =>
    new RegExp('function\\s+' + f + '\\s*\\(').test(js) ||
    new RegExp('(const|let|var)\\s+' + f + '\\s*=').test(js);
  for (const attr of ['onclick', 'onchange', 'oninput']) {
    const fs2 = handlerNames(attr);
    const bad = fs2.filter(f => !defined(f));
    ok(fs2.length > 0 && bad.length === 0,
       attr + ' 指向的函数全部已定义（' + fs2.length + ' 个）', bad.join(','));
  }
  const used = [...new Set([...js.matchAll(/getElementById\('([\w-]+)'\)/g)].map(m => m[1]))];
  const missing = used.filter(i => !ids.has(i));
  ok(missing.length === 0,
     'getElementById 取的 id 在 markup 里都存在（' + used.length + ' 个）',
     missing.join(','));
  // 反向：markup 里定义了 id、JS 从不取，且也不是 CSS 钩子 —— 只报数量，不当失败
  const touched = new Set([...js.matchAll(/getElementById\('([\w-]+)'\)/g)].map(m => m[1]));
  const orphan = [...ids].filter(i => !touched.has(i));
  console.log('  ..   ' + orphan.length + ' 个 id 只被 CSS/内联事件用：' + orphan.slice(0, 8).join(','));
  // 分应用档位这一层新写的函数，逐个点名要在（少一个是"按钮按了没反应"）
  for (const f of ['loadModes', 'setMode', 'delMode', 'togglePicker', 'loadApps',
                   'renderPicker', 'pkVal', 'pkModeChange', 'modeSelect',
                   'loadOvr', 'exportPkg', 'ovrNote']) {
    ok(defined(f), '前端函数 ' + f + ' 已定义');
  }
  // 适配表是作者随版本装进来的，页面上不该有能改它的入口
  ok(!/\bimportPkg\b|\bclearOvr\b/.test(html), '前端没有导入/清除覆盖的函数（用户改不到节点表）');
  // 整块必须解析得动：页面只有一个 <script>，一处语法错就是整页白屏，
  // 而不是"坏掉那一块不显示"。
  let syntaxErr = '';
  try { new Function(js); } catch (e) { syntaxErr = e.message; }
  ok(!syntaxErr, '页面 JS 整块语法解析得过', syntaxErr);
}

/* ── 台架脚本里不许调用不存在的函数 ──────────────────────────────
 * 这一份的断言函数叫 chk，不是别的台架里的 ok：
 * 结果 sh 报 "ok: command not found"，但**计数一次都不加**，
 * 末尾照样打"全部通过 (25)"——整段用例静默蒸发。
 * 这类假绿比红更糟：它让"跑过了"这件事失去意义。
 * 所以逐个台架检查：行首的"裸命令"要么是 shell 内建/外部程序，
 * 要么必须在本文件里定义过。 */
console.log('\n== 台架脚本的断言函数都存在 ==');
{
  const SHELL_WORDS = new Set(['if','then','else','elif','fi','for','while','until','do','done',
    'case','esac','in','echo','printf','cd','exit','return','local','export','set','trap',
    'read','shift','eval','exec','source','.','test','true','false','break','continue',
    'mkdir','rm','cp','mv','cat','sed','awk','grep','tr','cut','head','tail','sort','uniq',
    'wc','ls','chmod','chown','stat','mktemp','date','sleep','kill','wait','env','nohup',
    'command','type','basename','dirname','find','touch','od','seq','timeout','getevent',
    'su','sh','bash','node','ps','expr','printf','jl','[',']','[[',']]','function']);
  for (const f of ['src/test_install_merge.sh', 'src/test_action_keys.sh',
                   'src/test_api.sh', 'src/test_watchdog.sh']) {
    const src = fs.readFileSync(path.join(MOD0, f), 'utf8');
    const defined = new Set([...src.matchAll(/^(?:function\s+)?([A-Za-z_]\w*)\s*\(\)\s*\{/gm)].map(m => m[1]));
    const called = new Set();
    for (const m of src.matchAll(/^\s{0,4}([A-Za-z_]\w*)\s+(?=["'$\[])/gm)) called.add(m[1]);
    const unknown = [...called].filter(c =>
      !SHELL_WORDS.has(c) && !defined.has(c) && !/^(sudo|adb|pm|su)$/.test(c));
    ok(unknown.length === 0,
       f + ' 里没有调用未定义的函数（定义了 ' + defined.size + ' 个）',
       unknown.join(','));
  }
}

/* ── 设备上跑的脚本不许用 BRE 的 \| ────────────────────────────
 * toybox 的 sed 在 BRE 里不支持 `\|` 交替：写成 s/^\(a\|b\)=/…/ 时它一行都不放行。
 * 症状是"点了没反应/回执少一段"，而不是报错 —— 因为 sed 正常退出、输出为空。
 * 开发机上是 GNU sed（认 \|），这类写法在 PC 上永远测不出来，只能静态拦。
 * 台架脚本（src/test_*.sh）不在这条禁令里：它们只在 PC 上跑。 */
console.log('\n== 设备端脚本不用 BRE 交替 ==');
{
  const DEV = ['action.sh', 'service.sh', 'customize.sh', 'uninstall.sh', 'post-fs-data.sh',
               'webroot/api.sh', 'tools/smoke_ondevice.sh', 'tools/sandbox_probe.sh'];
  for (const f of DEV) {
    const bad = fs.readFileSync(path.join(MODDIR, f), 'utf8').split(/\r?\n/)
      .map((l, i) => [i + 1, l])
      .filter(([, l]) => /\\\|/.test(l) && !/^\s*#/.test(l) && !/-E\b/.test(l));
    ok(bad.length === 0, f + ' 里没有 BRE 的 \\| 交替（要交替就 sed -E / grep -E）',
       bad.map(([n, l]) => n + ':' + l.trim().slice(0, 60)).join(' | '));
  }
  // 第 1 项那条过滤必须带 -E，否则用户点「查看运行状态」还是一屏空
  ok(/api status \| sed -nE 's\/\^\(state\|game_pkg/.test(fs.readFileSync(path.join(MODDIR, 'action.sh'), 'utf8')),
     'action.sh 第 1 项用 ERE 过滤状态键（真机上报的就是这条）');
}

/* ── 按芯片套预设 + 未检测到的档位不摆控件 ───────────────────── */
console.log('\n== 芯片预设 / 档位行渲染 ==');
{
  const cz = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
  ok(/_adapt_match\(\)\s*\{/.test(cz) && /_soc_defaults\(\)\s*\{/.test(cz),
     '装机脚本用 adapt 匹配 + 芯片默认两个函数，不再有 _soc_preset');
  ok(!/_soc_preset|_big=65; _lit=62|_big=60; _lit=65/.test(cz),
     '装机脚本里没有第二套写死的芯片值');
  // 单一来源：值只在 adapt/<soc>.conf 里
  {
    const s8750 = fs.readFileSync(MOD0 + '/adapt/sm8750.conf', 'utf8');
    const s8850 = fs.readFileSync(MOD0 + '/adapt/sm8850.conf', 'utf8');
    ok(/^#match soc=SM8750$/m.test(s8750) && /^default_cpu_pct_big=66$/m.test(s8750) &&
       /^default_cpu_pct_little=68$/m.test(s8750) && /^default_gpu_pct=50$/m.test(s8750) &&
       /^default_th_high=75$/m.test(s8750) && /^default_psi_gain=10$/m.test(s8750),
       '8E 默认值在 adapt/sm8750.conf（66/68、GPU50、TH75、PSI10）');
    ok(/^#match soc=SM8850$/m.test(s8850) && /^default_cpu_pct_big=64$/m.test(s8850) &&
       /^default_cpu_pct_little=69$/m.test(s8850) && /^default_gpu_pct=50$/m.test(s8850),
       '8E5 默认值在 adapt/sm8850.conf（64/69、GPU50，Prime 比 Gold 低是特性）');
  }
  // 只对全新安装：_FRESH_CONF=1 才调 _soc_defaults；升级不碰
  ok(/if \[ "\$_FRESH_CONF" = 1 \]; then\s*\n\s*_soc_defaults/.test(cz) &&
     !/_soc_defaults[^\n]*keep/.test(cz),
     '芯片默认只在全新安装套用，升级不套（没有"值等于默认"的启发式）');
  {
    const body = (cz.match(/_soc_defaults\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(/default_cpu_pct_big\)\s+_key=CPU_PCT_BIG/.test(body) &&
       /default_cpu_pct_little\)\s+_key=CPU_PCT_LITTLE/.test(body) &&
       /default_th_high\)\s+_key=TH_HIGH/.test(body) &&
       /default_psi_gain\)\s+_key=PSI_GAIN/.test(body),
       '只认白名单 default_* 键（打错的行不会写进配置）');
    ok(/case "\$_v" in ''\|\*\[!0-9\]\*\) continue/.test(body) &&
       /\[ "\$_v" -ge "\$_lo" \]/.test(body),
       '套用前校验数字与范围');
  }
  // 三方默认值一致性不能被预设悄悄破坏：包里的 conf 仍是通用默认
  ok(/^CPU_PCT_BIG=65$/m.test(conf) && /^CPU_PCT_LITTLE=65$/m.test(conf),
     '包里那份 conf 保持通用默认（芯片默认只在装机时改安装后的副本）');

  /* 未检测到的档位：不摆滑杆、不显示百分比，只留名字 +（未检测到）。 */
  ok(/sl\.style\.display = absent \? 'none' : ''/.test(ui),
     '未检测到的档位把滑杆摘掉');
  ok(/pc\.style\.display = absent \? 'none' : ''/.test(ui),
     '未检测到的档位不显示百分比');
  ok(/inp\.disabled = absent/.test(ui) && /PCT_KEYS\.filter\(o=>!TIER_ABSENT\[o\.k\]\)/.test(ui),
     '未检测到的档位既不让人拖，也不写进 conf');
  ok(/el\.textContent = '（未检测到）'/.test(ui), '文案就是"（未检测到）"');
  // "检测失败"和"这台机没这一档"必须相反：前者保留控件
  {
    const fn = (ui.match(/function paintTierCores[\s\S]*?\n\}/) || [''])[0];
    ok(fn.indexOf('检测失败') < fn.indexOf('absent = true'),
       '检测失败那一支提前返回，不会被判成"这台机没这一档"（控件不该被藏）');
  }
}


/* ══════════════════════════════════════════════════════════════
 * 应用名 / 图标声明 / 页面结构 / action.sh 自证
 *
 * 这批断言对应四条用户反馈：
 *   "把应用名写在第一行，包名写在第二行"（白名单、候选、分应用档位都要）
 *   "获取应用图标有些也没生效，比如 com.tencent.tmgp.osgame"
 *   "分应用档位放到应用页 / 导航栏改成应用 / 首页那格删掉"
 *   "日常调速器默认为不干预，现在是默认不选择"
 * 外加一条自己加的：action.sh 那句"大概 15 秒就退出"必须留下事后查得到的证据。
 *
 * 这里钉的是**接线**（谁调谁、键对不对、位置在哪）；
 * conf_read 剥引号、labels 缓存复用这类"跑一遍才知道"的行为在
 * src/test_api.sh 里功能验，别在这儿用正则替代执行。
 * ══════════════════════════════════════════════════════════════ */
{
  const P = require('path');
  const readIf = rel => {
    const p = P.join(MODDIR, rel);
    return fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : null;
  };
  // 注释会干扰"代码里有没有这句"的判断，先剥掉再比
  const uiCode = html.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^[ \t]*\/\/.*$/gm, '');

  /* ── 日常调速器：默认必须显示成"不干预" ───────────────────── */
  // 老配置里那对字面双引号是三处不一致的根源：守护认不出引号（等价于空 = 不干预），
  // WebUI 的下拉框也匹配不到 option，于是显示成"谁都没选"，用户以为默认值丢了。
  const govM = conf.match(/^CPU_GOVERNOR=([^\n]*)$/m);
  ok(govM !== null, '打包的 socfreq.conf 里有 CPU_GOVERNOR 这一行');
  ok(govM && govM[1] === '', '出厂值是裸的空（不再写成 ""）', '实到 [' + (govM && govM[1]) + ']');
  {
    const cr = (apiSrc.match(/conf_read\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok((cr.match(/\bsed\b/g) || []).length >= 3,
       'conf_read 里多了一道剥引号的 sed（老装机副本里仍是 CPU_GOVERNOR=""，前端那边要一样空）',
       'sed 次数 ' + (cr.match(/\bsed\b/g) || []).length);
    ok(apiSrc.slice(Math.max(0, apiSrc.indexOf('conf_read()') - 900),
                    apiSrc.indexOf('conf_read()')).includes('引号'),
       'conf_read 的剥引号这一步有注释说明为什么存在');
  }
  {
    const gv = (uiCode.match(/const gv = [\s\S]*?pv\('governor'\);/) || [''])[0];
    ok(/GOVERNOR_OPTS\.some/.test(gv) && /: ''/.test(gv),
       '下拉框拿到认不出的值时回落"不干预"（和守护的口径一致）', gv.slice(0, 60));
  }
  {
    /* 配置页一进来要读完所有键。逐键起 sed 是两百多个进程、实测 0.72 秒，
       是"点配置页要等一下"的主因；单趟 awk 后同一台机约 0.03 秒。 */
    const cg = (apiSrc.match(/cfg_get\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(/awk/.test(cg) && !/\$\(conf_read/.test(cg),
       'cfg_get 单趟读完配置，不再逐键 fork', cg.slice(0, 60));
    ok(/in val\)\) next/.test(cg) && /val\[k\] = v/.test(cg),
       '同名键仍取第一行（和 conf_read 的 head -n 1 一致）');
    ok(/\[\[:space:\]\]\*#/.test(cg) && /\^"\[\^"\]\*"\$/.test(cg),
       '仍是"# 后当注释、只剥成对引号"那套规则');
  }

  /* ── 页面结构：应用页 / 分应用档位的位置 / 首页那一格 ─────── */
  ok(/data-p="game"[\s\S]{0,420}>应用</.test(html), '导航栏这一项叫"应用"（不再只说游戏）');
  ok(!/data-p="game"[\s\S]{0,420}>游戏</.test(html), '没有还挂着"游戏"的导航项');
  {
    const g = html.indexOf('id="p-game"'), c2 = html.indexOf('id="p-conf"');
    const rows = html.indexOf('id="mode-rows"'), gl = html.indexOf('id="glist"');
    ok(g > 0 && c2 > 0 && gl > 0, '三个锚点都找得到（页面结构被改了要同步这几条）');
    ok(rows > g && rows > gl,
       '分应用档位在应用页、且在"游戏白名单"下面（先后顺序就是这条断言的全部内容）',
       'p-game@' + g + ' glist@' + gl + ' mode-rows@' + rows);
    ok(rows > c2, '配置页里已经没有分应用档位那张卡');
  }
  /* ── 「功能」页 ───────────────────────────────────────────────
     实时偏移那张卡搬过去了。这条断言钉的是搬完之后三件容易漏的事：
     导航项、页面容器、以及切页时要有人给这些控件填值。 */
  {
    const nav = html.indexOf('data-p="func"'), pg = html.indexOf('id="p-func"');
    const chgCard = html.indexOf('插着电的时候'), confPg = html.indexOf('id="p-conf"');
    const gamePg = html.indexOf('id="p-game"');
    ok(nav > 0 && pg > 0 && chgCard > 0, '功能页的导航项、容器、第一张卡都在');
    ok(pg < chgCard && chgCard < gamePg, '三张功能卡确实在功能页里（不是留在配置页）',
       'p-func@' + pg + ' 卡@' + chgCard + ' p-game@' + gamePg);
    ok(chgCard > confPg, '配置页里已经没有这些功能卡');
    ok(/b\.dataset\.p === 'conf' \|\| b\.dataset\.p === 'func'\) loadConf\(\)/.test(html),
       '切到功能页也会 loadConf 填值（不填就是滑杆全停在 HTML 里写死的假数）');
    ok((html.match(/<button[^>]*onclick="saveAll\(\)"/g) || []).length === 2,
       '配置页和功能页各有一个「保存并生效」（共用同一个 saveAll）');
    const saveFn = (html.match(/let saveBusy[\s\S]*?async function saveAll\(\)\{[\s\S]*?\n\}/) || [''])[0];
    ok(/if\(saveBusy\) return/.test(saveFn) && /保存中…/.test(saveFn)
       && /b\.disabled = true/.test(saveFn),
       '保存期间锁住按钮并显示"保存中…"（否则用户以为没点上会反复按）');
    ok(/\['conf_set_many'\]\.concat/.test(saveFn) && /api\.apply\(null, args\)/.test(saveFn)
       && !/await api\('conf_set'/.test(saveFn),
       '一次保存只发一个 conf_set_many（以前三十多个键 = 三十多次 root IPC，要十几秒）');
    ok(/cmd_conf_set_many\(\) \{/.test(apiSrc) && /conf_set_many\) shift;/.test(apiSrc)
       && apiSrc.includes('conf_read "$_k")" = "$_v" ] && continue'),
       '后端批量入口存在，且值没变的键跳过（通常真正要写的只有一两个）');
    const funcBody = (html.match(/id="p-func"[\s\S]*?(?=<div class="pg" id="p-game")/) || [''])[0];
    const miss = ['psi_adapt', 'th_cap', 'chg_uncap', 'chg_pct', 'th_high', 'th_low',
                  'th_max', 'psi_gain'].filter(id => !funcBody.includes('id="' + id + '"'));
    ok(funcBody.length > 500 && miss.length === 0,
       '三条开关和它们的阈值都在功能页里', miss.join() || ('功能页 ' + funcBody.length + ' 字符'));
  }
  {
    /* 锚点要带上 tabbar 那句：分段按钮内部也有 b.onclick = ()=>{ ，
       只按后半截匹配会先抓到控件那块，报"没刷档位"的假红。 */
    const tab = (uiCode.match(/querySelectorAll\('\.tabbar button'\)[\s\S]*?\n  \};/) || [''])[0];
    ok(/'game'\)\{[^}]*loadModes\(\)/.test(tab),
       '切到应用页会刷分应用档位（卡片挪了位置，加载也得跟着挪）');
    ok(!/'conf'\)\{[^}]*loadModes/.test(tab),
       '配置页不再顺手刷档位（两处入口迟早只改一处）');
    /* 切页必须先画再发 root 调用：管理器的 exec 是同步的，
       紧跟着发会把这一帧一起卡住，看起来就是"点了没反应"。 */
    const cls = tab.indexOf("classList.add('on')");
    const raf = tab.indexOf('requestAnimationFrame');
    const firstLoad = tab.indexOf('loadConf()');
    ok(cls >= 0 && raf > cls && firstLoad > raf,
       '先切页面并让出一帧，再开始读数据（点击反馈不等 root 调用）');
  }

  /* ── 一行两句话：应用名在上、包名在下 ─────────────────────── */
  ok(/function makeNameEl/.test(html) && /function pkgRow/.test(html),
     '有两行式那套行构造器');
  ok(/\.pk \.nm \.t\{/.test(html) && /\.pk \.nm \.s\{/.test(html),
     'CSS 里有名字行和包名行两种样式');
  /* 档位行比名单多两个下拉；并排时 38 图标 + 2×88 下拉 + 删除把名字挤成“c.”。
     名字和包名占第一行，控制项换到第二行。 */
  ok(/\.pk-mode\{[^}]*flex-wrap:wrap/.test(html) &&
     /\.pk-mode \.md-sel\{[^}]*flex:1 1 110px/.test(html),
     '档位行的下拉换到第二行，不再挤掉应用名');
  {
    const lm = (html.match(/function loadModes\([\s\S]*?\n\}/) || [''])[0];
    ok(/row\.el\.classList\.add\('pk-mode'\)/.test(lm),
       'loadModes 渲染的档位行套了换行样式');
  }
  for (const fn of ['loadGames', 'loadSuggest', 'loadModes', 'renderPicker']) {
    const body = (html.match(new RegExp('function ' + fn + '\\([\\s\\S]*?\\n\\}')) || [''])[0];
    ok(body.length > 0, fn + ' 找得到（改名了要同步这条）');
    ok(/pkgRow\(/.test(body), fn + ' 用的是两行式的行（不是老的"图标 + 单个 code"）');
    ok(!/createElement\('code'\)/.test(body), fn + ' 不再自己拼 <code> 包名');
    ok(/ensureLabels\(/.test(body), fn + ' 渲染完会去点名要名字');
  }
  {
    const pr = (html.match(/function pkgRow\([\s\S]*?\n\}/) || [''])[0];
    ok(/el\.dataset\.pkg = p/.test(pr) && /applyLabel\(name\.t, name\.s, \(LABELS\[p\][\s\S]{0,140}FALLBACK_LABELS\[p\]\)/.test(pr),
       '行上带 data-pkg，名字已经有了就直接写（插入文档前就要写，见 test_pkgrow_ui）');
    ok(/IntersectionObserver/.test(pr) && /loadIcon\(p, ph\)/.test(pr),
       '图标仍按可视区懒取');
    /* 图标一次取一批 + 会话内缓存：逐行 api('icon') 在应用页是几十次
       root 往返，切页重画还会再取一遍。 */
    ok(/ICONS\[p\] !== undefined/.test(pr) && /applyIcon\(p, ph, ICONS\[p\], true\)/.test(pr),
       '已有图标直接换上，不再重复发 root 调用（detached 放行：行还没插入文档）');
    ok(/function flushIcons/.test(html) && /\['icons'\]\.concat\(batch\)/.test(html) &&
       /ICON_Q\[pkg\]/.test(html),
       '新图标攒成一批走 icons，不再一包一次');
    /* 未缓存的图要现解 APK：批量大了会撞 12 秒超时；调用失败也不能
       把整批记成"没有图"，否则这一整轮会话都不再重试。 */
    ok(/const batch = all\.slice\(0, 8\)/.test(html) && /ICON_TRY/.test(html) &&
       /if\(ok && got\[p\] !== undefined\)/.test(html) &&
       /setTimeout\(flushIcons, 0\)/.test(html),
       '图标小批量取；失败/缺行放回队列重试，不当成"这包没图"');
  }
  {
    const rp = (html.match(/function renderPicker\([\s\S]*?\n\}/) || [''])[0];
    ok(rp.length > 0, 'renderPicker 找得到');
    ok(/APPS_MODE\.filter\(p => !MODED\[p\]/.test(rp),
       '候选栏只列还没设档位的应用（已设的去上面改）');
    ok(!/MODED\[p\] \? '改'/.test(rp), '候选栏不再给已设应用留"改"按钮');
  }
  ok(!/id="m-mode"|getElementById\('m-mode'\)/.test(html),
     '首页"前台 / 档位"那一格和它的 JS 都没了');

  /* ── 应用名这一列的协议：前端点名 → api.sh 缓存 → 守护批量读 ── */
  ok(/async function ensureLabels\(pkgs\)/.test(html), '前端有一次点名要名字的入口');
  ok(/api\.apply\(null, \['labels'\]\.concat\(list\)\)/.test(uiCode),
     'labels 是把包名当参数传过去的（不是让后端全量扫）');
  ok(/catch\(_\)\{[\s\S]{0,200}delete LABELS\[p\]/.test(uiCode),
     'labels 失败时把占位撤掉，下次还能重试（不能把"这一轮没问到"记成"这包没名字"）');
  /* 应用页一次会点名三批（名单/候选/档位）。并发进后端时，
     固定文件名的临时文件会互相覆盖，表现为"有时候一部分名字出不来"。 */
  ok(/let labelBusy = false/.test(uiCode) && /LABEL_Q/.test(uiCode) &&
     /if \(!LABEL_Q\.length \|\| labelBusy\) return/.test(uiCode),
     'labels 串行化：一次一批，路上新点名的并入队');
  ok(/labels\.cur\.\$\$/.test(apiSrc) && /labels\.want\.\$\$/.test(apiSrc),
     'cmd_labels 的临时文件带 $$（并发调用不互相踩）');
  ok(/function searchHit\(p, terms\)/.test(uiCode) &&
     /p\.toLowerCase\(\)[\s\S]{0,140}FALLBACK_LABELS\[p\]/.test(uiCode),
     '搜索框同时按包名和已取到的应用名过滤（中文别名见 test_apps_search）');
  {
    const lm = (apiSrc.match(/label_map\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(/pm list packages -f/.test(lm) && !/pm list packages -3 -f/.test(lm),
       '包名→APK 路径取全部包（分应用档位也列系统应用，名字同样要读）');
    ok(lm.includes('=[^=]+' + '$') && lm.includes('match($0'),
       '按最后一个 = 切：路径里本来就带 =（~~XX==、-base==），按第一个切一定切错');
    ok(/return 0/.test(lm), 'label_map 空结果不算失败');
    const cl = (apiSrc.match(/cmd_labels\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(/LABEL_CACHE/.test(cl) && /mv -f "\$_cf" "\$LABEL_CACHE"/.test(cl),
       '名字有缓存，且整体原子替换（卸载掉的包不留残行）');
    ok(/"\$BIN" labels < "\$_need"/.test(cl),
       '没缓存的那批一次交给守护批量读（不是循环里逐包 fork）');
    ok(/"\$#" -gt 0/.test(cl) && /is_package/.test(cl),
       '带参数时按点名的包过滤，而且每个都过 is_package（不能把任意字符串当包名传下去）');
    ok(/capk\[p\] == a/.test(cl),
       '缓存比对用的是 APK 路径整串相等（装过新版本就自动重取，不需要 TTL）');
    ok(/labels\)     shift; cmd_labels "\$@" ;;/.test(apiSrc),
       '分发处把后面的包名都传给 cmd_labels');
  }
  ok(/games_suggest\|labels\|icon/.test(html),
     'labels 在"可以为空"的白名单里（一个名字都没读到是合法状态，不该弹执行失败）');

  /* ── 守护那三个新子命令：值对上才算接好 ─────────────────── */
  const info = readIf('src/appinfo.c');
  if (!info) {
    console.log('  SKIP 没有 src/appinfo.c —— 应用名这一整块一条都没验');
  } else {
    ok(/^int appinfo_label\(const char \*apk, char \*out, size_t osz\)/m.test(info),
       'appinfo_label 有定义');
    ok(/^int appinfo_iconpath\(const char \*apk, char \*out, size_t osz\)/m.test(info),
       'appinfo_iconpath 有定义');
    ok(/^int appinfo_labels_stdin\(void\)/m.test(info), 'appinfo_labels_stdin 有定义');
    for (const cmd of ['label', 'labels', 'iconpath']) {
      const re = new RegExp('if \\(!strcmp\\(cmd, "' + cmd + '"\\)\\)\\s*\\{([\\s\\S]*?)\\n  \\}');
      const blk = (csrc.match(re) || ['', ''])[1];
      ok(blk.length > 0, '守护有 ' + cmd + ' 子命令');
      const want = cmd === 'labels' ? 'appinfo_labels_stdin'
                 : cmd === 'label' ? 'appinfo_label' : 'appinfo_iconpath';
      ok(new RegExp(want + '\\(').test(blk), cmd + ' 子命令真的调了 ' + want + '（不是空壳）');
    }
    ok(/int appinfo_label\(const char \*apk, char \*out, size_t osz\);/.test(csrc) &&
       /int appinfo_labels_stdin\(void\);/.test(csrc),
       '守护声明了 appinfo 那三个入口（声明和定义分开写，签名必须逐字段对上）');
    // 名字只能从 APK 里读：不许有人再加一条"用 pm 猜"的路
    ok(!/pm dump|cmd package/.test(info), 'appinfo.c 里没有 pm/dumpsys 那类假想来源');
    // 属性名不能写死框架 ID —— 0x01010003 是 android:name 而不是 label，
    // 照某些资料写死就会稳定取到活动类名当应用名，而且一句错话都不报。
    ok(!/0x01010003/.test(info.replace(/\/\*[\s\S]*?\*\//g, '')
                                        .replace(/^[ \t]*\/\/.*$/gm, '')),
       '代码里没有写死的 android:* 属性 ID（只按属性本地名认）');
    ok(/read_attr\(ax, rec, "label"/.test(info), 'label 是按属性**本地名**认的');
    {
      const sc = (info.match(/static void scrub\(char \*s\)\s*\{[\s\S]*?\n\}/) || [''])[0];
      ok(sc.includes('0x09') && sc.includes('0x0a') && sc.includes('< 0x20'),
         '应用名出来前清掉制表符/换行/控制符（TSV 被拆行整列就废了）');
    }
    ok(/printf\("%s\\t%s\\t%s\\n", pkg, apk, name\)/.test(info),
       'labels 输出 pkg<TAB>apk<TAB>名字 三列（shell 侧不靠行号对齐拼名字）');
    // 值读法：FLAG_ORIG 不能跳过第一个 Res_value（实测真值在前）
    ok(!/eflags & 0x02\) v \+= 8/.test(info),
       '没有"FLAG_ORIG 就跳过一个 Res_value"那种写法（实测跳了读到的是占位空值）');
    // UTF-8 池按"字节数"切，不是字符数
    {
      const ps = (info.match(/static int pool_str\([\s\S]*?\n\}/) || [''])[0];
      ok(ps.includes('(p[o + l1] & 0x80)') && ps.includes('memcpy(out, p + o, bl)'),
         'UTF-8 串的切片长度取的是第二个变长整数（字节数）',
         (ps.match(/const size_t bl[\s\S]{0,80}/) || [''])[0].replace(/\s+/g, ' '));
    }
  }

  /* ── 图标：声明过的那张优先，不再靠猜 ───────────────────── */
  ok(/int icon_from_apk\(const char \*apk, const char \*declared\)/.test(iconSrc),
     '取图函数收得到"声明的那张"');
  ok(/declared && !strcmp\(nm, declared\)\) sc = 100/.test(iconSrc),
     '声明过的那张直接记满分：它不需要"像不像图标"这轮判断');
  {
    const ci = (apiSrc.match(/cmd_icon\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
    ok(/iconpath "\$_apk"/.test(ci), '取图先问一次声明的是哪张');
    ok(!/res\/\*\)/.test(ci),
       '声明路径不按 res/ 前缀过滤（AGP 会把 res/ 缩短成 r/）');
    ok(/\|\/\*\|/.test(ci) && /\*\.\.\*/.test(ci),
       '绝对路径和 .. 仍然被拒（空行/报错串也当没有声明）');
    ok(/"\$BIN" icon "\$_apk" "\$_want"/.test(ci),
       '有声明时把声明名真的传给 icon 子命令');
    ok(/"\$BIN" icon "\$_apk" 2>/.test(ci),
       '没声明时仍然走两轮打分（兜底不能被新主路挤掉）');
  }

  /* ── action.sh 的自证日志（"15 秒就退出"那条反馈的抓手） ─── */
  ok(/atrace\(\)\s*\{/.test(act), '有 atrace 这个入口');
  ok(act.includes('>> "$DATA/socfreqd.log"'),
     '自证写进守护那份日志：WebUI 的日志卡片和菜单第 3 项都在读它，用户不用 adb');
  {
    const logInApi = (apiSrc.match(/^LOG="([^"]*)"/m) || [, ''])[1];
    const logInC = (csrc.match(/#define\s+LOG_FILE\s+SF_DATADIR "([^"]*)"/) || [, ''])[1];
    ok(/socfreqd\.log$/.test(logInApi) && logInApi.endsWith(logInC),
       'action.sh / api.sh / 守护三处指的是同一份日志文件',
       'api=' + logInApi + ' c=' + logInC);
  }
  ok(/trap '[^\n]*exit 129' HUP/.test(act) && /trap '[^\n]*exit 143' TERM/.test(act) &&
     /trap 'atrace[^\n]*EXIT/.test(act),
     'HUP/TERM/EXIT 三个 trap 都在（被管理器杀掉时屏幕上那句"超时退出"是假象）');
  // SIGPIPE 必须**不**退出：管理器的输出区到一定量就不再读管道，之后任何一次
  // ui_print 都拿到 SIGPIPE，默认动作是整个脚本死掉。真机日志里
  // "结束 rc=141 因为是 PIPE（用时 11s）"就是这么来的——按键一直好使。
  ok(/trap 'atrace[^\n]*输出管道[^\n]*' PIPE/.test(act) && !/exit 141' PIPE/.test(act),
     '管道断了只记一笔、继续等按键（死在写输出上才是 bug）');
  ok(/atrace "超时 lim=\$\{_lim\}s 读数=\$\{_n\} 次"/.test(act),
     '超时会带读数次数：次数远小于用时 = getevent 立刻返回在空转，不是真等满');
  ok(/atrace "空转：\$\{_el\} 秒里读了 \$\{_n\} 次/.test(act),
     '读不到输入设备时把"用时 vs 读数次数"写进日志（不再依赖管道里取不到的退出码）');
  ok(/timeout 0\.1 getevent -lqc 1/.test(act) && /sleep 0\.05/.test(act),
     '读数节奏是窗口 0.1 秒 + 隔 0.05 秒再看，不是一秒一次的阻塞读');
  ok(/atrace "菜单已画出/.test(act),
     '菜单画出来这件事本身也记一行（日志停在这行之后 = 被 SIGKILL，谁都没写下一句）');
}


/* ---- 智能学习：守护采样 ↔ api.sh 判据 ↔ 页面渲染 ---- */
// 这条链上三份文件只靠"列数、字段名、路径"对接，任何一处改名都不会有人报错，
// 页面只是安静地空白。所以这里把三端拼起来比一次。
{
  const learnSrc = fs.readFileSync(path.join(MODDIR, 'src/learn.c'), "utf8");  const bs = String.fromCharCode(92);
  const uiAdv = (html.match(/\/\* ── 智能建议 ── \*\/[\s\S]*?\/\* ── 游戏名单 ── \*\//) || [''])[0];

  const bands = Number((learnSrc.match(/#define\s+LEARN_BANDS\s+(\d+)/) || [, '0'])[1]);
  ok(bands === 24 && apiSrc.includes('for (i = 0; i < 24; i++) bd[t, i]'),
     '采样桶数两端一致（守护写 24 个桶，判据按 24 个桶读）', 'learn.c=' + bands);
  // 一行的列数 = 目录+档+采样+顶帽+被压 + 24 个桶 + 天位图 + 帽%
  ok(apiSrc.includes('NF >= 31') && bands === 24,
     '半截行的判据写的是列数（31），不是"这行有没有内容"');
  ok(learnSrc.includes('held_ms') && apiSrc.includes('$30 + 0') &&
     apiSrc.includes('$31 + 0'),
     '天位图在第 30 列、采样帽在第 31 列：守护按这个顺序写、判据按这两列读');
  ok(learnSrc.includes('scaling_max_freq') && apiSrc.includes('held >= 50'),
     '上限被厂商压在我们的帽之下时不给升降（那份分布量的是它的帽，不是我们的）');

  const learnFileC = (csrc.match(/#define\s+LEARN_FILE\s+SF_DATADIR "([^"]*)"/) || [, ''])[1];
  const learnFileSh = (apiSrc.match(/^LEARN="([^"]*)"/m) || [, ''])[1];
  ok(/learn\.tsv$/.test(learnFileC) && learnFileSh.includes('learn.tsv'),
     '守护和 api 指的是同一份样本文件', 'c=' + learnFileC + ' sh=' + learnFileSh);

  ok(/cfg\.learn = 1/.test(csrc) && /_lv=0 ;; \*\) _lv=1/.test(apiSrc),
     '样本文件里没有 LEARN 时，两端都按"开"处理（升级装机的 conf 就是没有这一行）');
  ok(/lm\.checked = c\.LEARN !== '0'/.test(html),
     '页面的开关同样按"缺省即开"显示');
  ok(/^LEARN=1$/m.test(conf), '出厂配置里明确写了 LEARN=1');
  ok(/\bKEYS='[^']*LEARN/.test(apiSrc) && /LEARN\)\s+is_switch/.test(apiSrc),
     'LEARN 在可写键清单里，且取值只认 0/1');

  // 记录里有的字段，页面读的必须都在；反过来页面不能凭空发明字段。
  // 两层建议（全局 advice / 按应用 advice_app）共用同一个渲染入口，
  // 所以"打得出来的字段"是两条 printf 的并集。
  const recLine = (apiSrc.match(/printf "tier=[^"]*"/) || [''])[0];
  const recAppLine = (apiSrc.match(/printf "pkg=[^"]*"/) || [''])[0];
  const emitted = (recLine + ' ' + recAppLine)
    .split(bs + "n").map(x => ((x.match(/([a-z0-9_]+)=/) || [])[1] || ""))
    .filter(Boolean);
  const read = (uiAdv.match(/\br\.[a-z0-9_]+/g) || []).map(s => s.slice(2));
  const uniqRead = Array.from(new Set(read)).filter(k => k !== 'length');
  ok(emitted.length >= 8 && uniqRead.every(k => emitted.includes(k)),
     '页面读的字段全在 api 打出的字段里', '出=' + emitted.join(',') + ' 读=' + uniqRead.join(','));
  ok(apiSrc.includes('printf "---REC---\\n"') && uiAdv.includes("split('---REC---')"),
     '记录分隔标记两端同名（---REC---）');
  ok(/api\('advice'\)/.test(uiAdv) && /api advice \|/.test(act),
     '页面和音量键菜单读的是同一个 advice 子命令');

  // 阈值只许有一份：页面里不该出现判据数字
  ok(!/>= ?30|pin >= |p90/.test(uiAdv),
     '页面不自己判升降（判据只在 api.sh 一份）');
  ok(/applyAdvice[\s\S]*?api\('conf_set', r\.key, r\.to\)/.test(uiAdv),
     '应用建议走的是既有 conf_set，不自己写 conf');

  ok(/learn_tick\(!strcmp\(g_state, "normal"\)/.test(csrc),
     '只在常态封顶生效时才累计样本（熄屏/满血/游戏时段不算）');
  ok(/learn_sample_state\(st, &d, dt, hw\[i\], eff\)/.test(learnSrc)
     && /if \(eligible\) \{\s*\n\s*if \(got && !cap_changed\) learn_put\(&st->a, &d\);/.test(learnSrc),
     '分布本身也只采封顶生效的那段（增量先攒进 delta，再由 eligible 决定进不进桶）');
  ok(/learn_flush\(\);/.test(csrc.split('static void cleanup')[1] || ''),
     '守护退出前落一次盘');
  ok(/learn_check_reset\(\);[\s\S]{0,160}?if \(g_n && g_path\[0\]/.test(learnSrc),
     '落盘前先认"文件已删"：清零后不能被退出时那一次写盘复活');
  // 真机上量到的一条：文件"从来没有过"和"写过又被删"必须分得开，
  // 混在一起的话每一拍都把刚攒的累计连同计时基准抹掉，样本一次也落不了盘。
  ok(/if \(!access\(g_path, F_OK\)\) g_seen_file = 1;/.test(learnSrc) &&
     /else if \(g_seen_file && g_n\)/.test(learnSrc),
     '首装那台机"还没有样本文件"不当成清零');
  ok(apiSrc.includes('rm -f "$LEARN"') && uiAdv.includes("api('learn_reset')"),
     '页面上的清零删的就是那份样本');
}


/* ---- 声明图标与体积预算 ---- */
{
  // 真机取包量到的：manifest 声明的桌面图标是 88KB 的 1024px webp，
  // 而预算那一趟上限 24KB —— 不豁免的话被跳过后选中的是一张 275 字节的错图。
  ok(iconSrc.includes('ents[j].csize > ICON_BUDGET && ents[j].score < 100'),
     '体积预算不拦声明过的那张图标（满分候选豁免）');
  ok(/sc = 100/.test(iconSrc),
     '声明匹配给的是那个被豁免的满分值（两处必须是同一个数）');
}


/* ---- 图标缓存键 ---- */
{
  // 缓存键只带 mtime 的话，改了选图规则之后老缓存里那张错图会一直留着，
  // 用户看到的是"修了但还是错的图标"。键里必须有算法版本。
  ok(apiSrc.includes('_out=$ICON_DIR/$_pkg.$_mt.$ICON_ALGO.b64') &&
     new RegExp("^ICON_ALGO=[0-9]+$", 'm').test(apiSrc),
     '图标缓存键里带取图算法版本（改规则时老缓存自然失效）');
}


/* ---- GPU 进智能学习：三端同时认第 5 档 ---- */
{
  const learnSrc = fs.readFileSync(path.join(MODDIR, 'src/learn.c'), 'utf8');
  // 少任何一端的表现都是静默：要么 GPU 那一行根本不出现，
  // 要么点了「应用」什么都没改。所以三端一起钉。
  ok(learnSrc.includes('gpu_clock_stats') &&
     learnSrc.includes('gpu_available_frequencies'),
     '采样侧读 KGSL 的每档驻留表（trans_stat 在 KGSL 上是冻的，采了等于没采）');
  ok(csrc.includes('GPU_STATS_FILE') && /lt\[ln\] = 4/.test(csrc),
     '守护把 GPU 追加成第 5 档，且没有那张表就不追加');
  ok(apiSrc.includes('CPU_PCT_LITTLE GPU_PCT') && apiSrc.includes('c[4] = c4') &&
     apiSrc.includes('t > 4'),
     '判据侧认第 5 档，键名是 GPU_PCT');
  ok(/GPU_PCT[^\n]*gpu_pct/.test(html),
     '页面把 GPU_PCT 对到那颗 id 叫 gpu_pct 的滑杆（两边不同名，必须显式映射）');
}


/* ---- MTK gpufreqv2 GPU 后端（荣耀 MT6993 / 天玑 9500 实测）---- */
{
  const realSrc = fs.readFileSync(path.join(MODDIR, 'src/test_real.c'), 'utf8');
  ok(/#define MTK_LIMIT\s+"\/proc\/gpufreqv2\/limit_table"/.test(csrc)
     && /#define MTK_OPP\s+"\/proc\/gpufreqv2\/stack_working_opp_table"/.test(csrc)
     && /mtk_parse_opp_text/.test(csrc) && /mtk_write_row/.test(csrc)
     && /mtk_restore_row/.test(csrc) && /mtk_restore_snapshot/.test(csrc),
     'MTK 天玑走 gpufreqv2：解析 OPP 表、写 limit_table、崩溃/退出都还原');
  ok(/set %d %d %d\\n/.test(csrc) && /g_gpu\.mtk \? "MTK"/.test(csrc)
     && /g_gpu\.mtk\) \{[\s\S]{0,260}?g_gpu\.mtk_orig_ceiling/.test(csrc),
     'limit_table 写入格式 set id ceiling floor；状态报 gpu_kind=MTK；保存原始行');
  ok(/mtk_parse_opp_text/.test(realSrc) && /gpu_pwr_for\(754000000L\)/.test(realSrc),
     'test_real 覆盖 MTK OPP 解析与频率→index 映射');
}

/* ---- 机型适配包 / 节点覆盖 ---- */
{
  // 覆盖文件两端必须同名同路径：一个写一个读，写歪了就是"导入了但没生效"
  const ovrC = (csrc.match(/#define\s+OVR_FILE\s+SF_DATADIR\s+"([^"]+)"/) || [])[1] || '';
  const ovrSh = (apiSrc.match(/^OVR="([^"]+)"/m) || [])[1] || '';
  ok(/\/override\.conf$/.test(ovrC) && ovrSh.includes('override.conf'),
     '守护和 api 指的是同一份覆盖文件', 'c=' + ovrC + ' sh=' + ovrSh);
  // 覆盖只在探测不到时兜底：正常机器不能被人塞一份文件就改掉实测拓扑
  ok(/if \(!n\) n = ovr_policies\(tmp\)/.test(csrc),
     'CPU 覆盖只在自动探测一个都没拿到时生效');
  ok(csrc.includes('if (!n && ovr_gpu_freqs[0])'),
     'GPU 频率表也走同一条兜底顺序（在"未找到就跳过"之前）');
  ok(/ovr_load\(\);/.test(csrc), '启动时读一次覆盖，且在任何探测之前');
  const kApi = ["cpu-dir","gpu-max","gpu-min","gpu-freqs"].filter(x => apiSrc.includes(x));
  const kC = ["cpu-dir","gpu-max","gpu-min","gpu-freqs"].map(x => x + ' ').filter(x => csrc.includes('"' + x + ''));
  ok(kApi.length === 4 && kC.length === 4,
     '四个覆盖键两端都认（少一个就是导入能写、守护读不到）', kApi.join() + ' / ' + kC.join());
  ok(apiSrc.includes('/storage/emulated/0/Download/Soc智能限频'),
     '导出落点是公共 Download（QQ/微信 发文件能直接翻到）');
  ok(apiSrc.includes('SOCFREQ_PKG_DIR'),
     '导出目录有环境变量钩子（否则这段只能上真机验）');
  // devfreq 下面不止 GPU：UFS 控制器（ufshci）、总线带宽监控、dvfsrc 也有 max_freq。
  // 只靠黑名单挡不住新机型（荣耀 MT6993 的 16810000.ufshci 就被误当成 GPU），
  // 所以改成名字白名单：mali/kgsl/gpufreq/gpu/3d0，再排掉已知非 GPU。
  const povr = (apiSrc.match(/pkg_ovr\(\) \{[\s\S]*?\n\}/) || [''])[0];
  ok(/gpu_node_ok\(\) \{[\s\S]{0,320}?\*mali\*\|\*kgsl\*\|\*gpufreq\*\|\*gpu\*\|\*3d0\*/.test(apiSrc)
     && /gpu_node_ok\(\) \{[\s\S]{0,520}?\*ufshci\*/.test(apiSrc),
     'GPU 候选按名字白名单（mali/kgsl/gpufreq/gpu/3d0），并排掉 ufshci/dvfsrc/busmon',
     (apiSrc.match(/gpu_node_ok\(\)[\s\S]{0,200}/) || [''])[0]);
  ok((povr.match(/gpu_node_ok "\$f"/g) || []).length >= 2,
     '导出的 max_freq 与 available_frequencies 两处都走同一 GPU 判据');
  ok(/\/proc\/gpufreqv2\/limit_table/.test(apiSrc) &&
     /\/proc\/gpufreqv2\/stack_working_opp_table/.test(apiSrc) &&
     /\/sys\/module\/ged\/parameters/.test(apiSrc),
     '适配包会把 MTK gpufreqv2 / GED 的现场打进包（只读，不写）');
  // 这条功能的两份验证都得在，而且要在 README 里点名 —— 否则下一个人只知道有 api.sh，
  // 不知道适配路径还有专门的渲染测试和真机沙箱自检。
  for (const [f, why] of [['src/test_ovr_ui.js', '适配卡片的渲染测试（喂真 api.sh 输出）'],
                          ['src/test_func_ui.js', '功能页档位的测试（档位值要过后端校验区间）'],
                          ['tools/sandbox_probe.sh', '探测扑空时的兜底路径（chroot 沙箱，要 root）']]) {
    ok(fs.existsSync(MOD0 + '/' + f), '存在 ' + f + '（' + why + '）');
    ok(DOC.includes(f.split('/').pop()),
       'README 的自检清单里点了 ' + f + ' 的名字');
  }
  ok(/pkg_import\) shift; cmd_pkg_import/.test(apiSrc),
     'pkg_import 允许不带参数（作者侧命令行取目录里最新的包）');
  ok(/api\('pkg'\)/.test(html) && /api\('ovr_show'\)/.test(html) &&
     !/api\('pkg_import'\)/.test(html) && !/api\('ovr_clear'\)/.test(html),
     '页面只有导出与只读状态两个动作（导入/清除只留作者侧命令行）',
     /api\('(pkg_import|ovr_clear)'\)/.exec(html)?.[0] || '没找到导入调用');
  // 导入的是哪一份文件必须打在回执里：目录里同时躺着"自己导出的"和"作者回的"
  ok(/printf '  用的文件：%s\\n' "\$_f"/.test(apiSrc),
     '导入回执打印用的是哪一份文件（默认取最新，不打印就没法核对）');
  // "缺什么"那段判 GPU 上限节点要逐个试。三个候选塞一条 ls 的话
  // 只要有一个打不开就返回非零 —— 高通机器上 mali 那条必然不存在，
  // 明明有 devfreq/…/max_freq 也会被报成"没找到"（真机导出实测踩过）。
  const lack = (apiSrc.match(/这份包缺什么[\s\S]*?\n  \} >/) || [''])[0];
  ok(!!lack, '"缺什么"那段抠得到（锚点：这份包缺什么）', lack ? '长度 ' + lack.length : '没找到');
  ok(/for f in [^;]*devfreq\/\*\/max_freq[\s\S]*?\[ -f "\$f" \][\s\S]*break/.test(lack),
     'GPU 上限节点逐个候选判存在（不是一条 ls 列三个候选）');
  ok(!/ls \/\sys\/class\/devfreq\/\*\/max_freq \S+ \S+ >\/dev\/null/.test(lack),
     '没有把多个 GPU 候选塞进同一条 ls');

  /* 适配表的发布通道：作者把改好的表放进包里的 adapt/，装机脚本按 #match 认机器，
     命中才写进数据目录当 override.conf。三方（装机脚本 / 守护 / 导出端）用的是同一组键，
     少一边对不上就是"装上了但守护读不到"。 */
  const czc = fs.readFileSync(MOD0 + '/customize.sh', 'utf8');
  const adBlk = (czc.match(/_soc_adapt\(\) \{[\s\S]*?\n\}/) || [''])[0];
  const matchBlk = (czc.match(/_adapt_match\(\) \{[\s\S]*?\n\}/) || [''])[0];
  ok(!!matchBlk && /"\$MODPATH"\/adapt\/\*\.conf/.test(matchBlk),
     '装机脚本会扫包里的 adapt/*.conf（匹配函数抠得到）', matchBlk ? '长度 ' + matchBlk.length : '没抠到');
  ok(matchBlk.includes('grep -E \'^(cpu-dir|gpu-max|gpu-min|gpu-freqs) \'') === false &&
     adBlk.includes('grep -E \'^(cpu-dir|gpu-max|gpu-min|gpu-freqs) \''),
     '只有那四种节点键的行会被带进数据目录（打错前缀的行不会被静默装上）');
  const kCz = ["cpu-dir", "gpu-max", "gpu-min", "gpu-freqs"].filter(x => adBlk.includes(x));
  ok(kCz.length === 4, '装机脚本认的键和守护读的一致', kCz.join());
  ok(/grep '\^#match '/.test(matchBlk) && /\[ -n "\$_ml" \] \|\| continue/.test(matchBlk),
     '没有 #match 的文件不会被装到任何机器上');
  ok(/只有芯片默认/.test(adBlk),
     '命中只有 default_* 的芯片文件时只套默认，不写 override.conf');
  // 撤表的条件：只撤"上一版由 adapt 装进去的那份"。
  // 无条件 rm -f 的话，作者自己用命令行导进去的表会被下一次装机删掉。
  ok(/head -n 1 "\$DATA\/override\.conf"[^\n]*grep -q '\^#from adapt\//.test(adBlk),
     '撤表只撤带 #from adapt/ 标记的那份（命令行导入的表不动）',
     (adBlk.match(/#from adapt[^\n]*/) || ['没找到'])[0]);
  ok(/printf '#from adapt\//.test(adBlk), '装上时会写下来源标记');
  // 那行标记对守护必须是"不认识的行"，被跳过而不是当成一条节点
  ok(csrc.includes('strncmp(line, "cpu-dir ", 8)') && !csrc.includes('"#from'),
     '守护的解析只认四种前缀，来源标记行会被跳过');
  // 沙箱自检要按"真被装上那份的格式"来验，否则装机脚本写的标记行从没被守护读过的东西算通过
  const sb = fs.readFileSync(MOD0 + '/tools/sandbox_probe.sh', 'utf8');
  ok(/#from adapt\//.test(sb), '真机沙箱自检的覆盖文件也带来源标记（量的是装机后的真实格式）');
  // 随包发的那个模板必须永远命中不了
  const tpl = fs.existsSync(MOD0 + '/adapt/_TEMPLATE.conf')
    ? fs.readFileSync(MOD0 + '/adapt/_TEMPLATE.conf', 'utf8') : '';
  ok(/^#match model=__NO_SUCH_MODEL__$/m.test(tpl), '包内模板的 #match 命中不了任何机器');
  ok(!/^(cpu-dir|gpu-max|gpu-min|gpu-freqs) /m.test(tpl), '模板里的节点行是注释状态（不会被装上）');
  ok(/adapt\//.test(DOC),
     '文档写了 adapt/ 这条发布通道');
}

/* ── V1.2 实时偏移：PSI 借 / 温控压 / 充电抬 ─────────────────────
   三条都是"改生效值、不改配置"。最容易出的错有三类，这里一类钉一条：
     A 键对不上：conf 写了、守护不读、页面能填 —— 填了没反应；
     B 状态键对不上：守护发了、api 的白名单没放行 —— 页面永远显示"—"；
     C 方向错了：PSI 去收紧、或插电盖掉用户给某个应用设的档位。 */
console.log('\n== 实时偏移（PSI / 温控 / 充电态）==');
{
  const adaptC = fs.readFileSync(MOD0 + '/src/adapt.c', 'utf8');
  const adaptH = fs.readFileSync(MOD0 + '/src/adapt.h', 'utf8');
  const ADAPT_KEYS = ['PSI_ADAPT', 'PSI_GAIN', 'PSI_STEP', 'PSI_HOLD', 'PSI_BACK',
                      'TH_CAP', 'TH_HIGH', 'TH_LOW', 'TH_STEP', 'TH_MAX', 'TH_BACK',
                      'CHG_UNCAP', 'CHG_PCT', 'ADAPT_UP_MAX', 'ADAPT_DOWN_MAX'];
  const inConf = ADAPT_KEYS.filter(k => new RegExp('^' + k + '=').test(conf) ||
                                         conf.includes('\n' + k + '='));
  ok(inConf.length === ADAPT_KEYS.length,
     'conf 里 15 个偏移键全都写明默认值（升级装机靠这个补齐）', inConf.length + '/' + ADAPT_KEYS.length);
  const notRead = ADAPT_KEYS.filter(k => !csrc.includes('"' + k + '"'));
  ok(notRead.length === 0, '守护逐个读这些键（漏一个就是页面能填、实际不生效）', notRead.join());
  const notKey = ADAPT_KEYS.filter(k => !apiSrc.includes(k));
  ok(notKey.length === 0, 'api.sh 的 KEYS 白名单放行这些键（否则 conf_set 直接 BAD_KEY）', notKey.join());

  // 合计总闸：单项各自有上限，叠起来仍可能过狠
  ok(/int up_max;/.test(adaptH) && /int down_max;/.test(adaptH),
     '偏移配置带合计总闸的上/下限字段');
  ok(/if \(d > c->up_max\) d = c->up_max;/.test(adaptC) &&
     /if \(d < -c->down_max\) d = -c->down_max;/.test(adaptC),
     'adapt_apply 对最终 delta 做一次 clamp（不是只靠单项上限）');
  ok(/cfg\.ad\.up_max = 15;/.test(csrc) && /cfg\.ad\.down_max = 30;/.test(csrc),
     '总闸默认 +15 / -30');
  // PSI 基线：交权态和偏移生效期只读不写
  ok(/static void step_psi\([\s\S]{0,160}?int allow_base\)/.test(adaptC) &&
     /if \(allow_base\) \{/.test(adaptC),
     'PSI 基线更新有闸门（allow_base=0 时只读）');
  ok(/int base_ok = !strcmp\(g_state, "normal"\) && adapt_delta\(\) == 0;/.test(csrc) &&
     /adapt_tick\(&cfg\.ad, adt, base_ok\)/.test(csrc),
     '交权态不更新基线；PSI/温控正在生效时也不更新（插电不改 delta，照常更新）');

  // 状态键：守护 fprintf 出去的，api.sh 的 grep 白名单必须也认
  {
    /* 守护那行 fprintf 的格式串里，除第一个键外都跟在 \n 后面，
       所以取键名要按"引号后或 \n 后"来取，只认引号后面那个会漏掉 7 个。 */
    const pub = [...csrc.matchAll(/(?:\\n|")(dlt|stall|psib|avg10|temp|chg|psi|th)=/g)].map(m => m[1]);
    const grepLine = (apiSrc.match(/grep -E '\^\(state[^\n]*\)='/) || [''])[0];
    /* 白名单里最后一个键后面跟的是 )= 而不是 |，所以判"在不在"要按
       (^|,) 键 (| |)) 三种边界来查 —— 只查 "|th|" 会把真的 th 判成缺。 */
    const missing = pub.filter(k => !new RegExp('(^|\\|)' + k + '(\\||\\)=)').test(grepLine));
    ok(new Set(pub).size >= 8, '守护发布了 8 个偏移相关状态键', [...new Set(pub)].join());
    ok(missing.length === 0, 'api.sh 读状态时放行这些键（漏一个页面就永远显示 —）', missing.join());
    // 兜底那行也要给全：缺字段的前端不是"不显示"，是 parseInt(undefined)=NaN
    const fb = (apiSrc.match(/printf 'state=unknown[\s\S]*?'/) || [''])[0];
    const fbMiss = pub.filter(k => !fb.includes(k + '='));
    ok(fbMiss.length === 0, '状态文件不存在时的兜底也给全偏移键', fbMiss.join());
    const used = [...html.matchAll(/s\['(?:dlt|stall|psib|avg10|temp|chg|psi|th)'\]|n\('(dlt|stall|psib|avg10|temp|chg|psi|th)'\)/g)].map(m => m[1]);
    ok(used.length > 0, '页面确实读了偏移状态键', used.slice(0, 6).join());
  }

  // 方向：PSI 只借不拿
  /* 合计里 PSI 必须是加号、温控必须是减号。
     写成"PSI 那一段后面不许出现减号"是错的：下一行就是 `d -= th_steps…`，
     断言会因为挨着的那一句而红，报的原因和方向都没关系。 */
  ok(/d \+= a->psi_steps \* c->psi_step;/.test(adaptC) &&
     /d -= a->th_steps \* c->th_step;/.test(adaptC),
     '合计：PSI 是加（只借不拿），温控是减（收紧归学习判）');
  ok(/step_psi[\s\S]*?if \(!c->psi\) \{ a->psi_steps = 0;/.test(adaptC),
     '关掉 PSI 时步数立刻清零（不是留着上一次的借出）');
  ok(/stall > a->base \* 2 \+ 30/.test(adaptC),
     '判"卡"要同时过相对基线和绝对增量两道（只看倍数会被静置噪声点着）');
  ok(/#define PSI_TAU 600/.test(adaptH), '基线时间常数是 10 分钟量级（写死绝对阈值就是上一版的坑）');

  // 温控：迟滞带两端都拦
  ok(/if \(cfg\.ad\.th_low >= cfg\.ad\.th_high\) cfg\.ad\.th_low = cfg\.ad\.th_high - 5;/
       .test(csrc),
     '守护侧兜底：恢复线不低于触发线时贴着一档留 5 度');
  ok(/die "TH_ORDER"/.test(apiSrc) && (apiSrc.match(/TH_ORDER/g) || []).length >= 2,
     'api 侧双向拦：改 TH_HIGH 或 TH_LOW 都可能把迟滞带填反');
  ok(/if \(temp_m <= lo\)[\s\S]{0,200}a->th_steps--/.test(adaptC) &&
     /a->cool_until = 0;/.test(adaptC),
     '温控是"到线逐档压、回带逐档还"，迟滞带里保持不动');
  ok(/t > 200000\) continue;/.test(adaptC) && /strstr\(low, "cpu"\)/.test(adaptC),
     '只取 type 带 cpu/gpu/soc 的 zone，且排除恒 0 和电压读数');
  /* 单位必须钉住：内核 thermal_zone/temp 是**毫摄氏度**（43600 = 43.6℃）。
     真机第一版按 0.1℃ 比较（阈值 ×10），结果静置 43℃ 就被判成超阈值、
     温控白压一档；单测喂的也是同一套错单位，所以 140 条全绿也没拦住。 */
  ok(/int hi = c->th_high \* 1000, lo = c->th_low \* 1000;/.test(adaptC),
     '温控阈值按毫摄氏度换算（×1000，不是 ×10）');
  ok(/a\.temp_m > 0 \? a\.temp_m \/ 1000 : -1/.test(csrc),
     '发布给页面的温度换算成摄氏度（temp=43 而不是 4360）');
  {
    const tr = fs.readFileSync(MOD0 + '/src/test_real.c', 'utf8');
    const blk = (tr.match(/实时偏移的步进机[\s\S]*?插电时地板抬到/) || [''])[0];
    ok(/74900/.test(blk) && /75100/.test(blk) && !/749 \/\* 74\.9/.test(blk),
       '单测喂的是毫摄氏度（和真机同源，错单位不会再两边一起错）',
       (blk.match(/\d{3,5} \/\* 74/g) || ['没找到'])[0]);
  }

  // 充电态：地板不盖用户给单个应用设的档
  ok(/if \(!per_app && a\.chg_on && a\.chg_pct > base\)/.test(csrc),
     '插电抬地板只作用于全局分档（专属档位是用户的明确选择）');
  ok(/pct = eff_pct\(pct, app_pct > 0\)/.test(csrc),
     '专属档位也吃实时偏移（热了照样降），但不吃充电地板');
  /* GPU 原来只按加载配置时算出的 g_gpu.normal 写一次，温控读到的
     偏偏常是 GPU zone —— 压的是 CPU，GPU Boost 继续烧，闭环等于只做一半。 */
  ok(/static long gpu_target_for\(int base_pct, int delta\)/.test(csrc) &&
     /int d = adapt_delta\(\);/.test(csrc) &&
     /return gpu_target_for\(base, d\);/.test(csrc),
     'GPU 生效值吃 adapt_delta（PSI 借档 + 温控降档）');
  {
    const body = (csrc.match(/static long gpu_target_for\([\s\S]*?\n\}/) || [''])[0];
    ok(!/chg_pct/.test(body) && !/eff_pct/.test(body),
       'GPU 不吃充电地板（GPU_PCT 是用户明确设的上限）', body ? '' : '没抠到函数');
    ok(/if \(pct < 1\) pct = 1;/.test(body) && /if \(pct > 100\) pct = 100;/.test(body),
       'GPU 百分比夹到 1~100');
  }
  ok(/static void gpu_patrol\(void\)/.test(csrc) &&
     /if \(got > t\) write_gpu\(t, 0\)/.test((csrc.match(/static void gpu_patrol\(void\) \{[\s\S]*?\n\}/) || [''])[0])
     && !/got > g_gpu\.normal/.test(csrc),
     'GPU 巡检用同一个目标值（温控压下去后不会被 normal 顶回来）');
  ok(/if \(has_mode\) \{[\s\S]{0,240}?gpu_patrol\(\);/.test(csrc)
     && (csrc.match(/gpu_patrol\(\);/g) || []).length >= 2,
     '分应用档位分支也巡检 GPU（提前 continue 不能漏掉 Boost 回压）');
  ok(/FILE \*sf = fopen\(STATUS_FILE, "r"\)/.test(csrc) &&
     /gpu_target_for\(agpu > 0 \? agpu : cfg\.gpu_pct, dlt\)/.test(csrc) &&
     !/printf\("gpu_cap=%ld\\n", g_gpu\.normal\)/.test(csrc),
     'export 的 gpu_cap 按 status 的 app_gpu/dlt 重算生效值（独立进程里 active 恒为 0）');
  ok(/lc\[ln\] = g_gpu\.active > 0 \? g_gpu\.active : g_gpu\.normal;/.test(csrc),
     '学习采的是 GPU 生效值，不是静态配置值');
  /* 游戏态被实时偏移打断的 P0：apply_normal 会把 g_state 改回 normal、
     收回节点写权限，而游戏分支同一包名不会重新 unlock_game，
     于是整个游戏期间被封顶且厂商调度接不回去。 */
  ok(/static int reclaim_caps_allowed\(void\) \{[\s\S]{0,200}?strcmp\(g_state, "game"\) != 0/
       .test(csrc) &&
     /strcmp\(g_state, "full"\) != 0/.test(csrc) &&
     /if \(reclaim_caps_allowed\(\)\) apply_normal\(1\);/.test(csrc),
     '交权态（game/full）下重算只进内存，不抢回节点');
  ok((html.match(/pairs\.push\(\['(PSI_ADAPT|TH_CAP|CHG_UNCAP|CHG_PCT|TH_HIGH|TH_LOW|TH_MAX|PSI_GAIN)'/g) || []).length === 8,
     '页面保存清单里 8 个偏移控件都在（少一个就是拨了没反应）');
  ok(/LEARN', document\.getElementById\('learn'\)/.test(html),
     '智能学习开关在保存清单里（此前只渲染不保存，拨了按保存也不写盘）');
  ok(/int adapt_clean = !av\.delta && !av\.chg_on;/.test(csrc) &&
     /learn_tick\(!strcmp\(g_state, "normal"\) && adapt_clean/.test(csrc),
     '学习采样避开偏移生效时段（插电/温控/PSI 期间帽不是设定值那道）');
  /* 生效封顶只有一处施加，而且必须在读完传感器之后。
     以前 reload_if_changed 自己就 assign_pct + apply_normal，用的是上一拍的偏移
     快照 —— 真机上表现为"改一次配置先按旧地板写一遍节点，下一拍才纠正"。 */
  ok(/static int reload_if_changed\(uint64_t seen\)/.test(csrc) &&
     !/static int reload_if_changed[\s\S]{0,400}apply_normal/.test(csrc),
     'reload_if_changed 只吸收配置，不再自己施加封顶');
  ok(/int cfg_changed = reload_if_changed\(seen\);[\s\S]{0,4000}if \(cfg_changed \|\| adapt_changed\) \{\s*\n\s*assign_pct/.test(csrc),
     '主循环里：偏移算完之后才统一重算并施加（顺序错了就会写一遍旧地板）');
  ok(/#from adapt\//.test(fs.readFileSync(MOD0 + '/customize.sh', 'utf8')),
     '装机脚本写的来源标记和守护读的文件是同一份（撤表判据认它）');
}

/* ── 拉起守护的那条路不能被调用者吊住 ─────────────────────────────
   守护循环是前台常驻的。restart 如果在调用者这个进程里 exec 成守护，
   这条命令就永远不返回：WebUI 的请求、action.sh、适配包导入和清除的
   回执全被吊住，管理器那头只看到超时。
   判据钉在守护那一侧（所有调用者共用一份实现），不靠"调用者记得加 &"。 */
console.log('\n== restart 不吊住调用者 ==');
{
  const rb = (csrc.match(/strcmp\(cmd, "stop"\) \|\| !strcmp\(cmd, "restart"\)[\s\S]*?\n  \}/) || [''])[0];
  ok(!!rb, 'restart 分支抠得到（锚点：stop||restart）', rb ? '长度 ' + rb.length : '没找到');
  ok(!/execl\([^)]*"start"[^)]*\);\n\s*perror\("execl"\);\n\s*return 1;/.test(rb),
     'restart 不在调用者进程里 exec 成守护');
  ok(/fork\(\)/.test(rb) && /setsid\(\)/.test(rb),
     '子进程先 fork 再 setsid（管理器掐掉请求进程组时不该顺带杀掉守护）');
  ok(/dup2\(fd, 2\)/.test(rb),
     '子进程的标准三路都接走（不然守护的日志会写进 WebUI 请求的管道）');
  ok(/printf\("restart issued/.test(rb),
     'restart 立刻给调用者一行回执再返回');
  // 等旧实例退出这段：条件写反（"还活着就 break"）等于完全没等，
  // 新实例会在旧实例恢复节点之前就接手。
  ok(/if \(!pid_is_daemon\(pid\)\) break;/.test(rb),
     '等旧实例退出的条件是"不再活着"，不是"还活着"');
  // service.sh 那条是 nohup + &，和 restart 是两条不同的路，各自都要能落地
  ok(/nohup "\$DAEMON" start >\/dev\/null 2>&1 <\/dev\/null &/.test(svc),
     '看门狗用 nohup 后台拉起（这条必须是前台实现，靠调用者 detach）');
}

/* ── 按应用学习：两层样本、手填优先、只出数字 ─────────────────
   这一层最容易出的不是算错，而是两层互相喂数据、或者学习把用户显式填的
   值盖掉。下面这几条钉的就是这两件事。 */
{
  const lc = fs.readFileSync(MOD0 + '/src/learn.c', 'utf8');

  ok(/#define LEARN_APP_FILE SF_DATADIR "\/learn_app\.tsv"/.test(csrc)
     && /learn_init\(LEARN_FILE, LEARN_APP_FILE\)/.test(csrc),
     '守护按两份样本文件走：路径定义 + 一起交给 learn_init');
  ok(/LEARN_APP="\$DATA\/learn_app\.tsv"/.test(apiSrc),
     'api.sh 里那份 learn_app.tsv 的路径和守护一致');

  /* 默认关。这条判据和别的开关是反的（只认显式的 1），所以两处都要对上：
     老配置里没有这一行时，页面显示"开"而守护其实没采是最坏的组合。 */
  ok(/cfg\.learn_app = 0;/.test(csrc) && /LEARN_APP=0/.test(conf),
     '按应用学习默认关：守护缺省 0，包内配置也写着 0');
  ok(/cfg_get\("LEARN_APP", v\) == 0\)\s*\n\s*cfg\.learn_app = \(!strcmp\(v, "1"\)/.test(csrc),
     'LEARN_APP 只认显式的 1（缺行、写 2、写 true 之外的东西都不算开）');
  ok(/getElementById\('learn_app'\)\.checked = c\.LEARN_APP === '1'/.test(html),
     '页面也按"缺行 = 关"显示');

  /* 两层归属：专属帽只记应用行；全局帽下的同一份分布两层都记 */
  ok(/int app_cap_active = g_app_capped\[0\] != 0;/.test(csrc)
     && /&& !app_cap_active,/.test(csrc),
     '调用处保证戴应用帽时全局那层不再累计（和开没开按应用学习无关）');
  ok(/if \(own_cap\) eligible = 0;/.test(lc),
     'learn.c 自己也兜一次：专属帽不进全局行，不靠调用处自觉');
  ok(/int feed_app = app && \(own_cap \|\| eligible\);/.test(lc),
     '全局帽下两层同时采样，专属帽下只进应用行');
  ok(/\(want_pct > 0 \|\| want_gpu > 0\) && fg\[0\]/.test(csrc),
     'GPU-only 档（:40）也算应用上下文：那段 GPU 驻留不能算进全局样本');
  ok(/lpkg = g_app_capped/.test(csrc) && /lown = 1/.test(csrc)
     && /else if \(g_fg_global\[0\]\) lpkg = g_fg_global/.test(csrc),
     '调用处的归属有两个来源：专属帽优先，否则是全局帽下的前台包');
  ok(/static char g_fg_global\[PKG_LEN\]/.test(csrc)
     && /if \(fg\[0\]\) snprintf\(g_fg_global/.test(csrc),
     '全局帽稳态巡检结束时才写前台归属（游戏宽限/满血/专属档位那几支提前 continue）');
  ok(/as->a\.cap_pct != p/.test(lc) && /memset\(&as->a, 0, sizeof as->a\)/.test(lc)
     && /if \(got && !app_changed\)/.test(lc),
     '应用行的帽换了就从零重攒，且换帽那一拍不进样本（两道帽的分布混在一起，建议会沿旧帽一路加码）');

  /* 帽的凭据必须是"配置意图"。用生效值会在插拔电源（充电地板 100%）、
     PSI/温控抖动、切到带档位的应用时反复换帽，把攒了几小时的分布清零——
     真机上表现就是"CPU 学习不见了"，而 GPU 那行还在。 */
  ok(/int app_cap_active = g_app_capped\[0\] != 0;/.test(csrc)
     && /int app_layer = cfg\.learn_app && app_cap_active;/.test(csrc)
     && /lp\[ln\] = \(app_layer && g_app_pct > 0\) \? g_app_pct : cfg\.pct\[g_pol\[i\]\.tier\]/.test(csrc)
     && /lp\[ln\] = \(app_layer && g_app_gpu > 0\) \? g_app_gpu : cfg\.gpu_pct/.test(csrc),
     '样本里的帽记配置意图：插电/抖动不再清空样本；应用帽只在开了按应用学习时写进该应用');
  ok(/learn_tick\(!strcmp\(g_state, "normal"\) && adapt_clean && !app_cap_active,/.test(csrc),
     '任意应用专属帽生效时全局行都不采（和开没开按应用学习无关）');
  ok(/if \(!own_cap && p > 0 && st->a\.cap_pct != p\)/.test(lc),
     '戴应用帽的那一拍不许改写或清零全局行（不戴时允许纠正错帽，但不累计）');
  const appStruct = (lc.match(/struct app_stat \{[\s\S]*?\n\}/) || [''])[0];
  ok(appStruct && !/prev/.test(appStruct),
     '按应用行不存基线：驻留表一个目录只有一份读数，各存一份会把别的包算到这个包头上');
  ok(/static struct app_stat \*learn_app_slot\(const char \*pkg, int tier\)/.test(lc),
     '按应用行按 (包名, 档) 找，档越界直接不要');

  /* 字段数：判据按 31 列判，两份文件必须同构 */
  ok(/#define LEARN_TAIL \(3 \+ LEARN_BANDS \+ 1 \+ 1\)/.test(lc)
     && /#define LEARN_BANDS\s+24/.test(lc),
     'learn.c 的行长 = 采样/顶帽/被压 + 24 桶 + 天位图 + 配置帽 = 29（加键和档共 31 列）');
  const nfChecks = (apiSrc.match(/NF < 31/g) || []).length;
  ok(nfChecks >= 2,
     '全局和按应用两条路都按 31 列判半截行（一处放宽就是拿坏数据出建议）', '找到 ' + nfChecks + ' 处');

  /* 天数：从首次采样起每满 24 小时算一天，不按 0 点（更不是 UTC 0 点）切。
     旧口径在国内早上 8 点翻篇，新装 9 小时就可能显示 2 天。 */
  ok(/long learn_day_index\(long since, long now\)/.test(lc) &&
     /\(d \/ 86400\) % 28/.test(lc) &&
     !/time\(NULL\) \/ 86400/.test(lc),
     '天数按 24 小时切块（旧口径 time/86400 已移除）');
  ok(/snprintf\(g_since_path, sizeof g_since_path, "%s\.since", path\)/.test(lc) &&
     /learn_since_save\(\)/.test(lc) && /learn_since_load\(\)/.test(lc),
     '采样起点持久化到 learn.tsv.since（守护重启不重置天数）');
  ok(/!g_since && \(g_n \|\| g_nas\)[\s\S]{0,300}?sampled_ms > 0 \? 1u : 0u/.test(lc)
     && /learn_write_rows\(g_path, 0\)/.test(lc),
     '旧样本没有起点文件时重算天数：有分钟的行至少记第 1 天，并立刻写回');

  /* 旧格式升级：两份一起清、留标记、有数据后撤标记；关学习时标记也要撤 */
  ok(/learn_format\(path\) == 1 \|\| \(apath && apath\[0\] && learn_format\(apath\) == 1\)/.test(lc)
     && /fopen\(g_reset_mark, "w"\)/.test(lc),
     'v1 样本没有"帽是多少"这一列：升级时两份一起清空并留 v2reset 标记');
  ok(/if \(wrote && g_reset_mark\[0\]\) unlink\(g_reset_mark\)/.test(lc),
     '新样本真正落盘后才撤标记（提前撤会让页面以为已经在攒）');
  ok(/g_since <= 0\s*\n\s*&& \(!g_reset_mark\[0\] \|\| access\(g_reset_mark, F_OK\) != 0\)/.test(lc)
     && /if \(g_reset_mark\[0\]\) unlink\(g_reset_mark\)/.test(lc),
     '关掉学习时刚留下的重置标记也要撤（累计是空的，但标记还在），标记不在时别每轮 unlink');
  ok(/if \(apath && apath\[0\]\) snprintf\(g_apath[\s\S]{0,300}?learn_format\(path\)/.test(lc),
     'v1 重置分支返回前已经记下按应用路径（否则这一轮只写全局样本）');

  /* 手填优先 + 只出数字 */
  const mlearn = (apiSrc.match(/modes_learn\(\) \{[\s\S]*?\n\}/) || [''])[0];
  ok(/SKIP .*手填/.test(mlearn) && /#\[ \\t\]\*learn/.test(mlearn),
     'modes_learn 认出行尾没有 #learn 就拒绝改（手填的永远压过学出来的）');
  ok(/case "\$_v" in ''\|\*\[!0-9\]\*\) die "BAD_MODE"/.test(mlearn),
     '学习只能写数字：full（整个交还系统）这个动作不给自动通道');
  ok(/char \*hash = strchr\(val, '#'\);/.test(csrc),
     '守护解析值时先截掉 #learn 注释：截不掉就是整行被当坏值丢掉，档位静默不生效');
  ok(/&& r\.manual !== '1'/.test(html) && /r\.manual === '1'/.test(html),
     '按应用建议按字符串比 manual：parse() 交回来的 "0" 在 JS 里是真值，'
     + '直接 if(r.manual) 会把学习条目全当得手填、一条都发不出去');
  ok(/await api\('modes_learn', r\.pkg, r\.to\)/.test(html),
     '按应用的建议走 modes_learn，不是 conf_set');

  /* 第二段的判据：没设档位的包拿全局四簇平均当基准；full 不学；列表只留有建议的 */
  ok(/base = \(nc > 0\) \? int\(cs \/ nc \+ 0\.5\) : 0/.test(apiSrc),
     '没设档位的包按全局四簇平均当基准（四舍五入，缺档位的那几项不算）');
  ok(/bsrc = ismode \? "mode" : "global"/.test(apiSrc)
     && /base=%s/.test(apiSrc) && /days, why, bsrc/.test(apiSrc),
     '每条记录都带基准来源（档位 / 全局），页面才能挂「全局」标；'
     + '只在没档位时才多打一行，按行抽取的门就看不见这个字段');
  ok(/v == "full" \|\| v == "FULL"/.test(apiSrc),
     'full 的包整条跳过：满血没有帽，学它也没有可改的值');
  ok(/if \(!ismode && adv == "hold"\) continue/.test(apiSrc),
     '没档位的包只在真有 up/down 时列出，样本不够的 hold 不刷列表');
  ok(/else if\(r\.base === 'global'\)/.test(html) && /textContent = '全局'/.test(html),
     '页面给没档位的建议挂「全局」标（cur 不是已有的专属档位）');

  /* 清零：两层数据 + 学习写回的档位一起撤，手填的一条不动 */
  const lreset = (apiSrc.match(/cmd_learn_reset\(\) \{[\s\S]*?\n\}/) || [''])[0];
  ok(/rm -f "\$LEARN" "\$LEARN_APP" "\$LEARN\.since"/.test(lreset)
     && /grep -v '#\[ \\t\]\*learn'/.test(lreset),
     '清零删两份样本和计时起点，并且只撤带 #learn 标记的档位行');
  ok(/OK learn_reset modes=%s/.test(lreset),
     '回执说清撤了几条学习档位（说 0 也是信息，静默不响用户不知道改了什么）');

  /* 列表来源标记必须被前端剥掉再当值用 */
  ok(/function splitMode\(r\)/.test(html) && /v\.replace\(\/\\s\*learn\$\/, ''\)/.test(html),
     'modes_list 的 learn 词尾在渲染前剥掉：不然下拉框的当前值是 "75 learn"，认不出来会回落成第一项');
  ok(/textContent = '学出来'/.test(html) && /tag\.onclick = \(\)=>setMode/.test(html),
     '学习写回的档位标出来源，点一下夺回成手填');

  /* 门槛不能被默认分支的 1..100 管着 */
  ok(/LEARN_APP_MIN\)\s+\[ "\$_v" -ge 5 \] && \[ "\$_v" -le 600 \]/.test(apiSrc)
     && /LEARN_APP_DAY\)\s+\[ "\$_v" -ge 1 \] && \[ "\$_v" -le 28 \]/.test(apiSrc),
     '两个门槛都有显式区间（分钟能到 600，掉进默认的 1..100 就是填不进去）');
  ['LEARN_APP', 'LEARN_APP_MIN', 'LEARN_APP_DAY'].forEach(k => {
    ok(new RegExp("KEYS='[^']*\\b" + k + "\\b").test(apiSrc), `KEYS 名单里有 ${k}`);
    ok(conf.includes(k + '='), `socfreq.conf 里有 ${k}`);
  });
  ok(/pairs\.push\(\['LEARN_APP_MIN'/.test(html) && /pairs\.push\(\['LEARN_APP_DAY'/.test(html),
     '保存时把两个门槛一起发出去（只发开关的话，门槛改了页面显示新值、后端还是旧的）');
  ok(/pkg_kv learned_app /.test(apiSrc),
     '导出的适配包报一句有没有按应用样本，否则用户发来的包看不出这层在不在跑');
  ok(/pkg_sec '守护运行状态（run\/status）'/.test(apiSrc) && /cat "\$STATUS"/.test(apiSrc),
     '诊断包带运行状态：偏移/温度/亮屏/充电是"为什么没采样、为什么封顶偏低"的证据');

  /* v2：24 桶、按配置帽过滤、学习偏好。这些是这版算法的核心约束，
     任何一条被改回去，建议都会回到"只降不升"或"沿着旧帽一路加码"。 */
  ok(/elif \[ "\$_k" = LEARN_MODE \]; then\s*\n\s*case "\$_v" in eco\|balanced\|performance\)/.test(apiSrc)
     && /KEYS='[^']*\bLEARN_MODE\b/.test(apiSrc),
     'LEARN_MODE 只接受三档（缺省/写错都回 balanced，不接受数字）');
  ok(/LEARN_MODE=balanced/.test(conf), 'socfreq.conf 里的学习偏好默认是均衡');
  ok(/p-learn-mode/.test(html) && /MODE_TEXT = \{ eco: '省电', balanced: '均衡', performance: '性能' \}/.test(html),
     '功能页有学习偏好选项，值用 eco/balanced/performance（和后端同一套词）');
  ok(/pairs\.push\(\['LEARN_MODE', document\.getElementById\('p-learn-mode'\)\.value \|\| 'balanced'\]\)/.test(html),
     '保存时把学习偏好一起发出去（只显示不发等于没存）');

  const adv = (apiSrc.match(/cmd_advice\(\) \{[\s\S]*?\n\}/) || [''])[0];
  ok(/printf 'learn=%s\\nmode=%s\\n'/.test(adv) && /printf 'reset=%s\\n'/.test(adv),
     'advice 回报学习开关、偏好和旧格式重置标记');
  ok(/_learn_gate\(\) \{/.test(apiSrc) && /printf 'gate=%s\\n'/.test(adv)
     && /echo screen/.test(apiSrc) && /echo chg/.test(apiSrc)
     && /echo adapt/.test(apiSrc) && /echo full/.test(apiSrc),
     'advice 回报"这一拍为什么不采"（暂停/熄屏/插电/偏移/满血），'
     + '0 天时页面才有话可说');
  ok(/const GATE_TEXT = \{/.test(html) && /GATE_TEXT\[meta\.gate\]/.test(html)
     && /还没有攒到可判断的样本/.test(html),
     '页面把 gate 翻成人话；0 天不再显示"已学到 0 天"');
  ok(/\$31 \+ 0\) != cur/.test(adv),
     '全局建议只认当前这道配置帽下采的样本（旧帽的行即使还在也不参与）');
  ok(/capv != want/.test(apiSrc) && /capbin = int\(c \* 24 \/ 100\)/.test(apiSrc),
     '按应用建议也只认当前这道帽的样本（没档位时按全局那一档的帽），贴帽两桶跟着这道帽走');
  ok(/capbin = int\(cur \* 24 \/ 100\)/.test(adv) && /bd\[t, capbin\] \+ bd\[t, capbin - 1\]/.test(adv)
     && /qval\(t, 0\.99/.test(adv) && /qval\(t, 0\.90/.test(adv),
     '贴帽两桶按当前帽所在桶算（不能取绝对 22/23：65% 帽下那两桶永远是空的），'
     + '分位数用 24 桶插值算 p90/p99');
  ok(/nearcap = \(tot > 0 && p99 >= cur - 100 \/ 24\)/.test(adv)
     && /pressure = \(pin >= 10 \|\| top2 >= 25\)/.test(adv),
     '放宽的硬条件：p99 贴帽 且（顶帽≥10% 或贴帽两桶≥25%）');
  ok(/upmin = 60/.test(adv) && /updays = \(mode == "eco" \? 3 : \(mode == "performance" \? 1 : 2\)\)/.test(adv),
     '放宽要 2 天/60 分钟（省电 3 天、性能 1 天），不是凭一拍就动');
  ok(/if \(mode == "eco" && p99 <= cur - 25 && mins >= 180 && days >= 7\)/.test(adv),
     '大核只有省电模式才下调，而且要 7 天/180 分钟、p99 离帽 25 个点');
  ok(/大核是爆发型负载，均衡\/性能模式不自动下调/.test(adv),
     '均衡/性能下大核不降时，原因要说清且指向省电模式');
  ok(/now - mtsv < 7 \* 86400/.test(adv) && /7 天内刚往/.test(adv),
     '反向改动有 7 天冷却（刚上调过不立刻建议下调，反之亦然）');
  ok(/days >= 3/.test(apiSrc) && /按应用要 3 天/.test(apiSrc),
     '按应用的放宽门槛固定 3 天（不受 LEARN_MODE 影响）');

  ok(/const ev = document\.createElement\('div'\);[\s\S]{0,120}?adv-ev/.test(html)
     && /贴帽两桶/.test(html) && /p99/.test(html),
     '建议行下面带证据行（顶帽/贴帽两桶/p99/采样），用户能自己核');
  ok(/学习偏好：.*MODE_TEXT/.test(html),
     '建议卡片显示当前学习偏好，不是只存在配置里');
}

/* ── 深度睡眠：状态机、对称还原、榜单与页面接线 ─────────────────── */
{
  const dsPath = MOD0 + '/scripts/deep_sleep.sh';
  ok(fs.existsSync(dsPath), '深度睡眠守护脚本随包存在（scripts/deep_sleep.sh）');
  const ds = fs.existsSync(dsPath) ? fs.readFileSync(dsPath, 'utf8') : '';

  /* 产品语义：熄屏 20 秒、2 秒心跳、T0 冻结、7:30 一次性 Doze。 */
  ok(/SCREEN_OFF_DELAY=20/.test(ds), '熄屏满 20 秒才进入（不是熄屏立刻）');
  ok(/POLL_INTERVAL=2/.test(ds), '心跳 2 秒（熄屏后不加密轮询）');
  ok(/DS_FREEZE_WINDOW=180/.test(ds) && /DS_DOZE_AT=450/.test(ds)
     && /DS_DOZE_RETRY=300/.test(ds),
     '冻结窗口 3 分钟，7:30 才一次性收口，闹钟让路后 5 分钟再试');
  ok(/MAX_WAKE_APPS=63/.test(ds), '唤醒榜应用数封顶 63，防止文件无限膨胀');

  /* 不再改系统 Doze 常量：进入不再靠全局门槛，旧版快照仍能按原值还原。 */
  ok(!/DS_CONSTANTS=/.test(ds) && !/ds_save_constants/.test(ds)
     && /settings delete global device_idle_constants/.test(ds),
     '不再写 device_idle_constants（旧快照还原仍能 delete/put 原值）');

  /* 冻结：三种后端、排除项、账本与分批。 */
  ok(/DS_FREEZE_ENABLED=\$\{DS_FREEZE:-1\}/.test(ds) && /ds_freeze_probe\(\)/.test(ds)
     && /\/proc\/mounts/.test(ds) && /cgroup\.freeze/.test(ds) && /kill -STOP/.test(ds),
     '冻结后端按 v1 freezer → v2 cgroup.freeze → SIGSTOP 依次降级');
  ok(/ds_freeze_build_queue\(\)[\s\S]{0,1400}pm list packages -3/.test(ds)
     && /oom_score_adj/.test(ds) && /\[ "\$adj" -ge 200 \]/.test(ds),
     '只冻第三方包、只冻缓存/后台进程（adj >= 200），前台和可见应用不动');
  ok(/ds_freeze_add_skip/.test(ds) && /default_input_method/.test(ds)
     && /enabled_accessibility_services/.test(ds) && /always_on_vpn_app/.test(ds)
     && /app_modes\.conf/.test(ds) && /com\.coloros\.\*/.test(ds)
     && /alarmclock/.test(ds),
     '输入法/无障碍/VPN/桌面/当前前台/满血或 keep/厂商应用不冻（含 ColorOS 时钟）');
  {
    const code = ds.replace(/^[ \t]*#.*$/gm, '');
    ok(/IFS='\|' read -r pid st pkg/.test(code) && !/\$\{line%%\|\*\}/.test(code),
       '冻结队列用 IFS 切字段：mksh 把 | 当 ksh 模式的“或”，${x%%|*} 切不出 pid');
  }
  ok(/ds_freeze_ledger_save/.test(ds) && /ds_freeze_restore_if_any/.test(ds)
     && /ds_freeze_thaw_all/.test(ds) && /freeze\.saved/.test(ds),
     '冻结账本落盘，亮屏/关开关/启动/卸载都按账本解冻');
  ok(/DS_FREEZE_BATCH=8/.test(ds) && /ds_freeze_thaw_pkg/.test(ds),
     '冻结分批进行，媒体/新豁免应用能从组里单独解冻');
  ok(/ds_doze_once/.test(ds) && /cmd deviceidle force-idle deep/.test(ds)
     && /dumpsys deviceidle force-idle deep/.test(ds) && /本次不再重试/.test(ds),
     '7:30 一次性 Doze：cmd 优先、dumpsys 兜底、失败不重试');
  ok(/DS_DOZE_RETRY[\s\S]{0,800}ds_alarm_soon[\s\S]{0,200}暂不进入系统 Doze/.test(ds),
     'Doze 收口同样让路闹钟');
  /* 熄屏巡检不能把 system_server 当轮询接口。 */
  {
    const calls = ds.split('\n').filter(l => /(^|[^#\w])dumpsys /.test(l) && !/^\s*#/.test(l));
    ok(calls.length > 0 && calls.every(l => /timeout \d+ dumpsys /.test(l)),
       '每个 dumpsys 都套了 timeout：一次卡住不能把整轮巡检挂死、深睡再也退不出来');
  }
  {
    const code = ds.split('\n').filter(l => !/^\s*#/.test(l)).join('\n');
    ok(code.indexOf('dumpsys display') < 0, '不再调 dumpsys display（熄屏时最贵的一坨）');
  }
  ok(/DS_BL=\$\{DS_BL:-\/sys\/class\/backlight\}/.test(ds)
     && /ds_screen_on\(\)[\s\S]{0,900}ds_screen_node/.test(ds)
     && /ds_dumpsys_awake\(\)[\s\S]{0,220}timeout 2 dumpsys power/.test(ds),
     '亮屏主判据是背光节点，dumpsys power 只做复核和兜底');
  ok(/WAKE_SAMPLE_INTERVAL=\$\{DS_WAKE_INTERVAL:-60\}/.test(ds)
     && /MEDIA_CHECK_INTERVAL=\$\{DS_MEDIA_INTERVAL:-30\}/.test(ds)
     && /t - ds_last_wake/.test(ds) && /t - ds_last_media/.test(ds),
     '深睡期间唤醒数 60 秒、媒体 30 秒才读一次 dumpsys（累计计数器不会丢增量）');
  ok(/DS_STATUS=\$\{DS_STATUS:-\$CONF_DIR\/run\/status\}/.test(ds)
     && /ds_game_active\(\)[\s\S]{0,700}game_pkg[\s\S]{0,300}fg/.test(ds)
     && /if ds_game_active; then[\s\S]{0,400}暂不进入深睡/.test(ds),
     '游戏还在前台（熄屏挂机）时不 force-idle：Doze 会挂住游戏，系统也会把这段记成待机');
  ok(/dumpsys deviceidle force-idle deep/.test(ds) && /dumpsys deviceidle unforce/.test(ds),
     '强制深睡与解除强制成对出现');
  {
    const enter = (ds.match(/ds_enter\(\) \{[\s\S]*?\n\}/) || [''])[0];
    ok(enter.indexOf('ds_snap_write') > 0
       && enter.indexOf('ds_snap_write') < enter.indexOf('ds_freeze_build_queue')
       && enter.indexOf('force-idle deep') < 0,
       '先写快照再加冻：刚冻完就被杀时有还原依据；进入时不再 force-idle');
  }
  ok(/rm -f "\$DS_SNAP"/.test(ds) && /ds_snap_restore_if_any/.test(ds),
     '快照在正常退出时删除，启动时发现残留就按存档还原');

  /* 白名单只碰自己加的；用户名单退出后保留。 */
  ok(/ds_white_snapshot/.test(ds)
     && /case ",\$old," in \*",\$pkg,"\*\) return 0/.test(ds),
     '加白前先快照系统已有条目，已经在的不重复加、也不会在退出时误删');
  ok(/case ",\$DS_WHITE," in \*",\$pkg,"\*\) return 0/.test(ds),
     '退出只撤临时加白，用户白名单里的条目永久保留');
  ok(/ds_sms_pkg\(\)/.test(ds) && /android\.app\.role\.SMS/.test(ds)
     && /sms="\$\(ds_sms_pkg\)"/.test(ds) && /短信应用 \$sms 已临时加白/.test(ds),
     '默认短信应用进入时临时加白（SMS 广播不被 Doze 压到解锁后才发）');
  ok(/com\.hihonor\.mms/.test(fs.readFileSync(MOD0 + '/scripts/builtin_white.txt', 'utf8'))
     && /com\.hihonor\.contacts/.test(fs.readFileSync(MOD0 + '/scripts/builtin_white.txt', 'utf8')),
     '内置豁免包含荣耀信息/联系人等厂商包（不只 AOSP 包名）');
  ok(/reset_all\)\s+cmd_reset_all ;;/.test(apiSrc) && /cmd_reset_all\(\) \{/.test(apiSrc)
     && /_def_conf="\$DATA\/defaults\/socfreq.conf"/.test(apiSrc)
     && /cp -f "\$_def_conf" "\$CONF\.tmp"/.test(apiSrc)
     && /cp -f "\$_def_modes" "\$MODES\.tmp"/.test(apiSrc)
     && /rm -f "\$LEARN" "\$LEARN_APP"/.test(apiSrc),
     '恢复默认：后端重置配置/档位/深睡/覆盖/学习样本，游戏名单保留');
  ok(/id="reset-all"/.test(html) && /function resetAll\(\)/.test(html)
     && /await api\('reset_all'\)/.test(html),
     '关于页有「恢复全部默认」按钮，连点两次确认');

  /* 媒体保护与唤醒统计。 */
  ok(/dumpsys media_session/.test(ds) && /\{state=3/.test(ds) && /package=/.test(ds),
     '媒体保护同时认 PlaybackState、state=3、package= 三个锚点');
  ok(/Top Alarms/.test(ds) && /ds_wakes_delta/.test(ds) && /d>=1/.test(ds),
     '唤醒榜按增量入账，负增量（计数器重置）丢弃');
  ok(ds.indexOf('/^[[:space:]]*Top Alarms:/') >= 0
     && ds.indexOf('/^[[:space:]]*Alarm Stats/') >= 0,
     'Top Alarms / Alarm Stats 段头允许前导空格（ColorOS 实际输出是「  Top Alarms:」）');
  ok(/ENVIRON\["DS_WAKES_BASE"\]/.test(ds) && /ENVIRON\["DS_BOARD_OLD"\]/.test(ds),
     '多行榜单走 ENVIRON：toybox awk 用 -v 传换行会报 newline in string');
  ok(/ds_quit=1/.test(ds) && /\[ "\$ds_quit" = "1" \] && ds_shutdown/.test(ds),
     '关掉总开关后跑完最后一轮就退出，不留下每 2 秒空转的进程');
  ok(/ds_wakes_reset_on_unplug/.test(ds) && /ds_was_charging/.test(ds),
     '拔电检测放在每轮 tick，充电时拔线也能重置累计榜');
  ok(/dsw=\$dsw\n/.test(ds) && /dswc=\$dswc/.test(ds) && /wcsince=\$ds_wcsince/.test(ds)
     && /dsactive=\$ds_active/.test(ds),
     'state 输出 dsw/dswc/wcsince/dsactive 四个键');
  ok(/SOCFREQ_DATA_DIR/.test(ds), '数据目录可由环境变量覆盖（台架与其他管理器装机）');
  ok(!/若晴|elite_freq_cap/.test(ds), '脚本里不出现其他模块的标识');

  /* 开机看护与卸载还原。 */
  const svc = fs.readFileSync(MOD0 + '/service.sh', 'utf8');
  ok(/DS_SH="\$MODDIR\/scripts\/deep_sleep\.sh"/.test(svc)
     && /ds_alive\(\)/.test(svc) && /ds_start/.test(svc),
     'service.sh 认得到深睡脚本，并按 pid + cmdline 看护');
  ok(/tr '\\0' ' ' < "\/proc\/\$_p\/cmdline"[\s\S]{0,80}deep_sleep/.test(svc),
     '看护用 /proc/<pid>/cmdline 认人：kill -9 后 pid 文件残留也不会误判活着');
  ok(/\[ ! -f "\$PAUSE" \] && ds_start/.test(svc),
     '暂停期间看护不重新拉起深睡守护');
  const un = fs.readFileSync(MOD0 + '/uninstall.sh', 'utf8');
  ok(/deep_sleep\.sh/.test(un) && /ds_snap_restore_if_any/.test(un),
     '卸载先停深睡，再按快照兜底还原（常量/白名单不留残渣）');

  /* api.sh：配置默认关、原子写、命令齐全、暂停联动。 */
  ok(/DS_CONF="\$DATA\/dsleep\.conf"/.test(apiSrc)
     && /DS_STATE="\$DATA\/ds_state"/.test(apiSrc),
     '深睡配置和 state 都在数据目录，升级模块不会覆盖');
  ok(/_ds_conf_init\(\)[\s\S]*?enable=0/.test(apiSrc),
     '首次使用写默认配置，默认是关的（不替用户改系统 Doze）');
  ok(/ds_get\)\s+cmd_ds_get/.test(apiSrc) && /ds_state\)\s+cmd_ds_state/.test(apiSrc)
     && /ds_set\)\s+cmd_ds_set/.test(apiSrc)
     && /ds_white_add\)\s+cmd_ds_white_add/.test(apiSrc)
     && /ds_white_del\)\s+cmd_ds_white_del/.test(apiSrc),
     '五个深睡子命令都接进分发');
  ok(/cmd_pause\(\)\s*\{[\s\S]{0,80}_ds_stop/.test(apiSrc),
     '模块暂停时先退出深睡并还原，不能把系统留在 Doze 里');
  ok(/_ds_stop\(\)[\s\S]*?ds_snap_restore_if_any/.test(apiSrc),
     '停止用 TERM；进程没来得及退，按快照兜底');
  ok(/_ds_start\(\)[\s\S]*?_ds_enable.*= "1"/.test(apiSrc),
     '总开关关着时不常驻深睡进程');

  /* 页面：导航、开关、双榜、2 秒轮询只在本页可见时跑。 */
  ok(/data-p="sleep"[\s\S]{0,260}>睡眠</.test(html), '导航栏有「睡眠」一项');
  ok(/id="p-sleep"/.test(html) && /id="ds_enable"/.test(html),
     '睡眠页和总开关都在');
  ok(/id="ds-white"/.test(html) && /id="ds-run"/.test(html) && /id="ds-cur"/.test(html),
     '永久豁免名单 + 当次/累计两张榜都在页面上');
  ok(/function parseDs\(/.test(html) && /function dsBoard\(/.test(html),
     '前端解析 dsw/dswc 的 pkg:次数:白名单 格式');
  ok(/api\(full \? 'ds_get' : 'ds_state'\)/.test(html),
     '首次带白名单用 ds_get，轮询只取状态与榜单用 ds_state');
  ok(/sleepVisible\(\)\) loadDs\(false\)/.test(html),
     '2 秒轮询只在睡眠页可见时发 root 调用');
  ok(/paused=%s/.test(apiSrc) && /DS\.paused/.test(html),
     '模块暂停时页面说明「模块已暂停」，不误报成守护挂了');
  ok(/function setDsEnable[\s\S]{0,300}api\('ds_set', 'enable'/.test(html),
     '开关走 ds_set enable，不是直接改文件');
  /* 启停守护要 1~2 秒：开关必须先把状态画出来再等回执，
     旧轮询也要被 pending 挡住，否则会先把开关弹回去再改回来。 */
  ok(/DS\.pending = on \? 1 : -1/.test(html) && /if\(DS\.pending\)/.test(html)
     && /if\(my !== dsSeq \|\| DS\.pending\) return/.test(html),
     '开关乐观更新：先画目标态，期间丢弃旧轮询结果');
  ok(!/box\.disabled = true/.test(html),
     '不再把开关 disabled 掉（等回执时看着像没点动）');
  ok(/function addDsWhite[\s\S]{0,200}api\('ds_white_add'/.test(html)
     && /function delDsWhite[\s\S]{0,200}api\('ds_white_del'/.test(html),
     '豁免名单的增删都有后端命令');
  ok(/paintDsWhite[\s\S]{0,600}pkgRow\(p\)/.test(html)
     && /paintDsWhite[\s\S]{0,900}ensureLabels\(list\)/.test(html),
     '豁免名单复用两行式应用行，并点名取应用名');
  ok(/n\.textContent = \(LABELS\[it\.pkg\][\s\S]{0,140}FALLBACK_LABELS\[it\.pkg\]\)\) \|\| it\.pkg;/.test(html),
     '榜单应用名用 textContent 写，包名不会被当成 HTML');
  ok(/n\.dataset\.pkg = it\.pkg/.test(html)
     && /querySelectorAll\('\.ds-name\[data-pkg\]'\)/.test(html)
     && /el\.textContent = v;[\s\S]{0,60}appendChild\(wl\)/.test(html),
     '应用名异步回来后唤醒榜也跟着刷新（白名单小标要保留）');

  /* ── 深色模式 / 滑杆进度 ─────────────────────────────────────── */
  ok(/<meta name="color-scheme" content="light dark">/.test(html),
     'color-scheme 同时声明 light dark，原生控件跟着主题走');
  ok(/:root\[data-theme="dark"\]\{[\s\S]*?--bg:#22262e[\s\S]*?--fg:#dfe4ed/.test(html),
     '深色变量挂在 :root[data-theme="dark"]，不是把浅色整体调暗');
  ok(/html:not\(\[data-theme\]\) body\{visibility:hidden/.test(html),
     '主题定下来前先藏住 body，避免先用浅色画一遍再跳深色');
  ok(/function readTheme\(\)/.test(html) && /prefers-color-scheme/.test(html)
     && /localStorage\.getItem\(THEME_KEY\)/.test(html),
     '主题先读本地保存，再退回系统偏好');
  ok(/id="th-l"/.test(html) && /id="th-d"/.test(html)
     && /function setTheme\(mode\)/.test(html),
     '浅色 / 深色两个开关都在，点击会存主题');
  ok(/function paintSlider\(el\)/.test(html) && /setProperty\('--p'/.test(html)
     && /function syncSliders\(\)/.test(html) && /syncSliders\(\);/.test(html),
     '滑杆把进度写进 --p，并在启动/载入配置后补画');
  ok(/input\[type=text\],input\[type=number\]/.test(html),
     'number 输入框也套深色样式（否则是浏览器默认白底）');
  ok(/calc\(104px \+ env\(safe-area-inset-bottom\)\)/.test(html)
     && /calc\(108px \+ env\(safe-area-inset-bottom\)\)/.test(html),
     '底部留白和 toast 都含安全区，不被手势条压住');

  /* ── GPU 草稿预览：偏移 + 真实频点表 + 最低频点回落 ─────────── */
  ok(/printf\("gpu_dlt=%d\\n"/.test(csrc) && /printf\("gpufreq="/.test(csrc),
     'export 带出实时偏移和 GPU 频点表（前端不能自己猜吸附结果）');
  ok(!/cur \+ gdlt/.test(html) && /Math\.floor\(gmax \* cur \/ 100\)/.test(html)
     && /s\.gpufreq/.test(html),
     '适配表显示"设定值吸附后的频点"，不叠加实时偏移（实际值在 GPU 封顶卡片）');
  ok(/Math\.min\.apply\(null, fs\)/.test(html),
     '目标低于最低频点时回落到最低频点（和守护 gpu_snap 同口径）');
  ok(/只影响当前生效值/.test(html) && /不改这里显示的设定值/.test(html),
     '页头说明实时偏移只改当前生效值，不改适配表里的设定值');

  /* 版本：四处一起改，页面兜底值也要同步。 */
  const mp = fs.readFileSync(MOD0 + '/module.prop', 'utf8');
  ok(/^version=V1.6$/m.test(mp) && /^versionCode=10600$/m.test(mp),
     'module.prop 是 V1.6 / 10600');
  ok(/SF_VERSION "1\.6"/.test(csrc), '守护源码里的版本号也是 1.6');
  ok(/id="ver">V1\.6</.test(html) && /id="ver2">V1\.6</.test(html),
     '页面两个版本兜底值都是 V1.6');
  ok(/版本：V1\.6/.test(fs.readFileSync(MOD0 + '/README.md', 'utf8')),
     'README 标题行同步到 V1.6');
}

console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
process.exit(fail ? 1 : 0);
