// 睡眠页渲染测试：把 index.html 里深睡那段抠出来，喂 api.sh 的 state 输出格式，
// 看解析、状态条、双榜、豁免名单和 2 秒轮询接线是否对得上。
//
// 为什么单独一份：dsw/dswc 是 "包名:次数:是否白名单" 的逗号串，字段错位不会报错，
// 页面只会少一行或把次数画成包名。静态断言看不出这种问题。
//
// 用法： node src/test_ds_ui.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'webroot/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

const chunk = html.match(/\/\* ── 深度睡眠 ──[\s\S]*?(?=\/\* ── 维护 ── \*\/)/);
if (!chunk) {
  console.log('  FAIL 页面里找不到深睡那段代码');
  process.exit(1);
}
// renderDsPicker 复用全局的搜索匹配（含中文别名），这段在深睡段之外，得一起喂。
const searchChunk = html.match(/const SEARCH_ALIAS = \{[\s\S]*?\nfunction searchHit\(p, terms\)\{[\s\S]*?\n\}/);
if (!searchChunk) {
  console.log('  FAIL 页面里找不到搜索匹配函数');
  process.exit(1);
}

const FULL = [
  'enable=1', 'alive=1', 'dsactive=1', 'wcsince=1700000000',
  'dsw=com.a:5:0,com.b:2:1,com.bad:oops:0',
  'dswc=com.a:9:0',
  'white=com.b', 'white=com.c',
].join('\n');
const POLL = [
  'enable=1', 'alive=1', 'dsactive=1', 'wcsince=1700000000',
  'dsw=com.a:7:0,com.b:3:1',
  'dswc=com.a:11:0',
].join('\n');
const OFF = 'enable=0\nalive=0\ndsactive=0\nwcsince=0\ndsw=\ndswc=\n';

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], style: {}, className: '', disabled: false,
    value: '', checked: false, textContent: '', dataset: {}, title: '',
    appendChild(c) { el.children.push(c); return c; },
  };
  el.classList = {
    add(c) { if (!el.classList.contains(c)) el.className += (el.className ? ' ' : '') + c; },
    remove(c) { el.className = el.className.split(/\s+/).filter(x => x && x !== c).join(' '); },
    contains(c) { return el.className.split(/\s+/).indexOf(c) >= 0; },
  };
  Object.defineProperty(el, 'innerHTML', {
    get() { return el._html || ''; },
    set(v) { el._html = String(v); if (v === '') el.children = []; },
  });
  return el;
}
function textOf(el) {
  let t = (el.textContent || '') + ' ' + (el._html || '');
  el.children.forEach(c => { t += ' ' + textOf(c); });
  return t;
}

function sandbox() {
  const nodes = {};
  const get = id => nodes[id] || (nodes[id] = mkEl('div'));
  const calls = [];
  const labelled = [];
  const ctx = {
    document: {
      getElementById: get,
      createElement: mkEl,
      querySelectorAll: () => [],
    },
    api: function () {
      const a = Array.from(arguments);
      calls.push(a);
      if (a[0] === 'ds_get') return Promise.resolve(FULL);
      if (a[0] === 'ds_state') return Promise.resolve(POLL);
      return Promise.resolve('OK');
    },
    toast: m => calls.push(['toast', m]),
    LABELS: { 'com.a': '应用甲', 'com.b': '应用乙', 'com.bad': '坏行' },
    APPS: ['com.a', 'com.b', 'com.c', 'com.d'],
    pkgRow: p => {
      const el = mkEl('div');
      el.dataset.pkg = p;
      el.textContent = p;
      return { el };
    },
    ensureLabels: list => { labelled.push((list || []).slice()); },
    loadApps: () => calls.push(['loadApps']),
  };
  ctx.window = ctx;
  const box = get('ds-picker');
  box.style.display = '';
  vm.createContext(ctx);
  vm.runInContext(searchChunk[0] + '\n' + chunk[0], ctx);
  // const 声明不会挂到 vm 的全局对象上，单独取一次引用再改。
  const DS = vm.runInContext('DS', ctx);
  return { ctx, get, calls, labelled, nodes, DS };
}

(async () => {
  {
    const s = sandbox();
    const st = s.ctx.parseDs(FULL);
    ok(st.enable === 1 && st.alive === 1 && st.active === 1, 'parseDs 读出开关 / 守护 / 深睡状态');
    ok(st.run.length === 2 && st.run[0].pkg === 'com.a' && st.run[0].cnt === 5,
       '当次榜解析出包名和次数');
    ok(st.run[1].wl === true && st.run[0].wl === false,
       '白名单标记 1/0 转成布尔值');
    ok(st.run.every(x => x.pkg !== 'com.bad'),
       '次数不是数字的坏条目直接丢掉（不画成 0 次）');
    ok(st.cur.length === 1 && st.cur[0].cnt === 9, '累计榜单独解析');
    ok(Object.keys(st.white).sort().join(',') === 'com.b,com.c', '白名单逐行进对象');

    s.DS.white = st.white;
    s.ctx.applyDs(st, true);
    const box = s.get('ds-st');
    ok(box.classList.contains('on') && s.get('ds-stxt').textContent === '深度睡眠中',
       '深睡中状态条染成绿色并写「深度睡眠中」');
    ok(s.get('ds_enable').checked === true, '总开关跟着状态勾上');
    ok(s.get('ds-run').children.length === 2, '当次榜画两行（坏条目不算）');
    ok(s.get('ds-cur').children.length === 1, '累计榜画一行');
    const runTxt = textOf(s.get('ds-run'));
    ok(runTxt.indexOf('应用甲') >= 0 && runTxt.indexOf('应用乙') >= 0
       && runTxt.indexOf('白名单') >= 0 && runTxt.indexOf('5') >= 0,
       '榜单用应用名显示，白名单条目带标记，次数在行里');
    ok(s.get('ds-white').children.length === 2, '豁免名单画两行');
    ok(s.labelled.some(list => list.indexOf('com.a') >= 0 && list.indexOf('com.b') >= 0),
       '榜单和豁免都点名取应用名');
  }

  {
    const s = sandbox();
    s.ctx.applyDs(s.ctx.parseDs(OFF), true);
    const box = s.get('ds-st');
    ok(box.classList.contains('off') && s.get('ds-stxt').textContent === '未启用',
       '关着的时候写「未启用」');
    ok(s.get('ds-run').children.length === 0
       && textOf(s.get('ds-run')).indexOf('还没有记录') >= 0,
       '没有记录时给一句人话，不画空柱');
  }

  {
    const s = sandbox();
    s.ctx.applyDs(s.ctx.parseDs('enable=1\nalive=1\ndsactive=0\n'), true);
    ok(s.get('ds-st').classList.contains('wait') && s.get('ds-stxt').textContent === '监测中',
       '开着但没进深睡时显示「监测中」');
  }

  {
    const s = sandbox();
    s.ctx.applyDs(s.ctx.parseDs('enable=1\nalive=0\npaused=1\n'), true);
    ok(s.get('ds-st').classList.contains('off') && s.get('ds-stxt').textContent === '模块已暂停',
       '模块暂停时说明原因，不显示成「守护没在跑」');
  }

  {
    const s = sandbox();
    await s.ctx.setDsEnable(true);
    await s.ctx.setDsEnable(false);
    const sets = s.calls.filter(c => c[0] === 'ds_set').map(c => c.join(' '));
    ok(sets[0] === 'ds_set enable 1' && sets[1] === 'ds_set enable 0',
       '开关分别发 ds_set enable 1 / 0');
  }

  {
    const s = sandbox();
    s.DS.white = { 'com.a': 1 };
    await s.ctx.addDsWhite('com.b');
    ok(s.DS.white['com.b'] === 1
       && s.calls.some(c => c[0] === 'ds_white_add' && c[1] === 'com.b'),
       '添加豁免先发 ds_white_add 再更新本地名单');
    await s.ctx.delDsWhite('com.a');
    ok(!s.DS.white['com.a']
       && s.calls.some(c => c[0] === 'ds_white_del' && c[1] === 'com.a'),
       '移除豁免发 ds_white_del');
  }

  {
    const s = sandbox();
    s.DS.white = { 'com.a': 1 };
    s.ctx.APPS = ['com.a', 'com.b', 'com.c'];
    s.get('ds-q').value = '';
    s.ctx.renderDsPicker();
    const rows = s.get('ds-list').children.map(x => x.dataset.pkg);
    ok(rows.join(',') === 'com.b,com.c', '候选栏排除已在豁免里的应用');
    s.get('ds-q').value = 'com.c';
    s.ctx.renderDsPicker();
    ok(s.get('ds-list').children.map(x => x.dataset.pkg).join(',') === 'com.c',
       '搜索框按包名过滤');
    ok(textOf(s.get('ds-count')).indexOf('未加入豁免 2 个') >= 0,
       '计数写清还有几个没加入');
  }

  console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
  process.exit(fail ? 1 : 0);
})();
