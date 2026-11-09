/*
 * adapt.c — 三个实时传感器与"封顶偏移"的步进机
 *
 * 和智能调度的分工：智能模式按负载给目标，这里看的是**这一刻**的
 * 传感器，只在封顶确实不够用时借一点额度出去。
 *
 * 三条偏移合成一个数（百分点），加在用户设的百分比上：
 *   充电态   插着电时把常态封顶抬到 CHG_PCT（发热由充电器承担，没必要压）
 *   温控     最热 cpu/gpu 传感器超阈值就逐档往下压，凉下来逐档还回去
 *   PSI      stall 率明显高于这台机自己的基线时逐档借出额度，不卡了就收回
 *
 * 两条硬规矩：
 *   1) 偏移只加在生效值上，**从不写配置文件**。页面上的滑杆就是用户设的那个值，
 *      不会因为某个循环在背后改掉它。
 *   2) PSI 只能"借"（往 100% 方向），不能"拿"（往下压）——
 *      收紧是学习的活，它有自己的样本和门槛。真机上量过：这台机静置就有
 *      4~5 个 avg10、满载时有没有帽 stall 率几乎一样（129 vs 129 ms/s），
 *      拿绝对值判"卡了"会把模块自己顶到 100% 停在那儿。
 *
 * 传感器路径开成可替换的（环境变量），这样自检版能喂假数据把步进机单独验一遍。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>

#include "adapt.h"

static char g_psi_path[192]  = "/proc/pressure/cpu";
static char g_thm_dir[192]   = "/sys/class/thermal";
static char g_batt_path[192] = "/sys/class/power_supply/battery/status";
static char g_cap_path[192]  = "/sys/class/power_supply/battery/capacity";

/* 温度 zone 路径缓存：thermal 目录一开机就固定，没必要每 5 秒
 * opendir + 87 个 zone × 2 次 open/read。启动扫一次，读失败再重扫。 */
#define MAX_THERMAL 128
static char g_thm_paths[MAX_THERMAL][256];
static int  g_thm_n;
static int  g_thm_scanned;

static struct adapt g;
static long g_prev_total;     /* PSI total 上一次的读数（µs） */
static long g_prev_mono;

static long adapt_mono(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec;
}

static int read_str(const char *path, char *buf, int n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t r = read(fd, buf, n - 1);
  close(fd);
  if (r <= 0) return 0;
  buf[r] = 0;
  return 1;
}

/* /proc/pressure/cpu 第一行：some avg10=0.12 avg60=... total=1234567
 * avg10 放大 100 倍存成整数（页面显示 5.3 这种一位小数够用），
 * total 是累计 stall 微秒，交给调用方按墙钟差算速率。 */
static int parse_psi(const char *line, int *avg10x100, long *total) {
  const char *q = strstr(line, "avg10=");
  if (!q) return 0;
  q += 6;
  *avg10x100 = 0;
  long whole = 0, dec = 0, mul = 1;
  int seen_dot = 0;
  for (; *q; q++) {
    if (*q == '.') { seen_dot = 1; continue; }
    if (!isdigit((unsigned char)*q)) break;
    if (seen_dot) { if (mul < 100000) { dec = dec * 10 + (*q - '0'); mul *= 10; } }
    else whole = whole * 10 + (*q - '0');
  }
  while (mul < 100) { dec *= 10; mul *= 10; }
  if (mul > 100) dec /= (mul / 100);
  *avg10x100 = (int)(whole * 100 + dec);

  const char *p = strstr(line, "total=");
  if (!p) return 0;
  *total = strtol(p + 6, NULL, 10);
  return 1;
}

/* type 过滤判据。独立成纯函数，单测直接喂字符串；不按数值筛，
 * 免得把 vbat（4091 其实是 4.091V）、ibat、bcl 这些当温度。 */
int thermal_type_ok(const char *type) {
  char low[64];
  size_t i;
  for (i = 0; type[i] && i < sizeof low - 1; i++)
    low[i] = tolower((unsigned char)type[i]);
  low[i] = 0;
  /* trip point 是阈值，不是温度计。有些内核把它也放进 thermal_zone，
   * type 叫 cpu-hw-trip-0 / CPU-HW-TRIP-1，temp 恒等于阈值（实测 95000）。
   * 当温度取最大值会把整机误判成 95℃，温控永远压着不还，学习也永远不采样。 */
  if (strstr(low, "trip")) return 0;
  return strstr(low, "cpu") || strstr(low, "gpu") || strstr(low, "soc");
}

static int thermal_scan(void) {
  DIR *d = opendir(g_thm_dir);
  g_thm_n = 0;
  g_thm_scanned = 1;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) && g_thm_n < MAX_THERMAL) {
      if (strncmp(e->d_name, "thermal_zone", 12)) continue;
      char type[64], path[256];
      snprintf(path, sizeof path, "%s/%s/type", g_thm_dir, e->d_name);
      if (!read_str(path, type, sizeof type)) continue;
      type[strcspn(type, "\r\n")] = 0;
      if (!thermal_type_ok(type)) continue;
      snprintf(g_thm_paths[g_thm_n], sizeof g_thm_paths[0],
               "%s/%s/temp", g_thm_dir, e->d_name);
      g_thm_n++;
    }
    closedir(d);
  }
  return g_thm_n;
}

static int thermal_max_cached(int *failed) {
  int best = 0;
  *failed = 0;
  for (int i = 0; i < g_thm_n; i++) {
    char v[32];
    if (!read_str(g_thm_paths[i], v, sizeof v)) { *failed = 1; continue; }
    int t = atoi(v);
    /* 毫度：0 以下或超过 200℃ 的不是温度读数（有的 zone 恒定 0） */
    if (t <= 0 || t > 200000) continue;
    if (t > best) best = t;
  }
  return best;
}

/* 最热的那个 cpu/gpu 传感器，返回 0.1℃；没有可用传感器给 0。 */
static int hottest_zone(const char *dir) {
  (void)dir; /* 路径来自 adapt_init 注入的 g_thm_dir */
  if (!g_thm_scanned) thermal_scan();
  int failed = 0;
  int best = thermal_max_cached(&failed);
  if (failed) {
    /* zone 被热插拔/驱动重建：重扫一次再读，不要拿半截缓存撑到重启 */
    thermal_scan();
    best = thermal_max_cached(&failed);
  }
  /* 一个可用 zone 都没找到时下次继续尝试（有的机型 thermal 起得晚） */
  if (g_thm_n == 0) g_thm_scanned = 0;
  return best;
}

static int battery_charging(const char *path) {
  char s[32];
  if (!read_str(path, s, sizeof s)) return 0;
  s[strcspn(s, "\r\n")] = 0;
  return !strcasecmp(s, "Charging") || !strcasecmp(s, "Full");
}

void adapt_init(void) {
  const char *e;
  if ((e = getenv("SOCFREQ_PSI_PATH")) && e[0])
    snprintf(g_psi_path, sizeof g_psi_path, "%s", e);
  if ((e = getenv("SOCFREQ_THERMAL_DIR")) && e[0])
    snprintf(g_thm_dir, sizeof g_thm_dir, "%s", e);
  if ((e = getenv("SOCFREQ_BATT_PATH")) && e[0])
    snprintf(g_batt_path, sizeof g_batt_path, "%s", e);
  if ((e = getenv("SOCFREQ_CAP_PATH")) && e[0])
    snprintf(g_cap_path, sizeof g_cap_path, "%s", e);
  memset(&g, 0, sizeof g);
  g_prev_mono = 0;
  g_prev_total = 0;
  g_thm_n = 0;
  g_thm_scanned = 0;
}

/* ---- 纯步进逻辑：喂数就能验，不碰任何文件 ---------------------------- */

/* PSI：stall 率（ms/s）相对自身基线借额度。
 * 判"卡"要同时过两道：相对基线 2 倍以上，且绝对值高出基线 30 ms/s 以上。
 * 只靠倍数会在很静的机器上被噪声点着（基线 5、当前 12 就是 2.4 倍）。 */
static void step_psi(struct adapt *a, const struct adapt_cfg *c, int stall, int dt,
                     int allow_base) {
  /* 0 = 整条环路不动；GAIN < STEP 连一档都凑不齐，也不许借。
     以前这里兜底成"至少 1 档"，把界面上的 0 变成了实际 +5。 */
  if (!c->psi || c->psi_gain <= 0 || c->psi_step <= 0) {
    a->psi_steps = 0;
    return;
  }
  /* stall < 0 = 这一拍没有有效差分（读不到 / 长眠回来 / 计数器被清）。
     不学也不判：把 0 喂进 EMA 会让基线一路往下漂，门槛越来越低。
     上一拍的档位保持不变，等下一个有效样本。 */
  if (stall < 0) return;

  /* 基线只在"没在借额度"的时候学：借出期间 stall 高是这一拍正在生效的
     结果，拿它去抬高基线等于把护栏自己顶盲（持续高载 7 分钟后门槛超过
     实际 stall，最该放宽的场景反而不再触发）。 */
  if (a->psi_steps == 0) a->base_frozen = 0;   /* 连续冻结秒数：借出结束就归零 */
  if (allow_base && a->psi_steps == 0) {
    if (a->base <= 0) {
      /* 前两个有效样本取最小值再立基线：开机第二拍来一个 250 ms/s 的尖峰时，
         不能让基线直接等于它（门槛会被抬到 530，护栏瞎 10 分钟）。 */
      if (stall <= 0) return;                  /* 0 不是有效样本 */
      if (a->base_warm == 0 || stall < a->base_min) a->base_min = stall;
      a->base_warm++;
      if (a->base_warm < 2) return;            /* 预热期不判 */
      a->base = a->base_min;
    } else {
      /* 四舍五入：整数截断时 (base*(600-5)+stall*5)/600 的增量不足 1，
         基线会卡在一个"错误但稳定"的值上（stall=129 从 10 出发永远不动）。 */
      a->base = (a->base * (PSI_TAU - dt) + stall * dt + PSI_TAU / 2) / PSI_TAU;
    }
    if (a->base < 0) a->base = 0;
  } else if (allow_base && a->psi_steps > 0) {
    /* 阈值附近抖动时 psi_steps 可能长期 >0、基线也就长期不学。不是永久冻结，
       但值得在 status 里看出来（psifrz 是这一轮的连续冻结秒数）。 */
    a->base_frozen += dt;
  }
  /* 还没有基线（开机就在交权态）就先不判，等回到常态那一拍再立 */
  if (a->base <= 0) return;

  /* 温控优先：只要这一拍还有降温档在生效，PSI 一档都不借，并把旧额度清零。
     发热是硬件风险，卡顿是可恢复的体验问题，不能让 CPU 排队信号把
     发热保护抵消掉（默认参数下前 10 秒 PSI 能完全压过热）。 */
  /* 只有"确实读到了温度"时才让温控挡 PSI：传感器消失后档位会按降温
     方向释放（见 step_thermal），不能因为一个读不到的 zone 永久禁止放宽。 */
  if (a->th_steps > 0 && a->temp_m > 0) {
    a->psi_steps = 0;
    a->lend_until = a->now + c->psi_hold;
    return;
  }

  /* 借出只能花"没花掉的降温余量"：down_max 是合计收紧上限，
     电量阶梯已经占掉的部分不能再借出去。 */
  int budget = c->psi_gain;
  if (c->down_max > 0) {
    int down = a->th_steps * c->th_step + a->batt_cut;
    int avail = c->down_max - down;
    if (avail < 0) avail = 0;
    if (budget > avail) budget = avail;
  }
  int max_steps = budget / c->psi_step;
  if (max_steps < 1) {
    a->psi_steps = 0;
    return;
  }

  if (stall > a->base * 2 + 30) {
    if (a->psi_steps < max_steps) a->psi_steps++;
    a->lend_until = a->now + c->psi_hold;
  } else if (a->now >= a->lend_until) {
    /* 不卡了才收，而且一次只收一档：正在掉帧的时候别把额度抽干。
       收回的节律记在结构体里（不是文件级全局）——
       否则调用方 memset 或 adapt_reset 之后，上一轮的节律还会漏进来。 */
    if (a->psi_steps > 0 && a->now - a->back_until >= c->psi_back) {
      a->back_until = a->now;
      a->psi_steps--;
    }
  }
}

/* 温控：超线逐档压，回到恢复线以下按时间逐档还。
 * 迟滞带（high/low 之间）不动，避免在阈值附近来回抖。 */
static void step_thermal(struct adapt *a, const struct adapt_cfg *c, int temp_m) {
  /* 0 = 不主动降；TH_MAX < TH_STEP 连一档都凑不齐，也不许压。
     以前兜底成"至少 1 档"，界面写 0 实际仍会 -5。 */
  if (!c->th || c->th_max <= 0 || c->th_step <= 0) {
    a->th_steps = 0;
    return;
  }
  int max_steps = c->th_max / c->th_step;
  if (max_steps < 1) {
    a->th_steps = 0;
    return;
  }
  /* 阈值填的是℃，内核给的是毫摄氏度 —— 少乘一个 1000 会让静置 43℃
     就被判成超阈值（真机上第一版就是这么错的，单测喂同一套错单位没拦住） */
  int hi = c->th_high * 1000, lo = c->th_low * 1000;

  if (temp_m > 0 && temp_m >= hi) {
    if (a->th_steps < max_steps) a->th_steps++;
    a->cool_until = 0;
    return;
  }
  if (temp_m > 0 && temp_m > lo) {
    a->cool_until = 0;                      /* 迟滞带里：保持不动 */
    return;
  }
  /* 低于恢复线，或这一拍读不到温度（thermal zone 热插拔/驱动重建/
     部分机型熄屏后目录消失）：按 TH_BACK 逐档还，不能把上一次热到的
     档位永久冻住 —— 那会变成永久 -25 且温控优先永远禁止 PSI 放宽。 */
  if (!a->cool_until) a->cool_until = a->now + c->th_back;
  if (a->now >= a->cool_until) {
    if (a->th_steps > 0) { a->th_steps--; a->cool_until = a->now + c->th_back; }
    else a->cool_until = 0;
  }
}

/* 电量阶梯：只在不充电时生效；容量 -1 表示读不到，不猜。 */
int batt_cut_pct(int capacity, int charging, const struct adapt_cfg *c) {
  if (!c->batt || charging || capacity < 0) return 0;
  int cut = 0;
  if (capacity < c->batt_low) cut += c->batt_low_cut;
  if (capacity < c->batt_crit) cut += c->batt_crit_cut;
  return cut;
}

void adapt_apply_cap(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                     int charging, int capacity, int now, int dt, int allow_base) {
  a->now = now;
  a->stall = stall < 0 ? 0 : stall;   /* 负数是"无有效样本"的内部哨兵 */
  a->temp_m = temp_m;
  a->charging = charging;
  a->capacity = capacity;
  /* 顺序：先算电量收紧、再走温控，最后才让 PSI 决定借不借。
     step_psi 里要根据本拍温控档位判断"热了就停借"。 */
  a->batt_cut = batt_cut_pct(capacity, charging, c);
  step_thermal(a, c, temp_m);
  step_psi(a, c, stall, dt, allow_base);
  a->chg_on = c->chg && charging;
  a->chg_pct = a->chg_on ? c->chg_pct : 0;

  int d = 0;
  d += a->psi_steps * c->psi_step;
  d -= a->th_steps * c->th_step;
  d -= a->batt_cut;
  /* 三个来源各自有上限，但叠加后可能比任何单一来源都狠：
     PSI_GAIN 30 + TH_MAX 60 + 电量两刀 30 的极端配置下，65% 会被压到 5%。
     这里给合计再上总闸，避免"每一项都合法，合起来不可用"。 */
  if (d > c->up_max) d = c->up_max;
  if (d < -c->down_max) d = -c->down_max;
  a->delta = d;
}

void adapt_apply_gated(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                       int charging, int now, int dt, int allow_base) {
  adapt_apply_cap(a, c, stall, temp_m, charging, -1, now, dt, allow_base);
}

void adapt_apply(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                 int charging, int now, int dt) {
  adapt_apply_gated(a, c, stall, temp_m, charging, now, dt, 1);
}

/* ---- 采数与对外接口 -------------------------------------------------- */

/* 采一轮。返回 1 表示"生效封顶和上一轮不一样了"，调用处据此重算并 force 写。
 * 签名只取 delta 与充电地板两项 —— 偏移没变、步数变了（比如 PSI 从 1 档换到
 * 2 档同时温控补回 1 档）时封顶其实没动，没必要重写一遍节点。 */
int adapt_tick_ex(const struct adapt_cfg *c, int dt, int allow_base, int advance) {
  long now = adapt_mono();
  g.now = (int)now;

  /* PSI：total 是累计 µs，除以墙钟差得到 ms/s。
     隔太久没采（暂停、长眠）这一拍丢掉：拿一段很长的窗口的平均值去判"卡不卡"
     只会得到一个钝数。读不到 PSI 时不能拿上一拍的旧值继续判，直接清零。 */
  char line[192];
  int avg10x100 = 0;
  long total = 0;
  int stall = 0;
  int valid = 0;
  if (!c->psi) {
    /* PSI 整条关掉时不必每拍读 /proc：stall=-1 表示这一拍没有有效样本，
       step_psi 本来就整条不动作。
       同时把上一次的累计量作废：smart 期间 g_prev_total 会冻在切智能前那一拍，
       切回 cap 的第一拍会拿"整个 smart 期间的累计 µs / 5 秒"算出天文数字。
       清 0 后首拍按无基线冻结，下一拍重新建立差分。 */
    stall = -1;
    g.psi_valid = 0;
    g_prev_total = 0;
    g.avg10x100 = 0;
  } else if (read_str(g_psi_path, line, sizeof line) && parse_psi(line, &avg10x100, &total)) {
    g.avg10x100 = avg10x100;
    g.have_psi = 1;
    long dwall = g_prev_mono ? now - g_prev_mono : 0;
    if (g_prev_total && dwall > 0 && dwall <= 120) {
      long d = total - g_prev_total;
      if (d < 0) d = 0;                    /* 计数器被清（重启）：这一拍不算 */
      stall = (int)((d / 1000) / (dwall ? dwall : 1));
      valid = 1;
    }
    g_prev_total = total;
  } else {
    g.have_psi = 0;
    g.psi_steps = 0;
    g.lend_until = 0;
  }
  /* 无效样本用 -1 当哨兵：步进机只冻结、不把 0 喂进基线（0 会让门槛往下漂）。 */
  if (!valid) stall = -1;
  g.psi_valid = valid;
  g_prev_mono = now;

  g.temp_m = hottest_zone(g_thm_dir);
  g.charging = battery_charging(g_batt_path);
  int capacity = -1;
  char cb[32];
  if (read_str(g_cap_path, cb, sizeof cb)) {
    int v = atoi(cb);
    if (v >= 0 && v <= 100) capacity = v;
  }

  /* 交权态（游戏/满血/熄屏）：传感器照采，但步进机不推进、借期不续。
     以前这里照样每拍借档，游戏 8 拍攒下 +10%，出游戏后白拿 30~60 秒。 */
  if (!advance) {
    g.stall = stall < 0 ? 0 : stall;
    g.psi_steps = 0;
    g.th_steps = 0;
    g.lend_until = 0;
    g.back_until = 0;
    g.cool_until = 0;
    g.delta = 0;
    g.chg_on = 0;
    g.chg_pct = 0;
    g.batt_cut = 0;
    return 0;
  }

  struct adapt prev = g;
  g.stall = stall;
  adapt_apply_cap(&g, c, stall, g.temp_m, g.charging, capacity, (int)now, dt, allow_base);
  /* 地板值本身也要参与比较：CHG_PCT 从 90 调到 100 时 delta 没变，
     但生效封顶变了。 */
  return prev.delta != g.delta || prev.chg_on != g.chg_on || prev.chg_pct != g.chg_pct;
}

int adapt_tick(const struct adapt_cfg *c, int dt, int allow_base) {
  return adapt_tick_ex(c, dt, allow_base, 1);
}

int adapt_screen_off_tick(const struct adapt_cfg *c, int dt) {
  (void)dt;   /* 熄屏温控按拍步进，冷却节律用 th_back 的秒数，不需要 dt */
  int temp = hottest_zone(g_thm_dir);
  struct adapt prev = g;
  g.now = (int)adapt_mono();
  g.temp_m = temp;
  g.stall = 0;
  g.psi_valid = 0;
  g.charging = 0;
  g.capacity = -1;
  g.psi_steps = 0;
  g.batt_cut = 0;
  g.chg_on = 0;
  g.chg_pct = 0;
  step_thermal(&g, c, temp);
  int d = -(g.th_steps * c->th_step);
  if (d < -c->down_max) d = -c->down_max;
  g.delta = d;
  return prev.delta != g.delta;
}

void adapt_cancel(void) {
  g.psi_steps = 0;
  g.base_frozen = 0;
  g.th_steps = 0;
  g.lend_until = 0;
  g.back_until = 0;
  g.cool_until = 0;
  g.delta = 0;
  g.chg_on = 0;
  g.chg_pct = 0;
  g.batt_cut = 0;
}

int  adapt_delta(void)   { return g.delta; }
int  adapt_batt_cut(void) { return g.batt_cut; }
int  adapt_active(void)  { return g.delta != 0 || g.chg_on; }
void adapt_snapshot(struct adapt *out) { if (out) *out = g; }
void adapt_reset(void) { memset(&g, 0, sizeof g); g_prev_total = 0; g_prev_mono = 0; }
