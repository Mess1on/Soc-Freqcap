// 修 PT_TLS 的 p_align：NDK r25c 的静态 libc 留下 align=8 的 PT_TLS，
// Android Bionic 的 linker 要求 >=64，非零就拒载。
//
// 为什么必须改而不是绕过：loader 按 p_align 分配并对齐 TLS 块，
// 声明值调大只会让分配更严格，而检查的目的正是保证 >=64。
//
// ELF64 的字段位置（这版代码里最容易踩的坑）：
//   e_phoff   @ 0x20  (u64)
//   e_phentsize @ 0x36 (u16)   ← 不是 0x34，0x34 是 e_shentsize
//   e_phnum   @ 0x38  (u16)
// 之前用 0x34 读 e_phentsize，拿到 64 而不是 56，
// 于是 ph 数组全错位、一个 PT_TLS 都没匹配上，脚本却报告"改完了"。
// 静默什么都没改比直接报错更糟，所以下面每步都带断言。
//
// 用法: node build/fix_tls_align.js <elf> [--max-page-size=N]
//   不传 --max-page-size 就只修 TLS。
const fs = require('fs');

const PT_LOAD = 1, PT_TLS = 7;
const MIN_TLS_ALIGN = 64;

function phdrs(b) {
  const off = Number(b.readBigUInt64LE(0x20));
  const ent = b.readUInt16LE(0x36);
  const num = b.readUInt16LE(0x38);
  if (ent !== 56) throw new Error(`e_phentsize 应为 56，实际 ${ent}（ELF64 固定值，读错说明偏移用错了）`);
  const out = [];
  for (let i = 0; i < num; i++) {
    const o = off + i * ent;
    out.push({
      i, o,
      type: b.readUInt32LE(o),
      off: Number(b.readBigUInt64LE(o + 8)),
      vaddr: Number(b.readBigUInt64LE(o + 16)),
      align: Number(b.readBigUInt64LE(o + 48)),
    });
  }
  return out;
}

// loader 会核对 p_offset % p_align == p_vaddr % p_align，不满足直接拒载。
// 所以改 align 之前先确认改完仍然自洽。
function consistent(p, align) {
  return (p.off % align) === (p.vaddr % align);
}

function main() {
  const args = process.argv.slice(2);
  const file = args.find(a => !a.startsWith('--'));
  if (!file) { console.error('用法: node fix_tls_align.js <elf> [--max-page-size=N]'); process.exit(2); }
  const mpArg = args.find(a => a.startsWith('--max-page-size='));
  const wantMaxPage = mpArg ? parseInt(mpArg.split('=')[1], 10) : 0;

  const before = fs.statSync(file).size;
  const b = fs.readFileSync(file);
  const ph = phdrs(b);
  let tlsFixed = 0, pageFixed = 0;

  for (const p of ph) {
    if (p.type === PT_TLS) {
      if (p.align < MIN_TLS_ALIGN) {
        const a = consistent(p, MIN_TLS_ALIGN) ? MIN_TLS_ALIGN : p.align;
        if (a === p.align) {
          console.error(`  TLS align 本来就是 ${p.align}，但与 offset/vaddr 不自洽，改不了`);
          process.exit(1);
        }
        b.writeBigUInt64LE(BigInt(a), p.o + 48);
        console.log(`  PT_TLS p_align 0x${p.align.toString(16)} -> 0x${a.toString(16)}`);
        tlsFixed++;
      }
    }
    if (p.type === PT_LOAD && wantMaxPage > 0 && p.align < wantMaxPage) {
      if (!consistent(p, wantMaxPage)) {
        console.error(`  LOAD #${p.i} offset=0x${p.off.toString(16)} vaddr=0x${p.vaddr.toString(16)} ` +
                      `无法对齐到 ${wantMaxPage}（差 ${(p.vaddr - p.off) % wantMaxPage}）`);
        process.exit(1);
      }
      b.writeBigUInt64LE(BigInt(wantMaxPage), p.o + 48);
      console.log(`  PT_LOAD #${p.i} p_align 0x${p.align.toString(16)} -> 0x${wantMaxPage.toString(16)}`);
      pageFixed++;
    }
  }

  if (tlsFixed === 0 && pageFixed === 0) {
    console.log('  无需修改（TLS 对齐与页对齐都已达标）');
  }
  fs.writeFileSync(file, b);

  // 复验：改完必须真的达标，且大小没变。多这一步是吃过亏的。
  const after = phdrs(fs.readFileSync(file));
  const tls = after.filter(p => p.type === PT_TLS);
  for (const p of tls) {
    if (p.align < MIN_TLS_ALIGN) { console.error(`  复验失败: TLS align 仍是 ${p.align}`); process.exit(1); }
    if (!consistent(p, p.align)) { console.error('  复验失败: TLS offset/vaddr 与 align 不自洽'); process.exit(1); }
  }
  // 只有 PT_LOAD 参与 offset/vaddr 自洽检查。
  // GNU_STACK / GNU_RELRO / GNU_PROPERTY 的 align 本来就是 0x1/0x0，
  // 拿 p_align 去模它们的 offset 必然"错位"，那不是错，是这几个段
  // 压根没有对齐要求（第一版这里误报过 LOAD #8 错位）。
  const loads = after.filter(p => p.type === PT_LOAD);
  for (const p of loads) {
    if (!consistent(p, p.align)) { console.error(`  复验失败: LOAD #${p.i} 错位`); process.exit(1); }
    if (wantMaxPage > 0 && p.align < wantMaxPage) {
      console.error(`  复验失败: LOAD #${p.i} align 仍是 ${p.align}`); process.exit(1);
    }
  }
  if (fs.statSync(file).size !== before) { console.error('  复验失败: 文件大小变了'); process.exit(1); }
  console.log(`  复验通过：TLS=${tls.map(p => '0x' + p.align.toString(16)).join(',') || '无'}，` +
              `LOAD=${loads.map(p => '0x' + p.align.toString(16)).join(',')}，` +
              `大小 ${before} → ${fs.statSync(file).size}`);
}

main();