'use strict';
/* tools/apklabel.js — 「应用名 + 桌面图标」的 PC 侧参照实现
 *
 * 为什么要有这么一份：守护里那份 C 实现（src/appinfo.c）在这台 Windows 上
 * 跑不起来 —— NDK 只产 arm64 的安卓二进制，本机没有 host gcc、也没装 WSL。
 * 于是"AXML / resources.arsc 这两个格式我到底读对没有"没法用代码审查确认。
 * 这份能跑，先拿它把偏移和判定钉死，再照着抄进 C；
 * 它同时是回归工具：tools/test_appinfo.js 拿它和 SDK 里的 aapt2 对答案。
 *
 * 它不是产物的一部分，装机包里一个字节都没有它。
 *
 *   node tools/apklabel.js <apk> [<apk> ...]        一行一个包：label<TAB>icon
 *   node tools/apklabel.js <apk> --dump             连中间结构一起打，查格式时用
 */
const fs = require('fs');
const zlib = require('zlib');

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => ((b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0);
const u64 = (b, o) => u32(b, o) + u32(b, o + 4) * 4294967296;

/* ── zip：只按中央目录取条目 ─────────────────────────────────── */
function zipEntries(buf) {
  let eocd = -1;
  const lo = Math.max(0, buf.length - 65557);
  for (let i = buf.length - 22; i >= lo; i--)
    if (u32(buf, i) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('EOCD 没找到');
  let n = u16(buf, eocd + 10), cdsz = u32(buf, eocd + 12), cdoff = u32(buf, eocd + 16);
  if (n === 0xffff || cdsz === 0xffffffff || cdoff === 0xffffffff) {
    let loc = -1;
    for (let i = eocd - 20; i > Math.max(0, eocd - 40); i--)
      if (u32(buf, i) === 0x07064b50) { loc = i; break; }
    if (loc < 0) throw new Error('ZIP64 定位器没找到');
    const zoff = u64(buf, loc + 8);
    if (u32(buf, zoff) !== 0x06064b50) throw new Error('ZIP64 EOCD 魔数不对');
    n = u64(buf, zoff + 32); cdsz = u64(buf, zoff + 40); cdoff = u64(buf, zoff + 48);
  }
  const map = new Map();
  let q = cdoff; const end = cdoff + cdsz;
  while (q + 46 <= end) {
    if (u32(buf, q) !== 0x02014b50) break;
    const method = u16(buf, q + 10), csz = u32(buf, q + 20), usz = u32(buf, q + 24);
    const nlen = u16(buf, q + 28), elen = u16(buf, q + 30), clen = u16(buf, q + 32);
    const name = buf.slice(q + 46, q + 46 + nlen).toString('utf8');
    if (!map.has(name)) map.set(name, { name, off: u32(buf, q + 42), csz, usz, method });
    q += 46 + nlen + elen + clen;
  }
  return map;
}

/* 数据偏移要用**本地头**里的名字/扩展长度：zipalign 会给 STORED 条目的
   本地扩展字段加填充，两边长度不相等，用中央目录的值就读到填充字节。 */
function entryBytes(buf, e) {
  const lh = buf.slice(e.off, e.off + 30);
  if (u32(lh, 0) !== 0x04034b50) throw new Error('本地头魔数不对: ' + e.name);
  const data = e.off + 30 + u16(lh, 26) + u16(lh, 28);
  const raw = buf.slice(data, data + e.csz);
  if (e.method === 0) return raw;
  if (e.method === 8) return zlib.inflateRawSync(raw);
  throw new Error('不支持的压缩方式 ' + e.method);
}

/* ── ResStringPool ───────────────────────────────────────────── */
function readPool(buf, at) {
  if (u16(buf, at) !== 0x0001) throw new Error('不是字符串池 @' + at);
  const hs = u16(buf, at + 2), total = u32(buf, at + 4);
  const count = u32(buf, at + 8), flags = u32(buf, at + 16), sstart = u32(buf, at + 20);
  const utf8 = (flags & 0x100) !== 0;
  const tbl = at + hs, data = at + sstart;
  const out = [];
  for (let i = 0; i < count; i++) {
    let o = data + u32(buf, tbl + i * 4);
    if (o + 2 > at + total) { out.push(''); continue; }
    if (utf8) {
      // 两个变长整数：先"字符数"，再"字节数"；高位 0x80 表示这个数占两字节。
      // **切片必须按第二个数**：中文一个字符 3 字节，
      // 拿字符数去切就会切在半条 UTF-8 序列上，出来一个 Replacement Char
      // （ASCII 串两者相等，所以只在看中文标签时才暴露）。
      const lenAt = q => ((buf[q] & 0x80) ? (((buf[q] & 0x7f) << 8) | buf[q + 1]) : buf[q]);
      const skip = q => ((buf[q] & 0x80) ? 2 : 1);
      o += skip(o);                       // 字符数，读而不用
      const bytes = lenAt(o);
      o += skip(o);
      out.push(buf.slice(o, o + bytes).toString('utf8'));
    } else {
      let chars = u16(buf, o); o += 2;
      if (chars & 0x8000) { chars = ((chars & 0x7fff) << 16) | u16(buf, o); o += 2; }
      out.push(buf.slice(o, o + chars * 2).toString('utf16le'));
    }
  }
  return out;
}

/* ── AndroidManifest.xml（AXML）──────────────────────────────── */
/* 实测到的布局（gnirehtet / 酷安 / Kin 三个包一致）：
 *   文件头 8 字节（type=3, headerSize=8, size）
 *   RES_STRING_POOL_TYPE 0x0001     —— 元素名、属性名、字面量值都在这里面
 *   RES_XML_RESOURCE_MAP_TYPE 0x0180 —— 一组 u32 资源 ID，下标 = 属性名在池里的下标
 *   之后是一串节点：0x0100/0x0101 命名空间，0x0102 开始元素，0x0103 结束元素
 *   节点头 16 字节：chunk 头 8 + lineNumber 4 + comment 4
 *   开始元素的 attrExt 从 +16 起：ns(u32), name(u32), attributeStart(u16),
 *     attributeSize(u16), attributeCount(u16), 然后 2 字节补到 4 的倍数，
 *     再 idIndex / classIndex / styleIndex（各 u32）
 *   属性区从 attrExt+attributeStart 起，每条 attributeSize(=20) 字节：
 *     ns(u32) name(u32) rawValue(u32) ResValue{ size u16, res0 u8, dataType u8, data u32 }
 * 属性名这一栏：它同时是"资源 ID 表的索引"和"字符串池里属性本地名的索引"，
 *   两处下标一致（label / icon / name…），所以既拿得到框架属性 ID，
 *   又拿得到本地名——不需要认 "android" 命名空间。 */
const XML_NODE_START = 0x0102, XML_NODE_END = 0x0103;
/* android: 框架属性的 ID 是 0x01010000 起按 attrs.xml 顺序排的：
 *   theme=…0000, label=…0001, icon=…0002, name=…0003 —— 把 label 记成
 *   0x01010003 就会稳定取到 android:name，页面上显示的是活动类名而不是应用名。
 * 所以这里**不写死数字**：属性本地名（池里就是 "label"/"icon"）才是稳定的判据，
 * 资源 ID 只用来确认"这确实是 android: 命名空间的那个属性"（id 非 0）。 */
const A_LABEL = 'label', A_ICON = 'icon', A_DRAWABLE = 'drawable';
const T_NULL = 0x00, T_REFERENCE = 0x01, T_ATTRIBUTE = 0x02, T_STRING = 0x03;

function parseManifest(ax) {
  if (u16(ax, 0) !== 0x0003) throw new Error('不是编译后的 XML');
  let p = u16(ax, 2);
  let pool = null, resIds = [];
  const nodes = [];
  while (p + 8 <= ax.length) {
    const t = u16(ax, p), total = u32(ax, p + 4);
    if (!total) break;
    if (t === 0x0001) pool = readPool(ax, p);
    else if (t === 0x0180) {
      resIds = [];
      for (let i = 8; i + 4 <= total; i += 4) resIds.push(u32(ax, p + i));
    } else if (t === XML_NODE_START || t === XML_NODE_END) {
      nodes.push({ start: t === XML_NODE_START, at: p });
    }
    p += total;
  }
  if (!pool) throw new Error('AXML 里没有字符串池');
  const els = [];
  const stack = [];
  for (const nd of nodes) {
    if (!nd.start) { stack.pop(); continue; }
    const a = nd.at + 16;
    const name = u32(ax, a + 4);
    // attributeStart / attributeSize / attributeCount 是 **u16**，不是 u32。
    // 按 u32 读会读到相邻字段（实测那三个字段在文件里就是 14 00 14 00 07 00），
    // 算出"0 个属性"，于是每个包都取不到 label —— 一句错话都不报，只剩空白。
    const attrStart = u16(ax, a + 8), attrSize = u16(ax, a + 10) || 20, attrCount = u16(ax, a + 12);
    const el = { tag: pool[name] || '', attrs: [], parent: stack[stack.length - 1] || null, kids: [] };
    if (el.parent) el.parent.kids.push(el);
    stack.push(el);
    for (let i = 0; i < attrCount; i++) {
      const q = a + attrStart + i * attrSize;
      const rawName = u32(ax, q + 4);
      // 一条属性 20 字节：ns(u32) name(u32) rawValue(u32)
      //                  ResValue{ size u16, res0 u8, dataType u8, data u32 }
      // → dataType 在 +15、data 在 +16，不是紧跟 name 的 +8/+11。
      const dtype = ax[q + 15], data = u32(ax, q + 16);
      const id = resIds[rawName] || 0;
      el.attrs.push({ id, key: pool[rawName] || '', dtype, data,
                      str: dtype === T_STRING ? (pool[data] || '') : '' });
    }
    els.push(el);
  }
  return { els, pool };
}

function attrOf(el, key) { return el ? (el.attrs.find(a => a.key === key && a.id) || null) : null; }

/* ── resources.arsc ──────────────────────────────────────────── */
/* 实测布局（gnirehtet=老 aapt、酷安/Kin/WakeUp=aapt2，两边一致）：
 *   ResTable_header : type=2, headerSize=12, size, packageCount(u32 @8)
 *   紧跟一个**全局字符串池**(0x0001) —— label 的文本、drawable 的文件路径都在这里
 *   ResTable_package(0x0200) : headerSize=288
 *     +8   id(u32)          0x7f
 *     +12  name(128×u16)    256 字节
 *     +268 typeStrings(u32) 相对**包块开头**的偏移 → 一个 0x0001 池
 *     +276 keyStrings(u32)  同上
 *   包内 chunk：typeStrings 池、keyStrings 池、然后每个类型
 *     0x0202 = spec（16 字节头：id, entryCount, 然后 entryCount 个 u32）
 *     0x0201 = type，**每个配置一块**，头 84 字节：
 *        +8  id(u32)  类型号
 *        +12 entryCount(u32)
 *        +16 entriesStart(u32)   相对本块开头
 *        +20 config(size u32 打头，这里 64 字节) → 8+4+4+4+64 = 84 = headerSize
 *        之后 entryCount 个 u32 条目偏移（实测自洽：entriesStart-headerSize = 4×entryCount）
 *     条目 = ResTable_entry{size u16, flags u16, key u32} + Res_value{size u16, res0 u8, dataType u8, data u32}
 *
 * spec 块（0x0202）我们不看：它讲的是"每个条目有哪些配置"，取默认配置用不着。
 * dataType=0x03(TYPE_STRING) 的 data 是**全局池**的下标，不是 typeStrings。
 * drawable/mipmap 条目的值也是 TYPE_STRING，内容是文件路径 "res/BWP.png" ——
 * 这正是 aapt2 badging 里 icon= 后面那个串，于是能直接按路径去 zip 里取那张图。
 */
function parseTable(arsc) {
  if (u16(arsc, 0) !== 0x0002) throw new Error('不是 ResTable');
  const pkgs = [];
  let globalPool = null;
  let p = u16(arsc, 2);
  while (p + 8 <= arsc.length) {
    const t = u16(arsc, p), total = u32(arsc, p + 4);
    if (!total) break;
    if (t === 0x0200) {
      const hs = u16(arsc, p + 2);
      pkgs.push({ at: p, size: total, id: u32(arsc, p + 8),
                  typeStrings: p + u32(arsc, p + 268), keyStrings: p + u32(arsc, p + 276) });
      void hs;
    } else if (t === 0x0001 && !globalPool) {
      globalPool = readPool(arsc, p);
    }
    p += total;
  }
  if (!globalPool) throw new Error('ResTable 里没有全局字符串池');
  return { pkgs, globalPool, arsc };
}

const poolCache = new WeakMap();
function poolOf(T, at) {
  let c = poolCache.get(T.arsc); if (!c) poolCache.set(T.arsc, c = new Map());
  let v = c.get(at);
  if (!v) { try { v = readPool(T.arsc, at); } catch (_) { v = []; } c.set(at, v); }
  return v;
}

/* (pkg,type,entry) → { dataType, data, str, typeName }；找不到返回 null。 */
function lookup(T, resId) {
  const pkgId = (resId >>> 24) & 0xff, typeId = (resId >>> 16) & 0xff, entryId = resId & 0xffff;
  const arsc = T.arsc;
  for (const pkg of T.pkgs) {
    if (pkg.id !== pkgId) continue;
    const end = pkg.at + pkg.size;
    let q = pkg.at + u16(arsc, pkg.at + 2);
    while (q + 8 <= end) {
      const t = u16(arsc, q), hs = u16(arsc, q + 2), total = u32(arsc, q + 4);
      if (!total) break;
      if (t === 0x0201 && u32(arsc, q + 8) === typeId) {
        const entryCount = u32(arsc, q + 12), entriesStart = u32(arsc, q + 16);
        // 只认稠密布局。稀疏表（aapt2 的 --enable-sparse-encoding，默认不开）
        // 偏移表不是 4×entryCount，这里直接判假跳过这一块 ——
        // 宁可取不到名字退回显示包名，也不要按下标读到别人的条目上去。
        if (entriesStart - hs !== entryCount * 4 || entryId >= entryCount) { q += total; continue; }
        const off = u32(arsc, q + hs + entryId * 4);
        if (off === 0xffffffff) { q += total; continue; }   // 这个配置里没有这一条
        const e = q + entriesStart + off;
        if (e + 16 > arsc.length) { q += total; continue; }
        const esize = u16(arsc, e), eflags = u16(arsc, e + 2);
        if (eflags & 0x04) { q += total; continue; }        // FLAG_WEAK：只有名字没有值
        // 值紧跟在条目头后面，**不管 flags 是什么**。
        // FLAG_ORIG(0x02) 的那一条实测是"真值在前、TYPE_NULL 占位在"后"
        // （三合一包里 drawable 的两条都是：v1 = res/drawable/ic_launcher.xml，v2 = type 0），
        // 照某些资料"ORIG 就跳过一个 Res_value"去读，读到的是那个占位空值，
        // 表现是"条目明明在、值却是空的"——图标和名字一起取不到，还查不出原因。
        const v = e + esize;
        const dtype = arsc[v + 3], data = u32(arsc, v + 4);
        const tp = poolOf(T, pkg.typeStrings);
        return { dataType: dtype, data, typeName: tp[typeId - 1] || '',
                 str: dtype === T_STRING ? (T.globalPool[data] || '') : '' };
      }
      q += total;
    }
  }
  return null;
}

/* 引用值 → 字符串（label 用）；字面量直接返回。 */
function valueText(T, a) {
  if (!a) return '';
  if (a.dtype === T_STRING) return a.str;
  if (a.dtype === T_REFERENCE && T) {
    const r = lookup(T, a.data);
    return r && r.dataType === T_STRING ? r.str : '';
  }
  return '';
}

/* 声明的名字是 .xml（自适应图标、矢量图）时，WebView 解不了那张"图"。
 * 同一个资源名往往还留着位图版本（ic_launcher.png 在 mipmap-hdpi/ 那一堆里），
 * 按密度优先级找一张回来。找不到就返回空，让调用方退回按尺寸猜的兜底。 */
const DENS_ORDER = ['xxhdpi', 'xhdpi', 'xxxhdpi', 'hdpi', 'mdpi'];
function bitmapForPath(ents, p) {
  if (!p) return '';
  if (/\.(png|webp|jpe?g)$/i.test(p) && ents.has(p)) return p;
  const base = p.replace(/^.*\//, '').replace(/\.[a-z]+$/i, '');
  if (!base) return '';
  for (const d of DENS_ORDER) {
    for (const ext of ['png', 'webp']) {
      for (const name of ents.keys()) {
        if (!name.includes(d)) continue;
        if (name.endsWith('/' + base + '.' + ext)) return name;
      }
    }
  }
  return '';
}

/* ── 一个 APK 的 label + icon ────────────────────────────────── */

function appInfo(file) {
  const buf = fs.readFileSync(file);
  const ents = zipEntries(buf);
  const mm = ents.get('AndroidManifest.xml');
  if (!mm) throw new Error('没有 AndroidManifest.xml');
  const mf = parseManifest(entryBytes(buf, mm));

  /* 桌面入口是谁：
   * <activity> / <activity-alias> 里带 intent-filter，且 action=MAIN、
   * category=LAUNCHER 同时出现的那个；<application> 上的 label/icon 是兜底。
   * 两边都要看：Kin.apk 的名字同时写在两处，而有些包只在 activity 上写。 */
  const app = mf.els.find(e => e.tag === 'application') || null;
  const manifest = mf.els.find(e => e.tag === 'manifest') || null;
  const pkg = manifest ? (manifest.attrs.find(a => !a.id && a.key === 'package') || {}).str || '' : '';
  let launcher = null;
  for (const e of mf.els) {
    if (e.tag !== 'activity' && e.tag !== 'activity-alias') continue;
    const f = e.kids.find(k => k.tag === 'intent-filter');
    if (!f) continue;
    const act = f.kids.filter(k => k.tag === 'action').map(k => k.attrs.map(a => a.str).join(','));
    const cat = f.kids.filter(k => k.tag === 'category').map(k => k.attrs.map(a => a.str).join(','));
    if (act.some(s => s.includes('android.intent.action.MAIN')) &&
        cat.some(s => s.includes('android.intent.category.LAUNCHER'))) { launcher = e; break; }
  }
  let T = null;
  const ra = ents.get('resources.arsc');
  if (ra) { try { T = parseTable(entryBytes(buf, ra)); } catch (_) { T = null; } }

  const pick = (el, key) => (el ? valueText(T, attrOf(el, key)) : '');
  const labelSrc = (launcher && pick(launcher, A_LABEL)) ? launcher : app;
  const label = pick(labelSrc, A_LABEL);
  let icon = pick(launcher, A_ICON) || pick(app, A_ICON);
  let iconXml = '';
  /* 声明的是 .xml（自适应图标，Android 8 之后几乎全是）时，
   *  xml 本身不是位图，WebView 解不了 —— 里面 foreground 引用的那张才是看到的东西。 */
  if (/\.xml$/.test(icon) && ents.get(icon)) {
    iconXml = icon;
    try {
      const inner = parseManifest(entryBytes(buf, ents.get(icon)));
      const refEl = inner.els.find(e => e.tag === 'foreground') ||
                    inner.els.find(e => e.attrs.some(a => a.dtype === T_REFERENCE));
      const a = refEl && (attrOf(refEl, A_DRAWABLE) || refEl.attrs.find(x => x.dtype === T_REFERENCE));
      if (a) { const r = lookup(T, a.data); if (r && r.str) icon = r.str; }
    } catch (_) { /* 读不动就留着 xml 路径 */ }
  }
  icon = bitmapForPath(ents, icon) || '';
  return { pkg, label, icon, iconXml, hasTable: !!T,
           where: (labelSrc === app ? 'application' : 'activity') +
                  (launcher ? '+launcher' : '') };
}

if (require.main === module) {
  const args = process.argv.slice(2).filter(a => a !== '--dump');
  const dump = process.argv.includes('--dump');
  for (const f of args) {
    try {
      const r = appInfo(f);
      console.log([r.label, r.icon].join('\t'));
      if (dump) console.error(JSON.stringify(r, null, 1));
    } catch (e) {
      console.log('\t');
      console.error('E ' + f + ': ' + e.message);
    }
  }
}
module.exports = { zipEntries, entryBytes, readPool, parseManifest, parseTable, lookup, appInfo, u16, u32 };
