// 桥接测试：把 index.html 里的 exec() 抠出来，逐个喂各种 root 管理器。
//
// 为什么需要：原版只认 ksu.exec。新版 KernelSU Manager 注入的是 kernelsu，
// 于是整页显示"管理器不支持"，而守护完全正常。MM / APatch 同理。
// 这类"只在一个平台上能跑"的分支，光读代码看不出来，必须逐个平台喂桩。
//
// 用法：
//   node src/test_bridge.js webroot/index.html          各管理器桥接
//   node src/test_bridge.js webroot/index.html --api    模块根探测与失败提示
const fs = require('fs');
const vm = require('vm');

global.window = global;
const html = fs.readFileSync(process.argv[2], 'utf8');

// 抠出 MODID 到 exec() 结束（含 q() 与 ROOTS）
const m = html.match(/const MODID = [\s\S]*?function exec\(cmd\)\{[\s\S]*?\n\}/);
if (!m) { console.log('FAIL 没能从 index.html 里找到 exec()'); process.exit(1); }
const apiChunk = html.match(/async function api\(\)\{[\s\S]*?\n\}/);
if (!apiChunk) { console.log('FAIL 没能找到 api()'); process.exit(1); }

// setDead() 是桥接失败时的常驻提示条。exec()/api() 现在都会调它，
// 所以抠出来的代码片段必须把它一起带上 —— 少带就是
// "ReferenceError: setDead is not defined"，而那会被 exec 自己的
// try/catch 吞掉，测试拿到的是错得离谱的 stderr，看起来像页面坏了。
const deadChunk = html.match(/function setDead\(kind, detail\)\{[\s\S]*?\n\}/);
if (!deadChunk) { console.log('FAIL 没能找到 setDead()（常驻提示条的实现）'); process.exit(1); }

let pass = 0, fail = 0;
function ok(c, n, e) {
  if (c) { console.log('  ok   ' + n); pass++; }
  else { console.log('  FAIL ' + n + (e ? ': ' + e : '')); fail++; }
}

async function runBridge() {
  /* setDead 要一起抠进来，并且和 exec 放在**同一个 eval 作用域**里：
   * 它读的是同一份 ROOTS，分开 eval 的话第二个作用域看不到第一个的
   * const ROOTS，noapi 那支会 ReferenceError —— 而那条分支恰恰只在
   * "桥通了但 api.sh 找不到"时走，正是最该测的时候。 */
  const dead = html.match(/function setDead\(kind, detail\)\{[\s\S]*?\n\}/);
  if (!dead) { console.log('FAIL 没能找到 setDead()'); process.exit(1); }
  const dom = {};
  const el = (id) => (dom[id] ||= { id, style: {}, textContent: '', innerHTML: '' });
  global.document = { getElementById: el };
  global.timers = [7, 8];
  global.DEAD = null;
  global.clearInterval = (id) => { (global.__cleared ||= []).push(id); };
  eval(m[0] + '\n' + dead[0] +
       '\nglobal.exec = exec; global.q = q; global.__ROOTS = ROOTS; global.setDead = setDead;');

  // 0) 新版 KernelSU Manager：注入的是 kernelsu，不是 ksu。
  //    返回值可能是 string，也可能是 {code,stdout,stderr}，两种都要认。
  global.kernelsu = { exec() { return Promise.resolve('state=normal\nver=1.0'); } };
  let r = await exec('sh api.sh status');
  ok(r.errno === 0 && /state=normal/.test(r.stdout), 'kernelsu（新版 KSU）返回 string', JSON.stringify(r));

  global.kernelsu = { exec() { return Promise.resolve({ code: 0, stdout: 'state=normal', stderr: '' }); } };
  r = await exec('sh api.sh status');
  ok(r.errno === 0 && r.stdout === 'state=normal', 'kernelsu 返回 {code,stdout,stderr}', JSON.stringify(r));

  global.kernelsu = { exec() { return Promise.reject(new Error('boom')); } };
  r = await exec('sh api.sh status');
  ok(r.errno !== 0 && /boom/.test(r.stderr), 'kernelsu reject 能传出原因', JSON.stringify(r));

  // kernelsu 必须排在 ksu 前面：两个都在时命中 kernelsu
  let hit = '';
  global.kernelsu = { exec() { hit = 'kernelsu'; return Promise.resolve('ok'); } };
  global.ksu = { exec(c, o, id) { hit = 'ksu'; window[id](0, 'ok', ''); } };
  r = await exec('x');
  ok(hit === 'kernelsu', 'kernelsu 优先于 ksu', hit);
  delete global.ksu;
  delete global.kernelsu;

  // 1) 旧版 KernelSU / APatch：回调式 (errno, stdout, stderr)
  global.ksu = { exec(cmd, opts, id) { setTimeout(() => window[id](0, 'state=normal\nver=1.0', ''), 5); } };
  r = await exec('sh api.sh status');
  ok(r.errno === 0 && /state=normal/.test(r.stdout), 'KernelSU 回调式桥接', JSON.stringify(r));

  // 2) MM 管理器：Promise<string>，无 errno
  delete global.ksu;
  global.mm = { exec() { return Promise.resolve('state=normal\nver=1.0'); } };
  r = await exec('sh api.sh status');
  ok(r.errno === 0 && /state=normal/.test(r.stdout), 'MM 管理器桥接', JSON.stringify(r));

  // 3) MM 失败语义：api.sh 的 die() 打 E/<码>，不是 ERR:
  global.mm = { exec() { return Promise.resolve('E/NO_DAEMON: 守护程序不存在'); } };
  r = await exec('sh api.sh status');
  ok(r.errno !== 0 && /守护/.test(r.stderr), 'MM 的 E/ 错误能传出原因', JSON.stringify(r));

  // 3b) 空输出也必须算失败。api.sh 每个子命令都有话说，
  // 一片空白只可能是它没跑到这里；当成成功就会显示成"未运行"，
  // 而用户什么都没做错。
  global.mm = { exec() { return Promise.resolve('   \n  '); } };
  r = await exec('sh api.sh status');
  ok(r.errno !== 0, 'MM 空输出算失败（不是成功）', JSON.stringify(r));

  /* 3b-2) 但列表类命令的空是合法结果。
   * 真机就是这样坏的：装机模板 app_modes.conf 全是注释，
   * modes_list 输出为空、退出码 0，页面却弹"执行失败"——
   * 用户在 MM 那类没有退出码的桥上根本区分不出"空"和"挂了"，
   * 只能靠命令名。所以这一半也必须钉住。 */
  global.mm = { exec() { return Promise.resolve(''); } };
  for (const c of ['modes_list', 'games_list', 'games_suggest', 'icon']) {
    r = await exec('sh api.sh ' + c + ' com.x');
    ok(r.errno === 0, 'MM 空输出对 ' + c + ' 不算失败（空列表是正常状态）', JSON.stringify(r));
  }
  // 而 E/ 前缀对哪个命令都还是失败——白名单不能把真错误也吞了
  global.mm = { exec() { return Promise.resolve('E/WRITE'); } };
  r = await exec('sh api.sh modes_list');
  ok(r.errno !== 0, 'E/ 前缀即使在空列表命令上也算失败', JSON.stringify(r));
  global.mm = { exec() { return Promise.resolve(''); } };
  r = await exec('sh api.sh conf_get');
  ok(r.errno !== 0, 'conf_get 的空仍然算失败（它必然有输出）', JSON.stringify(r));

  // 3c) 旧判据 ERR: 不能再被当成错误标记：api.sh 根本不产出这个前缀，
  //     留着它就等于所有失败都走"成功"分支。
  global.mm = { exec() { return Promise.resolve('ERR: 守护程序不存在'); } };
  r = await exec('sh api.sh status');
  ok(r.errno === 0, 'ERR: 前缀不再被当成失败标记（api.sh 用 E/）', JSON.stringify(r));

  // 4) MM reject
  global.mm = { exec() { return Promise.reject(new Error('denied')); } };
  r = await exec('sh api.sh status');
  ok(r.errno !== 0 && /denied/.test(r.stderr), 'MM reject 能传出原因', JSON.stringify(r));

  // 5) 完全没有桥接：不能静默，要说清楚是管理器的问题
  delete global.mm;
  r = await exec('x');
  ok(r.errno !== 0 && /管理器/.test(r.stderr), '无桥接时提示明确指向管理器', JSON.stringify(r));

  /* 5b) 同时要把常驻提示条亮起来。
   * 这条是"页面顶部直接显示"那个需求的验收：光有 stderr 的话，
   * 每 5 秒一次失败会把 toast 刷成瀑布，用户只看到一闪一闪，
   * 不知道整台机器的这个管理器就是不支持。 */
  ok(dom.nobar && dom.nobar.style.display === '',
     '无桥接时顶部提示条显示出来', JSON.stringify(dom.nobar && dom.nobar.style));
  ok(dom['nobar-t'].textContent === '当前管理器不支持 WebUI，请用 action.sh 命令行操作',
     '措辞与要求逐字一致（不给用户第二种理解）', dom['nobar-t'].textContent);
  ok(dom.tabbar && dom.tabbar.style.display === 'none',
     '标签页藏起来：底下全是拿不到数据的空卡片');
  ok((global.__cleared || []).join(',') === '7,8',
     '两个轮询定时器都停了（不再每 5 秒白烧一次 root 调用）',
     JSON.stringify(global.__cleared));

  // 5c) 只认第一次：后面每 5 秒还会再失败一次，
  // 如果每次都重写文案，noapi 会把 nobridge 覆盖成另一件事。
  global.DEAD = null;                     // 重新来一次，专测幂等
  dom['nobar-t'].textContent = '';
  setDead('nobridge'); setDead('noapi');
  ok(dom['nobar-t'].textContent === '当前管理器不支持 WebUI，请用 action.sh 命令行操作',
     'setDead 幂等：第二次调用不会换掉已经显示的原因', dom['nobar-t'].textContent);

  // 6) magisk 分支（未实测，只保证存在时不报错）
  global.magisk = { exec() { return Promise.resolve('state=normal'); } };
  r = await exec('x');
  ok(r.errno === 0 && r.stdout === 'state=normal', 'magisk 分支存在', JSON.stringify(r));
  delete global.magisk;

  // 7) APatch 装在 /data/adb/ap/modules，两个根都要探
  ok(global.__ROOTS.indexOf('/data/adb/modules') >= 0 &&
     global.__ROOTS.indexOf('/data/adb/ap/modules') >= 0,
     'ROOTS 同时含 Magisk 与 APatch 两个根', JSON.stringify(global.__ROOTS));

  // 8) 参数转义：包名里的单引号不能逃出引号
  ok(q("com.foo'bar") === "'com.foo'\\''bar'", '单引号正确转义', q("com.foo'bar"));
  ok(q('a$b`c') === "'a$b`c'", '参数内 $ 和反引号是字面量', q('a$b`c'));

  console.log('\n' + (fail ? `${fail} 项失败 / ${pass} 项通过` : `全部通过 (${pass})`));
  process.exit(fail ? 1 : 0);
}

// 模块根探测 + 失败时界面不许显示残留。
// 这两条都是"失败时显示上次的残留"，属于最坏的 bug：
// 用户被要求截图反馈，那张截图里是假数据，就没法定位。
async function runApi() {
  const store = {};
  const el = (id) => (store[id] ||= { id, _t: '', style: {}, _title: '',
    set textContent(v){ this._t = String(v); }, get textContent(){ return this._t; },
    set className(v){ this._c = v; }, get className(){ return this._c || ''; },
    set title(v){ this._title = v; }, get title(){ return this._title; } });

  // 每条独立用例要一个**全新的**上下文：probed 是模块级状态，
  // 共用一个就会串味（上一条命中置位，下一条就永远走不到探测分支）。
  // 也不能拿 { ...sandbox } 复制——vm.createContext 记在对象的内部槽上，
  // 展开对象属性会把它丢掉，runInContext 直接抛
  // ERR_INVALID_ARG_TYPE: Received an instance of Object。
  const mkSandbox = () => {
    const s = {
      console, setTimeout: (fn, ms) => { if (ms < 10000) fn(); return 0; }, clearTimeout: () => {},
      clearInterval: (id) => { (s.__cleared ||= []).push(id); },
      Math, JSON, String, parseInt, parseFloat, Number, Boolean, RegExp, Array, Object, Error, Promise,
      document: { getElementById: el, querySelectorAll: () => [],
                  createElement: () => el('x'), addEventListener: () => {} },
      // setDead 依赖的两个模块级变量
      timers: [1, 2], DEAD: null,
    };
    s.window = s;
    vm.createContext(s);
    return s;
  };
  const sandbox = mkSandbox();

  // probed / probeP / probeOwner 的声明和 apiOut() 在 api() 附近，一并抠进来；
  // 少一样就是 ReferenceError，会被外层 try/catch 伪装成"探测失败"。
  const probedDecl = html.match(/let probed = false;[\s\S]*?let probeOwner = '';/);
  if (!probedDecl) { console.log('FAIL 没找到 probed/probeP/probeOwner 声明'); process.exit(1); }
  const apiOutFn = html.match(/function apiOut\(r\)\{[\s\S]*?\n\}/);
  if (!apiOutFn) { console.log('FAIL 没找到 apiOut()'); process.exit(1); }

  /* 把 exec / api / bridgeName / setDead / apiOut / 探测状态放进同一个上下文：
   * 它们是互相引用的（api 调 setDead，setDead 读 ROOTS），
   * 少一样就是运行时 ReferenceError，而那会被外层 try/catch 伪装成
   * "探测失败"，用例照样绿，验的却根本不是它宣称的那件事。 */
  const bridgeName = html.match(/function bridgeName\(\)\{[\s\S]*?\n\}/);
  const SRC = m[0] + '\n' + bridgeName[0] + '\n' + apiOutFn[0] + '\n' +
              apiChunk[0] + '\n' +
              deadChunk[0] + '\n' + probedDecl[0] +
              '\nglobalThis.api = api; globalThis.bridgeName = bridgeName;' +
              '\nglobalThis.setDead = setDead;';
  vm.runInContext(SRC, sandbox);

  // 1) 两个根都探不到：必须抛，且说清楚试过哪些根
  sandbox.kernelsu = { exec: () => Promise.resolve('') }; // 桥通，但 ping 没返回 OK
  let threw = '';
  try { await vm.runInContext('api("status")', sandbox); }
  catch (e) { threw = String(e && e.message || e); }
  ok(/找不到 api\.sh/.test(threw), '探不到模块根时抛明确错误', threw);
  ok(threw.includes('/data/adb/modules') && threw.includes('/data/adb/ap/modules'),
     '错误里列出试过的两个根', threw);
  // 失败后 probed 必须仍是 false，否则后续请求继续撞错路径且不再报错
  ok(vm.runInContext('probed', sandbox) !== true,
     '失败后 probed 仍为 false', String(vm.runInContext('probed', sandbox)));

  /* 提示条要写"位置不对"，不能写"这个管理器不支持"。
   * 两种失败的处置完全相反：前者是模块根没探到（报路径就能修），
   * 后者是管理器根本没注入接口（只能改用命令行）。
   * 合成一句"WebUI 不可用"的话，用户会去换管理器，而真正的问题是路径。 */
  ok(store['nobar'] && store['nobar'].style.display === '',
     'api.sh 找不到时同样亮出常驻提示条', JSON.stringify(store['nobar'] && store['nobar'].style));
  ok(/api\.sh/.test(store['nobar-t'].textContent) &&
     !/不支持 WebUI/.test(store['nobar-t'].textContent),
     '文案说的是"找不到 api.sh"，不是"管理器不支持"', store['nobar-t'].textContent);
  ok(store['nobar-s'].innerHTML.includes('/data/adb/modules') &&
     store['nobar-s'].innerHTML.includes('/data/adb/ap/modules'),
     '副标题列出试过的两个根（用户能直接照着回报路径）', store['nobar-s'].innerHTML);

  /* 2) 命中才置位。
   *
   * 探根和目标命令现在合并在一次 exec 里，桩要按真实输出形态回：
   * 第一行 @@ROOT=/data/adb/modules，后面才是目标命令的正文。
   * 只回 PONG 或只回 status 都不对——前者没有标记，后者看不出根。
   */
  sandbox.kernelsu = { exec: (c) => Promise.resolve(
    /@@ROOT=/.test(c) ? '@@ROOT=/data/adb/modules\nver=1.0\nalive=1'
                      : 'ver=1.0\nalive=1' ) };
  const out = await vm.runInContext('api("status")', sandbox);
  ok(/ver=1\.0/.test(out), '探到根后正常调用', out);
  ok(vm.runInContext('probed', sandbox) === true, '成功后 probed 置位');
  ok(vm.runInContext('API', sandbox).includes('/data/adb/modules'),
     'API 指向命中的根', vm.runInContext('API', sandbox));

  /* 3) 反向：回执不是 PONG 就必须继续失败。
   * 只测命中不测不命中，probed 的另一条分支依然没人走。 */
  {
    const sb2 = mkSandbox();
    vm.runInContext(SRC, sb2);
    sb2.kernelsu = { exec: () => Promise.resolve('OK') };   // 故意给错回执
    let t2 = '';
    try { await vm.runInContext('api("status")', sb2); }
    catch (e) { t2 = String(e && e.message || e); }
    ok(/找不到 api\.sh/.test(t2),
       '回执不是 PONG 时视为探不到（不能把 OK 当成命中）', t2);
    ok(vm.runInContext('probed', sb2) !== true, '回执不对时不置位 probed');
  }

  /* 4) APatch 根：标记里回 ap/modules 时必须按它置位 API。
   * shell 里的两轮循环由真机命令单独验（见台架记录）；这里验的是
   * JS 拿到 @@ROOT 后不再写死 /data/adb/modules。 */
  {
    const sb3 = mkSandbox();
    vm.runInContext(SRC, sb3);
    sb3.kernelsu = { exec: (c) => Promise.resolve(
      /@@ROOT=/.test(c) ? '@@ROOT=/data/adb/ap/modules\nver=1.0\nalive=1'
                        : 'ver=1.0\nalive=1' ) };
    const o3 = await vm.runInContext('api("status")', sb3);
    ok(/ap\/modules/.test(vm.runInContext('API', sb3)),
       '标记回 APatch 根时选中 APatch 根', vm.runInContext('API', sb3));
    ok(/ver=|alive=/.test(o3), '选中之后真的调通了', o3);
    ok(vm.runInContext('probed', sb3) === true, 'APatch 根命中后置位 probed');
  }

  /* 5) 管理器第一条 exec 偶发空回执：同一轮里重试一次就该成功，
   * 不能等 5 秒后的下一次轮询（那种表现是"先报无法连接，过 5 秒自己好了"）。 */
  {
    const sb4 = mkSandbox();
    vm.runInContext(SRC, sb4);
    let n = 0;
    sb4.kernelsu = { exec: (c) => {
      n++;
      return Promise.resolve(n === 1 ? ''
        : (/@@ROOT=/.test(c) ? '@@ROOT=/data/adb/modules\nver=1.0\nalive=1' : 'ver=1.0\nalive=1'));
    } };
    const o4 = await vm.runInContext('api("status")', sb4);
    ok(/ver=1\.0/.test(o4) && n === 2,
       '第一条 exec 空回执时同轮重试一次', `calls=${n} out=${o4}`);
  }

  console.log('\n' + (fail ? `${fail} 项失败 / ${pass} 项通过` : `全部通过 (${pass})`));
  process.exit(fail ? 1 : 0);
}

(process.argv[3] === '--api' ? runApi() : runBridge());