// test_mutation.js — 验证自检用例真的能抓到那类 bug。
//
// 目的不是测产品代码（那个在设备上跑），而是证明测试本身有效：
// 把 P0 那个反向判断重新植回去，用例必须失败。
//
// 这里的 C 逻辑是从 src/socfreqd.c 逐行照搬的（C 用 char*，此处用 index 模拟）。
// 之所以敢照搬，是因为它只被用来验证"测试能否分辨对错"，
// 真正的产品自检是 src/test_real.c，它 include 真实源码。

// 逐行照搬 C 版 grab_pkg（与 src/socfreqd.c 一致）
function grabPkg(line) {
  const rel = line.indexOf(" u");
  if (rel < 0) return { rc: -1, out: "" };
  let p = rel + 2;                       // 越过 " u"，落在用户号上
  if (!/[0-9]/.test(line[p] ?? "")) return { rc: -1, out: "" };
  while (/[0-9]/.test(line[p] ?? "")) p++;
  if (line[p] !== " ") return { rc: -1, out: "" };
  p++;
  // 从包名首字符向前数到斜杠，而不是从斜杠往回扫
  let l = 0;
  while (p + l < line.length && line[p + l] !== "/") l++;
  if (p + l >= line.length) return { rc: -1, out: "" };   // 没找到斜杠
  if (l === 0) return { rc: -1, out: "" };
  for (let i = 0; i < l; i++) {
    const c = line[p + i];
    if (!/[A-Za-z0-9._]/.test(c)) return { rc: -1, out: "" };
  }
  return { rc: 0, out: line.slice(p, p + l) };
}

// buggy=true 是把返回值判断反过来的版本：成功反而被清空
function fgFromStream(text, buggy) {
  let focus = "", app = "", top = "";
  for (const line of text.split("\n")) {
    if (!focus && line.includes("mCurrentFocus")) {
      const r = grabPkg(line);
      if (buggy ? r.rc === 0 : r.rc !== 0) focus = ""; else focus = r.out;
    } else if (!app && line.includes("mFocusedApp")) {
      const r = grabPkg(line);
      if (buggy ? r.rc === 0 : r.rc !== 0) app = ""; else app = r.out;
    } else if (!top && line.includes("mTopFullscreenOpaqueWindowState")) {
      const r = grabPkg(line);
      if (buggy ? r.rc === 0 : r.rc !== 0) top = ""; else top = r.out;
    }
  }
  let pick = focus || app || top;
  if (pick && top && ["com.omarea.vtools", "bin.mt.plus", "bin.mt.plus.canary"].includes(pick))
    pick = top;
  return pick ? { rc: 0, out: pick } : { rc: -1, out: "" };
}

const CASES = [
  ["单屏常规",
   "  mCurrentFocus=Window{a1b2c3 u0 com.tencent.tmgp.sgame/com.tencent.tmgp.sgame.MainActivity}",
   "com.tencent.tmgp.sgame"],
  ["多用户 u10",
   "  mCurrentFocus=Window{a1b2c3 u10 com.android.chrome/com.google.android.apps.chrome.Main}",
   "com.android.chrome"],
  ["活动名与包名不同",
   "  mCurrentFocus=Window{deadbeef u0 com.foo.bar.app/com.foo.bar.app.Main}",
   "com.foo.bar.app"],
  ["解析成功必须保留结果",
   "  mCurrentFocus=Window{cafe u0 com.probe.must.survive/com.probe.must.survive.A}",
   "com.probe.must.survive"],
  ["两位用户码 u99",
   "  mCurrentFocus=Window{a1b2c3 u99 com.multiuser.app/.Main}",
   "com.multiuser.app"],
  ["下划线与数字包名",
   "  mCurrentFocus=Window{a1b2c3 u0 com.garena.game_2024/com.garena.game_2024.A}",
   "com.garena.game_2024"],
  ["focus 优先于 focusedApp",
   "  mFocusedApp=AppWindowToken{a1b2c3 u0 com.ignored.app/.A}\n  mCurrentFocus=Window{a1b2c3 u0 com.wins.focus/.Main}",
   "com.wins.focus"],
  ["focus=null 退回 focusedApp",
   "  mCurrentFocus=null\n  mFocusedApp=AppWindowToken{a1b2c3 u0 com.fallback.app/.Main}",
   "com.fallback.app"],
  ["双缺退回全屏窗口",
   "  mCurrentFocus=null\n  mTopFullscreenOpaqueWindowState=Window{a1b2c3 u0 com.fullscreen.game/.Main}",
   "com.fullscreen.game"],
  ["双屏取第一个全屏窗口",
   "  mTopFullscreenOpaqueWindowState=Window{a1b2c3 u0 com.first.display/.Main}\n  mTopFullscreenOpaqueWindowState=Window{a1b2c3 u0 com.second.display/.Main}",
   "com.first.display"],
  ["浮窗遮挡时取下层窗口",
   "  mCurrentFocus=Window{a1b2c3 u0 com.omarea.vtools/.FloatingWindow}\n  mTopFullscreenOpaqueWindowState=Window{a1b2c3 u0 com.real.game/.Main}",
   "com.real.game"],
  ["MT 浮窗遮挡时取下层窗口",
   "  mCurrentFocus=Window{a1b2c3 u0 bin.mt.plus/.FloatWin}\n  mTopFullscreenOpaqueWindowState=Window{a1b2c3 u0 com.mt.game/.Main}",
   "com.mt.game"],
];

const FAIL_CASES = [
  ["空输入", ""],
  ["无任何前台信息", "  mCurrentFocus=null\n  mFocusedApp=null"],
  ["窗口名无斜杠", "  mCurrentFocus=Window{a1b2c3 u0 StatusBar}"],
];

function run(buggy) {
  let pass = 0, fail = 0;
  const detail = [];
  for (const [name, text, want] of CASES) {
    const r = fgFromStream(text, buggy);
    if (r.rc === 0 && r.out === want) pass++;
    else { fail++; detail.push(`    ${name}: got=[${r.out}] want=[${want}]`); }
  }
  for (const [name, text] of FAIL_CASES) {
    const r = fgFromStream(text, buggy);
    if (r.rc !== 0) pass++;
    else { fail++; detail.push(`    ${name}: 应当失败却得到 [${r.out}]`); }
  }
  return { pass, fail, detail };
}

console.log("== 变异测试：证明用例能抓到 P0 ==\n");
const good = run(false);
console.log(`修复后的代码: 通过 ${good.pass} / 失败 ${good.fail}`);
good.detail.forEach(d => console.log(d));

const bad = run(true);
console.log(`\n植入原 bug 后: 通过 ${bad.pass} / 失败 ${bad.fail}`);
bad.detail.forEach(d => console.log(d));

console.log("\n== 结论 ==");
if (good.fail !== 0) {
  console.log("FAIL 修复后的代码就没通过，自检用例本身有问题");
  process.exit(1);
}
if (bad.fail === 0) {
  console.log("FAIL 植入 bug 后仍然全过 —— 这些用例抓不到 P0，必须重写");
  process.exit(1);
}
console.log(`PASS 用例有效：${bad.fail} 条在 bug 版本上失败`);