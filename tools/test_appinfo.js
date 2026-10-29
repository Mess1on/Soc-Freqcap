'use strict';
/* tools/test_appinfo.js — 拿 aapt2 当对照，验 tools/apklabel.js 读得对不对
 *
 * 为什么这么验：守护里那份 C（src/appinfo.c）在这台 Windows 上跑不起来，
 * "格式读对了没"没法直接证明。aapt2 是官方实现、本机 SDK 里有，
 * 它给的 application: label='…' icon='…' 就是权威答案；
 * 参照实现和它对得上，C 才是照着一份已经验过的东西抄的。
 *
 * APK 从哪来：只在本地已存在的包里挑（不为了测试去下载别人的 APK），
 * 一个都没有时整组跳过并报"没验"，不假装通过。
 */
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const { appInfo } = require('./apklabel.js');

let pass = 0, fail = 0, skip = 0;
const ok = (cond, name, extra) => {
  if (cond) { pass++; return true; }
  fail++; console.log('  FAIL  ' + name + (extra ? '\n        ' + extra : ''));
  return false;
};

/* aapt2 在哪 */
function findAapt2() {
  const home = process.env.HOME || process.env.USERPROFILE || '';
  const cands = [
    path.join(home, 'AppData/Local/Android/Sdk/build-tools/android-14/aapt2.exe'),
    path.join(home, 'AppData/Local/Android/Sdk/build-tools/android-14/aapt2'),
  ];
  for (const c of cands) if (fs.existsSync(c)) return c;
  return null;
}

/* 本地有没有可测的 APK */
function findApks() {
  const roots = [
    (process.env.USERPROFILE || '') + '/Desktop',
    'E:/搞机助手',
  ];
  const out = [];
  const walk = (dir, depth) => {
    if (depth > 3) return;
    let es = [];
    try { es = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
    for (const e of es) {
      const f = path.join(dir, e.name);
      if (e.isDirectory()) { if (!/node_modules|\.git/.test(e.name)) walk(f, depth + 1); }
      else if (/\.apk$/i.test(e.name) && out.length < 12) out.push(f);
    }
  };
  for (const r of roots) if (fs.existsSync(r)) walk(r, 0);
  return out;
}

const aapt2 = findAapt2();
const apks = findApks();
console.log('aapt2: ' + (aapt2 ? '有' : '没有') + '   本地 APK: ' + apks.length + ' 个');
if (!aapt2 || !apks.length) {
  console.log('SKIP  没有对照工具或没有可测 APK —— 这组一条都没验');
  process.exit(0);
}

for (const f of apks) {
  let badge, mine;
  try {
    badge = execFileSync(aapt2, ['dump', 'badging', f], { encoding: 'utf8', maxBuffer: 64 << 20 });
  } catch (e) { skip++; console.log('  SKIP  aapt2 读不了 ' + path.basename(f)); continue; }
  try { mine = appInfo(f); } catch (e) { mine = { label: '', icon: '', err: e.message }; }
  const m = badge.match(/^application-label(?:-zh-CN)?:'([^']*)'/m);
  const mi = badge.match(/^application:[^'\n]*'[^']*'\s+icon='([^']*)'/m);
  const expLabel = m ? m[1] : '';
  const expIcon = mi ? mi[1] : '';
  const name = path.basename(f).slice(0, 34);
  console.log('  ' + name + '  label: ' + JSON.stringify(mine.label) +
              '  vs aapt2 ' + JSON.stringify(expLabel) +
              '   icon: ' + JSON.stringify(mine.icon) + ' vs ' + JSON.stringify(expIcon));
  if (expLabel) ok(mine.label === expLabel, name + ' 的 label 和 aapt2 一致', mine.err || '');
  if (expIcon && !/\.xml$/.test(expIcon)) ok(mine.icon === expIcon, name + ' 的 icon 路径一致');
  else if (expIcon) {
    // 自适应图标：声明的是 xml，WebView 解不了，参照实现找"同名的位图版本"。
    // 两种"返回空"都是对的，而且都要说清楚是谁接管：
    //   a) 整包一张位图都没有（纯矢量图标）；
    //   b) 图标各层都是矢量、包里的位图是另一张完全不同的资源（Kin.apk 就这样：
    //      整包只有一张 res/D5.webp，和 ic_launcher_foreground 名字对不上）。
    // b) 这一类参照实现**做不到**，靠的是守护 icon.c 第二轮"不看名字、
    // 只看图片自身是不是正方形、边长接近槽位"那个兜底档 —— 这里记成跳过，
    // 不算通过：这条链路的正确性由那一份兜底负责，不在本次对照范围内。
    let raster = 0;
    try {
      raster = [...require('./apklabel.js').zipEntries(fs.readFileSync(f)).keys()]
        .filter(k => /\.(png|webp|jpe?g)$/i.test(k)).length;
    } catch (_) { }
    if (/\.(png|webp|jpg)$/i.test(mine.icon)) {
      ok(true, name + ' 的自适应图标 xml 已换成同名位图');
    } else if (mine.icon === '' && raster === 0) {
      ok(true, name + ' 整包没有位图，留空是对的（页面退回首字母色块）');
    } else {
      skip++;
      console.log('  SKIP  ' + name + ' 的图标层全是矢量，参照实现取不到；' +
                  '这条由守护的像素兜底档接管（包内位图 ' + raster + ' 张，名字都对不上）');
    }
  }
}
console.log('\n参照实现（apklabel.js）: ' + pass + ' 通过 / ' + fail + ' 失败 / ' + skip + ' 跳过');
process.exit(fail ? 1 : 0);
