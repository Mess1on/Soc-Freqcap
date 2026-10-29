// 主题功能：保存优先、系统偏好兜底、两个按钮的选中态、color-scheme 跟着换。
// 只抠页面里的主题段，不碰日志/接口。
//
// 用法： node src/test_theme_ui.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'webroot/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

const chunk = html.match(/\/\* ── 主题：浅色 \/ 深色[\s\S]*?(?=\/\*[\r\n]+ \* 调用 root shell)/);
if (!chunk) {
  console.log('  FAIL 页面里找不到主题脚本');
  process.exit(1);
}

function mkEl() {
  const el = { className: '', attrs: {}, textContent: '' };
  el.classList = {
    toggle(c, on) {
      const s = new Set(el.className.split(/\s+/).filter(Boolean));
      if (on) s.add(c); else s.delete(c);
      el.className = Array.from(s).join(' ');
    },
    add(c) { el.classList.toggle(c, true); },
    remove(c) { el.classList.toggle(c, false); },
    contains(c) { return el.className.split(/\s+/).indexOf(c) >= 0; },
  };
  el.setAttribute = (k, v) => { el.attrs[k] = String(v); };
  el.getAttribute = k => el.attrs[k] === undefined ? null : el.attrs[k];
  return el;
}

function sandbox(opts) {
  opts = opts || {};
  const root = mkEl(), meta = mkEl(), light = mkEl(), dark = mkEl();
  const store = Object.assign({}, opts.store || {});
  const ctx = {
    document: {
      documentElement: root,
      querySelector: sel => /color-scheme/.test(sel) ? meta : null,
      getElementById: id => id === 'th-l' ? light : id === 'th-d' ? dark : null,
    },
    window: {},
    localStorage: {
      getItem(k) { if (opts.throwStore) throw new Error('blocked'); return store[k] || null; },
      setItem(k, v) { if (opts.throwStore) throw new Error('blocked'); store[k] = String(v); },
    },
    console: console, String: String, Object: Object,
  };
  if (!opts.noMatchMedia) {
    ctx.window.matchMedia = q => ({
      matches: !!opts.systemDark,
      addEventListener() {},
    });
  }
  vm.createContext(ctx);
  vm.runInContext(chunk[0], ctx);
  return { ctx, root, meta, light, dark, store };
}

{
  const s = sandbox({ systemDark: true });
  ok(s.root.getAttribute('data-theme') === 'dark', '没保存过、系统深色 → 深色');
  ok(s.meta.getAttribute('content') === 'dark', 'color-scheme 跟着切到 dark');
  ok(s.dark.classList.contains('on') && !s.light.classList.contains('on'),
     '深色按钮选中、浅色按钮取消');
}
{
  const s = sandbox({ systemDark: false });
  ok(s.root.getAttribute('data-theme') === 'light', '没保存过、系统浅色 → 浅色');
}
{
  const s = sandbox({ systemDark: false, store: { 'socfreq-theme': 'dark' } });
  ok(s.root.getAttribute('data-theme') === 'dark', '保存过 dark：系统浅色也不覆盖');
}
{
  const s = sandbox({ systemDark: true, store: { 'socfreq-theme': 'light' } });
  ok(s.root.getAttribute('data-theme') === 'light', '保存过 light：系统深色也不覆盖');
}
{
  const s = sandbox({ systemDark: false });
  s.ctx.setTheme('dark');
  ok(s.root.getAttribute('data-theme') === 'dark' && s.store['socfreq-theme'] === 'dark',
     '点深色：立刻换主题并写入本地');
  s.ctx.setTheme('light');
  ok(s.root.getAttribute('data-theme') === 'light' && s.store['socfreq-theme'] === 'light',
     '点浅色：立刻换回来并写入本地');
}
{
  const s = sandbox({ systemDark: true, throwStore: true });
  ok(s.root.getAttribute('data-theme') === 'dark',
     'localStorage 被禁用时仍能按系统偏好显示');
}
{
  const s = sandbox({ systemDark: true, noMatchMedia: true });
  ok(s.root.getAttribute('data-theme') === 'light',
     '没有 matchMedia 时退回浅色，不白屏');
}

console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
process.exit(fail ? 1 : 0);
