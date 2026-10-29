// 搜索匹配：系统应用的名字常是英文（相机 = Camera），中文查询要能命中。
// 只测匹配函数，不碰 DOM —— 显示名不改，别替厂商翻译。
//
// 用法： node src/test_apps_search.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2] || 'webroot/index.html', 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

const chunk = html.match(/const SEARCH_ALIAS = \{[\s\S]*?\nfunction searchHit\(p, terms\)\{[\s\S]*?\n\}/);
if (!chunk) {
  console.log('  FAIL 页面里找不到 SEARCH_ALIAS / searchTerms / searchHit');
  process.exit(1);
}

const ctx = {
  LABELS: {
    'com.oplus.camera': 'Camera',
    'com.android.settings': 'Settings',
    'com.tencent.tmgp.sgame': '王者荣耀',
  },
  console, Object, Array, String, Math, Number, RegExp,
};
vm.createContext(ctx);
vm.runInContext(chunk[0], ctx);

function hit(pkg, q) {
  const ql = q.trim().toLowerCase();
  if (!ql) return true;
  return ctx.searchHit(pkg, ctx.searchTerms(ql));
}

console.log('== 中文搜英文名的系统应用 ==');
ok(hit('com.oplus.camera', '相机'), '搜「相机」命中 Camera');
ok(hit('com.oplus.camera', '相'), '打一半「相」也命中（按别名前缀放宽）');
ok(hit('com.android.settings', '设置'), '搜「设置」命中 Settings');
ok(hit('com.android.settings', 'settings'), '英文查询照旧命中');
ok(hit('com.oplus.camera', 'camera'), '包名/英文名查询照旧命中');
ok(hit('com.tencent.tmgp.sgame', '王者'), '已取到中文名的应用按名字命中');

console.log('\n== 不误伤 ==');
ok(!hit('com.oplus.camera', '设置'), '「设置」不会把所有应用都带出来');
ok(!hit('com.android.settings', '相机'), '「相机」不会命中设置');
ok(!hit('com.tencent.mm', '相册'), '没有这个名字/别名的包不命中');
ok(hit('com.example.game', 'game'), '普通英文子串仍然有效');

console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
process.exit(fail ? 1 : 0);
