// GPU 草稿预览：滑杆还没保存时显示的封顶值，必须和守护保存后的算法一致
//   ——先加实时偏移（温控/PSI），再按真实频点表向下吸附。
// 只乘百分比会在两种机器上骗人：频点表稀疏（600 不在表里）和有实时偏移。
//
// 用法： node src/test_gpu_preview.js webroot/index.html
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

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], className: '', textContent: '', value: '',
    appendChild(c) { el.children.push(c); return c; },
  };
  Object.defineProperty(el, 'innerHTML', {
    get() { return ''; }, set(v) { if (v === '') el.children = []; },
  });
  return el;
}
function textOf(el) {
  let t = el.textContent || '';
  el.children.forEach(c => { t += ' ' + textOf(c); });
  return t;
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
  vm.runInContext(chunk[0], ctx);
  return { ctx, get };
}

function gpuRow(s, saved, cur) {
  const data = {
    soc: 'SM8850',
    gpu_max: '1200000000', gpu_cap: '420000000', gpu_pct: String(saved),
    gpu_dlt: String(s.dlt), gpu_unit: 'Hz', gpu_kind: 'KGSL',
    gpufreq: s.freqs,
  };
  // lastSt 是 vm 里 let 声明的词法变量，不能靠给 context 加属性改。
  vm.runInContext('lastSt = ' + JSON.stringify(data) + ';', s.ctx);
  s.get('gpu_pct').value = String(cur);
  vm.runInContext('paintHw()', s.ctx);
  const rows = s.get('hw-table').children;
  const g = rows.length ? rows[rows.length - 1] : null;
  return {
    row: g ? textOf(g).replace(/\s+/g, ' ').trim() : '',
    note: s.get('soc-hw').textContent,
  };
}

{
  const s = sandbox();
  s.dlt = 0;
  s.freqs = '1200000000 1100000000 1000000000 900000000 800000000 700000000 600000000 500000000 400000000 300000000 282000000';
  let r = gpuRow(s, 60, 50);
  ok(r.row.indexOf('≈600 MHz') >= 0, '无偏移、表里有 600：50% 预览 ≈600 MHz', r.row);
  ok(r.row.indexOf('≈282') < 0, '不会莫名掉到 282');
  ok(r.note.indexOf('实时偏移') < 0, '没有偏移时不显示偏移说明');

  /* 这张表显示"你设的值对应哪个频点"，实时偏移只影响当前生效值。
     以前把偏移叠进预览，100% 会显示 826，看起来像拉满没生效。 */
  s.dlt = -25;
  r = gpuRow(s, 60, 50);
  ok(r.row.indexOf('≈600 MHz') >= 0, '有 -25% 偏移时预览仍显示设定值的 ≈600 MHz', r.row);
  ok(r.row.indexOf('600 MHz（-25%') < 0 && r.row.indexOf('-25%') < 0,
     '偏移不写进这一行（实际值在 GPU 封顶卡片里看）', r.row);
  ok(r.note.indexOf('-25%') >= 0 && r.note.indexOf('只影响当前生效值') >= 0,
     '页头说明偏移只影响当前生效值，不改这里的设定值', r.note);

  s.dlt = -27;
  r = gpuRow(s, 60, 50);
  ok(r.row.indexOf('≈600 MHz') >= 0, '-27% 也不改预览：仍是 ≈600 MHz', r.row);

  s.dlt = 0;
  s.freqs = '1200000000 700000000 282000000';
  r = gpuRow(s, 60, 50);
  ok(r.row.indexOf('≈282 MHz') >= 0, '频点表没有 600 这一档：预览按真实表吸附到 282', r.row);

  /* 用户截图那种：表里有 1200，滑杆拉到 100%，预览就该是 1200 */
  s.freqs = '1200000000 1100000000 1000000000 902000000 826000000 726000000 646000000';
  s.dlt = -25;
  r = gpuRow(s, 60, 100);
  ok(r.row.indexOf('≈1200 MHz') >= 0, '滑杆 100%：预览显示 1200MHz，不叠偏移', r.row);
  ok(r.note.indexOf('不改这里显示的设定值') >= 0, '页头把"实际值看别处"说清楚', r.note);

  s.freqs = '1200000000 1100000000 1000000000 900000000 800000000 700000000 600000000 500000000 400000000 300000000 282000000';
  s.dlt = 10;
  r = gpuRow(s, 60, 50);
  ok(r.row.indexOf('≈600 MHz') >= 0, '正偏移也不抬高预览（50% 仍是 600）', r.row);
  ok(r.note.indexOf('+10%') >= 0, '正偏移仍写在页头，说明实际值会更高');
}

console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
process.exit(fail ? 1 : 0);
