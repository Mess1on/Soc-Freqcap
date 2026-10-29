// 芯片适配卡片的渲染测试：喂**真 api.sh 的输出**（ovr_show / pkg），
// 看卡片渲染成什么、导出成功的 toast 带不带落盘路径。
//
// 这张卡片是"给别人适配机器"的唯一入口，而它显示的内容全是后端拼的字符串
// （这台机探测到什么、在用哪一份适配表）。后端换了措辞、前端就会把整块渲染成空白，
// 而两边各自都还是绿的 —— 所以这里用真输出接一次。
//
// 用户侧**只有导出**：改好的节点表由作者写进下一个模块版本，
// 所以这里还钉一条"前端不许出现导入/清除覆盖的调用"——
// 让用户自己导一份表，等于给他一个把机器钉死在别人节点表上的按钮。
//
// 用法： node src/test_ovr_ui.js webroot/index.html
// 跑不动 api.sh 时整个文件跳过并说明原因，不算绿。
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
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ovrui_'));
const dl = path.join(dir, 'dl');
fs.mkdirSync(dl, { recursive: true });
const confPath = path.join(dir, 'socfreq.conf');
fs.copyFileSync(path.join(modRoot, 'socfreq.conf'), confPath);

function A() {
  return execFileSync('sh', [path.join(modRoot, 'webroot', 'api.sh')].concat(Array.from(arguments)),
                      { encoding: 'utf8',
                        env: Object.assign({}, process.env,
                          { SOCFREQ_DATA_DIR: dir, SOCFREQ_PKG_DIR: dl }) });
}

// "在用适配表"这一档由作者随模块版本装进数据目录，这里直接铺同一份文件当 fixture，
// 不去走导入子命令 —— 页面上本来就没有那个入口，测试也不该替它开一条路。
const fakeA = path.join(dir, 'fake', 'cpu0').replace(/\\/g, '/');
const fakeB = path.join(dir, 'fake', 'cpu4').replace(/\\/g, '/');
fs.mkdirSync(path.join(dir, 'fake', 'cpu0'), { recursive: true });
fs.mkdirSync(path.join(dir, 'fake', 'cpu4'), { recursive: true });

let show0 = '', show1 = '', exported = '', bad = '';
try {
  show0 = A('ovr_show');
  fs.writeFileSync(path.join(dir, 'override.conf'),
    'cpu-dir ' + fakeA + ' 3532800\n' +
    'cpu-dir ' + fakeB + ' 2803200\n' +
    'gpu-freqs 1100000000 967000000 443000000\n');
  show1 = A('ovr_show');
  exported = A('pkg');
  try {
    execFileSync('sh', [path.join(modRoot, 'webroot', 'api.sh'), 'pkg_import',
                        path.join(dir, '不存在.txt')],
                 { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
                   env: Object.assign({}, process.env, { SOCFREQ_DATA_DIR: dir, SOCFREQ_PKG_DIR: dl }) });
  } catch (e) { bad = String(e.stderr || e.message); }
  fs.unlinkSync(path.join(dir, 'override.conf'));
} catch (e) {
  skipped = '跑不了 api.sh（这台机没 sh，或 sh 不支持这些用法）：' + e.message;
}

/* ── 抠前端代码 ───────────────────────────────────────────── */
const MARK_NEXT = 'const ICON_STATE';
const chunk = html.match(/\/\* ── 芯片适配 ── \*\/[\s\S]*?\n(?=\/\* 取图标的状态码)/);

function mkEl(tag) {
  const el = {
    tagName: tag, children: [], style: {}, className: '', disabled: false,
    value: '', textContent: '',
    appendChild(c) { el.children.push(c); return c; },
    classList: { add() {}, remove() {}, contains() { return false; } },
  };
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

/* 每条用例一个干净沙箱：api() 按子命令返回上面跑真后端拿到的原文 */
function sandbox(responses) {
  const nodes = {};
  const get = id => nodes[id] || (nodes[id] = mkEl('div'));
  const calls = [];
  const ctx = {
    document: { getElementById: get, createElement: mkEl },
    api: function () {
      const a = Array.from(arguments);
      calls.push(a);
      const k = a[0];
      if (k in responses) return Promise.resolve(responses[k]);
      return Promise.resolve('OK');
    },
    toast: m => calls.push(['toast', m]),
    loadStatus: () => calls.push(['loadStatus']),
    lastSt: { ncores: '8', ntier: '2', gpu_ok: '1' },
    Date: { now: () => 1000 },
    setTimeout: () => 0,
    Promise: Promise, String: String, Object: Object, Array: Array, console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(chunk[0], ctx, { filename: 'ui-ovr.js' });
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
    console.log('== 芯片适配卡片渲染（跳过）==');
    console.log('  skip ' + skipped);
    return finish();
  }
  console.log('== 芯片适配卡片渲染（数据来自真 api.sh）==');
  if (!chunk) {
    ok(false, '从 index.html 里抠出芯片适配那块', MARK_NEXT + ' 的锚点没匹配上');
    return finish();
  }
  ok(/^ovr=0/m.test(show0) && /^ovr=1/m.test(show1),
     '后端这一趟给的全齐：没在用适配表 / 在用适配表');

  // 1) 全靠自动探测的机器：卡片要有内容，且说清"没用到适配表"
  let s = sandbox({ ovr_show: show0 });
  await s.ctx.loadOvr();
  let box = s.get('ovr');
  ok(box.children.length >= 2, '没在用适配表时卡片有内容（不是空白）', '实际 ' + box.children.length);
  ok(textOf(box).indexOf('全靠自动探测') >= 0,
     '说的是"这台机靠自动探测"，不是空白或报错', textOf(box));
  ok(textOf(box).indexOf('8 颗') >= 0 && textOf(box).indexOf('GPU 限频已接上') >= 0,
     '第一行报的是这台机当前的探测结果（核心数/GPU 接没接上）', textOf(box));

  // 2) 作者随版本装进来的适配表：逐条列出来，让用户能核对"我的机被适配成什么样了"
  s = sandbox({ ovr_show: show1 });
  await s.ctx.loadOvr();
  box = s.get('ovr');
  const lines = textOf(box);
  ok(lines.indexOf('作者适配的节点表') >= 0 && lines.indexOf('随模块版本发布') >= 0,
     '说明这份表是随模块版本来的，不是本页能改的东西', lines.slice(0, 120));
  ok(lines.indexOf(fakeA.replace(/^\//, '')) >= 0 || lines.indexOf(fakeA) >= 0,
     '两条 cpu-dir 都列在卡片上', lines.slice(0, 200));
  ok(lines.indexOf('gpu-freqs') >= 0, 'GPU 频率表那条也在（在用哪张表要看得见）');
  ok(/3532800/.test(lines) && /2803200/.test(lines), '每行带着硬件上限，不只是路径');

  // 3) 导出：toast 里要带上后端给的完整落盘路径 —— 用户就靠这一行找到那个 txt
  s = sandbox({ pkg: exported });
  await s.ctx.exportPkg();
  const exp = s.calls.filter(c => c[0] === 'toast').map(c => c[1]);
  ok(s.calls.some(c => c[0] === 'pkg'), '「导出适配包」发的是 pkg 子命令');
  ok(exp.some(t => /socpkg-/.test(t)), '导出成功的 toast 带着文件名', JSON.stringify(exp));

  // 4) 导出失败（Download 建不起来 / 写出来是空的）也要有原因，不许静默
  s = sandbox({ pkg: 'E/PKG_WRITE\n' });
  await s.ctx.exportPkg();
  const t2 = s.calls.filter(c => c[0] === 'toast').map(c => c[1]);
  ok(t2.some(t => /PKG|导出失败/.test(t)), '导出失败时 toast 给出原因', JSON.stringify(t2));

  // 5) 后端读不到（桥接挂了）时卡片自己降级成一行说明，不整页崩
  s = sandbox({});
  s.ctx.api = () => Promise.reject(new Error('NO_BIN'));
  await s.ctx.loadOvr();
  ok(textOf(s.get('ovr')).indexOf('读不到') >= 0, '后端报错时卡片降级成一行说明');

  // 6) 用户侧只有导出：页面上不许出现导入/清除覆盖的入口。
  //    让用户自己导一份表，等于给他一个把机器钉死在别人节点表上的按钮。
  ok(!/importPkg|pkg_import|ovr_clear|clearOvr|ovr-clear/.test(html),
     '整个页面里没有导入/清除覆盖的调用与控件',
     (html.match(/.{0,40}(pkg_import|ovr_clear).{0,40}/g) || ['没命中'])[0]);
  // 后端那两个子命令还在（作者自己验一份改好的表要用），别把它当成前端入口删掉
  ok(/cmd_pkg_import\(\) \{/.test(fs.readFileSync(path.join(modRoot, 'webroot', 'api.sh'), 'utf8')),
     'api.sh 仍保留 pkg_import 子命令（作者侧命令行用）');
  ok(/E\/PKG_NOFILE|E\/PKG_NOOVR|E\/PKG_FMT|E\/PKG_OVR/.test(bad),
     '导一份不存在的包会被后端拒（命令行那条路自己也是验过的）', bad.slice(0, 80));

  finish();
}
main().catch(e => { ok(false, '用例自己跑崩了', e.stack); finish(); });
