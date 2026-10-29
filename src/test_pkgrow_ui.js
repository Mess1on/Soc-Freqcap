// 行构造的回归测试：缓存命中的名字/图标必须在"行还没插入文档"时就画上。
//
// 真机表现：第一次进应用页（异步取回、插入后再画）正常；再进来走缓存，
// 此时 pkgRow 还在拼节点、isConnected=false，旧的 applyLabel/applyIcon
// 会直接 return —— 名字和图标就都不显示了。
//
// 用法： node src/test_pkgrow_ui.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2] || 'webroot/index.html', 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

function fn(name) {
  return (html.match(new RegExp('\\nfunction ' + name + '\\([^)]*\\)\\{[\\s\\S]*?\\n\\}')) || [''])[0];
}
const iconChunk = (html.match(/\nconst ICONS = \{\};[\s\S]*?\n\}\n\nasync function loadGames\(/) || [''])[0]
  .replace(/\n\nasync function loadGames\($/, '');
const chunks = [fn('makeIconEl'), fn('makeNameEl'), iconChunk,
                '\nconst LABELS = {};', fn('applyLabel'), fn('pkgRow')];
if (chunks.some(c => c.length < 15)) {
  console.log('  FAIL 页面里缺函数：' + chunks.map(c => c.length).join(','));
  process.exit(1);
}

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], parentNode: null, className: '', dataset: {},
    textContent: '', title: '', _root: false,
    style: { setProperty() {}, display: '' },
    appendChild(c) { c.parentNode = el; el.children.push(c); return c; },
    replaceWith(x) {
      const p = el.parentNode; if (!p) return;
      const i = p.children.indexOf(el); if (i >= 0) p.children[i] = x;
      x.parentNode = p; el.parentNode = null;
    },
    classList: { add() {}, remove() {}, contains() { return false; } },
    querySelector() { return null; },
  };
  Object.defineProperty(el, 'isConnected', {
    get() { let n = el; while (n.parentNode) n = n.parentNode; return !!n._root; },
  });
  return el;
}

function sandbox() {
  const observed = [];
  const ctx = {
    api: () => Promise.resolve(''),
    document: {
      createElement: mkEl,
      querySelectorAll: () => [],
    },
    IntersectionObserver: class {
      constructor(cb) { this.cb = cb; }
      observe(el) { observed.push(el); }
      unobserve() {}
    },
    setTimeout: () => 0,
    clearTimeout: () => {},
    hueOf: () => 0,
    console, Promise, Object, Array, String, JSON, Math, Number, RegExp,
  };
  vm.createContext(ctx);
  vm.runInContext(chunks.join('\n'), ctx);
  return {
    ctx,
    observed,
    ICONS: vm.runInContext('ICONS', ctx),
    LABELS: vm.runInContext('LABELS', ctx),
  };
}

function findImg(el) {
  for (const c of el.children) {
    if (c.tagName === 'img') return c;
    const d = findImg(c);
    if (d) return d;
  }
  return null;
}

(async () => {
  {
    // 第一次：没有缓存 → 只放色块，注册观察器等滚进视野再取
    const s = sandbox();
    const row = s.ctx.pkgRow('com.a');
    ok(!row.el.isConnected, '测试前提：行在构造时还没插入文档');
    ok(findImg(row.el) === null, '没有缓存时先放色块');
    ok(s.observed.length === 1, '没有缓存时注册了可视区观察器');
  }
  {
    // 再次进入：缓存命中 → 名字和图标要当场画上，且不再发请求
    const s = sandbox();
    s.LABELS['com.a'] = '应用甲';
    s.ICONS['com.a'] = 'data:image/png;base64,AA';
    const row = s.ctx.pkgRow('com.a');
    ok(!row.el.isConnected, '缓存路径同样是在插入文档之前画');
    const img = findImg(row.el);
    ok(img && img.src === 'data:image/png;base64,AA',
       '缓存命中的图标当场换上（不再等 isConnected）',
       img ? img.src : '没有 img');
    ok(row.t.textContent === '应用甲' && row.t.title === '应用甲',
       '缓存命中的应用名当场写上', row.t.textContent);
    ok(row.s.style.display === '', '名称行下面的包名行显示出来');
    ok(s.observed.length === 0, '已有图标不再注册观察器、不再发请求');
  }
  {
    // 明确"没有图标"（空串）也不能重新去问
    const s = sandbox();
    s.ICONS['com.b'] = '';
    const row = s.ctx.pkgRow('com.b');
    ok(findImg(row.el) === null && s.observed.length === 0,
       '已知没有图标的包保持色块，也不再请求');
  }

  console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
  process.exit(fail ? 1 : 0);
})();
