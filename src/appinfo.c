/*
 * appinfo.c — 从 APK 里读「应用名」和「桌面图标声明的那张图」
 *
 * 应用名在 Android 上没有便宜的外壳来源：
 *   - pm list packages 没有显示标签的选项；
 *   - dumpsys package 里没有标签；
 *   - label 只存在于 APK 里：AndroidManifest.xml 的
 *     android:label，而且它通常是个**引用**（@string/app_name），
 *     文本还要再去 resources.arsc 的字符串池里查一次。
 * 所以只能在守护里把这两个二进制格式读出来。
 *
 * 顺带把图标也钉准：resources.arsc 里 drawable/mipmap 条目的值就是
 * 文件路径（res/xx.png），manifest 的 android:icon 指到哪就是哪张，
 * 比 icon.c 那套"按名字打分 + 按尺寸猜"准得多 —— 那套继续留着当兜底，
 * 因为**声明的常常是一张 .xml（自适应图标 / 矢量图）**，WebView 解不了，
 * 还得回去猜位图。
 *
 * 边界：所有读都带长度上界。这个进程是 root，越界读不是"显示不对"，
 * 是守护被杀。宁可返回"没读到"。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

#define AI_LABEL_MAX 128      /* UTF-8 字节数：应用名再长也不至于超过 128 */
#define AI_PATH_MAX  200      /* zip 条目名上限，和 icon.c 的 struct entry 对齐 */

/* resources.arsc 的大小上限。16MB 是宽裕的停止位置：
   读不下就退回不显示名字，不为一个标签 mmap 几十兆。 */
#define AI_ARSC_MAX (16u * 1024 * 1024)

static uint32_t ai_r16(const uint8_t *p, size_t n, size_t o) {
  return (o + 2 <= n) ? (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) : 0u;
}
static uint32_t ai_r32(const uint8_t *p, size_t n, size_t o) {
  if (o + 4 > n) return 0u;
  return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) |
         ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

/* ── zip：中央目录走一遍，两种取法共用 ──────────────────────────
 * 中央目录在文件末尾。这里有两个需求：
 *   1) 按**确定名字**取一个条目（AndroidManifest.xml / resources.arsc / 声明的图标）；
 *   2) 按**主名**找同名位图（声明的是 .xml 时，桌面上的那张一般是同名 png/webp）。
 * 两个都走同一遍 EOCD→ZIP64→条目循环（条目数超 65535 走 ZIP64、
 * 目录大过 4MB 窗口要按真实大小重读），不另写第二份解析。
 * icon.c 那边是"扫全部条目打分"，另有一份；两份共同遵守同一条规矩：
 * **数据偏移按本地文件头算**（中央目录那两个长度在 zipalign 过的包里是不等的）。
 */
struct zip_ent { uint32_t loff, csz, usz, method; };

struct zip_pick {
  const char *exact;                 /* 要这个确定的名字（先到先得） */
  const char *base;                  /* 或要"主名==base"的位图里最合适的一张 */
  int best;                          /* 密度档排名，越小越好；99 = 还没命中 */
  char name[AI_PATH_MAX];
  struct zip_ent ent;
};

/* 密度档按"离屏幕上那一格多远"排，和 icon.c 的 ICON_SLOT_PX 同一个思路：
   xxhdpi(144) 最好，xhdpi(96)/xxxhdpi(192) 次之，再 hdpi、mdpi，
   不带密度名的最末。 */
static int dens_rank(const char *n) {
  if (strstr(n, "xxhdpi") && !strstr(n, "xxxhdpi")) return 1;
  if (strstr(n, "xhdpi")) return 2;
  if (strstr(n, "xxxhdpi")) return 2;
  if (strstr(n, "hdpi")) return 3;
  if (strstr(n, "mdpi")) return 4;
  return 5;
}

static int is_bitmap(const char *n) {
  const char *d = strrchr(n, '.');
  return d && (!strcmp(d, ".png") || !strcmp(d, ".webp") ||
               !strcmp(d, ".jpg") || !strcmp(d, ".jpeg"));
}

static void zip_consider(struct zip_pick *pk, const char *name, struct zip_ent e) {
  if (pk->exact) {
    if (!strcmp(name, pk->exact) && !pk->name[0]) {
      snprintf(pk->name, sizeof pk->name, "%s", name);
      pk->ent = e;
    }
    return;
  }
  if (!pk->base || !is_bitmap(name)) return;
  const char *b = strrchr(name, '/');
  b = b ? b + 1 : name;
  size_t bl = strlen(pk->base);
  if (strncmp(b, pk->base, bl)) return;
  if (b[bl] != '.') return;                    /* 主名必须整段对上，别收 foreground_round */
  int r = dens_rank(name);
  if (r < pk->best) { pk->best = r; snprintf(pk->name, sizeof pk->name, "%s", name); pk->ent = e; }
}

static int zip_walk(const char *apk, struct zip_pick *pk) {
  pk->name[0] = 0;
  pk->best = 99;
  int fd = open(apk, O_RDONLY);
  if (fd < 0) return 0;
  off_t sz = lseek(fd, 0, SEEK_END);
  if (sz <= 0) { close(fd); return 0; }
  if (sz > 4LL * 1024 * 1024 * 1024) { close(fd); return 0; }   /* 同 icon.c 的硬边界 */

  size_t want_win = 4u * 1024 * 1024;
  if ((size_t)sz < want_win) want_win = (size_t)sz;
  uint8_t *buf = malloc(want_win);
  if (!buf) { close(fd); return 0; }
  size_t base = (size_t)sz - want_win;
  ssize_t got = pread(fd, buf, want_win, (off_t)base);
  if (got <= 0) { close(fd); free(buf); return 0; }

  long eocd = -1;
  size_t lim = (size_t)got > 65536 ? 65536 : (size_t)got;
  for (size_t i = (size_t)got - lim; i + 22 <= (size_t)got; i++)
    if (ai_r32(buf, (size_t)got, i) == 0x06054b50u) { eocd = (long)i; break; }
  if (eocd < 0) { close(fd); free(buf); return 0; }

  uint32_t cdsz = ai_r32(buf, (size_t)got, eocd + 12);
  uint32_t cdoff = ai_r32(buf, (size_t)got, eocd + 16);
  if (cdsz == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) {
    long zl = eocd - 20;
    if (zl >= 0 && ai_r32(buf, (size_t)got, (size_t)zl) == 0x07064b50u) {
      uint64_t zoff = (uint64_t)ai_r32(buf, (size_t)got, (size_t)zl + 8)
                    | ((uint64_t)ai_r32(buf, (size_t)got, (size_t)zl + 12) << 32);
      if (zoff >= base && zoff - base + 56 <= (size_t)got &&
          ai_r32(buf, (size_t)got, (size_t)(zoff - base)) == 0x06064b50u) {
        cdsz  = ai_r32(buf, (size_t)got, (size_t)(zoff - base) + 40);
        cdoff = ai_r32(buf, (size_t)got, (size_t)(zoff - base) + 48);
      }
    }
  }
  /* 目录装不进当前窗口时按它的大小再读一段（同 icon.c 的两段读） */
  if (!(cdoff >= base && (size_t)cdoff + cdsz <= (size_t)sz)) {
    if (cdoff < (size_t)sz && cdsz > 0 && (size_t)cdoff + (size_t)cdsz <= (size_t)sz
        && (size_t)cdsz <= 128u * 1024 * 1024) {
      /* realloc 成功后旧指针就已经失效，不能再 free(buf)：
         大 APK 的中央目录超过 4MB 窗口时走这里，旧写法会双重释放。 */
      uint8_t *nb = realloc(buf, (size_t)cdsz);
      if (nb) {
        ssize_t g2 = pread(fd, nb, (size_t)cdsz, (off_t)cdoff);
        if (g2 > 0) { buf = nb; base = (size_t)cdoff; got = g2; }
        else { free(nb); buf = NULL; base = 0; got = 0; }
      }
    }
  }

  if (cdoff >= base && (size_t)cdoff - base + cdsz <= (size_t)got) {
    size_t p = (size_t)cdoff - base, end = p + cdsz;
    if (end > (size_t)got) end = (size_t)got;   /* pread 可能短读，别扫未初始化的尾巴 */
    char nm[AI_PATH_MAX];
    for (; p + 46 <= end; ) {
      if (ai_r32(buf, (size_t)got, p) != 0x02014b50u) break;
      uint16_t nlen = (uint16_t)ai_r16(buf, (size_t)got, p + 28);
      uint16_t elen = (uint16_t)ai_r16(buf, (size_t)got, p + 30);
      uint16_t clen = (uint16_t)ai_r16(buf, (size_t)got, p + 32);
      if (p + 46u + nlen > end) break;
      if (nlen + 1 <= sizeof nm) {
        /* 补 NUL 再比：中央目录里的名字没有结尾 0，
           直接 strstr/strcmp 会顺着读到下一条条目的 PK\x01\x02 里去 */
        memcpy(nm, buf + p + 46, nlen);
        nm[nlen] = 0;
        struct zip_ent e = { ai_r32(buf, (size_t)got, p + 42),
                             ai_r32(buf, (size_t)got, p + 20),
                             ai_r32(buf, (size_t)got, p + 24),
                             ai_r16(buf, (size_t)got, p + 10) };
        zip_consider(pk, nm, e);
        if (pk->exact && pk->name[0]) break;       /* 精确名字：找到就走人 */
      }
      p += 46u + nlen + elen + clen;
    }
  }
  free(buf);
  close(fd);
  return pk->name[0] != 0;
}

/* 把找到的条目读出来（必要时 deflate 解压）。调用方 free(*out)。 */
static int ai_zip_load(const char *apk, const struct zip_ent *e, uint8_t **out, size_t *olen) {
  *out = NULL; *olen = 0;
  if (e->csz == 0 || e->csz > AI_ARSC_MAX) return 0;
  int fd = open(apk, O_RDONLY);
  if (fd < 0) return 0;
  uint8_t lh[30];
  if (pread(fd, lh, 30, (off_t)e->loff) != 30 || ai_r32(lh, 30, 0) != 0x04034b50u) {
    close(fd); return 0;
  }
  uint64_t data = (uint64_t)e->loff + 30u + ai_r16(lh, 30, 26) + ai_r16(lh, 30, 28);
  uint8_t *in = malloc(e->csz);
  if (!in) { close(fd); return 0; }
  if (pread(fd, in, e->csz, (off_t)data) != (ssize_t)e->csz) { close(fd); free(in); return 0; }
  close(fd);

  if (e->method == 0) { *out = in; *olen = e->csz; return 1; }
  if (e->method != 8) { free(in); return 0; }     /* 不支持的压缩方式按读失败处理 */
  /* 输出上限以 zip 目录里的**原始大小**为准。4 倍压缩后大小的经验值对
     高压缩率的 manifest 不够（抖音 736KB 压到 73KB，约 10 倍），
     旧写法会把解压判失败，表现就是"这个应用永远没有名字"。
     ZIP64 的 0xFFFFFFFF 也在这里挡住：我们不解析 extra 里的 64 位尺寸。 */
  if (e->usz > AI_ARSC_MAX) { free(in); return 0; }
  size_t cap = e->usz ? (size_t)e->usz : ((size_t)e->csz * 4 + 65536);
  if (cap > AI_ARSC_MAX) cap = AI_ARSC_MAX;
  uint8_t *ob = malloc(cap);
  if (!ob) { free(in); return 0; }
  z_stream z;
  memset(&z, 0, sizeof z);
  /* windowBits 必须是负数：-15 = raw deflate。zip 里的流（RFC 1951）
     没有 zlib 头也没有 gzip 头，写 15/32 一律 Z_DATA_ERROR。 */
  if (inflateInit2(&z, -15) != Z_OK) { free(in); free(ob); return 0; }
  z.next_in = in; z.avail_in = e->csz;
  z.next_out = ob; z.avail_out = (uInt)cap;
  int r = inflate(&z, Z_FINISH);
  size_t gotlen = cap - z.avail_out;
  inflateEnd(&z);
  free(in);
  if (r != Z_STREAM_END) { free(ob); return 0; }   /* 没解完就当读失败，不拿半截数据去解析 */
  if (!gotlen) { free(ob); return 0; }
  *out = ob; *olen = gotlen;
  return 1;
}

static int ai_zip_read(const char *apk, const char *want, uint8_t **out, size_t *olen) {
  struct zip_pick pk;
  memset(&pk, 0, sizeof pk);
  pk.exact = want;
  if (!zip_walk(apk, &pk)) return 0;
  return ai_zip_load(apk, &pk.ent, out, olen);
}

static int ai_zip_exists(const char *apk, const char *name) {
  struct zip_pick pk;
  memset(&pk, 0, sizeof pk);
  pk.exact = name;
  return zip_walk(apk, &pk);
}

/* 声明的图标是 .xml（自适应图标 / 矢量图）时，WebView 解不了那张"图"，
 * 桌面上的其实是同名的位图版本。找一张回来；找不到返回 0，
 * 让调用方退回 icon.c 那套按名字/尺寸猜的兜底。 */
static int ai_zip_pick_bitmap(const char *apk, const char *path, char *out, size_t osz) {
  out[0] = 0;
  if (!path || !path[0]) return 0;
  if (is_bitmap(path) && ai_zip_exists(apk, path)) { snprintf(out, osz, "%s", path); return 1; }
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  char base[AI_PATH_MAX];
  const char *dot = strrchr(b, '.');
  size_t bl = dot ? (size_t)(dot - b) : strlen(b);
  if (!bl || bl >= sizeof base) return 0;
  memcpy(base, b, bl);
  base[bl] = 0;
  struct zip_pick pk;
  memset(&pk, 0, sizeof pk);
  pk.base = base;
  if (!zip_walk(apk, &pk)) return 0;
  snprintf(out, osz, "%s", pk.name);
  return 1;
}

/* ── ResStringPool ─────────────────────────────────────────────
 * 头 28 字节：type(u16=1) headerSize(u16) size(u32) count(u32) styleCount(u32)
 *             flags(u32) stringsStart(u32) stylesStart(u32)
 * 之后是 count 个 u32 偏移，**偏移相对 stringsStart 那个位置**。
 * flags 的 0x100 = 字符串是 UTF-8；否则是 UTF-16LE。
 * UTF-8 串前面有**两个**变长整数：先字符数、后字节数（高位 0x80 = 占两字节）。
 * 切片必须按第二个数：中文一个字符 3 字节，按字符数切会切在半个序列上，
 * 出来一个 U+FFFD —— ASCII 时两者相等，所以这个错只在看中文应用名时才暴露。 */
static const uint8_t *pool_data(const uint8_t *p, size_t n, size_t *count, uint32_t *flags,
                               size_t *tbl) {
  if (n < 28 || ai_r16(p, n, 0) != 0x0001u) return NULL;
  size_t hs = ai_r16(p, n, 2);
  size_t total = ai_r32(p, n, 4);
  uint32_t cnt = ai_r32(p, n, 8);
  uint32_t fl = ai_r32(p, n, 16);
  size_t sstart = ai_r32(p, n, 20);
  if (!total || total > n || sstart < hs || sstart >= total) return NULL;
  *count = cnt; *flags = fl; *tbl = hs;
  return p + sstart;                    /* 数据区起点（相对池头） */
}

/* UTF-16LE → UTF-8。代理对合成一个码点；落单的代理项当 U+FFFD。 */
static void u16_to_u8(const uint8_t *s, size_t chars, char *out, size_t osz) {
  size_t o = 0;
  for (size_t i = 0; i < chars; i++) {
    uint32_t c = (uint32_t)s[i * 2] | ((uint32_t)s[i * 2 + 1] << 8);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars) {
      uint32_t lo = (uint32_t)s[(i + 1) * 2] | ((uint32_t)s[(i + 1) * 2 + 1] << 8);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    size_t need;
    char b[4];
    if (c < 0x80)        { b[0] = (char)c; need = 1; }
    else if (c < 0x800)  { b[0] = (char)(0xC0 | (c >> 6));  b[1] = (char)(0x80 | (c & 0x3F)); need = 2; }
    else if (c < 0x10000){ b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                           b[2] = (char)(0x80 | (c & 0x3F)); need = 3; }
    else                 { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
                           b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); need = 4; }
    if (o + need >= osz) break;
    memcpy(out + o, b, need);
    o += need;
  }
  out[o] = 0;
}

static int pool_str(const uint8_t *p, size_t n, uint32_t idx, char *out, size_t osz) {
  out[0] = 0;
  size_t count = 0, tbl = 0;
  uint32_t flags = 0;
  const uint8_t *data = pool_data(p, n, &count, &flags, &tbl);
  if (!data || !count) return 0;
  size_t pool_total = ai_r32(p, n, 4);
  if (tbl + idx * 4 + 4 > pool_total) return 0;
  uint32_t off = ai_r32(p, n, tbl + idx * 4);
  size_t o = (size_t)(data - p) + off;
  if (o + 2 > n) return 0;
  if (flags & 0x100u) {                       /* UTF-8 */
    size_t l1 = (p[o] & 0x80) ? 2 : 1;
    if (o + l1 + 1 > n) return 0;
    size_t bl = (p[o + l1] & 0x80)
      ? (((size_t)(p[o + l1] & 0x7f) << 8) | p[o + l1 + 1])
      : (size_t)p[o + l1];
    o += l1 + ((p[o + l1] & 0x80) ? 2 : 1);
    if (bl >= osz) bl = osz - 1;
    if (o + bl > n) return 0;                 /* 声称的长度超出池 = 读飞了 */
    memcpy(out, p + o, bl);
    out[bl] = 0;
  } else {                                    /* UTF-16LE */
    uint32_t c1 = ai_r16(p, n, o);
    o += 2;
    size_t chars = (c1 & 0x8000u) ? (((size_t)(c1 & 0x7fff) << 16) | ai_r16(p, n, o)) : c1;
    if (c1 & 0x8000u) o += 2;
    if (chars > 4096) chars = 4096;           /* 名字不至于这么长，先夹住再验边界 */
    if (o + chars * 2 > n) return 0;
    u16_to_u8(p + o, chars, out, osz);
  }
  return out[0] != 0;
}

/* ── AndroidManifest.xml（AXML）────────────────────────────────
 *   文件头 8：type(u16=3) headerSize(u16) size(u32)
 *   然后 0x0001 字符串池、0x0180 资源 ID 表（一组 u32，下标=属性名在池里的下标）
 *   然后节点：0x0100/0x0101 命名空间，0x0102 开始元素，0x0103 结束元素
 *   节点头 16 = chunk 头 8 + lineNumber 4 + comment 4
 *   attrExt 从 +16 起：ns(u32) name(u32) attributeStart(u16) attributeSize(u16)
 *                      attributeCount(u16) [补 2] idIndex classIndex styleIndex
 *   属性区从 attrExt+attributeStart 起，每条 attributeSize(=20) 字节：
 *     ns(u32) name(u32) rawValue(u32) ResValue{size u16,res0 u8,dataType u8,data u32}
 *   → dataType 在记录 +15，data 在 +16（**不是** name 后面的 +8）
 *
 * 属性 ID：android:theme=0x01010000、label=…0001、icon=…0002、name=…0003。
 * 这里不写死数字，改按**池里的本地名**认（"label"/"icon"/"drawable"），
 * 再用"资源 ID 表里查得到"确认它是 android: 命名空间的属性——
 * 写死数字写错时取到的是别的属性值（如把 label 记成 name 的 ID
 * 会稳定取到活动类名），而且一句错话都不报。 */
#define AX_T_STRING 0x03
#define AX_T_REFERENCE 0x01

struct axml {
  const uint8_t *p;
  size_t n;
  size_t pool_at;      /* 字符串池在文件里的偏移，0 = 没有 */
  size_t map_at;       /* 资源 ID 表 */
  size_t map_n;
};

static int axml_open(const uint8_t *p, size_t n, struct axml *ax) {
  ax->p = p; ax->n = n; ax->pool_at = 0; ax->map_at = 0; ax->map_n = 0;
  if (n < 8 || ai_r16(p, n, 0) != 0x0003u) return 0;
  size_t total = ai_r32(p, n, 4);
  if (total && total <= n) n = total;             /* 以块里声明的长度为准 */
  ax->n = n;
  for (size_t q = ai_r16(p, n, 2); q + 8 <= n; ) {
    uint32_t t = ai_r16(p, n, q);
    size_t sz = ai_r32(p, n, q + 4);
    if (!sz || q + sz > n) break;
    if (t == 0x0001u && !ax->pool_at) ax->pool_at = q;
    else if (t == 0x0180u) { ax->map_at = q; ax->map_n = (sz - 8) / 4; }
    q += sz;
  }
  return ax->pool_at != 0;
}

/* 元素/属性的本地名 */
static int axml_name(const struct axml *ax, uint32_t idx, char *out, size_t osz) {
  return pool_str(ax->p + ax->pool_at, ax->n - ax->pool_at, idx, out, osz);
}
static uint32_t axml_resid(const struct axml *ax, uint32_t idx) {
  if (!ax->map_at || idx >= ax->map_n) return 0;
  return ai_r32(ax->p, ax->n, ax->map_at + 8 + idx * 4);
}

/* 一条属性的取值：字面量直接给文本；引用给资源 ID（*ref 非 0）。 */
static void axml_attr_value(const struct axml *ax, size_t rec,
                            char *txt, size_t tsz, uint32_t *ref) {
  uint32_t dt = ax->p[rec + 15];
  uint32_t data = ai_r32(ax->p, ax->n, rec + 16);
  if (txt) txt[0] = 0;
  if (ref) *ref = 0;
  if (dt == AX_T_STRING) axml_name(ax, data, txt, tsz);
  else if (dt == AX_T_REFERENCE && ref) *ref = data;
}

/* ── resources.arsc ───────────────────────────────────────────
 * 布局：
 *   表头 12：type(u16=2) headerSize(u16=12) size(u32) packageCount(u32)
 *   紧跟一个**全局字符串池**（标签文本、drawable 的文件路径都在这里）
 *   ResTable_package(0x0200) headerSize=288：
 *        +8 id(u32，通常 0x7f)  +12 name(128×u16=256)
 *        +268 typeStrings(u32，**相对包块开头**)  +276 keyStrings(u32)
 *   包内：typeStrings 池、keyStrings 池、然后每个类型
 *        0x0202 = spec（头 16：id, entryCount, 然后 entryCount 个 u32）—— 不看
 *        0x0201 = type，**每个配置一块**，头 84：
 *             +8 id  +12 entryCount  +16 entriesStart  +20 config(size u32 打头，64 字节)
 *             之后 entryCount 个 u32 条目偏移（自洽式：entriesStart-84 == 4×entryCount）
 *   条目 ResTable_entry{size u16, flags u16, key u32} 紧跟 ResValue{…}
 *        FLAG_WEAK(0x04) 的条目只有名字没有值 → 跳过
 *        FLAG_ORIG(0x02) **不跳过第一个值**：真值在前、TYPE_NULL 占位在后，
 *        若按"ORIG 就跳一个 Res_value"的常见说法去读，读到的是占位空值，
 *        表现是"条目明明在、值却是空的"，名字和图标一起取不到。
 */
struct arsc {
  const uint8_t *p;
  size_t n;
  size_t gpool;        /* 全局字符串池的偏移 */
};

static int arsc_open(const uint8_t *p, size_t n, struct arsc *t) {
  t->p = p; t->n = n; t->gpool = 0;
  if (n < 12 || ai_r16(p, n, 0) != 0x0002u) return 0;
  size_t total = ai_r32(p, n, 4);
  if (total && total <= n) t->n = total;
  for (size_t q = ai_r16(p, t->n, 2); q + 8 <= t->n; ) {
    uint32_t ty = ai_r16(p, t->n, q);
    size_t sz = ai_r32(p, t->n, q + 4);
    if (!sz || q + sz > t->n) break;
    if (ty == 0x0001u && !t->gpool) { t->gpool = q; break; }  /* 全局池在包之前 */
    if (ty == 0x0200u) break;                                  /* 没有池就不再往下走 */
    q += sz;
  }
  return t->gpool != 0;
}

/* 把一个资源 ID 解析成文本（TYPE_STRING 的条目）。 */
static int arsc_str(const struct arsc *t, uint32_t id, char *out, size_t osz) {
  out[0] = 0;
  uint32_t pkgId = (id >> 24) & 0xff, typeId = (id >> 16) & 0xff, entryId = id & 0xffff;
  if (!typeId || !pkgId) return 0;
  const uint8_t *p = t->p; size_t n = t->n;
  for (size_t q = ai_r16(p, n, 2); q + 8 <= n; ) {
    uint32_t ty = ai_r16(p, n, q);
    size_t sz = ai_r32(p, n, q + 4), hs = ai_r16(p, n, q + 2);
    if (!sz || q + sz > n) break;
    if (ty == 0x0200u) {
      if (ai_r32(p, n, q + 8) == pkgId) {
        /* 包内找 id==typeId 的 0x0201 块 */
        for (size_t r = q + hs; r + 8 <= q + sz; ) {
          uint32_t rt = ai_r16(p, n, r);
          size_t rhs = ai_r16(p, n, r + 2), rsz = ai_r32(p, n, r + 4);
          if (!rsz || r + rsz > q + sz) break;
          if (rt == 0x0201u && ai_r32(p, n, r + 8) == typeId) {
            uint32_t entryCount = ai_r32(p, n, r + 12);
            size_t entriesStart = ai_r32(p, n, r + 16);
            /* 只认稠密布局。稀疏表（aapt2 的 --enable-sparse-encoding，默认不开）
               的偏移表长度对不上，这里判假跳块 ——
               宁可显示包名，也不要按下标读到别人的条目上去。 */
            if (entriesStart >= rhs && entriesStart - rhs == (size_t)entryCount * 4
                && entryId < entryCount) {
              size_t tbl = r + rhs;
              if (tbl + entryId * 4 + 4 > q + sz) { r += rsz; continue; }
              uint32_t eo = ai_r32(p, n, tbl + entryId * 4);
              if (eo == 0xFFFFFFFFu) { r += rsz; continue; }
              size_t e = r + entriesStart + eo;
              if (e + 16 > n) { r += rsz; continue; }
              size_t esz = ai_r16(p, n, e);
              uint32_t efl = ai_r16(p, n, e + 2);
              if (esz >= 8 && !(efl & 0x04u)) {
                size_t v = e + esz;
                uint32_t dt = (v + 4 <= n) ? p[v + 3] : 0;
                uint32_t data = ai_r32(p, n, v + 4);
                if (dt == AX_T_STRING) {
                  size_t gp = t->gpool;
                  if (pool_str(p + gp, n - gp, data, out, osz)) return 1;
                }
              }
            }
            /* 后面的配置块（中文/横屏/高 dpi）多半是同一张图 */
          }
          r += rsz;
        }
      }
    }
    q += sz;
  }
  return out[0] != 0;
}


/* ── manifest：把 label 与"声明的图标"挖出来 ─────────────────
 * 桌面入口是谁：<activity> / <activity-alias> 里带 intent-filter，
 * 且 action=android.intent.action.MAIN、category=android.intent.category.LAUNCHER
 * 同时出现的那个。<application> 上的 label/icon 是兜底。
 * 两边都要看：有些包只把名字写在活动上，
 * 只看 application 会显示成空。 */
struct mval { char txt[AI_LABEL_MAX]; uint32_t ref; };

#define AX_DEPTH 12
struct axslot {
  int is_act;
  int is_filter;
  int main_seen;
  int launcher_seen;
  struct mval label;
  struct mval icon;
};

/* 属性本地名是 key 且**是框架属性**（资源 ID 表里查得到）才收。
 * 自定义命名空间里同名的属性不算 —— 有些包自己定义过 label。 */
static int read_attr(const struct axml *ax, size_t rec, const char *key, struct mval *mv) {
  uint32_t nm = ai_r32(ax->p, ax->n, rec + 4);
  if (!axml_resid(ax, nm)) return 0;
  char k[24];
  k[0] = 0;
  axml_name(ax, nm, k, sizeof k);
  if (strcmp(k, key)) return 0;
  axml_attr_value(ax, rec, mv->txt, sizeof mv->txt, &mv->ref);
  return 1;
}

/* 字面量优先；是引用就去 arsc 的字符串池里查一次 */
static void mval_text(const struct axml *ax, struct arsc *t, const struct mval *mv,
                      char *out, size_t osz) {
  (void)ax;
  out[0] = 0;
  if (mv->txt[0]) { snprintf(out, osz, "%s", mv->txt); return; }
  if (mv->ref && t) arsc_str(t, mv->ref, out, osz);
}

/* 输出前的清洗：换行/制表符会拆坏 TSV，控制字符会让页面错行。
 * 名字来自 APK 内容，不可信，全部过这一道。 */
static void scrub(char *s) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == 0x09 || *p == 0x0a || *p == 0x0d) { s[o++] = 0x20; continue; }
    if (*p < 0x20) continue;
    s[o++] = (char)*p;
  }
  s[o] = 0;
  size_t l = strlen(s);
  while (l && s[l - 1] == 0x20) s[--l] = 0;
  size_t b = 0;
  while (s[b] == 0x20) b++;
  if (b) memmove(s, s + b, l - b + 1);
}

static void axml_scan(const struct axml *ax, struct arsc *t,
                      char *label, size_t lsz, char *iconpath, size_t psz) {
  if (label) label[0] = 0;
  if (iconpath) iconpath[0] = 0;
  const uint8_t *p = ax->p;
  size_t n = ax->n;
  struct mval app_label = {{0}, 0}, app_icon = {{0}, 0};
  struct mval got_label = {{0}, 0}, got_icon = {{0}, 0};
  struct axslot st[AX_DEPTH];
  int depth = 0;

  for (size_t q = ai_r16(p, n, 2); q + 8 <= n; ) {
    uint32_t ty = ai_r16(p, n, q);
    size_t sz = ai_r32(p, n, q + 4);
    if (!sz || q + sz > n) break;
    if (ty == 0x0102u) {                       /* START_ELEMENT */
      size_t a = q + 16;
      if (a + 16 > n) break;
      uint32_t attrStart = ai_r16(p, n, a + 8);
      uint32_t attrSize  = ai_r16(p, n, a + 10);
      uint32_t attrCount = ai_r16(p, n, a + 12);
      if (attrStart < 8) { q += sz; continue; }   /* 属性区不可能在 attrExt 之前 */
      char tag[24];
      tag[0] = 0;
      axml_name(ax, ai_r32(p, n, a + 4), tag, sizeof tag);
      int is_act = (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias"));
      int is_filter = !strcmp(tag, "intent-filter");
      int is_ac = (!strcmp(tag, "action") || !strcmp(tag, "category"));
      memset(&st[depth < AX_DEPTH ? depth : AX_DEPTH - 1], 0, sizeof(struct axslot));
      struct axslot *sp = &st[depth < AX_DEPTH ? depth : AX_DEPTH - 1];
      sp->is_act = is_act;
      sp->is_filter = is_filter;
      for (uint32_t i = 0; i < attrCount; i++) {
        size_t rec = a + attrStart + (size_t)i * (attrSize ? attrSize : 20);
        if (rec + 20 > n) break;
        if (is_ac) {
          struct mval mv = {{0}, 0};
          if (!read_attr(ax, rec, "name", &mv)) continue;
          char v[64];
          mval_text(ax, t, &mv, v, sizeof v);  /* MAIN/LAUNCHER 一般是字面量，引用也认 */
          /* 记在**当前这个 intent-filter** 上：一个活动可以有多个 filter，
             必须同一个 filter 里凑齐 MAIN+LAUNCHER 才算桌面入口。 */
          if (depth > 0 && st[depth - 1].is_filter) {
            if (!strcmp(v, "android.intent.action.MAIN")) st[depth - 1].main_seen = 1;
            else if (!strcmp(v, "android.intent.category.LAUNCHER")) st[depth - 1].launcher_seen = 1;
          }
          continue;
        }
        read_attr(ax, rec, "label", &sp->label);
        read_attr(ax, rec, "icon", &sp->icon);
      }
      if (!strcmp(tag, "application")) { app_label = sp->label; app_icon = sp->icon; }
      if (depth < AX_DEPTH) depth++;
    } else if (ty == 0x0103u) {                 /* END_ELEMENT */
      if (depth > 0) {
        struct axslot s = st[--depth];
        if (s.is_filter && s.main_seen && s.launcher_seen) {
          /* 往上找最近的一个活动，把它声明的 label/icon 收成桌面入口。
             只收**第一个**这样的活动——多个启动图标并存时，
             取第一个，与 launcher 只显一个的行为一致。 */
          for (int i = depth - 1; i >= 0; i--) {
            if (!st[i].is_act) continue;
            if (!got_label.txt[0] && !got_label.ref) got_label = st[i].label;
            if (!got_icon.txt[0] && !got_icon.ref) got_icon = st[i].icon;
            break;
          }
        }
      }
    }
    q += sz;
  }
  if (label) {
    if (got_label.txt[0] || got_label.ref) mval_text(ax, t, &got_label, label, lsz);
    /* 活动上的 label 是个解析不出来的引用时不能就此认输：
       抖音这类包的启动活动引用落在别的包/配置里，而 <application>
       上的名字是好的。先试活动、空了再退应用级。 */
    if (!label[0]) mval_text(ax, t, &app_label, label, lsz);
    scrub(label);
  }
  if (iconpath) {
    if (got_icon.txt[0] || got_icon.ref) mval_text(ax, t, &got_icon, iconpath, psz);
    if (!iconpath[0]) mval_text(ax, t, &app_icon, iconpath, psz);
    /* 路径里有空格/控制符就当没读到：下一跳要拿去当 zip 条目名比对 */
    if (strpbrk(iconpath, "\r\n\t ") || strlen(iconpath) >= psz) iconpath[0] = 0;
  }
}

/* 自适应图标那张 .xml 里，foreground 引用的才是看到的那张图。
 * 只回传引用值，解析交给调用方（它手里有 arsc）。 */
static uint32_t axml_foreground_ref(const struct axml *ax) {
  const uint8_t *p = ax->p;
  size_t n = ax->n;
  uint32_t any = 0;
  for (size_t q = ai_r16(p, n, 2); q + 8 <= n; ) {
    uint32_t ty = ai_r16(p, n, q);
    size_t sz = ai_r32(p, n, q + 4);
    if (!sz || q + sz > n) break;
    if (ty == 0x0102u) {
      size_t a = q + 16;
      if (a + 16 > n) break;
      uint32_t attrStart = ai_r16(p, n, a + 8), attrSize = ai_r16(p, n, a + 10);
      uint32_t attrCount = ai_r16(p, n, a + 12);
      if (attrStart < 8) { q += sz; continue; }
      char tag[24];
      tag[0] = 0;
      axml_name(ax, ai_r32(p, n, a + 4), tag, sizeof tag);
      int in_fg = !strcmp(tag, "foreground");
      for (uint32_t i = 0; i < attrCount; i++) {
        size_t rec = a + attrStart + (size_t)i * (attrSize ? attrSize : 20);
        if (rec + 20 > n) break;
        /* 只要一个引用型的值：android:drawable="@mipmap/..." */
        if (ai_r16(p, n, rec + 8) != 8 || p[rec + 15] != AX_T_REFERENCE) continue;
        uint32_t d = ai_r32(p, n, rec + 16);
        if (in_fg) return d;
        if (!any) any = d;
      }
    }
    q += sz;
  }
  return any;
}

/* ── 对外：一个 APK 的名字 / 图标 ───────────────────────────── */
static int appinfo_load(const char *apk, char *label, size_t lsz, char *decl, size_t dsz) {
  if (label) label[0] = 0;
  if (decl) decl[0] = 0;
  uint8_t *am = NULL;
  size_t amn = 0;
  if (!ai_zip_read(apk, "AndroidManifest.xml", &am, &amn)) return 0;
  struct axml ax;
  if (!axml_open(am, amn, &ax)) { free(am); return 0; }
  uint8_t *ar = NULL;
  size_t arn = 0;
  struct arsc T;
  struct arsc *tp = NULL;
  if (ai_zip_read(apk, "resources.arsc", &ar, &arn) && arsc_open(ar, arn, &T)) tp = &T;
  axml_scan(&ax, tp, label, lsz, decl, dsz);
  if (ar) free(ar);
  free(am);
  return (label && label[0]) || (decl && decl[0]);
}

int appinfo_label(const char *apk, char *out, size_t osz) {
  char decl[AI_PATH_MAX];
  if (osz < AI_LABEL_MAX) return 0;
  return appinfo_load(apk, out, osz, decl, sizeof decl);
}

/* 桌面那张**位图**的路径；取不到返回 0 —— 调用方退回按名字/尺寸猜的兜底。 */
int appinfo_iconpath(const char *apk, char *out, size_t osz) {
  char label[AI_LABEL_MAX], decl[AI_PATH_MAX];
  out[0] = 0;
  if (!appinfo_load(apk, label, sizeof label, decl, sizeof decl)) return 0;
  if (!decl[0]) return 0;
  size_t dl = strlen(decl);
  if (dl > 4 && !strcmp(decl + dl - 4, ".xml")) {
    /* 声明的是 xml（自适应图标 / 矢量图）：先钻进去取 foreground 的引用 */
    uint8_t *xb = NULL;
    size_t xbn = 0;
    char inner[AI_PATH_MAX];
    inner[0] = 0;
    if (ai_zip_read(apk, decl, &xb, &xbn)) {
      struct axml iax;
      uint8_t *ar = NULL;
      size_t arn = 0;
      struct arsc T;
      struct arsc *tp = NULL;
      uint32_t ref = 0;
      if (axml_open(xb, xbn, &iax)) {
        ref = axml_foreground_ref(&iax);
        if (ref && ai_zip_read(apk, "resources.arsc", &ar, &arn) && arsc_open(ar, arn, &T))
          tp = &T;
        if (ref && tp) arsc_str(tp, ref, inner, sizeof inner);
      }
      if (ar) free(ar);
      free(xb);
    }
    if (inner[0]) snprintf(decl, sizeof decl, "%s", inner);
  }
  if (!ai_zip_pick_bitmap(apk, decl, out, osz)) return 0;
  return out[0] != 0;
}

/* labels 子命令：stdin 每行 "包名<TAB>APK 路径"，
 * stdout 每行 "包名<TAB>APK 路径<TAB>应用名"（读不到名字的第三列为空）。
 *
 * APK 路径**原样带回**：shell 那侧直接把这一批结果拼进缓存
 * （pkg → apk → 名字），不依赖"每个输入恰好一行输出"的行号对齐。
 *
 * 批处理而不是一个包一次调用：读一次名字要在中央目录里定位并解出
 * manifest 和 arsc（几 MB），逐包 fork 会几百次重复读目录；
 * 一个进程里循环，IO 一样，省掉进程开销。 */
int appinfo_labels_stdin(void) {
  char line[1024];
  while (fgets(line, sizeof line, stdin)) {
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    char *tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    const char *pkg = line, *apk = tab + 1;
    if (!pkg[0] || !apk[0]) continue;
    char name[AI_LABEL_MAX], decl[AI_PATH_MAX];
    appinfo_load(apk, name, sizeof name, decl, sizeof decl);
    printf("%s\t%s\t%s\n", pkg, apk, name);
    fflush(stdout);
  }
  return 0;
}
