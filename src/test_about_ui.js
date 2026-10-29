// 关于页的后端兼容：标记缺失时还要能从 module.prop 行恢复。
// 有的 ROM 的 sh 会把 `printf -- '---PROP---'` 的 -- 当格式串，只吐两个减号，
// 旧前端就永远弹「about 输出里没有 ---PROP--- 标记」。
//
// 用法： node src/test_about_ui.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2] || 'webroot/index.html';
const html = fs.readFileSync(htmlPath, 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

const chunkParse = html.match(/function parse\(s\)\{[\s\S]*?\n\}/);
const chunkAbout = html.match(/async function loadAbout\(\)\{[\s\S]*?\n\}/);
if (!chunkParse || !chunkAbout) {
  console.log('  FAIL 页面里找不到 parse / loadAbout');
  process.exit(1);
}

const HEAD = 'MOD=/data/adb/modules/soc_freqcap\nBIN=1\nCONF=1\nLIST=1\n' +
             'COUNT=406\nMODES=1\nNMODE=1\nICON=/data/adb/soc_freqcap/icon\nICONLAST=OK\n';
const PROP = 'id=soc_freqcap\nname=Soc智能限频\nversion=V1.5\nversionCode=10500\n' +
             'author=梦笙Mess1on\ndescription=test\n';

function mkEl() {
  const el = { _html: '', style: {}, textContent: '' };
  Object.defineProperty(el, 'innerHTML', {
    get() { return el._html; }, set(v) { el._html = String(v); },
  });
  return el;
}

function sandbox(out) {
  const about = mkEl();
  const toasts = [];
  const ctx = {
    document: { getElementById: id => id === 'about' ? about : mkEl() },
    api: () => Promise.resolve(out),
    toast: m => toasts.push(m),
    iconRow: () => '',
    AUTHOR_URL: 'https://example.invalid',
    String: String, Object: Object, Array: Array,
  };
  vm.createContext(ctx);
  vm.runInContext(chunkParse[0] + '\n' + chunkAbout[0], ctx);
  return { ctx, about, toasts };
}

(async () => {
  {
    const s = sandbox(HEAD + '---PROP---\n' + PROP);
    await s.ctx.loadAbout();
    ok(s.about.innerHTML.indexOf('V1.5') >= 0 && s.about.innerHTML.indexOf('梦笙Mess1on') >= 0,
       '正常标记：关于页显示版本和作者');
    ok(s.toasts.length === 0, '正常标记不弹错误');
  }
  {
    const propNoId = PROP.split('\n').slice(1).join('\n');
    const s = sandbox(HEAD + '--id=soc_freqcap\n' + propNoId);
    await s.ctx.loadAbout();
    ok(s.about.innerHTML.indexOf('V1.5') >= 0 && s.about.innerHTML.indexOf('梦笙Mess1on') >= 0,
       'printf -- 被吃掉、只剩 --id=：从 module.prop 行恢复');
    ok(s.toasts.length === 0, '恢复成功时不弹「没有标记」');
  }
  {
    const s = sandbox(HEAD + PROP);
    await s.ctx.loadAbout();
    ok(s.about.innerHTML.indexOf('V1.5') >= 0, '标记整行丢失：仍从 id= 行恢复');
  }
  {
    const s = sandbox(HEAD);
    await s.ctx.loadAbout();
    ok(s.toasts.length === 1 && s.toasts[0].indexOf('没有模块信息') >= 0,
       'module.prop 也读不到时才报错，且说清原因');
    ok(s.about.innerHTML === '', '报错时不画半截关于页');
  }

  console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
  process.exit(fail ? 1 : 0);
})();
