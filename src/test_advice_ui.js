// 智能建议卡片的渲染测试：把 index.html 里那段建议代码抠出来，
// 喂**真的 api.sh 输出**（不是手抄的字符串），看它渲染成什么、点下去发几条 conf_set。
//
// 为什么值得单独一份：判据在后端、渲染在前端，中间只靠
// "---REC---" 和几个字段名对接。字段名改了后端不会报错，页面只会显示空白，
// 而这种"绿着的坏"是静态断言抓不住的。
//
// 用法： node src/test_advice_ui.js webroot/index.html
// 没有 sh（或 sh 跑不动 api.sh）时整个文件跳过并说明原因，不算绿。
const fs = require('fs');
const os = require('os');
const path = require('path');
const vm = require('vm');
const { execFileSync } = require('child_process');

const htmlPath = process.argv[2] || 'webroot/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');
const modRoot = path.dirname(path.dirname(path.resolve(htmlPath)));

let pass = 0, fail = 0, skipped = '';
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

/* ── 造一份真数据，跑真 api.sh ─────────────────────────────── */
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'advui_'));
fs.mkdirSync(path.join(dir, 'webroot'));
fs.copyFileSync(path.join(modRoot, 'webroot', 'api.sh'), path.join(dir, 'webroot', 'api.sh'));
const confPath = path.join(dir, 'socfreq.conf');
fs.copyFileSync(path.join(modRoot, 'socfreq.conf'), confPath);

function A() {
  return execFileSync('sh', [path.join(dir, 'webroot', 'api.sh')].concat(Array.from(arguments)),
                      { encoding: 'utf8',
                        env: Object.assign({}, process.env, { SOCFREQ_DATA_DIR: dir }) });
}
function setConf(pairs) {
  let s = fs.readFileSync(confPath, 'utf8');
  pairs.forEach(p => { s = s.replace(new RegExp('^' + p[0] + '=.*$', 'm'), p[0] + '=' + p[1]); });
  fs.writeFileSync(confPath, s);
}
// 字段顺序和 src/learn.c 的 learn_write 一致：
//   目录/包名 档 采样ms 顶帽ms 被压ms b0..b23 天位图 配置帽（共 31 列）
// 桶默认全 0，只写用例关心的；手工数 24 个 0 迟早错位。
function lrow(id, tier, sm, pin, held, days, cap, buckets) {
  const bins = new Array(24).fill(0);
  Object.keys(buckets || {}).forEach(i => { bins[Number(i)] = buckets[i]; });
  return [id, tier, sm, pin, held].concat(bins, [days, cap]).join(' ');
}
const LEARN4 = [
  lrow('/sys/devices/system/cpu/cpufreq/policy0', 0, 3600000, 40000, 0, 7, 65, { 14: 10000, 15: 60000 }),
  lrow('/sys/devices/system/cpu/cpufreq/policy1', 1, 3600000, 30000, 0, 7, 65, { 15: 30000 }),
  lrow('/sys/devices/system/cpu/cpufreq/policy2', 2, 1800000, 90000, 0, 7, 100, { 23: 90000 }),
  lrow('/sys/devices/system/cpu/cpufreq/policy3', 3, 7200000, 0, 0, 7, 62,
       { 1: 10000, 2: 20000, 3: 20000, 4: 50000 }),
  // GPU 当第 5 档：驻留来自 KGSL 的 gpu_clock_stats
  lrow('/sys/class/kgsl/kgsl-3d0', 4, 7200000, 0, 0, 7, 45, { 0: 900000 }),
].join('\n') + '\n';

let full = '', off = '', empty = '', resetOut = '';
let appFull = '', appOff = '', appEmpty = '';
try {
  fs.rmSync(path.join(dir, 'learn.tsv'), { force: true });
  // 旧格式被守护清掉时留的标记：页面要说明"正在重新积累"，不能只说没样本
  fs.writeFileSync(path.join(dir, 'learn.tsv.v2reset'), '');
  resetOut = A('advice');
  fs.rmSync(path.join(dir, 'learn.tsv.v2reset'), { force: true });
  empty = A('advice');
  setConf([['CPU_PCT_BIG', 65], ['CPU_PCT_MID2', 65], ['CPU_PCT_MID', 100],
           ['CPU_PCT_LITTLE', 62], ['GPU_PCT', 45], ['LEARN', 1]]);
  fs.writeFileSync(path.join(dir, 'learn.tsv'), LEARN4);
  full = A('advice');
  setConf([['LEARN', 0]]);
  off = A('advice');
  setConf([['LEARN', 1]]);

  /* 按应用那一层：同样喂真 api.sh，字段顺序同 src/learn.c 的 learn_app 行
   *   包名 档 采样ms 顶帽ms 被压ms b0..b23 天位图 配置帽
   * 三个包分别该放宽（学习条目）、该收紧（学习条目）、该放宽但是用户手填的。 */
  fs.rmSync(path.join(dir, 'learn_app.tsv'), { force: true });
  fs.rmSync(path.join(dir, 'app_modes.conf'), { force: true });
  // 先开到"按应用学习"再取空样本那份：出厂 conf 里这个开关是关的，
  // 不先开就会把"开关关着"的输出当成"没有样本"来断言，两条说明长得像但原因不同。
  setConf([['LEARN_APP', 1]]);
  appEmpty = A('advice_app');
  setConf([['LEARN_APP', 0]]);
  appOff = A('advice_app');
  /* 第二段的 fixture 用一道整齐的全局帽（四簇都 60），
     这样"没设档位的包按全局平均当基准"才有确定的数字可断言。 */
  setConf([['CPU_PCT_BIG', 60], ['CPU_PCT_MID2', 60], ['CPU_PCT_MID', 60],
           ['CPU_PCT_LITTLE', 60], ['LEARN_APP', 1]]);
  fs.writeFileSync(path.join(dir, 'app_modes.conf'),
    'com.miHoYo.Yuanshen=65  #learn\n'
    + 'com.netease.eggparty=80\n'
    + 'com.android.camera=60  #learn\n');
  fs.writeFileSync(path.join(dir, 'learn_app.tsv'), [
    lrow('com.miHoYo.Yuanshen', 0, 4800000, 600, 0, 7, 65, { 15: 1200 }),
    lrow('com.netease.eggparty', 0, 3600000, 600, 0, 7, 80, { 19: 1200 }),
    lrow('com.android.camera', 0, 2400000, 0, 0, 3, 60, { 0: 1200 }),
    // 没有档位的包：归到全局帽下，基准取四簇平均 60
    lrow('com.tencent.mm', 0, 3000000, 600, 0, 7, 60, { 14: 1200 }),
  ].join('\n') + '\n');
  appFull = A('advice_app');
} catch (e) {
  skipped = '跑不了 api.sh（这台机没 sh，或 sh 不支持这些用法）：' + e.message;
}

/* ── 抠前端代码 ───────────────────────────────────────────── */
const MARK_GAME = '/* ── 游戏名单 ── */';
const chunkKeys = html.match(/const PCT_KEYS = \[[\s\S]*?\];/);
const chunkParse = html.match(/function parse\(s\)\{[\s\S]*?\n\}/);
const chunkAdv = html.match(/\/\* ── 智能建议 ── \*\/[\s\S]*?\/\* ── 游戏名单 ── \*\//);

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], style: {}, className: '', disabled: false,
    value: '', checked: false, textContent: '', options: [],
    appendChild(c) { el.children.push(c); return c; },
    classList: { add() {}, remove() {}, contains() { return false; } },
  };
  // 页面清空容器用的是 innerHTML = ''，这里要跟着把 children 一起清掉
  Object.defineProperty(el, 'innerHTML', {
    get() { return ''; },
    set(v) { if (v === '') el.children = []; },
  });
  return el;
}
function textOf(el) {
  let t = el.textContent || '';
  el.children.forEach(c => { t += ' ' + textOf(c); });
  return t;
}

/* 每条用例一个干净沙箱：api() 直接把 fixture 当后端返回。
 * 按应用那一层的 fixture 是真跑出来的 advice_app 输出，两个子命令各给各的。 */
function sandbox(text, textApp) {
  const nodes = {};
  const get = id => nodes[id] || (nodes[id] = mkEl('div'));
  const calls = [];
  const ctx = {
    document: { getElementById: get, createElement: mkEl, querySelectorAll: () => [] },
    api: function () {
      const a = Array.from(arguments);
      calls.push(a);
      if (a[0] === 'advice') return Promise.resolve(text);
      if (a[0] === 'advice_app') return Promise.resolve(textApp || '');
      return Promise.resolve('OK');
    },
    toast: m => calls.push(['toast', m]),
    pv: k => calls.push(['pv', k]),
    loadStatus: () => calls.push(['loadStatus']),
    /* 按应用的行复用应用页那套图标/名字行，这里给个最小替身：
       要验的是"值、来源、点下去发什么"，不是图标怎么取。 */
    LABELS: { 'com.miHoYo.Yuanshen': '原神' },
    ensureLabels: () => {},
    pkgRow: p => {
      const el = mkEl('div');
      el.className = 'pk';
      el.dataset = { pkg: p };
      const t = mkEl('b'), s = mkEl('span');
      // 真实现把名字/包名写进这两个子节点，替身不写就没法断言"这行是哪个包"
      t.textContent = ctx.LABELS[p] || p;
      s.textContent = p;
      el.appendChild(t); el.appendChild(s);
      return { el, ph: mkEl('div'), t, s };
    },
    Date: { now: () => 1000 },
    setTimeout: () => 0,
    Promise: Promise, Number: Number, String: String, Object: Object, Array: Array,
    console: console,
  };
  vm.createContext(ctx);
  // 抠到的块尾带着下一节的标题注释，去掉它只是为了出错时行号好读
  vm.runInContext(chunkKeys[0] + '\n' + chunkParse[0] + '\n'
    + chunkAdv[0].replace(MARK_GAME, ''), ctx, { filename: 'ui-advice.js' });
  return { ctx, get, calls };
}

function finish() {
  fs.rmSync(dir, { recursive: true, force: true });
  console.log('');
  if (skipped) { console.log('跳过（原因见上）'); process.exit(0); }
  if (fail === 0) console.log('全部通过 (' + pass + ')');
  else console.log(fail + ' 项失败 / ' + pass + ' 项通过');
  process.exit(fail);
}

async function main() {
  if (skipped) {
    console.log('== advice 卡片渲染（跳过）==');
    console.log('  skip ' + skipped);
    return finish();
  }
  console.log('== advice 卡片渲染（数据来自真 api.sh）==');
  if (!chunkKeys || !chunkParse || !chunkAdv) {
    ok(false, '从 index.html 里抠出 PCT_KEYS/parse 和建议代码块',
       'keys=' + !!chunkKeys + ' parse=' + !!chunkParse + ' advice=' + !!chunkAdv);
    return finish();
  }
  ok(/---REC---/.test(full) && /advice=up/.test(full) && /advice=down/.test(full),
     'api.sh 这一趟确实给出了 up / down / hold 三种记录（用例有东西可渲染）');

  let s = sandbox(full);
  await s.ctx.loadAdvice();
  const box = s.get('advice');
  // 每条建议渲染成一个 wrap（行 + 证据行），说明文字是单独的 hint
  const wraps = box.children.filter(c => c.children[0] && c.children[0].className === 'row');
  const rows = wraps.map(c => c.children[0]);
  ok(rows.length === 5, '四档 CPU + GPU 渲染成五行（不是整块空白）', '实际 ' + rows.length);
  ok(textOf(box).indexOf('学习偏好：均衡') >= 0, '卡片标出当前学习偏好（eco/balanced/performance）');
  ok(rows[4].children[0].children[0].textContent === 'GPU',
     'GPU 那行的标题是 GPU，不是裸键名 GPU_PCT', rows[4].children[0].children[0].textContent);
  ok(textOf(rows[0]).indexOf('超大核 (BIG)') >= 0, '行标题用页面自己的档位名，不是后端裸键名');
  ok(textOf(rows[0]).indexOf('65% → 70%') >= 0, '放宽那行显示 当前→建议', textOf(rows[0]));
  ok(textOf(rows[2]).indexOf('100%') >= 0 && textOf(rows[2]).indexOf('→') < 0,
     '维持那行只显示当前值、不带箭头');
  ok(rows.every(x => textOf(x).indexOf('顶帽') >= 0 || textOf(x).indexOf('100%') >= 0),
     '每行都带依据文字');
  ok(textOf(wraps[0]).indexOf('顶帽 57% · 贴帽两桶 100% · p99 67% · 采样 60 分钟 / 3 天') >= 0,
     '每行下面带证据行（顶帽/贴帽两桶/p99/采样）', textOf(wraps[0]));
  ok(s.get('adv-apply').disabled === false, '有可应用的建议时按钮是亮的');

  // 点「应用建议」：只该发 up/down 那几条，hold 那条不许动配置
  // （先 loadAdvice：ADV 是它填的，没取过建议时按钮本来就不该发任何写）
  s = sandbox(full);
  await s.ctx.loadAdvice();
  await s.ctx.applyAdvice();
  const sets = s.calls.filter(c => c[0] === 'conf_set');
  const m = {};
  sets.forEach(c => { m[c[1]] = c[2]; });
  ok(sets.length === 4, '应用建议发了 4 条 conf_set（两条放宽、CPU/GPU 各一条收紧）', '实际 ' + sets.length);
  ok(m.CPU_PCT_BIG === '70' && m.CPU_PCT_MID2 === '70' && m.CPU_PCT_LITTLE === '57',
     '写回去的就是建议值', JSON.stringify(m));
  ok(!('CPU_PCT_MID' in m), '维持那一档没被写（advice=hold 不参与应用）');
  ok(m.GPU_PCT === '40', 'GPU 的建议写回的是 GPU_PCT 这个 conf 键', m.GPU_PCT);
  ok(s.get('gpu_pct').value === '40', 'GPU 滑杆（id 是 gpu_pct，和键名不同）也跟着刷了');
  ok(s.get('CPU_PCT_BIG').value === '70', '滑杆跟着建议值走了');

  // 学习关闭：只说明原因，一行建议都不给，按钮必须是灰的
  s = sandbox(off);
  await s.ctx.loadAdvice();
  const box2 = s.get('advice');
  ok(box2.children.length === 1, '关闭学习时只有一行说明', '实际 ' + box2.children.length);
  ok(textOf(box2).indexOf('智能学习是关的') >= 0, '说明文字提到的是那个开关', textOf(box2));
  ok(s.get('adv-apply').disabled === true, '关闭时「应用建议」是灰的');

  // 还没有样本：后端那句 note 直接显示
  s = sandbox(empty);
  await s.ctx.loadAdvice();
  ok(textOf(s.get('advice')).indexOf('还没采到样本') >= 0,
     '没样本时显示后端给的原因', textOf(s.get('advice')));
  ok(s.get('adv-apply').disabled === true, '没样本时按钮是灰的');

  // 旧格式被清空：显示"已升级、重新积累"，不是"没有样本"
  ok(/^reset=1$/m.test(resetOut), '后端在旧格式清理后报 reset=1');
  s = sandbox(resetOut);
  await s.ctx.loadAdvice();
  ok(textOf(s.get('advice')).indexOf('学习算法已升级') >= 0
     && textOf(s.get('advice')).indexOf('旧样本已清空') >= 0,
     '清空旧样本时页面说明在重新积累', textOf(s.get('advice')).trim().slice(0, 80));

  // "已学到 0 天"是一分钟都没采到：要把"在等什么"写出来
  ok(/^gate=ok$/m.test(full), '真实后端给出了 gate=ok');
  const waiting = 'learn=1\nmode=balanced\ngate=chg\nrows=1\n'
    + '---REC---\ntier=4\nkey=GPU_PCT\ncur=80\nadvice=hold\nto=80\n'
    + 'pin=0\nheld=0\np90=0\np99=0\ntop2=0\nmin=0\ndays=0\n'
    + 'why=还没累计到驻留样本（墙钟 0 分钟），先继续用\n';
  s = sandbox(waiting);
  await s.ctx.loadAdvice();
  ok(textOf(s.get('advice')).indexOf('插电不封顶') >= 0
     && textOf(s.get('advice')).indexOf('拔电') >= 0,
     '0 天时点名"插电不封顶在生效"，并给出可操作的两条路', textOf(s.get('advice')).slice(0, 90));
  ok(textOf(s.get('advice')).indexOf('还没有攒到可判断的样本') >= 0
     && textOf(s.get('advice')).indexOf('已学到 0 天') < 0,
     '0 天时不再说"已学到 0 天"', textOf(s.get('advice')).slice(-40));

  // 取建议失败（桥接挂了）也不能把整页带崩
  s = sandbox(full);
  s.ctx.api = () => Promise.reject(new Error('NO_BIN'));
  await s.ctx.loadAdvice();
  ok(textOf(s.get('advice')).indexOf('取建议失败') >= 0, '后端报错时卡片自己降级成一行说明');

  // 清零要两次点击：第一次只是把按钮变成问句
  s = sandbox(full);
  s.ctx.resetLearn();
  ok(s.calls.filter(c => c[0] === 'learn_reset').length === 0, '第一次点「清零重学」不真删');
  ok(s.get('adv-reset').textContent === '再点一次确认清零',
     '第一次点把按钮变成确认', s.get('adv-reset').textContent);
  s.ctx.resetLearn();
  ok(s.calls.filter(c => c[0] === 'learn_reset').length === 1, '第二次点才真删');

  /* ── 按应用那一层 ──────────────────────────────────────── */
  console.log('');
  if (!/---REC---/.test(appFull)) {
    ok(false, 'advice_app 这一趟给出了记录', '实际输出: ' + appFull.replace(/\n/g, ' | ').slice(0, 160));
  } else {
    ok(/advice=up/.test(appFull) && /advice=down/.test(appFull) && /manual=1/.test(appFull)
       && /base=global/.test(appFull),
       '后端这一趟四种情形都有：放宽、收紧、手填过、没档位按全局基准');

    let s2 = sandbox(full, appFull);
    await s2.ctx.refreshAdvice();
    const abox = s2.get('advice-app');
    const arows = abox.children.filter(c => c.className !== 'hint');
    ok(arows.length === 4, '按应用渲染出四行（不是整块空白）', '实际 ' + arows.length);
    ok(textOf(arows[0]).indexOf('com.miHoYo.Yuanshen') >= 0
       && textOf(arows[1]).indexOf('com.netease.eggparty') >= 0,
       '用得最多的排前面（按这个包自己的前台时长，不是按名字）',
       textOf(arows[0]).trim().slice(0, 40) + ' / ' + textOf(arows[1]).trim().slice(0, 40));
    ok(textOf(arows[0]).indexOf('65% → 70%') >= 0, '按应用那行显示 当前→建议');
    ok(textOf(arows[0]).indexOf('顶帽 50% · 贴帽两桶 100% · p99 67% · 前台 80 分钟 / 3 天') >= 0,
       '按应用那行也有证据行（顶帽/贴帽两桶/p99/前台时长）', textOf(arows[0]).trim().slice(-70));
    ok(textOf(arows[0]).indexOf('手填') < 0 && textOf(arows[1]).indexOf('手填') >= 0,
       '只有手填过的那条挂「手填」标');
    ok(textOf(arows[2]).indexOf('com.tencent.mm') >= 0
       && textOf(arows[2]).indexOf('60% → 65%') >= 0,
       '没设档位的包按全局平均 60 出建议', textOf(arows[2]).trim());
    ok(textOf(arows[2]).indexOf('全局') >= 0,
       '没设档位的行挂「全局」标，点明 cur 不是已有的专属档位');
    ok(textOf(arows[3]).indexOf('60% → 55%') >= 0, '收紧那条也是同一个箭头写法');

    // 点「应用建议」：学习条目和没档位的包都走 modes_learn，手填的那条一个字都不发
    s2 = sandbox(full, appFull);
    await s2.ctx.refreshAdvice();
    await s2.ctx.applyAdvice();
    const learn = s2.calls.filter(c => c[0] === 'modes_learn');
    ok(learn.length === 3, '发了三条 modes_learn（手填的那条不该动）', '实际 ' + learn.length);
    const lm = {};
    learn.forEach(c => { lm[c[1]] = c[2]; });
    ok(lm['com.miHoYo.Yuanshen'] === '70' && lm['com.android.camera'] === '55'
       && lm['com.tencent.mm'] === '65',
       '写回去的就是建议值（没档位的包在这里新建一条）', JSON.stringify(lm));
    ok(!('com.netease.eggparty' in lm), '手填过的那个包没被学习改（哪怕它自己也该放宽）');
    ok(s2.calls.filter(c => c[0] === 'conf_set' && c[1] === 'CPU_PCT_BIG').length === 1,
       '同一个按钮也照样应用全局那几条（两层一次点完）');

    // 开关关着：一句说明，没有行，按钮只由全局那层决定
    s2 = sandbox(full, appOff);
    await s2.ctx.refreshAdvice();
    const off2 = s2.get('advice-app').children.filter(c => c.className !== 'hint');
    ok(off2.length === 0, '按应用学习关着时不渲染任何行', '实际 ' + off2.length);
    ok(textOf(s2.get('advice-app')).indexOf('按应用学习') >= 0,
       '关着时说的是那个开关，不是"没有数据"');

    // 没样本：显示后端的原因
    s2 = sandbox(full, appEmpty);
    await s2.ctx.refreshAdvice();
    ok(textOf(s2.get('advice-app')).indexOf('还没有') >= 0
       || textOf(s2.get('advice-app')).indexOf('没攒到') >= 0,
       '没样本时显示后端给的原因', textOf(s2.get('advice-app')).trim().slice(0, 60));
  }

  /* splitMode：列表里 "pkg=75 learn" 的词尾必须在渲染前剥掉 */
  {
    const chunkSplit = html.match(/function splitMode\(r\)\{[\s\S]*?\n\}/);
    ok(!!chunkSplit, '抠得到 splitMode');
    if (chunkSplit) {
      const c2 = vm.createContext({});
      vm.runInContext(chunkSplit[0], c2);
      const a = c2.splitMode('com.a.b=75 learn');
      ok(a && a.val === '75' && a.learned === true, '学习条目剥掉词尾并标出来源', JSON.stringify(a));
      const b = c2.splitMode('com.a.b=full');
      ok(b && b.val === 'full' && b.learned === false, '手填的 full 不受影响');
      const c3 = c2.splitMode('com.a.b=80');
      ok(c3 && c3.val === '80' && c3.learned === false, '手填的数字条不带来源');
      ok(c2.splitMode('novalue') === null && c2.splitMode('=50') === null,
         '坏行返回 null（渲染时直接跳过，不会画出一条空的）');
    }
  }

  finish();
}
main().catch(e => { ok(false, '用例自己跑崩了', e.stack); finish(); });
