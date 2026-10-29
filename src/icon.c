/*
 * icon.c — 从 APK 里取应用图标，输出 data URI
 *
 * 主路径是守护自己解析 zip；unzip 只当兜底：
   * 部分 ROM 的 toybox 没有 unzip，自带 unzip 的 ROM 之间
   * 版本和列格式也不一致，不能当唯一路径。
   *
   * 兜底不另写一套挑图规则：
   * api.sh 把 `unzip -l` 的条目名喂给守护的 `score` 子命令，
   * 打分只有 icon.c 里 score_name 这一个实现。
 *
 * zip 的中央目录在文件末尾，直接读，不用解包整个 APK（游戏的 res 常上百 MB）。
 * 条目数据用 zlib 的 inflate 解——静态链 NDK sysroot 自带的 libz.a，
 * 不依赖系统 .so。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>   /* has_word 判单词边界要用 isalnum */
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

/* 图标槽位实际占多少**物理**像素：CSS 38px（index.html 的 .pk .ico）
   × dpr（3168×1440 的屏在 WebView 里 dpr 约 3~3.5，取中值 3.25）≈ 124。
   按名字打分和按像素打分都以它为目标，全文件只此一个目标值。
   必须定义在使用之前（score_pixels 在密度表之前引用它）。 */
#define ICON_SLOT_PX 124

/* zip 是小端；显式按字节拼，不依赖主机字节序。 */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* PNG 的字段是大端，和 zip 相反，不能复用上面那个 rd32。 */
static uint32_t rd32be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 一张候选图标。只留名字和偏移量，不留数据：中央目录可能有上千个条目。 */
struct entry {
  char name[192];
  uint64_t off;
  uint32_t csize;
  uint32_t method;
  int score;
};

/* ── 按图片自身特征判断，而不是靠名字 ──────────────────────────────
 *
 * 正式发布包经过 AGP 的资源路径缩短：
 * res/mipmap-xxhdpi/ic_launcher.png 会变成 res/aK.png，
 * mipmap、drawable、launcher 关键词都搜不到，按名字的打分全是 -1，
 * 一张候选都选不出来。这一档就是给这种情况兜底。
 *
 * 不看名字，只看这张图本身：
 * 必须是正方形（图标是正方形，横幅/启动图不是），
 * 边长在 48~512 之间（太小是缩略图，太大 base64 后撑爆 WebView），
 * 再按接近槽位尺寸的程度打分。
 *
 * 没有 resources.arsc 时只能"猜一张方图"，做不到准确，
 * 所以这档的分数压在所有按名字识别的结果之下，只在按名字
 * 选不出候选时才起作用；且只接受 res/ 下的图，不含 assets/
 * （assets 里多是动画帧和纹理，错选概率高）。
 */

/* 只接受 res/ 下的（而不是任何 *.png）。 */
static int under_res(const char *n) {
  return !strncmp(n, "res/", 4);
}

/* PNG：宽高在 IHDR 里，明文大端，不用 inflate。
 * 偏移 16/20 —— 8 字节签名 + 4 长度 + 4 "IHDR"。 */
static int png_dim(const uint8_t *p, size_t len, int *w, int *h) {
  if (len < 24) return 0;
  if (memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return 0;
  if (memcmp(p + 12, "IHDR", 4)) return 0;
  *w = (int)rd32be(p + 16);
  *h = (int)rd32be(p + 20);
  return (*w > 0 && *h > 0);
}

/* WebP：三种容器都要认。
 *   VP8L（无损） 字节 12..14 低 14 位是宽、高，各减 1
 *   VP8 （有损） 帧头起始 0x9d 0x01 0x2a，紧跟 2 字节宽、2 字节高（各 14 位）
 *   VP8X（扩展）  画布宽高在字节 24..29，每维 24 位小端，值是"实际值减 1" */
static int webp_dim(const uint8_t *p, size_t len, int *w, int *h) {
  if (len < 30) return 0;
  if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WEBP", 4)) return 0;
  if (!memcmp(p + 12, "VP8X", 4) && len >= 30) {
    *w = (int)(p[24] | (p[25] << 8) | (p[26] << 16)) + 1;
    *h = (int)(p[27] | (p[28] << 8) | (p[29] << 16)) + 1;
    return (*w > 0 && *h > 0);
  }
  if (!memcmp(p + 12, "VP8L", 4) && len >= 25 && p[20] == 0x2f) {
    uint32_t v = (uint32_t)p[21] | ((uint32_t)p[22] << 8) | ((uint32_t)p[23] << 16);
    *w = (int)(v & 0x3fff) + 1;
    *h = (int)((v >> 14) & 0x3fff) + 1;
    return (*w > 0 && *h > 0);
  }
  if (!memcmp(p + 12, "VP8 ", 4) && len >= 30 && p[23] == 0x9d) {
    *w = (int)(p[26] | (p[27] << 8)) & 0x3fff;
    *h = (int)(p[28] | (p[29] << 8)) & 0x3fff;
    return (*w > 0 && *h > 0);
  }
  return 0;
}

/* 兜底档的分数。返回 -1 表示"这张不像图标"。
 * 分数范围 1..9，刻意压在 drawable 的 10 之下。 */
static int score_pixels(const char *name, const uint8_t *head, size_t hlen) {
  int w, h;
  if (!under_res(name)) return -1;
  const char *dot = strrchr(name, '.');
  if (!dot) return -1;
  int ok;
  if (!strcmp(dot + 1, "png")) ok = png_dim(head, hlen, &w, &h);
  else if (!strcmp(dot + 1, "webp")) ok = webp_dim(head, hlen, &w, &h);
  else return -1;      /* jpg/gif 的头解析没做，宁可不选 */
  if (!ok) return -1;
  if (w != h) return -1;                    /* 必须正方形 */
  if (w < 48 || w > 512) return -1;         /* 太小是缩略图，太大撑爆页面 */
  /* 越接近槽位越高：按对 ICON_SLOT_PX 的相对偏差算，10 分制取整。
   * 目标值与按名字那轮统一，都用 ICON_SLOT_PX。 */
  int d = w > ICON_SLOT_PX ? w - ICON_SLOT_PX : ICON_SLOT_PX - w;
  int q = (d * 9) / ICON_SLOT_PX;
  int s = 9 - q;
  return s < 1 ? 1 : s;
}

#ifdef SF_TEST
/* 仅在 -DSF_TEST 时编译：把上面的 static 函数包一层外部符号供测试链接，
   不改原函数的链接属性，正式产物中不存在。 */
int sf_test_png_dim(const uint8_t *p, size_t len, int *w, int *h) {
  return png_dim(p, len, w, h);
}
int sf_test_webp_dim(const uint8_t *p, size_t len, int *w, int *h) {
  return webp_dim(p, len, w, h);
}
int sf_test_score_pixels(const char *name, const uint8_t *head, size_t hlen) {
  return score_pixels(name, head, hlen);
}
#endif

/* 找 w 这个"整词"：前后都不能再接字母或数字。
   下划线不算字母，是词边界——资源名用下划线分词，
   ic_launcher_round 里的 round 要能认出来；
   background、roundabout、playground 里字母紧贴的 round 要挡住。 */
static int has_word(const char *s, const char *w) {
  size_t wl = strlen(w);
  const char *p = s;
  while ((p = strstr(p, w)) != NULL) {
    int okl = (p == s) || !isalnum((unsigned char)p[-1]);
    int okr = !isalnum((unsigned char)p[wl]);
    if (okl && okr) return 1;
    p += wl;
  }
  return 0;
}

/* 密度名按清晰度从低到高。
   打分是**中间那档最高**，两边递减：xxhdpi(144) > xhdpi(96) > xxxhdpi(192)
   > hdpi(72) > mdpi(48)。
   槽位约 124 物理像素（见 ICON_SLOT_PX）：mdpi 的 48px 要塞进槽位
   是放大约 2.4 倍，糊；xxxhdpi 的 192px 清楚但 base64 后一张几十 KB，
   多了会拖垮 WebView；xxhdpi 的 144px 最接近槽位。
   体积不在这一层卡：score_name 拿不到 csize，
   实际的闸在候选择优处（超预算的先跳，同一档全超才用大的）。 */
static const char *DENSITY[] = { "mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi", NULL };
/* 各密度档的源图边长（px）。mdpi 48 一路到 xxxhdpi 192。 */
static const int DENS_PX[] = { 48, 72, 96, 144, 192 };

/* 取路径的最后一段。分主次看文件名，不看整条路径：
   "icon" 这个子串在目录段 res/mipmap-xxhdpi/ 里本来就有。 */
static const char *base_of(const char *n) {
  const char *s = strrchr(n, '/');
  return s ? s + 1 : n;
}

/* 是图标就打分，越大越好；不是图标返回 -1。 */
static int score_name(const char *n) {
  int i;
  const char *dot = strrchr(n, '.');
  /* 目录条目一律不算候选。中央目录里目录也占一条，
     名字以 '/' 结尾且没有点号，交给下面 strrchr 会拿到 NULL。 */
  if (n[0] && n[strlen(n) - 1] == '/') return -1;
  /* dot 判空不能省：strrchr 找不到点号返回 NULL，
     下面 strcmp(dot + 1, ...) 会解引用空指针，SIGSEGV。
     包里没点号的文件名（"LICENSES"、"META-INF/services/xxx"、
     "assets/channel" 这类）很常见，且每个条目都走 score_name，
     没有前置过滤。 */
  if (!dot) return -1;
  if (strcmp(dot + 1, "png") && strcmp(dot + 1, "webp")
      && strcmp(dot + 1, "jpg") && strcmp(dot + 1, "jpeg")
      && strcmp(dot + 1, "gif")) return -1;
  /* 名字里带 round 的一律让位：ic_launcher_round.png 是很多包自带的
   * 圆形版本，尺寸往往比方形图标小，而同分规则取 csize 小的（见
   * 末尾的择小规则）——圆图标会稳定赢过真正的方形图标。
   *
   * 做法是第一轮整个不收，不是扣分：扣分要么跨档、要么只在本档垫底，
   * 都挡不住 round 胜出。真的只剩 round 的包也没损失：
   * 第二轮像素兜底按图片尺寸挑，会把那张圆图收下，不会变成零候选。
   *
   * 必须按"整词"匹配（has_word），不能直接 strstr(n, "round")：
   * background、playground、roundabout 都含 "round" 子串，
   * 写成子串匹配会把 ic_launcher_background 一并否掉——
   * 而那是下面专门要收的兜底类别。 */
  if (has_word(n, "round")) return -1;
  for (i = 0; DENSITY[i]; i++) {
    char pat[24];
    snprintf(pat, sizeof pat, "mipmap-%s", DENSITY[i]);
    const char *hit = strstr(n, pat);
    size_t plen = strlen(pat);
    /* 密度后面常跟版本限定符：mipmap-xxxhdpi-v4、mipmap-anydpi-v26。
       所以后面要跟 [-/] 或结束，写死 "mipmap-<密度>/" 会一个都选不到。 */
    if (hit && (hit[plen] == '-' || hit[plen] == '/' || hit[plen] == '\0')) {
      /* 越接近 124 槽位越高：xxhdpi 拿满分，mdpi/xxxhdpi 两端递减。
         起点/间距保证全部 mipmap 都高于 drawable 的 10，
         "有 mipmap 就绝不选 drawable"这条不变。 */
      int d = DENS_PX[i] > ICON_SLOT_PX ? DENS_PX[i] - ICON_SLOT_PX
                                        : ICON_SLOT_PX - DENS_PX[i];
      int sc = 39 - (d * 9) / 144;
      /* 同分就选密度更高的（xxhdpi 必须赢过 xhdpi）：
         偏差除以 144 会把两个档压成同一个整数，
         需要再拉开一档，否则"最接近槽位"这个理由就落空了。 */
      if (i >= 3) sc += 1;

      /* ── 按文件名分主次（这一档才是决定性的）──────────────────
       * 自适应图标的包里 mipmap-<密度>/ 同时躺着好几张：
       *   ic_launcher.png            ← 真正的桌面图标（合成前的 legacy 图）
       *   ic_launcher_foreground.png ← 前景层，单独看是残缺的一块
       *   ic_launcher_background.png ← 背景层，通常就是一张纯色/纯形状
       * 三张密度相同 → 分数相同 → 再叠加"同分取字节小的"，
       * 而背景层往往是三者里最小的那张，会被稳定选中，
       * 页面显示为一个纯色方块。
       *
       * 判据用 basename，不用整条路径（见 base_of）。
         加分幅度拉开到 12/6/-10/-20，保证：
           任何一档的 launcher  >  任何一档的普通素材
           任何一档的 launcher  >  任何一档的 fg/bg（fg/bg 只作最后备选）
           最低的 mipmap(35-20=15) 仍 > background 兜底(12) > drawable(10)
         即"有真图标不选底图，整包只剩底图时也有得显示"。 */
      const char *b = base_of(n);
      if (has_word(b, "foreground") || has_word(b, "background"))
        sc -= 20;                                   /* 自适应图标的分层素材 */
      else if (has_word(b, "launcher"))
        sc += 12;                                   /* ic_launcher / ic_launcher_legacy */
      else if (has_word(b, "icon") || has_word(b, "logo"))
        sc += 6;
      else
        sc -= 10;                                   /* mipmap 里的普通素材 */
      return sc;
    }
  }
  /* 现代包（自适应图标）常只剩两张光栅图：一张 background、一张 foreground；
   * XML 变体（mipmap-anydpi-v26/ic_launcher.xml）不解析。
   * 前两条路都选不到时退到 background：它至少是图标底色，
   * 不会是启动图/横幅那类无关素材。
   * 分数 12：压在 drawable(10) 之上、所有 mipmap(35+) 之下——
   * 有真 mipmap 就不选 background。 */
  if (strstr(n, "ic_launcher_background.")) return 12;
  /* 一个 mipmap 都没有的 APK 并不罕见——有些应用（尤其游戏和工具类）
     把图标放在 drawable 里。分数给 10，压在所有 mipmap 之下：
     有 mipmap 就不选 drawable；drawable 里多是启动图、横幅、按钮，
     优先级不能高过真图标，要求名字含 ic_/icon/logo。 */
  if (strstr(n, "drawable-") || strstr(n, "drawable/"))
    return (strstr(n, "ic_") || strstr(n, "icon") || strstr(n, "logo")) ? 10 : -1;
  return -1;
}

#ifdef SF_TEST
/* score_name 的测试壳，仅在 -DSF_TEST 时编译。
   必须放在 score_name 定义之后：放前面会触发前向隐式声明，
   与后面的 static 定义冲突（C99 下是 error：
     static declaration of 'score_name' follows non-static declaration）。 */
int sf_test_score_name(const char *name) {
  return score_name(name);
}
#endif

/* score 子命令：stdin 每行一个条目名，stdout 输出 "分数<TAB>条目名"。
 *
 * 供 api.sh 的 unzip 兜底复用：shell 只负责读条目字节，
 * 挑图规则始终只有 score_name 这一份实现，两边不会各写一套。 */
int icon_score_stdin(void) {
  char nm[256];
  /* 名字可能超 255 字节（深路径的资源名）。fgets 会截断并留下半行，
   * 下一轮读到的是剩余部分、会被当成另一个条目名打分。
   * 分数只用于排序，取错名字的分数不会导致崩溃，但会选错图。
   * 丢弃超长行而不是截断：截断出来的名字多半匹配不上规则，
   * 反而是这个逻辑的正常淘汰路径。 */
  while (fgets(nm, sizeof nm, stdin)) {
    size_t l = strlen(nm);
    int toolong = (l + 1 == sizeof nm) && nm[l - 1] != '\n';
    while (l && (nm[l - 1] == '\n' || nm[l - 1] == '\r')) nm[--l] = 0;
    if (toolong) {
      int c;
      while ((c = getchar()) != '\n' && c != EOF) { }   /* 丢掉这行剩下的部分 */
      continue;
    }
    int sc;
    const char *name = nm, *len = "0";
    /* 输入有两种形态：
       "名字"              —— 只有名字
       "字节数<TAB>名字"    —— shell 从 unzip -l 里保留了 Length 列
       第二种要把字节数原样带回第二列，shell 才能按
       "分数降序、同分取字节小的" 排——与主路择优同一规则。
       只有第一段全是数字才当长度，否则整行还是名字
       （zip 条目名理论上可以含 TAB，不能拿它当分隔符就误切）。 */
    char *tab = strchr(nm, '\t');
    if (tab && tab != nm) {
      size_t k;
      int digits = 1;
      for (k = 0; k < (size_t)(tab - nm); k++)
        if (nm[k] < '0' || nm[k] > '9') { digits = 0; break; }
      if (digits) { *tab = 0; len = nm; name = tab + 1; }
    }
    sc = score_name(name);
    if (sc >= 0) printf("%d\t%s\t%s\n", sc, len, name);
  }
  return 0;
}

static const char *mime_of(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return "image/png";
  if (!strcmp(dot + 1, "webp")) return "image/webp";
  if (!strcmp(dot + 1, "gif")) return "image/gif";
  if (!strcmp(dot + 1, "jpg") || !strcmp(dot + 1, "jpeg")) return "image/jpeg";
  return "image/png";
}

/* 把一个条目的字节解码后写到 stdout 的 data URI。成功返回 1。 */
static int emit(int fd, const struct entry *e) {
  /* 上限 4MB：一张 4K 图 base64 后约 5.5MB，名单几百条时单个包传这么大
     会把管理器的 WebView 撑爆。 */
  if (e->csize == 0 || e->csize > 4u * 1024 * 1024) return 0;

  uint8_t hdr[30];
  if (pread(fd, hdr, 30, (off_t)e->off) != 30) return 0;
  if (rd32(hdr) != 0x04034b50u) return 0;
  uint16_t nlen = rd16(hdr + 26), elen = rd16(hdr + 28);
  uint64_t data = e->off + 30u + nlen + elen;

  static uint8_t in[4u * 1024 * 1024];
  if (pread(fd, in, e->csize, (off_t)data) != (ssize_t)e->csize) return 0;

  uint8_t *body = in;
  size_t blen = e->csize;

  /* APK 里 res 基本都是 store，但有些是 deflate。用之前必须留出解压空间，
     4MB 的上限给 8MB 输出——zlib 的膨胀上界远小于 2，
     超出就当这张图不合格，换下一张。 */
  static uint8_t out[8u * 1024 * 1024];
  if (e->method == 8) {
    z_stream z;
    memset(&z, 0, sizeof z);
    /* windowBits 必须是**负数**：-15 = raw deflate。
       写成 15 会去找 zlib 的两个魔数字节，32 是 gzip 自动识别（RFC 1952），
       而 zip 里的 deflate 流（RFC 1951）两者都不匹配，一律 Z_DATA_ERROR。 */
    if (inflateInit2(&z, -15) != Z_OK) return 0;
    z.next_in = in;
    z.avail_in = e->csize;
    z.next_out = out;
    z.avail_out = sizeof out;
    int r = inflate(&z, Z_FINISH);
    uLong got = sizeof out - z.avail_out;
    inflateEnd(&z);
    if (r != Z_STREAM_END && r != Z_OK) return 0;
    body = out;
    blen = got;
    if (!blen) return 0;
  }

  printf("data:%s;base64,", mime_of(e->name));
  static const char B64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i;
  for (i = 0; i + 2 < blen; i += 3) {
    uint32_t v = ((uint32_t)body[i] << 16) | ((uint32_t)body[i + 1] << 8) | body[i + 2];
    putchar(B64[(v >> 18) & 63]); putchar(B64[(v >> 12) & 63]);
    putchar(B64[(v >> 6) & 63]);  putchar(B64[v & 63]);
  }
  if (i < blen) {
    uint32_t v = (uint32_t)body[i] << 16;
    int rest = (int)(blen - i);
    if (rest == 2) v |= (uint32_t)body[i + 1] << 8;
    putchar(B64[(v >> 18) & 63]); putchar(B64[(v >> 12) & 63]);
    putchar(rest == 2 ? B64[(v >> 6) & 63] : '=');
    putchar('=');
  }
  putchar('\n');
  return 1;
}

/* 单张图留给 WebView 的预算。base64 之后大约膨胀 4/3，
   24KB 压后约 32KB，几百个图标同时挂上去不至于把列表卡住。
   真 launcher 图标（192px PNG）一般 8~20KB，
   会顶穿这条的多半是游戏内美术图。 */
#define ICON_BUDGET (24u * 1024)

/* 按择优规则挑下一张；skip_over_budget 非 0 时先跳过超预算的候选。
   分两趟：先把装得进预算的挑一遍，一个都没有再放开预算挑——
   "预算内没有"就退化成"用最大的那张"，而不是整行没有图标。 */
static int pick_next(struct entry *ents, int n, int skip_over_budget) {
  int best = -1;
  for (int j = 0; j < n; j++) {
    if (ents[j].score < 0) continue;
    /* 声明过的那张（满分）不受预算限制：它就是 manifest 里写的桌面图标，
       大也得用。超预算限制要挡的是"按尺寸猜"那一档里混进来的游戏内美术图，
       拿它去筛确定的答案，结果是用一张几百字节的错图换掉真图。 */
    if (skip_over_budget && ents[j].csize > ICON_BUDGET && ents[j].score < 100) continue;
    /* 同分时取文件更小的一张：图标本身通常只有几 KB，
       而 res/ 里同样尺寸的游戏内素材往往大得多，取小的可降低误选代价。 */
    if (best < 0 || ents[j].score > ents[best].score ||
        (ents[j].score == ents[best].score && ents[j].csize < ents[best].csize))
      best = j;
  }
  return best;
}

/* 入口：apk 路径在 argv[1]。成功往 stdout 吐一行 data URI，失败什么都不吐。
   什么都不吐而不是报错：这只是页面上一张图标，取不到就该退回色块，
   不该把整个名单页变成一个错误提示。

   declared = appinfo.c 从 manifest 的 android:icon（自适应图标还要再钻一层
   xml 取 foreground）加上 resources.arsc 里那张条目的路径。
   给了且匹配上就直接记满分：这张确定是桌面图标，
   不需要再走按名字/按尺寸的猜分流程。 */
int icon_from_apk(const char *apk, const char *declared) {
  int fd = open(apk, O_RDONLY);
  if (fd < 0) { fprintf(stderr, "E/OPEN\n"); return 0; }
  off_t sz = lseek(fd, 0, SEEK_END);
  /* 每一条"打不开/读不了"的出口分开报码，api.sh 才能给出准确说明：
   * 挑不到候选是 NOENTRY，打得开但解不出是 EMIT，
   * 压根不是文件是 TOOBIG，这里是 OPEN。
   * "包太大"和"文件坏了"不能合成同一个码。 */
  if (sz <= 0) {
    fprintf(stderr, "E/OPEN\n");   /* 不是普通文件，或长度读不出来 */
    close(fd);
    return 0;
  }
  /* 4GB 是硬上限：下面 EOCD 解析里的 cdsz/cdoff 是 uint32_t，
   * ZIP64 分支也只取 8 字节字段的低 32 位。超过 4GB 时 CD 偏移会被
   * 截断成错的值，而截断后 (cdoff + cdsz <= sz) 反而更容易判成"装得下"，
   * 造成静默读错数据。
   * 真要支持 >4GB，必须先把 cdsz/cdoff 改成 uint64_t 并读满 8 字节。 */
  if (sz > 4LL * 1024 * 1024 * 1024) {
    fprintf(stderr, "E/TOOBIG\n");
    close(fd);
    return 0;
  }

  /* 只读中央目录所在的尾部，不把整个 APK 读进内存。
   *
   * 4MB 的窗口盖不住大条目数的包（ZIP64 的包条目 > 65535 时
   * 中央目录轻松上兆，大型游戏包光资源条目就几万个），
   * 所以分两段读：
   *   1) 先读 4MB 尾部，找 EOCD；
   *   2) 从 EOCD 拿到中央目录的真实大小和偏移，
   *      不装进当前窗口就**再读一段**把目录本身读进来。
   * 仍然不读整个文件——图标只需要中央目录，
   * 图片数据是后面 emit() 按偏移单独 pread 的。 */
  size_t want = 4u * 1024 * 1024;
  if ((size_t)sz < want) want = (size_t)sz;
  uint8_t *buf = malloc(want);
  if (!buf) { fprintf(stderr, "E/OPEN\n"); close(fd); return 0; }
  size_t base = (size_t)sz - want;
  ssize_t got = pread(fd, buf, want, (off_t)base);
  if (got <= 0) { fprintf(stderr, "E/OPEN\n"); close(fd); free(buf); return 0; }
  /* 目录不在第一段窗口里时，buf 会被换成只装中央目录的那一段，
     base 随之改成 cdoff。见下面 EOCD 解析处的重读分支。
     fd 一直留着不关，重读分支还要用它。 */

  /* 中央目录扫描里的偏移是相对整个文件的，往回加回 base。 */
#define ENTS_MAX 64
  struct entry ents[ENTS_MAX];
  /* 候选最多 64 个：真 APK 里能用的图标远少于这个数，
     而中央目录可能有几千个条目，不全存下来。
     不够用时才动用像素兜底。 */
  int n = 0;
  /* 中央目录到底有没有读到。没读到却一个候选也没有时，
     原因码是 OPEN 而不是 NOENTRY："没读到目录"和
     "包里没有方图"是两种不同的故障，要能区分。 */
  int saw_cd = 0;
  {
    size_t i;
    long eocd = -1;
    size_t lim = (size_t)got > 65536 ? 65536 : (size_t)got;
    for (i = (size_t)got - lim; i + 22 <= (size_t)got; i++)
      if (rd32(buf + i) == 0x06054b50u) { eocd = (long)i; break; }
    if (eocd >= 0) {
      uint32_t cdsz = rd32(buf + eocd + 12);
      uint32_t cdoff = rd32(buf + eocd + 16);

      /* ZIP64：条目数或偏移超过 32 位时，标准 EOCD 里
         cd 尺寸、cd 偏移、条目总数三个字段被写满 0xFFFFFFFF，
         真值放在紧挨着它前面的 ZIP64 记录（0x06064b50）里。
         不读这一段就只能拿到 0xFFFFFFFF，
         "目录落在窗口里"的判断必然不成立 → 整包 E/OPEN。
         定位器（0x07064b50，20 字节）紧贴在 EOCD 前面，
         里面第 4 个 u64 是 ZIP64 EOCD 记录自己的文件偏移。 */
      if (cdsz == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) {
        long zl = eocd - 20;
        if (zl >= 0 && rd32(buf + zl) == 0x07064b50u) {
          uint64_t zoff = (uint64_t)rd32(buf + zl + 8)
                        | ((uint64_t)rd32(buf + zl + 12) << 32);
          /* zoff 是**整个文件里的绝对偏移**，got 是**当前窗口的长度**，
             两者不能直接比：先把绝对偏移减掉窗口起点 base，
             再跟缓冲区长度比。 */
          if (zoff >= base && zoff - base + 56 <= (size_t)got) {
            size_t zp = (size_t)(zoff - base);
            if (rd32(buf + zp) == 0x06064b50u) {
              /* ZIP64 EOCD 固定部分：+24 条目数、+40 cd 大小、+48 cd 偏移 */
              cdsz = (uint32_t)rd32(buf + zp + 40);
              cdoff = (uint32_t)rd32(buf + zp + 48);
              /* 这两个字段本身也可能是 32 位放不下的真 ZIP64 记录，
                 真值在 ZIP64 EOCD 记录后面第 8 字节起（偏移 +56），
                 每项 8 字节，顺序：cd 大小、cd 偏移。
                 本实现上限就是 4GB（上面 TOOBIG 那条硬边界），
                 cd 偏移一定落在 32 位里，先按 32 位处理；
                 真超出时不能截断成 0xFFFFFFFF——截断出来的偏移
                 会让后面的范围判断误判成"装得下"。 */
              if (cdsz == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) {
                if (zp + 56 + 16 <= (size_t)got) {
                  uint64_t v = (uint64_t)rd32(buf + zp + 56)
                             | ((uint64_t)rd32(buf + zp + 60) << 32);
                  if (cdsz == 0xFFFFFFFFu)
                    cdsz = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
                  if (cdoff == 0xFFFFFFFFu && zp + 56 + 16 <= (size_t)got) {
                    uint64_t w = (uint64_t)rd32(buf + zp + 64)
                               | ((uint64_t)rd32(buf + zp + 68) << 32);
                    cdoff = w > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)w;
                  }
                }
              }
            }
          }
        }
      }

      /* 中央目录装不进当前窗口时，按它的大小再读一段。
         不调大第一段：那等于把整个 APK 读进内存。
         base 改成 cdoff 之后，下面所有"往回加 base"的换算都还成立。 */
      if (!(cdoff >= base && (size_t)cdoff + cdsz <= (size_t)sz)) {
        if (cdoff < (size_t)sz && cdsz > 0 && (size_t)cdoff + (size_t)cdsz <= (size_t)sz
            && (size_t)cdsz <= 128u * 1024 * 1024) {
          /* realloc 成功后旧指针失效，不能再 free(buf)：
             大 APK 的中央目录超过 4MB 窗口时走这里，旧写法会双重释放。 */
          uint8_t *nb = realloc(buf, (size_t)cdsz);
          if (nb) {
            ssize_t g2 = pread(fd, nb, (size_t)cdsz, (off_t)cdoff);
            if (g2 > 0) {
              buf = nb;
              base = (size_t)cdoff;
              got = g2;
            } else {
              free(nb);
              buf = NULL;
              got = 0;
              base = 0;
            }
          }
        }
      }

      /* 目录已经整个在内存里了（第一次就装下，或重读成功）。 */
      if (cdoff >= base && (size_t)cdoff - base + cdsz <= (size_t)got
          && (size_t)cdoff + cdsz <= (size_t)sz) {
        saw_cd = 1;
        size_t p = (size_t)cdoff - base, end = p + cdsz;
        if (end > (size_t)got) end = (size_t)got;   /* pread 可能短读 */

        /* 两轮扫描：
         *   第一轮只按名字挑。分数高的**挤掉**分数低的，
         *   不是先到先得——中央目录里可能有几千条，
         *   正式包里 res/ 排前面、assets/ 排后面，
         *   谁在前是打包器决定的，跟是不是图标无关。
         *   第二轮只在第一轮一个都没选出来时才跑，
         *   按图片自身特征兜底。
         */
        char nm[192];
        /* 第二轮要逐个条目去读图片头，所以文件只开一次、整轮复用。 */
        int fdpeek = open(apk, O_RDONLY);
        /* 条件必须是 n==0：第二轮只在第一轮一个候选都没选出来时才跑，
         * 不是"没填满就跑"。 */
        for (int pass = 0; pass < 2 && n == 0; pass++) {
          for (size_t q = (size_t)cdoff - base; q + 46 <= end; ) {
            if (rd32(buf + q) != 0x02014b50u) break;
            uint16_t nlen = rd16(buf + q + 28), elen = rd16(buf + q + 30),
                     clen = rd16(buf + q + 32);
            uint32_t loff = rd32(buf + q + 42);
            uint32_t csz  = rd32(buf + q + 20);
            uint16_t meth = rd16(buf + q + 10);
            if (q + 46u + nlen > end) break;

            if (nlen < sizeof nm) {
              /* 先把文件名拷出来并补 NUL，再打分。
               * 中央目录里没有 NUL 结尾，文件名后面紧挨的是下一个
               * 条目头的头几个字节（PK\x01\x02）。strrchr/strstr 就顺着
               * 读过去了，"....png" 后面多出 0x50 0x4b 0x01 0x02，
               * 扩展名判断于是对每个条目都不成立。
               */
              memcpy(nm, buf + q + 46, nlen);
              nm[nlen] = 0;

              int sc = pass == 0 ? score_name(nm) : -1;
              /* 声明过的那张直接封顶：它不需要"像不像图标"这轮判断。
                 必须放在 pass 判断之后、像素兜底之前，
                 否则资源名被缩短的包还是先落进"按尺寸猜"那一档。 */
              if (declared && !strcmp(nm, declared)) sc = 100;
              if (sc < 0 && pass == 1 && fdpeek >= 0) {
                /* 像素兜底要读图片头，得跳到数据偏移去 pread。
                 * 读前 32 字节：PNG 的 IHDR 到 24 字节，
                 * WebP 的 VP8X 画布宽高到 30 字节，都够。
                 * （VP8L 只要到 25，但按最大的那个算，统一 32。）
                 *
                 * 数据偏移必须用**本地文件头**里的名字长度和扩展长度算，
                 * 不能用中央目录那两个值：zipalign/AAPT 会给 STORED 条目
                 * 的本地扩展字段加对齐填充，两边长度就是不相等，
                 * 用中央目录的值会读到填充字节，图片头解析失败，
                 * 这张图被无声丢掉。emit() 里已经是按本地头做的，
                 * 这里保持一致。 */
                uint8_t lh[30], head[32];
                if (pread(fdpeek, lh, 30, (off_t)loff) == 30 &&
                    rd32(lh) == 0x04034b50u) {
                  uint64_t data = (uint64_t)loff + 30u + rd16(lh + 26) + rd16(lh + 28);
                  size_t want2 = csz < sizeof head ? csz : sizeof head;
                  ssize_t g2 = pread(fdpeek, head, want2, (off_t)data);
                  if (g2 > 0) {
                    /* DEFLATE 条目这里读到的是**压缩流**，不是图片头，
                     * PNG 魔数不在，score_pixels 必然 -1，
                     * 不处理的话第二轮只对 STORED 条目有效。
                     * 就地解头几 KB 就够读 PNG/WebP 的头，代价很小——
                     * 第二轮本来就只在一张都没挑出来时才跑。
                     * 判据只能是 meth == 8：不能加"读到的字节比 csz 少"
                     * 这类条件——pread 拿到的长度恒等于 min(csz,32)，
                     * 那种条件永远为假。 */
                    if (meth == 8) {
                      uint8_t zin[2048];
                      uint8_t zout[2048];
                      size_t zn = csz < sizeof zin ? csz : sizeof zin;
                      if (pread(fdpeek, zin, zn, (off_t)data) == (ssize_t)zn) {
                        z_stream z;
                        memset(&z, 0, sizeof z);
                        if (inflateInit2(&z, -15) == Z_OK) {
                          z.next_in = zin;
                          z.avail_in = zn;
                          z.next_out = zout;
                          z.avail_out = sizeof zout;
                          (void)inflate(&z, Z_NO_FLUSH);
                          size_t zn2 = sizeof zout - z.avail_out;
                          inflateEnd(&z);
                          if (zn2)
                            sc = score_pixels(nm, zout, zn2);
                        }
                      }
                    }
                    if (sc < 0)
                      sc = score_pixels(nm, head, (size_t)g2);
                  }
                }
              }

              if (sc >= 0) {
                /* 满了就挤掉分数最低的那个（自己不比新来的高就不换）。 */
                int slot = n;
                if (n < ENTS_MAX) {
                  n++;
                } else {
                  int worst = 0;
                  for (int k = 1; k < n; k++)
                    if (ents[k].score < ents[worst].score) worst = k;
                  if (ents[worst].score >= sc) { q += 46u + nlen + elen + clen; continue; }
                  slot = worst;
                }
                memcpy(ents[slot].name, nm, nlen + 1);
                ents[slot].off   = loff;
                ents[slot].csize = csz;
                ents[slot].method = meth;
                ents[slot].score = sc;
              }
            }
            q += 46u + nlen + elen + clen;
          }
        }
        if (fdpeek >= 0) close(fdpeek);
      }
    }
  }
  /* 尾部那个 fd 是给重读中央目录用的，扫完就还回去。
     不关的话每次取图标都漏一个描述符，列表滚几十行就撞上限。 */
  close(fd);
  free(buf);
  /* 顺序不能反：saw_cd 只在中央目录块里置 1，n 也只在同一块里自增，
     所以 saw_cd==0 必然蕴含 n==0；先判 !n 的话"目录没读到"
     会被报成 NOENTRY。先判 saw_cd：目录没读到报 OPEN，
     读到了却没挑出候选才是 NOENTRY。 */
  if (!saw_cd) { fprintf(stderr, "E/OPEN\n"); return 0; }
  if (!n) { fprintf(stderr, "E/NOENTRY\n"); return 0; }   /* 一张候选都没挑中 */

  /* 分数高的先试。一张不合格再试下一张——
     免得"最想选的那张恰好是损坏的"直接整个功能失效。 */
  for (int tried = 0; tried < n; tried++) {
    int best = pick_next(ents, n, 1);
    if (best < 0) best = pick_next(ents, n, 0);   /* 预算内没有就放开 */
    if (best < 0) break;   /* 没有可用候选了 */
    fd = open(apk, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "E/OPEN\n"); return 0; }
    int ok = emit(fd, &ents[best]);
    close(fd);
    if (ok) return 1;
    ents[best].score = -1;   /* 这张不行，换下一张 */
  }
  fprintf(stderr, "E/EMIT\n");   /* 挑中了但一张都解不出来 */
  return 0;
}