// GPU 草稿预览：滑杆还没保存时显示的封顶值，必须和守护保存后的算法一致
//   ——先按百分比算目标，再按真实频点表向下吸附；实时偏移不参与预览。
// 只乘百分比会在两种机器上骗人：频点表稀疏（600 不在表里）和有实时偏移。
//
// 预览的出口是 #gpu-mhz 那个 span（"封顶百分比"这一行标题下面），不是一张表：
// 页面上已经没有"原生频率表"了，去 #hw-table 里找行只会找到 renderCores 的核心卡。
// 偏移那句说明也不再由 paintHw 动态写，它是 GPU 卡片里的静态 hint，单独断言一次。
//
// paintHw 同一个函数里还写每档 CPU 的 #tiermhz_*（"硬件 X → Y"），一起测：
// 那半边用 kHz，是唯一能看出"封顶列向下取整、硬件列四舍五入"这个区别的地方
// （1497600 kHz = 1497.6 MHz，向下是 1497，四舍五入是 1498）；
// GPU 的频点表都是整 MHz，取整方式在那一行观察不到。
//
// 用法： node src/test_gpu_preview.js [webroot/index.html]
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'webroot/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

const chunk = html.match(/const fmtHz = [\s\S]*?(?=\/\* 每个核心一张卡)/);
if (!chunk) {
  console.log('  FAIL 页面里找不到 fmtHz / paintHw');
  process.exit(1);
}
/* paintHw 还会遍历 PCT_KEYS 去写每档 CPU 的 "硬件 → 封顶"，而 PCT_KEYS 声明在
 * chunk 起点（fmtHz）上面两百多行。不把它一起喂进沙箱就是 ReferenceError，
 * 整个 paintHw 在 CPU 那段就断了，根本走不到我们要测的 GPU 分支。
 * 抠页面里的真身而不是在台架里抄一份：抄的那份和页面脱钩之后就测不出回归了。 */
const pctKeys = html.match(/const PCT_KEYS = \[[\s\S]*?\n\];/);
if (!pctKeys) {
  console.log('  FAIL 页面里找不到 PCT_KEYS（paintHw 依赖它）');
  process.exit(1);
}

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], className: '', textContent: '', value: '', title: '',
    appendChild(c) { el.children.push(c); return c; },
  };
  Object.defineProperty(el, 'innerHTML', {
    get() { return ''; }, set(v) { if (v === '') el.children = []; },
  });
  return el;
}

function sandbox() {
  const nodes = {};
  const get = id => nodes[id] || (nodes[id] = mkEl('div'));
  const ctx = {
    document: { getElementById: get, createElement: mkEl },
    renderCores: () => {},
    Math: Math, Object: Object, String: String, Number: Number,
    parseInt: parseInt, isNaN: isNaN, RegExp: RegExp, Array: Array,
  };
  vm.createContext(ctx);
  vm.runInContext(pctKeys[0], ctx);
  vm.runInContext(chunk[0], ctx);
  return { ctx, get };
}

function gpuText(s, saved, cur, extra) {
  const data = Object.assign({
    soc: 'SM8850',
    gpu_max: '1200000000', gpu_cap: '420000000', gpu_pct: String(saved),
    gpu_dlt: '0', gpu_unit: 'Hz', gpu_kind: 'KGSL',
    gpufreq: '1200000000 1100000000 1000000000 900000000 800000000 ' +
             '700000000 600000000 500000000 400000000 300000000 282000000',
  }, extra || {});
  // lastSt 是 vm 里 let 声明的词法变量，不能靠给 context 加属性改。
  vm.runInContext('lastSt = ' + JSON.stringify(data) + ';', s.ctx);
  s.get('gpu_pct').value = String(cur);
  vm.runInContext('paintHw()', s.ctx);
  return s.get('gpu-mhz').textContent;
}

{
  const s = sandbox();
  let t = gpuText(s, 60, 50);
  ok(t.indexOf('≈600 MHz') >= 0, '无偏移、表里有 600：50% 预览 ≈600 MHz', t);
  ok(t.indexOf('≈282') < 0, '不会莫名掉到 282', t);
  ok(t.indexOf('硬件 1200 MHz') >= 0, '同一行把原生最高也写出来（1200 MHz）', t);

  /* 这张表显示"你设的值对应哪个频点"，实时偏移只影响当前生效值。
     以前把偏移叠进预览，100% 会显示 826，看起来像拉满没生效。 */
  t = gpuText(s, 60, 50, { gpu_dlt: '-25' });
  ok(t.indexOf('≈600 MHz') >= 0, '有 -25% 偏移时预览仍显示设定值的 ≈600 MHz', t);
  ok(t.indexOf('-25%') < 0, '偏移不写进这一行（实际值在 GPU 封顶卡片里看）', t);

  t = gpuText(s, 60, 50, { gpu_dlt: '-27' });
  ok(t.indexOf('≈600 MHz') >= 0, '-27% 也不改预览：仍是 ≈600 MHz', t);

  t = gpuText(s, 60, 50, {
    gpufreq: '1200000000 700000000 282000000',
  });
  ok(t.indexOf('≈282 MHz') >= 0, '频点表没有 600 这一档：预览按真实表吸附到 282', t);

  /* 用户截图那种：表里有 1200，滑杆拉到 100%，预览就该是 1200 */
  t = gpuText(s, 60, 100, {
    gpu_dlt: '-25',
    gpufreq: '1200000000 1100000000 1000000000 902000000 826000000 726000000 646000000',
  });
  ok(t.indexOf('≈1200 MHz') >= 0, '滑杆 100%：预览显示 1200MHz，不叠偏移', t);

  t = gpuText(s, 60, 50, { gpu_dlt: '10' });
  ok(t.indexOf('≈600 MHz') >= 0, '正偏移也不抬高预览（50% 仍是 600）', t);
}

{
  /* 没拖动时走的是"与守护一致"那一支：直接用守护 export 的 gpu_cap，不加 ≈。
     这一支只在**本机没有频点表**时才看得出区别 —— 有表的时候 cap 一律按
     want 重新吸附，守护给的那个值会被覆盖掉。 */
  const s = sandbox();
  let t = gpuText(s, 60, 60, { gpufreq: '' });
  ok(t.indexOf('420 MHz') >= 0, '未拖动 + 无频点表：用守护给的 cap（420 MHz）', t);
  ok(t.indexOf('≈') < 0, '未拖动时不加 ≈（这个值就是守护正在执行的）', t);

  /* 有偏移时守护那个 cap 是**叠过偏移的实际值**，拿它当预览就等于把偏移
     显示进来了 —— 必须退回按百分比算：1200 的 60% = 720。 */
  t = gpuText(s, 60, 60, { gpufreq: '', gpu_dlt: '-25' });
  ok(t.indexOf('720 MHz') >= 0, '未拖动但有偏移：按百分比算（720 MHz）', t);
  ok(t.indexOf('420') < 0, '不会把叠过偏移的实际值 420 当成预览', t);
}

/* 每档 CPU 的 "硬件 X → Y" 也在 paintHw 里，用的是 kHz。
   这一半边是"向下取整 vs 四舍五入"唯一能看出差别的地方。 */
function cpuText(s, o) {
  const data = {
    soc: 'SM8750',
    cpu_0: o.hw || '2457600',            // kHz
    cpukey_0: 'CPU_PCT_BIG',
    cpupct_0: String(o.saved),
    cpucap_0: o.cap === undefined ? '' : String(o.cap),
  };
  if (o.freqs) data.policyfreq_0 = o.freqs;
  vm.runInContext('lastSt = ' + JSON.stringify(data) + ';', s.ctx);
  s.get('CPU_PCT_BIG').value = String(o.cur);
  vm.runInContext('paintHw()', s.ctx);
  return s.get('tiermhz_CPU_PCT_BIG').textContent;
}

{
  const s = sandbox();
  /* 未拖动：直接用守护 export 的 cap。1497600 kHz = 1497.6 MHz，
     封顶那一列必须向下取整成 1497 —— 四舍五入会显示得比真实封顶还高。
     硬件那一列是标称值，四舍五入成 2458 是对的。 */
  let t = cpuText(s, { saved: 65, cur: 65, cap: 1497600 });
  ok(t.indexOf('→ 1497 MHz') >= 0, 'CPU 封顶列向下取整：1497600 kHz 显示 1497 MHz', t);
  ok(t.indexOf('1498') < 0, '不会四舍五入成 1498（那比真实封顶高）', t);
  ok(t.indexOf('硬件 2458 MHz') >= 0, 'CPU 硬件列是标称值，四舍五入成 2458 MHz', t);
  ok(t.indexOf('≈') < 0, '未拖动时不加 ≈（守护正在执行这个值）', t);

  /* 拖动未保存：守护还不知道，只能按百分比估算并沿频点表向下吸附，加 ≈。
     2457600 的 50% = 1228800，表里 <= 它的最高一档是 1190400。 */
  t = cpuText(s, {
    saved: 65, cur: 50, cap: 1497600,
    freqs: '2457600 1996800 1497600 1190400 806400',
  });
  ok(t.indexOf('≈1190 MHz') >= 0, 'CPU 拖动未保存：按频点表向下吸附到 1190400', t);
  ok(t.indexOf('1228') < 0, '不会显示算出来但表里没有的 1228800', t);
  ok(t.indexOf('≈') >= 0, '估算值要带 ≈，和守护已执行的值区分开', t);
}

/* 偏移那句说明现在是 GPU 卡片里的静态 hint，不由 paintHw 写。
   它必须在页面上：预览不叠偏移这件事，得让用户在页面上读到解释。 */
ok(/实时偏移只影响当前生效值，不改这里显示的设定值/.test(html),
   '页面写明"实时偏移只影响当前生效值，不改这里显示的设定值"');

console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
process.exit(fail ? 1 : 0);
