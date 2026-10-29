// 应用页名字/图标的行为测试：
//   1) labels 串行化：同一页三批点名不能同时进后端（后端临时文件曾互相覆盖）
//   2) 图标批量失败/缺行不能记成"这包没图"，要放回队列重试
//   3) 只有后端明确回空行才缓存"没有图标"
//   4) 一批最多 8 个，防止未缓存图标撞 12 秒超时
//
// 用法： node src/test_icons_ui.js webroot/index.html
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2] || 'webroot/index.html', 'utf8');

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}
const tick = () => new Promise(r => setTimeout(r, 0));

const iconChunk = (html.match(/\nconst ICONS = \{\};[\s\S]*?\n\}\n\nasync function loadGames\(/) || [''])[0]
  .replace(/\n\nasync function loadGames\($/, '');
const labelChunk = (html.match(/\nconst LABELS = \{\};[\s\S]*?\n\}\nfunction applyLabel\(/) || [''])[0]
  .replace(/\nfunction applyLabel\($/, '');
if (iconChunk.length < 500 || labelChunk.length < 300) {
  console.log('  FAIL 页面里找不到图标/名字那两段');
  process.exit(1);
}

function ph() {
  return { isConnected: true, replaced: null, replaceWith(x) { this.replaced = x; } };
}

function iconSandbox(respond) {
  const calls = [];
  const ctx = {
    api: function () {
      const a = Array.prototype.slice.call(arguments);
      calls.push(a);
      return respond(a);
    },
    document: {
      createElement: () => ({ className: '', alt: '', src: '', title: '', onerror: null }),
      querySelectorAll: () => [],
    },
    // 由测试手动调 flushIcons，别让 40ms / 0ms 的定时器插进来。
    setTimeout: () => 0,
    clearTimeout: () => {},
    console, Promise, Object, Array, String, JSON, Math, Number, RegExp,
  };
  vm.createContext(ctx);
  vm.runInContext(iconChunk, ctx);
  const ICONS = vm.runInContext('ICONS', ctx);
  const ICON_Q = vm.runInContext('ICON_Q', ctx);
  return { ctx, calls, ICONS, ICON_Q };
}

function labelSandbox(respond) {
  const calls = [];
  const painted = [];
  const ctx = {
    api: function () {
      const a = Array.prototype.slice.call(arguments);
      calls.push(a);
      return respond(a);
    },
    document: { querySelectorAll: () => [] },
    paintLabels: () => painted.push(1),
    console, Promise, Object, Array, String, JSON, Math, Number, RegExp,
  };
  vm.createContext(ctx);
  vm.runInContext(labelChunk, ctx);
  const LABELS = vm.runInContext('LABELS', ctx);
  return { ctx, calls, LABELS, painted };
}

(async () => {
  console.log('== 图标批量 ==');
  {
    // 第一次调用失败：不能把包记成"没有图"，要留着重试
    let n = 0;
    const s = iconSandbox(() => {
      if (n++ === 0) return Promise.reject(new Error('timeout'));
      return Promise.resolve('com.a\tdata:image/png;base64,AAA\n');
    });
    const p = ph();
    s.ctx.loadIcon('com.a', p);
    await s.ctx.flushIcons();
    ok(s.ICONS['com.a'] === undefined, '调用失败后不写成"没有图"');
    ok(s.ICON_Q['com.a'] !== undefined, '失败的包放回队列');
    await s.ctx.flushIcons();
    ok(s.ICONS['com.a'] === 'data:image/png;base64,AAA', '第二次成功后进缓存');
    ok(p.replaced && p.replaced.src === 'data:image/png;base64,AAA', '成功后占位块换成 img');
  }
  {
    // 回包成功但少了某一行（截断）：缺的那一行要重试，不能当没图
    let n = 0;
    const s = iconSandbox(() => {
      if (n++ === 0) return Promise.resolve('com.a\tdata:image/png;base64,AAA\n');
      return Promise.resolve('com.b\t\n');
    });
    s.ctx.loadIcon('com.a', ph());
    s.ctx.loadIcon('com.b', ph());
    await s.ctx.flushIcons();
    ok(s.ICONS['com.b'] === undefined, '回包里缺行时不写"没有图"');
    await s.ctx.flushIcons();
    ok(s.ICONS['com.b'] === '', '重试后后端明确回空 → 缓存成没有图标');
  }
  {
    // 后端明确回空行：直接缓存，不再重问
    let calls = 0;
    const s = iconSandbox(() => { calls++; return Promise.resolve('com.x\t\n'); });
    s.ctx.loadIcon('com.x', ph());
    await s.ctx.flushIcons();
    ok(s.ICONS['com.x'] === '', '后端回空行 → 会话内记成没有图标');
    await s.ctx.flushIcons();
    ok(calls === 1, '已确认没有图的包不再请求');
  }
  {
    // 一批最多 8 个；剩下的下一轮
    const s = iconSandbox(a => Promise.resolve(
      a.slice(1).map(p => p + '\tdata:image/png;base64,AA').join('\n') + '\n'));
    for (let i = 0; i < 10; i++) s.ctx.loadIcon('com.p' + i, ph());
    await s.ctx.flushIcons();
    ok(s.calls[0].length === 9, '一批最多 8 个包', '实际 ' + (s.calls[0].length - 1));
    await s.ctx.flushIcons();
    ok(s.calls[1] && s.calls[1].length === 3, '剩下的 2 个下一轮取');
  }
  {
    // 缓存命中：直接换图，不再发请求
    let calls = 0;
    const s = iconSandbox(() => { calls++; return Promise.resolve(''); });
    s.ICONS['com.cached'] = 'data:image/png;base64,OLD';
    const p = ph();
    s.ctx.loadIcon('com.cached', p);
    ok(p.replaced && p.replaced.src.indexOf('OLD') > 0 && calls === 0,
       '已有图标直接用，不发 root 调用');
  }

  console.log('\n== 应用名串行化 ==');
  {
    const pending = [];
    const s = labelSandbox(a => new Promise(res => pending.push({ a, res })));
    const p1 = s.ctx.ensureLabels(['com.a']);
    s.ctx.ensureLabels(['com.b']);       // 第一批还在路上，第二批只入队
    await tick();
    ok(s.calls.length === 1 && s.calls[0].join(' ') === 'labels com.a',
       '同一时刻只发一批 labels', JSON.stringify(s.calls));
    pending[0].res('com.a\t应用甲\n');
    await tick(); await tick();
    ok(s.calls.length === 2 && s.calls[1].join(' ') === 'labels com.b',
       '第一批回来后接着取排队的那批');
    pending[1].res('com.b\t应用乙\n');
    await p1;
    ok(s.LABELS['com.a'] === '应用甲' && s.LABELS['com.b'] === '应用乙',
       '两批名字都落到缓存里');
    ok(s.painted.length >= 2, '每批回来都重画一次名字');
  }
  {
    // 失败撤占位，下次还能重试
    let n = 0;
    const s = labelSandbox(() => (n++ === 0 ? Promise.reject(new Error('x'))
                                            : Promise.resolve('com.c\t应用丙\n')));
    await s.ctx.ensureLabels(['com.c']);
    ok(!Object.prototype.hasOwnProperty.call(s.LABELS, 'com.c'),
       '失败后撤掉占位（不是记成"没名字"）');
    await s.ctx.ensureLabels(['com.c']);
    ok(s.LABELS['com.c'] === '应用丙', '下次点名能拿到名字');
  }
  {
    // 已经取过的包不再问
    let calls = 0;
    const s = labelSandbox(() => { calls++; return Promise.resolve('com.d\t应用丁\n'); });
    await s.ctx.ensureLabels(['com.d']);
    await s.ctx.ensureLabels(['com.d']);
    ok(calls === 1, '已取过名字的包不再请求');
  }

  console.log('\n' + (fail === 0 ? `全部通过 (${pass})` : `${fail} 项失败 / ${pass} 项通过`));
  process.exit(fail ? 1 : 0);
})();
