// 功能页的档位测试：普通用户只在下拉框里选一档，这里验的是
//   1) 每一档写进控件的值，后端（api.sh 的校验区间）会不会拒；
//   2) 手动改了滑杆之后，下拉框会不会还谎称"你在某一档"；
//   3) 顶上那句"现在这台机器会怎么做"是不是人话（不许出现 pp / ms/s / 档 这类术语）。
//
// 档位和校验区间分处两个文件，正是最容易各改各的地方：
// 页面填了个 api.sh 认为越界的值，用户按保存只会看到一句"参数不对"。
//
// 用法： node src/test_func_ui.js webroot/index.html webroot/api.sh
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'webroot/index.html';
const apiPath = process.argv[3] || 'webroot/api.sh';
const html = fs.readFileSync(htmlPath, 'utf8');
const api = fs.readFileSync(apiPath, 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

/* ── 从 api.sh 里读每个键的合法区间（不抄第二份表） ─────────────── */
const RANGE = {};
for (const m of api.matchAll(/([A-Z_]+)\)\s+\[ "\$_v" -ge (\d+) \] && \[ "\$_v" -le (\d+) \]/g))
  RANGE[m[1]] = [+m[2], +m[3]];
const SWITCH = {};
for (const m of api.matchAll(/([A-Z_ |]+)\)\s+is_switch/g))
  m[1].split('|').map(x => x.trim()).filter(Boolean).forEach(k => { SWITCH[k] = 1; });

const ID2KEY = { chg_uncap: 'CHG_UNCAP', chg_pct: 'CHG_PCT', th_cap: 'TH_CAP',
                 th_high: 'TH_HIGH', th_low: 'TH_LOW', th_max: 'TH_MAX',
                 psi_adapt: 'PSI_ADAPT', psi_gain: 'PSI_GAIN' };

/* 组件（ddEl / segInto）在功能页代码上面一截，中间还夹着滑杆那些无关代码。
   分两段抠出来拼一起：整段吞掉的话会把 pointerdown 那套事件绑定也拖进沙箱，
   替身就得假装自己有完整 DOM —— 那测的就不再是这段逻辑了。
   起点用 function ddEl( 而不是那段注释：同样的注释文字在 <style> 里也有一份。 */
const wBlock = html.match(/function ddEl\([\s\S]*?(?=\/\* ── 滑杆只认)/);
const fBlock = html.match(/\/\* ── 功能页：人话档位[\s\S]*?(?=async function saveAll\(\)\{)/);
/* 分应用档位那两个下拉的构造函数。只要常量 + 拆/拼 + 两个 select，
   不要后面的 loadModes/setMode（它们要 api、pkgRow 一整套替身）。 */
const mBlock = html.match(/const MODE_OPTS = \[[\s\S]*?(?=\n\/\* modes_list)/);
const block = wBlock && fBlock ? [wBlock[0], fBlock[0]] : null;

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], options: [], style: {}, className: '',
    value: '', checked: false, textContent: '', type: '', dataset: {},
    appendChild(c) { el.children.push(c); if (c.tagName === 'option') el.options.push(c); return c; },
    classList: { add() {}, remove() {}, contains() { return false; } },
  };
  Object.defineProperty(el, 'innerHTML', {
    get() { return ''; }, set(v) { if (v === '') { el.children = []; el.options = []; } },
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
  Object.entries(ID2KEY).forEach(([id, key]) => {
    const el = get(id);
    // 开关在页面里是 <input type=checkbox>，替身必须照做：
    // applyPreset 就是靠 el.type 判该写 checked 还是写 value 的。
    if (SWITCH[key]) el.type = 'checkbox';
  });
  const ctx = {
    document: { getElementById: get, createElement: mkEl, addEventListener(){},
                createTextNode: t => { const n = mkEl('#text'); n.textContent = t; return n; },
                querySelectorAll: () => [] },
    pv: id => { const el = get(id); const out = get(id + '_v'); if (out) out.textContent = el.value; },
    saveAll: () => {}, loadConf: () => {},
    Promise: Promise, Object: Object, Array: Array, Number: Number, String: String,
    parseInt: parseInt, console: console,
  };
  vm.createContext(ctx);
  /* 块里全是 const/let 声明，它们不会挂到 context 对象上（脚本作用域是词法的），
     所以要在同一份代码尾巴上把要用的东西递出来。 */
  vm.runInContext(block[0] + block[1] + (mBlock ? mBlock[0] : '') +
    '\n;this.x = { FUNC_PRESETS: FUNC_PRESETS, CUSTOM_OPT: CUSTOM_OPT,' +
    ' applyPreset: applyPreset, syncFunc: syncFunc, buildPresetSelects: buildPresetSelects,' +
    ' paintFuncSum: paintFuncSum, fv: fv,' +
    ' modeSelect: typeof modeSelect === "function" ? modeSelect : null,' +
    ' modeGpuSelect: typeof modeGpuSelect === "function" ? modeGpuSelect : null };',
    ctx, { filename: 'ui-func.js' });
  const api2 = ctx.x;
  return { ctx: api2, nodes, get };
}

console.log('== 功能页档位（下拉 = 一组具体值）==');
if (!block) {
  ok(false, '从 index.html 里抠出功能页那段', '锚点：/* ── 功能页：人话档位');
  console.log(fail + ' 项失败 / ' + pass + ' 项通过');
  process.exit(1);
}
ok(Object.keys(RANGE).length >= 6 && Object.keys(SWITCH).length >= 3,
   '从 api.sh 读到了校验区间（不抄第二份表）',
   '区间 ' + Object.keys(RANGE).length + ' 个、开关 ' + Object.keys(SWITCH).length + ' 个');

let s = sandbox();
const G = s.ctx.FUNC_PRESETS;
ok(G && G.chg && G.th && G.psi, '三组档位都在（chg / th / psi）');
ok(G.th.opts.length === 4 && G.psi.opts.length === 4 && G.chg.opts.length === 3,
   '每组都有"关掉"这一档，且不是只给一个默认值',
   'chg ' + G.chg.opts.length + ' th ' + G.th.opts.length + ' psi ' + G.psi.opts.length);

/* 1) 每一档的每个值都要过后端的校验 */
let bad = [];
for (const g of Object.keys(G)) {
  for (const o of G[g].opts) {
    for (const id of Object.keys(o.set)) {
      const key = ID2KEY[id];
      if (!key) { bad.push(g + '/' + o.v + ' 写了个不认识的控件 ' + id); continue; }
      if (SWITCH[key]) {
        if (o.set[id] !== 0 && o.set[id] !== 1) bad.push(key + '=' + o.set[id] + ' 不是开关值');
        continue;
      }
      const r = RANGE[key];
      if (!r) { bad.push(key + ' 在 api.sh 里没有区间（新增键没配校验？）'); continue; }
      if (o.set[id] < r[0] || o.set[id] > r[1])
        bad.push(key + '=' + o.set[id] + ' 越出 ' + r.join('~'));
    }
  }
}
ok(bad.length === 0, '每一档写进的值都在后端校验区间内（按保存不会被打回）', bad.join(' | '));

/* 2) 温控档位的迟滞带：恢复线必须低于触发线 */
const thBad = G.th.opts.filter(o => o.set.th_cap === 1 && o.set.th_low >= o.set.th_high)
                      .map(o => o.v);
ok(thBad.length === 0, '每一档的"放开温度"都低于"开始降的温度"', thBad.join());

/* 3) 点一档 → 控件真的变成那组值，且下拉框认得出这一档 */
s = sandbox();
s.get('p-th').value = 'early';
s.ctx.applyPreset('th');
ok(s.nodes.th_high.value === 70 && s.nodes.th_low.value === 64 && s.nodes.th_max.value === 35,
   '选「早点降」把三个值都写进去了',
   [s.nodes.th_high.value, s.nodes.th_low.value, s.nodes.th_max.value].join('/'));
ok(s.nodes.th_cap.checked === true, '选档位不会把总开关顺手关掉');
s.ctx.syncFunc();
ok(s.get('p-th').value === 'early', '刚选完再同步，下拉框还停在「早点降」');

/* 4) 手动改滑杆 → 下拉框必须改口"自己定的值"，不许继续谎称在某一档 */
s.nodes.th_high.value = 77;
s.ctx.syncFunc();
ok(s.get('p-th').value === s.ctx.CUSTOM_OPT,
   '手动改过之后显示「自己定的值」（不谎称还在某档）', String(s.get('p-th').value));
// 用户手动改过之后下拉框显示「自己定的值」；他要收回档位是重新点一个选项，
// 那时 sel.value 已经是那档的代号了 —— 这里就照这个动作演一遍。
s.get('p-th').value = 'early';
s.ctx.applyPreset('th');
ok(s.get('p-th').value === 'early' && s.nodes.th_high.value === 70,
   '再点一次档位能把改乱的值收回来', String(s.nodes.th_high.value));

/* 5) PSI 只放宽不收紧：档位里不许出现"借出=负"或把开关写成别的值 */
ok(G.psi.opts.every(o => (o.set.psi_gain === undefined || o.set.psi_gain >= 0)),
   'PSI 档位没有负值（这一条只能往上放宽）');

/* 6) 顶上那句人话：不许出现术语 */
s = sandbox();
s.ctx.buildPresetSelects();
s.get('p-psi').value = 'mid'; s.ctx.applyPreset('psi');
s.get('p-chg').value = 'free'; s.ctx.applyPreset('chg');
const sum = textOf(s.get('func-sum'));
ok(sum.length > 40, '「现在这台机器会怎么做」有三句话', '长度 ' + sum.length);
const jargon = ['pp', 'ms/s', 'stall', 'PSI ', '℃ 开始一档档', 'CHG_PCT', '阈值']
  .filter(w => new RegExp('\\b' + w.replace(/[^\w℃]/g, '') + '\\b').test(sum) && sum.includes(w));
ok(jargon.length === 0, '那句总结里没有术语（pp / ms/s / CHG_PCT / 阈值）', jargon.join(' | '));
ok(sum.includes('插着电时不封顶') && sum.includes('发热'), '说的是插电和发热两件实际行为',
   sum.slice(0, 80));

/* 7) 关掉某条之后，那句话要跟着换成"不做什么"，而不是继续报数值 */
s = sandbox();
s.get('p-th').value = 'off'; s.ctx.applyPreset('th');
const off = textOf(s.get('func-sum'));
ok(off.includes('不主动降速'), '温控选「不主动降」时说明跟着改口', off.slice(0, 90));
s.get('p-psi').value = 'off'; s.ctx.applyPreset('psi');
ok(textOf(s.get('func-sum')).includes('不会放宽'), 'PSI 关掉时也说清楚不做什么');

/* 8) 折叠不影响取值：控件在 <details> 里也照样被保存读到（同一份文档） */
ok(/<details class="more">[\s\S]*?id="th_high"[\s\S]*?<\/details>/.test(html),
   '温控滑杆收在「自己定」里（默认不打扰人，但值还在原处）');
/* 功能页那四张卡各自带一个「自己定」。数的是这一页，不是整份文档：
   配置页的「按应用学习门槛」也用了同一种折叠，跟着数进去就会误报成"多了一个"。 */
const funcHtml = html.slice(html.indexOf('id="p-func"'), html.indexOf('id="p-game"'));
ok((funcHtml.match(/<details class="more">/g) || []).length === 4,
   '功能页四张卡都有「自己定」，没有任何一条只剩档位');
ok(/<details class="more">[\s\S]*?id="learn_app_min"[\s\S]*?id="learn_app_day"[\s\S]*?<\/details>/.test(html),
   '按应用学习的两个门槛也收在「自己定」里（默认不给非专业用户看）');

/* 9) 这一改的本来目的：页面上不许再有原生下拉控件。
      在 WebView 里点它弹的是系统单选框，样式完全不受我们控制。
      判据只认元素（<select 后面跟属性），注释里提到这个词是难免的。 */
ok(!/<select\s/i.test(html), '页面里没有任何 <select> 元素（原生弹框和新拟物不是一个世界）',
   (html.match(/.{0,40}<select\s.{0,30}/) || ['没命中'])[0]);
ok(!/<option[\s>]/.test(html), '也没有残留的 <option>');
ok(!/\.options\b/.test((html.match(/<script>([\s\S]*)<\/script>/) || ['', ''])[1]),
   'JS 里不再遍历 select.options（改成查选项表）');

/* 10) 分段按钮真的建出来了，点一下会写值 */
s = sandbox();
s.ctx.buildPresetSelects();
const host = s.get('p-th');
ok(host.children.length === G.th.opts.length + 1,
   '温控那排按钮数 = 档位 + 「自己定的值」', '实际 ' + host.children.length);
const labels = host.children.map(c => c.textContent);
ok(labels[1] === '适中（推荐）' && labels[labels.length - 1] === '自己定的值',
   '按钮上写的是行为，不是参数名', labels.join(' / '));
// 模拟点第三档（尽量保性能）
host.children[2].onclick();
ok(s.nodes.th_high.value === 82 && s.nodes.th_max.value === 15,
   '点「尽量保性能」把三个值都写进控件', String(s.nodes.th_high.value));
ok(host.value === 'late', '点完按钮自己也停在「尽量保性能」', String(host.value));

/* 11) 自绘下拉：容器上有 .value，老代码读它不用改 */
const ddBlock = html.match(/function ddEl\([\s\S]*?\n\}/)[0];
ok(/Object\.defineProperty\(box, 'value'/.test(ddBlock) && /box\.onchange/.test(ddBlock),
   '自绘下拉保留了 .value / .onchange 约定（调用处一行没改）');
ok(/function segInto\(box, opts/.test(html),
   '分段按钮往宿主元素上渲染（值不会长在临时子节点里）');

/* 12) 窄屏排版：按钮组必须整行独占
   曾经写成 .row>.seg，可 seg 是渲染在 <div id="p-chg"> 里面的，选择器落空，
   于是 flex 把标题列压成一字一行、按钮冲出屏幕。手机宽度只有 393 上下，
   这三条就是那个画面不许回来。 */
['p-chg', 'p-th', 'p-psi'].forEach(id => {
  ok(new RegExp('<div class="pset" id="' + id + '"></div>').test(html),
     id + ' 这个容器带 pset（按钮组靠它独占整行）');
});
ok(/\.row\{[^}]*flex-wrap:wrap/.test(html), '.row 允许换行（否则 100% 挤不进同一行）');
ok(/\.row>\.pset\{flex:1 0 100%\}/.test(html), 'pset 规则是 flex:1 0 100%');
ok(!/\.row>\.seg\b/.test(html), '没有残留的 .row>.seg（那条从来没命中过元素）');

/* 12b) 分段按钮的选中态与"三档均分"
   真机反馈两条：① 点了没感觉，看不出现在选的是哪个；② 学习偏好三颗挤在左边。
   根因是学习偏好那组没拿到 seg 类（功能页三组是 host.classList.add('seg') 才吃到样式的），
   而 .seg button.on 旧样式只换字色 + 内阴影，在浅灰平面上几乎不可见。 */
const segOn = (html.match(/\.seg button\.on\{[\s\S]*?\}/) || [''])[0];
ok(/background:linear-gradient\(145deg,var\(--acc\),var\(--acc2\)\)/.test(segOn)
   && /color:var\(--on-acc\)/.test(segOn),
   '选中项压成主色面 + 反色字（不再只靠字色区分）', segOn.slice(0, 60));
const segCustOn = (html.match(/\.seg button\.cust\.on\{[\s\S]*?\}/) || [''])[0];
ok(/background:var\(--warn\)/.test(segCustOn),
   '「自己定的值」选中时用警示色面，和普通选中项分得开');
ok(/\.seg\.fill button\{[^}]*flex:1 1 0/.test(html),
   '.seg.fill 里每颗 flex:1 1 0（均分整行）');
ok(/lm\.classList\.add\('seg', 'fill'\)/.test(html),
   '学习偏好用的是同一个分段按钮组件，并且带 fill（短标签均分）');

/* 13) CPU:GPU 双值的拆/拼规则。页面的写法和 api.sh、守护必须能互换：
   70 / 70: / :40 / 70:40 四种形式，拼出来是什么就原样发给 modes_set。 */
{
  const chunk = html.match(/\/\* 档位值(?:三|四)种写法[\s\S]*?(?=\nfunction modeSelect\()/);
  ok(!!chunk, '抠得到 CPU:GPU 的拆/拼函数');
  if (chunk) {
    const c = vm.createContext({ String: String, Object: Object });
    vm.runInContext(chunk[0], c);
    const a = c.parseModeVal('70:40');
    ok(a && a.cpu === '70' && a.gpu === '40' && !a.full, '70:40 拆成两段', JSON.stringify(a));
    const b = c.parseModeVal('60:');
    ok(b && b.cpu === '60' && b.gpu === '', '60: 的 GPU 段为空（跟随全局）', JSON.stringify(b));
    const d = c.parseModeVal(':40');
    ok(d && d.cpu === '' && d.gpu === '40', ':40 的 CPU 段为空（跟随全局）', JSON.stringify(d));
    ok(c.parseModeVal('full').full === true, 'full 单独一种形态');
    ok(c.joinModeVal('70', '40') === '70:40', '两段都填拼成 70:40');
    ok(c.joinModeVal('70', '') === '70', 'GPU 跟随全局时退回裸数字（老写法）');
    ok(c.joinModeVal('', '40') === ':40', 'CPU 跟随全局时写成 :40');
    ok(c.joinModeVal('full', '40') === 'full', 'full 不参与拼接（GPU 也一起交还）');
    ok(c.modeText('70:40') === 'CPU 70% · GPU 40%', '列表里两段都说清楚', c.modeText('70:40'));
    ok(c.modeText('full') === '满血', 'full 仍显示"满血"');
  }
  s = sandbox();
  ok(s.ctx.modeSelect && s.ctx.modeGpuSelect, '行内的 CPU/GPU 两个下拉都建得出来');
  if (s.ctx.modeSelect && s.ctx.modeGpuSelect) {
    ok(s.ctx.modeSelect('com.a', '70:40').value === '70', '双值行的 CPU 下拉停在 70');
    ok(s.ctx.modeGpuSelect('com.a', '70:40').value === '40', '双值行的 GPU 下拉停在 40');
    ok(s.ctx.modeSelect('com.a', ':40').value === '', 'GPU-only 行的 CPU 下拉显示跟随全局');
    ok(s.ctx.modeGpuSelect('com.a', '').value === '', '没设 GPU 的行显示跟随全局');
    ok(s.ctx.modeSelect('com.a', 'full').value === 'full', 'full 行走"满血"档');
    ok(s.ctx.modeGpuSelect('com.a', ':35').value === '35',
       '手写的非预设 GPU 值会临时插进下拉（不会一开就变成 90）');
    /* 非预设值的临时选项原本自带 "CPU " 前缀，外层 map 又加一次，
       按钮上会显示 "CPU CPU 65%"。 */
    const cpuCustom = textOf(s.ctx.modeSelect('com.a', '65'));
    ok(/CPU 65%/.test(cpuCustom) && !/CPU CPU/.test(cpuCustom),
       '非预设 CPU 值只带一个 CPU 前缀', cpuCustom.trim());
    const cpuFollow = textOf(s.ctx.modeSelect('com.a', ':40'));
    ok(/CPU 跟随全局/.test(cpuFollow) && !/CPU CPU/.test(cpuFollow),
       'GPU-only 行的 CPU 下拉显示"CPU 跟随全局"', cpuFollow.trim());
  }
}

console.log('\n' + (fail === 0 ? '全部通过 (' + pass + ')' : fail + ' 项失败 / ' + pass + ' 项通过'));
process.exit(fail ? 1 : 0);
