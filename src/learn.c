/*
 * learn.c — 常态封顶下的频率驻留采样
 *
 * 只采集，不判断。判据在 webroot/api.sh 的 advice 子命令里（awk），
 * 改阈值不用重编二进制。
 *
 * 两份样本、两层用途：
 *   learn.tsv      一行一个 policy 目录  -> "全局这道帽紧不紧"
 *   learn_app.tsv  一行一个 (包, 档)     -> "这个应用自己那道帽紧不紧"
 * 全局帽生效的那一段，两层都记：它既是全局样本，也是这个前台包在全局帽下的
 * 表现。只有应用专属档位生效时才互斥——那段分布只属于这个应用，核心身上是
 * 应用帽，把它记进全局行等于拿应用值下的表现去建议全局值。互相喂出的偏差
 * 会一路自我强化，比样本变稀糟得多。
 *
 * 采样前提：这一簇的封顶此刻真的生效，且生效的就是用户设的那道帽。
 * 熄屏、开机满血窗口、游戏解锁、应用专属满血、实时偏移改过帽的时段都不算。
 *
 * 应用行带一个"采样时那道帽是多少"的记忆：帽换过就从零重攒。
 * 同一个包里混着 65% 和 70% 两段分布，顶帽率会一直停在旧帽上，
 * 判据会沿着 70→75→80 一路放宽，哪怕应用早就碰不到新帽了。
 *
 * 记录的是**占硬件最高频的百分比分布**（24 个桶），不是逐频点计数：
 * 建议的输出粒度就是百分比，桶和内核的频点表无关，换内核也不用重算。
 * 24 个桶 = 每桶 4.17%，够判"贴没贴帽"；再细也超出建议本身 5% 的粒度。
 *
 * 单位一律不换算。time_in_state 在不同内核里可能是 jiffies 也可能是 ms，
 * 这里只用比值（顶帽占比）和分位数（在时间分布里的位置）；
 * 需要"多少分钟"时用的是真实流逝的墙钟时间，不碰 time_in_state 的数值语义。
 *
 * 基线（prev 数组）只存在全局行里，一个目录一份；按应用行只存累计量。
 * 驻留表一个目录只有一份读数，让每个包各存一份基线的话，从第二轮起算出来的
 * 差值会把别的包跑掉的时间也记在这个包头上。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define LEARN_MAX_POL 16
#define LEARN_APP_MAX 320         /* 按应用行上限：包数 × 档数 */
#define LEARN_PKG_LEN 256         /* 与守护的 PKG_LEN 同宽，包名不截断 */
#define LEARN_BANDS   24
#define LEARN_ROWS    64          /* time_in_state 最多看前 64 行 */
#define LEARN_FLUSH_S 300         /* 落盘间隔 */
#define LEARN_STUCK_MS 60000      /* 单次 tick 增量上限：超过说明隔了很久，丢掉 */
#define LEARN_FORMAT  2           /* 样本格式：24 桶 + 采样时的帽百分比 */
#define LEARN_HEADER  "#fmt=2"

/* 两份样本共用的累计量，差别只在键是目录还是包名 */
struct learn_acc {
  long sampled_ms;
  long pin_ms;
  long held_ms;       /* 实际上限被别人压在我们的帽之下的墙钟时间 */
  long band[LEARN_BANDS];
  unsigned daybits;
  long cap_pct;       /* 这段累计是在哪道配置帽（百分比）下攒的；0 = 未对上 */
};

struct tier_stat {
  char dir[160];
  int  tier;
  struct learn_acc a;
  /* time_in_state 基线：按行号对齐；行序变了就当没读到，宁可少采一拍 */
  long prev[LEARN_ROWS];
  long prev_sleep[LEARN_ROWS];
  int  prev_n;
};

struct app_stat {
  char pkg[LEARN_PKG_LEN];
  int  tier;
  struct learn_acc a;
};

/* 这一拍某个目录攒出来的增量，先攒着再分发到两层 */
struct learn_delta {
  long band[LEARN_BANDS];
  long pin_ms;
};

static struct tier_stat g_st[LEARN_MAX_POL];
static int g_n;
static struct app_stat g_as[LEARN_APP_MAX];
static int g_nas;
static long g_last_mono_ms;
static time_t g_last_flush;
static int g_seen_file;   /* 样本文件确实存在过——区别于"这台机还从来没写过" */
static int g_seen_app;
static int g_learn_off;
static char g_path[224];
static char g_apath[224];
/* 学习计时起点（epoch 秒）。天数是"从起点起满 24 小时算一天"，
 * 不按本地/UTC 零点切——否则晚上装、早上看就成两天了。 */
static long g_since;
static char g_since_path[240];
/* 旧格式被重置时留的标记：页面据此提示"新算法需重新积累样本"。有样本后删除。 */
static char g_reset_mark[240];

static long learn_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 纯函数：自检直接喂时钟验"过 0 点不加天、满 24 小时才加"。 */
long learn_day_index(long since, long now) {
  if (since <= 0) return 0;
  long d = now - since;
  if (d < 0) d = 0;
  return (d / 86400) % 28;
}

static void learn_since_load(void) {
  g_since = 0;
  if (!g_since_path[0]) return;
  FILE *f = fopen(g_since_path, "r");
  if (!f) return;
  long v = 0;
  if (fscanf(f, "%ld", &v) == 1 && v > 0) g_since = v;
  fclose(f);
}

static void learn_since_save(void) {
  if (!g_since_path[0] || g_since <= 0) return;
  FILE *f = fopen(g_since_path, "w");
  if (!f) return;
  fprintf(f, "%ld\n", g_since);
  fclose(f);
}

static void learn_since_drop(void) {
  g_since = 0;
  if (g_since_path[0]) unlink(g_since_path);
}

static int learn_band(long freq, long hw) {
  if (hw <= 0 || freq <= 0) return 0;
  int b = (int)((freq * LEARN_BANDS) / hw);
  return b > LEARN_BANDS - 1 ? LEARN_BANDS - 1 : b;
}

/* 这一簇**现在**的实际上限。厂商热限频会直接把 scaling_max_freq 压到
 * 我们写的值以下（一加 13 上量到过：要 62% 是 2678400，实际停在 1689600），
 * 那种时段"核贴着上限跑"贴的是它的帽、不是我们的帽，
 * 拿这份分布建议收紧或放宽都是错的。读不到就按 0，调用处回落到我们写的值。 */
static long learn_read_max(const char *dir) {
  char path[224];
  snprintf(path, sizeof path, "%s/scaling_max_freq", dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  char b[32];
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return 0;
  b[n] = 0;
  return strtol(b, NULL, 10);
}

static struct tier_stat *learn_slot(const char *dir, int tier) {
  for (int i = 0; i < g_n; i++)
    if (!strcmp(g_st[i].dir, dir)) { g_st[i].tier = tier; return &g_st[i]; }
  if (g_n >= LEARN_MAX_POL) return NULL;
  struct tier_stat *s = &g_st[g_n++];
  memset(s, 0, sizeof *s);
  snprintf(s->dir, sizeof s->dir, "%s", dir);
  s->tier = tier;
  return s;
}

/* 找到（或腾出）这个包这一档的那一行。
 * 满了就换掉累计时长最小的那行：按应用的样本本来就稀，一个天天用的包
 * 不该被几个只开过一次的包挤掉。 */
static struct app_stat *learn_app_slot(const char *pkg, int tier) {
  if (!pkg || !pkg[0] || tier < 0 || tier > 4) return NULL;
  for (int i = 0; i < g_nas; i++)
    if (g_as[i].pkg[0] && g_as[i].tier == tier && !strcmp(g_as[i].pkg, pkg))
      return &g_as[i];
  int slot = -1, least_i = -1;
  long least = 0;
  for (int i = 0; i < g_nas; i++) {
    if (!g_as[i].pkg[0]) { slot = i; break; }
    if (least_i < 0 || g_as[i].a.sampled_ms < least) {
      least_i = i; least = g_as[i].a.sampled_ms;
    }
  }
  if (slot < 0 && g_nas < LEARN_APP_MAX) slot = g_nas++;
  if (slot < 0) slot = least_i;
  if (slot < 0) return NULL;
  struct app_stat *a = &g_as[slot];
  memset(a, 0, sizeof *a);
  snprintf(a->pkg, sizeof a->pkg, "%s", pkg);
  a->tier = tier;
  return a;
}

static void learn_put(struct learn_acc *a, const struct learn_delta *d) {
  for (int i = 0; i < LEARN_BANDS; i++) a->band[i] += d->band[i];
  a->pin_ms += d->pin_ms;
}

/* LEARN=0：停采，并把累计和两份数据文件一起清掉。
 * 关掉学习不该留下半截数据——下次打开时那些样本已经对不上当时的配置了。
 * 两份一起清：只清全局那份的话，重新打开时按应用那层会拿旧分布接着攒。 */
void learn_set_enabled(int on) {
  g_learn_off = !on;
  if (!g_learn_off) return;
  /* 关掉学习 = 清空样本与计时起点；已经清干净了就不要再每轮 unlink 一遍。
     旧格式刚被重置时累计是空的、但标记还在，那也要进来把标记撤掉。 */
  if (!g_seen_file && !g_seen_app && !g_n && !g_nas && g_since <= 0
      && (!g_reset_mark[0] || access(g_reset_mark, F_OK) != 0)) return;
  if (g_path[0]) unlink(g_path);
  if (g_apath[0]) unlink(g_apath);
  if (g_reset_mark[0]) unlink(g_reset_mark);
  learn_since_drop();
  g_n = 0;
  g_nas = 0;
  g_last_mono_ms = 0;
  g_seen_file = 0;
  g_seen_app = 0;
  memset(g_st, 0, sizeof g_st);
  memset(g_as, 0, sizeof g_as);
}

/*
 * time_in_state 行格式：`<freq> <residency> [sleep_residency]`
 * 第三列存在时是"该频点上处于休眠的时间"，扣掉才是真正在跑的驻留。
 * dt_ms 传 0 表示只刷基线、不取增量（冷启动、隔久了第一拍）。
 * d 为 NULL 时同样只刷基线：封顶没生效的时段（熄屏、满血窗口、游戏解锁）
 * 采进来的分布回答不了"这道帽扣得紧不紧"，混进去会把顶帽占比抬高。
 */
static int learn_sample_state(struct tier_stat *st, struct learn_delta *d,
                              long dt_ms, long hw, long cap) {
  char path[224];
  snprintf(path, sizeof path, "%s/stats/time_in_state", st->dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  static char buf[8192];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;

  int rows = 0, had_base = 0;
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line && rows < LEARN_ROWS;
       line = strtok_r(NULL, "\n", &save)) {
    long f = 0, r = 0, s = 0;
    int got = sscanf(line, "%ld %ld %ld", &f, &r, &s);
    if (got < 2 || f <= 0) continue;
    if (st->prev_n > rows) {
      had_base = 1;
      long dd = r - st->prev[rows];
      long ds = (got >= 3) ? s - st->prev_sleep[rows] : 0;
      if (dt_ms > 0 && d && dd > 0) {
        long active = dd - (ds > 0 ? ds : 0);
        if (active < 0) active = 0;
        /* 一个 policy 下多颗核同频点时驻留会叠加，可能超过墙钟时间；
           桶里存的是"核·毫秒"，只用来算分布，不当时长用，所以不强行摊平。 */
        d->band[learn_band(f, hw)] += active;
        long pin_at = cap > 0 ? cap : hw;
        if (f * 100 >= pin_at * 97) d->pin_ms += active;
      }
    }
    st->prev[rows] = r;
    st->prev_sleep[rows] = (got >= 3) ? s : 0;
    rows++;
  }
  int aligned = (st->prev_n >= rows);
  st->prev_n = rows;
  return had_base && aligned;
}

/* 没有 stats 目录时的降级通道：看一眼当前频率，整段墙钟时间记进那个桶 */
static int learn_sample_cur(struct tier_stat *st, struct learn_delta *d,
                            long dt_ms, long hw, long cap) {
  if (!d || hw <= 0 || dt_ms <= 0) return 0;
  char path[224];
  snprintf(path, sizeof path, "%s/scaling_cur_freq", st->dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  char b[32];
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return 0;
  b[n] = 0;
  long f = strtol(b, NULL, 10);
  if (f <= 0) return 0;
  d->band[learn_band(f, hw)] += dt_ms;
  if (cap > 0 && f * 100 >= cap * 97) d->pin_ms += dt_ms;
  return 1;
}

/* 读一行（或几行）空格分隔的整数，返回读到的个数 */
static int learn_read_list(const char *path, long *out, int max) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  static char buf[4096];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;
  int c = 0;
  char *save = NULL;
  for (char *t = strtok_r(buf, " \t\n", &save); t && c < max;
       t = strtok_r(NULL, " \t\n", &save)) {
    char *end = NULL;
    long v = strtol(t, &end, 10);
    if (end == t) continue;          /* 表头那种 "time(ms)" 直接跳过 */
    out[c++] = v;
  }
  return c;
}

/*
 * KGSL 的 GPU 驻留表：<dir>/gpu_clock_stats 是一行 N 个数，
 * 与 <dir>/gpu_available_frequencies 按位置一一对应（同一张表，降序）。
 * 数值单位和 time_in_state 一样不去解释，只做差值进直方图。
 *
 * 为什么不用 devfreq 的 trans_stat：KGSL 走厂商 governor，那表最后
 * 一列 time(ms) 在真机上 5 秒纹丝不动（只在切换事件时被顺手更新），
 * 采了等于没采；gpu_clock_stats 是连续累计的，实测会动。
 */
static int learn_sample_kgsl(struct tier_stat *st, struct learn_delta *d,
                             long dt_ms, long hw, long cap) {
  char path[224];
  static long f[LEARN_ROWS], s[LEARN_ROWS];
  snprintf(path, sizeof path, "%s/gpu_available_frequencies", st->dir);
  int nf = learn_read_list(path, f, LEARN_ROWS);
  if (nf <= 0) return 0;
  snprintf(path, sizeof path, "%s/gpu_clock_stats", st->dir);
  int ns = learn_read_list(path, s, LEARN_ROWS);
  if (ns != nf) return 0;            /* 两张表对不上就不采，宁缺勿假 */
  int had = (st->prev_n == nf);
  if (had) {
    for (int i = 0; i < nf; i++) {
      long dd = s[i] - st->prev[i];
      if (dt_ms <= 0 || !d || dd <= 0 || f[i] <= 0) continue;
      d->band[learn_band(f[i], hw)] += dd;
      long pin_at = cap > 0 ? cap : hw;
      if (f[i] * 100 >= pin_at * 97) d->pin_ms += dd;
    }
  }
  st->prev_n = nf;
  for (int i = 0; i < nf; i++) st->prev[i] = s[i];
  return had;
}

/*
 * 两份文件同构，一行 31 个字段：
 *   #fmt=2
 *   <键> <档> <采样ms> <顶帽ms> <被压ms> <24 个桶> <哪几天见过> <采样时的帽%>
 * 键是目录（全局）或包名（按应用）。档 0..4 = BIG/MID2/MID/LITTLE/GPU。
 * cap% 是**配置意图**（CPU_PCT_* / GPU_PCT），不是吸附后的 Hz：
 * 用户改百分比就作废这一档的累计，厂商把上限压得更低由 held 单独记。
 * 字段不齐的行（写盘被打断留下的半截行）整条丢掉：宁可少一层数据，
 * 也不能让判据拿着半截分布出建议。
 */
#define LEARN_TAIL (3 + LEARN_BANDS + 1 + 1)   /* 键和档之后还剩多少字段 */

/* 0=文件不存在/空；1=旧格式（没有 v2 头）；2=v2 */
static int learn_format(const char *path) {
  if (!path || !path[0]) return 0;
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[96];
  int r = 0;
  if (fgets(line, sizeof line, f)) {
    if (!strncmp(line, LEARN_HEADER, strlen(LEARN_HEADER))) r = LEARN_FORMAT;
    else {
      char *p = line;
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
      r = *p ? 1 : 0;
    }
  }
  fclose(f);
  return r;
}

static int learn_read_rows(const char *path, int is_app) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[2048];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    char *save = NULL;
    char *key = strtok_r(line, " \t\n", &save);
    char *tt = strtok_r(NULL, " \t\n", &save);
    if (!key || !tt) continue;
    int tier = (int)strtol(tt, NULL, 10);
    if (tier < 0 || tier > 4) continue;
    /* 键被截断过就不可信：那可能指向另一个应用、另一个目录 */
    size_t klen = strlen(key);
    if (!klen || klen >= (is_app ? (size_t)LEARN_PKG_LEN : sizeof g_st[0].dir)) continue;
    long v[LEARN_TAIL];
    int k = 0;
    for (char *tok = strtok_r(NULL, " \t\n", &save); tok && k < LEARN_TAIL;
         k++, tok = strtok_r(NULL, " \t\n", &save))
      v[k] = strtol(tok, NULL, 10);
    if (k < LEARN_TAIL) continue;
    struct learn_acc a;
    memset(&a, 0, sizeof a);
    a.sampled_ms = v[0];
    a.pin_ms = v[1];
    a.held_ms = v[2];
    for (int i = 0; i < LEARN_BANDS; i++) a.band[i] = v[3 + i];
    a.daybits = (unsigned)v[3 + LEARN_BANDS];
    a.cap_pct = v[3 + LEARN_BANDS + 1];
    if (a.cap_pct < 0 || a.cap_pct > 100) continue;
    if (is_app) {
      struct app_stat *s = learn_app_slot(key, tier);
      if (!s) continue;
      s->a = a;
    } else {
      struct tier_stat *s = learn_slot(key, tier);
      if (!s) continue;
      s->a = a;
    }
    n++;
  }
  fclose(f);
  return n;
}

static void learn_write_acc(FILE *f, const struct learn_acc *a) {
  fprintf(f, " %ld %ld %ld", a->sampled_ms, a->pin_ms, a->held_ms);
  for (int b = 0; b < LEARN_BANDS; b++) fprintf(f, " %ld", a->band[b]);
  fprintf(f, " %u %ld\n", a->daybits, a->cap_pct);
}

static int learn_write_rows(const char *path, int is_app) {
  char tmp[264];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) return 0;
  fprintf(f, "%s\n", LEARN_HEADER);
  if (is_app) {
    for (int i = 0; i < g_nas; i++) {
      if (!g_as[i].pkg[0]) continue;
      fprintf(f, "%s %d", g_as[i].pkg, g_as[i].tier);
      learn_write_acc(f, &g_as[i].a);
    }
  } else {
    for (int i = 0; i < g_n; i++) {
      fprintf(f, "%s %d", g_st[i].dir, g_st[i].tier);
      learn_write_acc(f, &g_st[i].a);
    }
  }
  fclose(f);
  /* 原子替换：半截文件会让下一次读回拿到不完整的行 */
  if (rename(tmp, path)) { remove(tmp); return 0; }
  return 1;
}

void learn_init(const char *path, const char *apath) {
  /* 从零初始化：同一进程里换一份样本路径（自检会这么做）时，
     上一份的 seen 标记不清掉，新文件的第一拍会被 learn_check_reset
     当成"文件被删过"而丢掉。守护只在启动时调一次，这里不涉及热切换。 */
  g_n = 0;
  g_nas = 0;
  g_last_mono_ms = 0;
  g_seen_file = 0;
  g_seen_app = 0;
  memset(g_st, 0, sizeof g_st);
  memset(g_as, 0, sizeof g_as);
  snprintf(g_path, sizeof g_path, "%s", path);
  /* 应用路径要在旧格式分支之前定下来：重置后仍然要继续往那份文件里写，
     否则这一轮守护直到下次启动前都只写全局样本。 */
  if (apath && apath[0]) snprintf(g_apath, sizeof g_apath, "%s", apath);
  snprintf(g_since_path, sizeof g_since_path, "%s.since", path);
  snprintf(g_reset_mark, sizeof g_reset_mark, "%s.v2reset", path);
  /* v1 没有记录“样本是在哪道帽下采的”，和新判据的帽有效性冲突。
     升级这一刻两份样本一起清掉，从头学；页面按标记提示重新积累。 */
  if (learn_format(path) == 1 || (apath && apath[0] && learn_format(apath) == 1)) {
    unlink(path);
    if (apath && apath[0]) unlink(apath);
    learn_since_drop();
    FILE *m = fopen(g_reset_mark, "w");
    if (m) fclose(m);
    return;
  }
  if (learn_read_rows(path, 0) >= 0) g_seen_file = 1;
  if (g_apath[0] && learn_read_rows(g_apath, 1) >= 0) g_seen_app = 1;
  if ((g_n || g_nas) && g_reset_mark[0]) unlink(g_reset_mark);
  learn_since_load();
  /* 起点文件丢了但样本还在：按“今天用过”补一天，别显示 0 天。
     正常升级（v1→v2）已经在上面整份重置，不会走到这里。 */
  if (!g_since && (g_n || g_nas)) {
    /* 旧数据确实在攒，迁移后至少算"今天用过"：否则分钟数还在、天数却是 0，
       页面会显示"已学到 0 天"。之后按新的 24 小时块继续累加。 */
    for (int i = 0; i < g_n; i++)
      g_st[i].a.daybits = g_st[i].a.sampled_ms > 0 ? 1u : 0u;
    for (int i = 0; i < g_nas; i++)
      g_as[i].a.daybits = g_as[i].a.sampled_ms > 0 ? 1u : 0u;
    /* 立刻写回：不写的话，升级后页面在下次落盘前仍会读到旧口径的天数 */
    if (g_n && g_path[0]) learn_write_rows(g_path, 0);
    if (g_nas && g_apath[0]) learn_write_rows(g_apath, 1);
  }
}

/* 样本文件被删 = 清零重来（页面上的「清零重学」删的就是它）。
 * 只按存在性判断，不跟 mtime 较劲：守护自己每 5 分钟会写它一次。
 * 前提是这份文件**确实存在过**：首装的那台机上它本来就没有，
 * 把"不存在"当成清零，每一拍都会把刚攒的累计连同计时基准一起抹掉，
 * dt 永远是 0、一次也落不了盘——这条只有真机能发现。
 * 每轮都查，不设节流：节流窗口内正好按到「重启守护」的话，
 * 退出时那一次落盘会把刚清掉的旧累计原样写回去。
 * 两层各查各的：只删按应用那份，就只清按应用那一层。 */
static void learn_check_reset(void) {
  int had_data = g_seen_file || g_seen_app;
  if (g_path[0]) {
    if (!access(g_path, F_OK)) g_seen_file = 1;
    else if (g_seen_file && g_n) {
      g_seen_file = 0;
      g_n = 0;
      g_last_mono_ms = 0;
      memset(g_st, 0, sizeof g_st);
    }
  }
  if (g_apath[0]) {
    if (!access(g_apath, F_OK)) g_seen_app = 1;
    else if (g_seen_app && g_nas) {
      g_seen_app = 0;
      g_nas = 0;
      memset(g_as, 0, sizeof g_as);
    }
  }
  /* 两份样本都被删 = 页面上的"清零重学"：计时起点也回到下一次采样。
     只删其中一份时保留起点，另一份的"天数"还能接着算。 */
  if (had_data && g_path[0] && g_apath[0] &&
      access(g_path, F_OK) && access(g_apath, F_OK)) learn_since_drop();
}

/* 落盘之前先认一次"文件已被删"：页面上点「清零重学」删的就是这两个文件，
 * 而守护退出时也走到这里——不先查的话，屏幕上显示"已清零"，
 * 数据却一条没少。 */
void learn_flush(void) {
  learn_check_reset();
  int wrote = 0;
  if (g_n && g_path[0] && learn_write_rows(g_path, 0)) { g_seen_file = 1; wrote = 1; }
  if (g_nas && g_apath[0] && learn_write_rows(g_apath, 1)) { g_seen_app = 1; wrote = 1; }
  if (wrote && g_reset_mark[0]) unlink(g_reset_mark);
}

/*
 * 每轮巡检调一次。dirs/tiers/hw/cap/pct 是 g_pol 的平行数组。
 * pct = 配置意图百分比（CPU_PCT_* / GPU_PCT），进样本行做“这道帽”的凭据：
 *       用户改了百分比，这一档的累计就作废，不拿旧帽下采的分布出建议。
 * eligible = 封顶此刻真的生效，且这一轮身上的是**全局**那道帽。
 * pkg      = 这一轮的前台包；own_cap = 它戴的是自己的专属档位。
 *   own_cap=1: 只记按应用那一层（全局行不要这段分布）。
 *   own_cap=0: 两层都记（全局帽下的同一个分布，既算全局也算这个包的）。
 * pkg 为空表示这一轮没有可用归属：eligible 只决定全局行记不记。
 */
void learn_tick(int eligible, const char *pkg, int own_cap,
                const char *const *dirs, const int *tiers,
                const long *hw, const long *cap, const long *pct, int n) {
  if (g_learn_off || n <= 0) return;
  learn_check_reset();
  int app = (pkg && pkg[0]) ? 1 : 0;
  /* 应用专属帽生效时全局行不能要这段分布。调用处已经保证了，这里再兜一次，
     免得以后新增一条调用路径时悄悄破掉。全局帽下的归属不受影响：那段
     分布本来就是全局帽的结果，两层同时记是它的本义。 */
  if (own_cap) eligible = 0;
  int feed_app = app && (own_cap || eligible);
  long now = learn_mono_ms();
  long dt = g_last_mono_ms ? now - g_last_mono_ms : 0;
  g_last_mono_ms = now;

  if (dt <= 0 || dt > LEARN_STUCK_MS) {
    /* 冷启动第一拍、或中间被暂停/长眠隔久了：只刷基线，不累计。
       不然重启后第一次 tick 会把"开机到现在"的整段驻留一次性算进今天。 */
    for (int i = 0; i < n && i < LEARN_MAX_POL; i++) {
      struct tier_stat *st = learn_slot(dirs[i], tiers[i]);
      if (!st) continue;
      if (!learn_sample_state(st, NULL, 0, hw[i], cap[i]))
        learn_sample_kgsl(st, NULL, 0, hw[i], cap[i]);
    }
    return;
  }
  long now_wall = (long)time(NULL);
  if (g_since <= 0) {
    g_since = now_wall;      /* 第一拍真正开始采样时定起点 */
    learn_since_save();
  }
  long day = learn_day_index(g_since, now_wall);
  for (int i = 0; i < n && i < LEARN_MAX_POL; i++) {
    struct tier_stat *st = learn_slot(dirs[i], tiers[i]);
    if (!st) continue;
    long p = (pct && pct[i] > 0) ? pct[i] : 0;
    /* 帽变了：这一档的旧分布作废。当前 dt 覆盖的是旧帽那段时间，
       也不能记进新帽——只刷基线，从下一拍开始累计。 */
    int cap_changed = 0;
    /* 凭据在"这一拍没戴应用帽"时可以纠正（插电/热限频时段也允许），但**不累计**；
       戴应用帽时 p 是应用的百分比，动它就会把全局 CPU 样本清零。
       旧版本曾把生效值写进凭据，文件里可能留着错帽，这里顺手自愈。 */
    if (!own_cap && p > 0 && st->a.cap_pct != p) {
      /* 只有"旧样本换帽"才丢掉这一拍；新行还没有旧分布，直接开始攒。
         否则新装/新前台的第一拍会被白扔，样本从第二拍才起步。 */
      cap_changed = (st->a.cap_pct > 0);
      memset(&st->a, 0, sizeof st->a);
      st->a.cap_pct = p;
    }
    /* 顶帽要贴着**实际上限**算，不是贴着我们写下去的那个值 */
    long eff = cap[i];
    long live = learn_read_max(dirs[i]);
    if (eff > 0 && live > 0 && live < eff) eff = live;

    struct learn_delta d;
    memset(&d, 0, sizeof d);
    int got = learn_sample_state(st, &d, dt, hw[i], eff);
    if (!got) got = learn_sample_kgsl(st, &d, dt, hw[i], eff);
    if (!got) got = learn_sample_cur(st, &d, dt, hw[i], eff);
    /* 低 2% 以上才算"这道帽不在身上"：吸附到频点会有零点几% 的差，那不是被压 */
    int held = (cap[i] > 0 && eff < cap[i] - cap[i] / 50);
    if (eligible) {
      if (got && !cap_changed) learn_put(&st->a, &d);
      if (!cap_changed) {
        st->a.sampled_ms += dt;
        if (held) st->a.held_ms += dt;
        st->a.daybits |= 1u << (day % 28);
      }
    }
    if (!feed_app) continue;
    struct app_stat *as = learn_app_slot(pkg, tiers[i]);
    if (!as) continue;
    int app_changed = 0;
    if (p > 0 && as->a.cap_pct != p) {
      app_changed = (as->a.cap_pct > 0);
      memset(&as->a, 0, sizeof as->a);
      as->a.cap_pct = p;
    }
    if (got && !app_changed) learn_put(&as->a, &d);
    if (!app_changed) {
      as->a.sampled_ms += dt;
      if (held) as->a.held_ms += dt;
      as->a.daybits |= 1u << (day % 28);
    }
  }
  time_t now2 = time(NULL);
  if (now2 - g_last_flush >= LEARN_FLUSH_S) { g_last_flush = now2; learn_flush(); }
}
