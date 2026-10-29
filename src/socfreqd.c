/*
 * socfreqd.c — Soc智能限频 守护进程
 * 梦笙Mess1on · v1.1
 *
 * 常态按百分比封顶 CPU 各簇与 GPU；检测到游戏在前台时解除封顶，
 * 并把相关节点权限放宽交还系统调度，让厂商框架真正接管。
 *
 * 构建：aarch64-linux-android21-clang -O2 -static -o socfreqd socfreqd.c
 * 只依赖 libc，无第三方库。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <sys/system_properties.h>
#include <unistd.h>

/* icon.c：取应用图标，与限频逻辑无关，只在 icon 子命令里调用。
   want = 已确定的图标条目名（appinfo.c 从 manifest+arsc 读出），
   给了就直接按它取，不给则走按名字打分、按尺寸猜测的流程。 */
int icon_from_apk(const char *apk, const char *want);
/* score 子命令：stdin 收条目名，stdout 打 "分数\t名字"，给 api.sh 的
   unzip 兜底挑图用。挑图规则只在 icon.c 的 score_name 一处实现
   （score_name 是 static，实现在 icon.c）。 */
int icon_score_stdin(void);
/* appinfo.c：从 APK 里读「应用名」和「声明的桌面图标路径」。
   同样和限频逻辑无关，只在 label / labels / iconpath 三个子命令里用到。 */
int appinfo_label(const char *apk, char *out, size_t osz);
int appinfo_iconpath(const char *apk, char *out, size_t osz);
int appinfo_labels_stdin(void);
/* learn.c：常态封顶下的频率驻留采样。只攒计数，判据在 api.sh 的 advice 里。
   采样窗口里"有效时长"用的是墙钟，桶存的是占硬件最高频的百分比分布。
   两份样本：全局一行一个目录，按应用一行一个 (包, 档)。全局帽下的那一段两层都记，
   应用专属帽下的那一段只记应用行。 */
void learn_init(const char *path, const char *apath);
void learn_tick(int eligible, const char *pkg, int own_cap,
                const char *const *dirs, const int *tiers,
                const long *hw, const long *cap, const long *pct, int n);
void learn_flush(void);
void learn_set_enabled(int on);
/* 从学习起点按 24 小时切块（不按零点切天），返回 0..27。 */
long learn_day_index(long since, long now);
/* adapt.c：三个实时传感器（PSI / 温度 / 充电态）合成一个"封顶偏移"。
   偏移只加在生效值上，从不写配置 —— 见 adapt.h。 */
#include "adapt.h"

#ifndef SF_MODDIR
#define SF_MODDIR "/data/adb/modules/soc_freqcap"
#endif
/* 可变状态的家。**不能放在模块目录里**：管理器升级模块会重建
 * /data/adb/modules/<id>，配置和名单放在里面会被新包默认值覆盖。
 * 也不放 /sdcard：那是 FUSE/sdcardfs，root 直接写 /data/media/0 会绕过
 * MediaProvider（文件管理器看不见、时间戳错乱），读取还要处理 sdcardfs
 * 的 SELinux 标签。/data/adb 下的同级目录既在升级范围之外，又是 root
 * 私有的普通 ext4。 */
#ifndef SF_DATADIR
#define SF_DATADIR "/data/adb/soc_freqcap"
#endif
#ifndef SF_VERSION
#define SF_VERSION "1.6"
#endif

/* policy 的容器目录。里面只有 policyN/ 子目录，没有 cpuN/。
 * 逐核路径要用 CPUBASE，见 core_exists 的注释。 */
#define CPUFREQ "/sys/devices/system/cpu/cpufreq"
/* 核心的容器目录。逐核路径是 CPUBASE "/cpuN/cpufreq/..."。 */
#define CPUBASE "/sys/devices/system/cpu"
#define RUN_DIR   SF_DATADIR "/run"
#define STATE_FILE SF_DATADIR "/state"
#define LOCK_FILE SF_DATADIR "/socfreqd.lock"
#define LOG_FILE  SF_DATADIR "/socfreqd.log"
#define PAUSE_FILE SF_DATADIR "/pause"
#define NODES_JOURNAL RUN_DIR "/nodes.saved"
#define PERM_JOURNAL RUN_DIR "/perms.saved"
#define GOV_JOURNAL RUN_DIR "/govs.saved"
/* 开机满血窗口的截止秒。service.sh 写、守护读并在到点时删除。 */
#define BOOT_BOOST_FILE RUN_DIR "/boot_boost_until"
#define CFG_FILE   SF_DATADIR "/socfreq.conf"
#define GAME_LIST  SF_DATADIR "/games.txt"
/* 学习采样结果。守护只写计数，判据在 api.sh 的 advice 里算。 */
#define LEARN_FILE SF_DATADIR "/learn.tsv"
#define LEARN_APP_FILE SF_DATADIR "/learn_app.tsv"
/* GPU 的学习采样点：KGSL 的每档驻留累计表。
 * 只有这一种来源能用（devfreq 的 trans_stat 在 KGSL 上是冻的，见 learn.c）。 */
#define GPU_LEARN_DIR "/sys/class/kgsl/kgsl-3d0"
#define GPU_STATS_FILE GPU_LEARN_DIR "/gpu_clock_stats"
/* 分应用档位：pkg=full / pkg=60 / pkg=60:40。优先级 应用专属 > 全局百分比。
 * 由 api.sh / WebUI 维护，守护只读——写的一侧有校验，读的一侧就不需要第二套。 */
#define MODE_FILE  SF_DATADIR "/app_modes.conf"
#define STATUS_FILE RUN_DIR "/status"
#define HEARTBEAT_FILE RUN_DIR "/heartbeat"
#define CORE_STATE_FILE RUN_DIR "/cores.state"
#define CORE_STATE_LOCK RUN_DIR "/cores.lock"
/* MTK 天玑（gpufreqv2）：没有 GPU devfreq，上限走 limit_table 的层约束。
 * stack_working_opp_table 是 57 档频率表，index 0=最高频；limit_table 的
 * 一行是 id/name/priority/ceiling/floor/c_enable/f_enable，写 "set id c f"。
 * 实测写 layer 1(DEBUG) 37/56 后 PPM Ceiling 从 24 变 37，还原正常。 */
#define MTK_LIMIT   "/proc/gpufreqv2/limit_table"
#define MTK_OPP     "/proc/gpufreqv2/stack_working_opp_table"
#define MTK_GPU_SNAP RUN_DIR "/gpu_mtk.saved"

#define MAX_POLICY 16
/* 核心数上限。8 核是当前主流（骁龙 8 Elite 到 8 核，天玑 9400 也 8+1）。
 * 取 16 是给"超大核独立成 policy + 小核多簇"的布局留余量，
 * 真到 16 核也不会越界，因为下面 core_exists 会挡住不存在的核。 */
#define MAX_CORES 16
#define MAX_NODE 64
#define MAX_PKG 512
#define PKG_LEN 256

/* ------------------------------------------------------------------ */
/* 配置与全局状态                                                     */
/* ------------------------------------------------------------------ */

#define SCREEN_KEEP  0   /* 熄屏不改变封顶（旧行为） */
#define SCREEN_UNCAP 1   /* 熄屏交还系统，轻巡检 */

struct cfg {
  int interval;        /* 常态巡检间隔秒 */
  int grace;           /* 游戏退出宽限秒 */
  int pct[4];          /* big / mid2 / mid / little 封顶百分比 */
  int gpu_pct;         /* GPU 封顶百分比 */
  int gpu_min_lock;    /* 1 = 把 GPU 下限钉在最低档，抑制 boost 越界 */
  int screen_uncap;    /* 旧键：1 = 熄屏时解除限频；仅无 SCREEN_POLICY 时参与映射 */
  int screen_policy;   /* SCREEN_KEEP / SCREEN_UNCAP */
  int soff_interval;   /* 熄屏轻巡检周期秒 */
  char screen_state_path[192]; /* 亮屏信号；空 = 启动时自动挑 */
  /* P0b：深度空闲（电流归一化后判定） */
  int idle_current_ma;
  int idle_hold_cycle;
  int idle_interval;
  int wake_delta_ma;
  char batt_current_path[192];
  int batt_current_ma_unit; /* 1 = 原始值已经是 mA；0 = µA */
  int boot_full_s;     /* 开机后满血秒数，0 = 关 */
  int learn;           /* 1 = 常态封顶下采样频率驻留，给"智能建议"攒证据 */
  int learn_app;       /* 1 = 同一份分布另记一份到"这个应用自己的帽"上，默认关 */
  char governor[32];   /* 日常调速器，空 = 不干预 */
  /* 实时偏移（adapt.c）。都是"百分点"，只加在生效值上，不写回配置。 */
  struct adapt_cfg ad;
};

static struct cfg cfg;
static char g_state[16] = "normal";
/* 交还系统的原因：game / screen / boot / app。
 * state = normal | game | full：game 是游戏名单/满血应用，full 是熄屏交还。
 * 细分原因走 reason= 这个键单独报。 */
static char g_reason[16] = "-";
static char g_game[PKG_LEN];
/* 本轮读到的前台包名（不代表命中游戏名单）。
 * 状态页要它：分应用档位的入口是"当前前台是谁"，不是"在不在名单里"。 */
static char g_fg[PKG_LEN];
/* 熄屏轻巡检（SCREEN_POLICY=uncap）。g_screen_off 与封顶状态分开：
 * 熄屏时可能仍是 game 态（游戏里锁屏），不能把它冲成 full。 */
static int g_screen_off;
static char g_scr_path[192];  /* 亮屏信号文件；空 = 没有可用文件 */
static int g_scr_invert;      /* 1 = blank 语义（0=亮）；0 = 亮度（>0=亮） */
static int g_scr_bad;         /* 信号与 dumpsys 不符的次数；连续 3 次就停用 */
static int g_scr_check;       /* 兜底校验计数 */
/* P0b：熄屏深睡。g_deep 只影响周期和电流判定；亮屏信号每轮仍读。 */
static int g_deep;
static long g_idle_ema;
static int g_idle_hold;
static int g_soff_keep;       /* 熄屏瞬间的快照应用是 pkg=keep */
static char g_soff_pkg[PKG_LEN];
static int g_have_lock;
static volatile sig_atomic_t g_stop;

static void load_cfg(void);

static long mono_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

static long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 看门狗心跳：每轮巡检写一次 mtime。service.sh 只 stat 这个文件，
 * 超过 90s 没更新才拉起；比读锁文件 pid 更能发现"活着但卡死"。 */
static void heartbeat(void) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp", HEARTBEAT_FILE);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  fprintf(f, "%ld\n", mono_sec());
  fclose(f);
  rename(tmp, HEARTBEAT_FILE);
}

/* ------------------------------------------------------------------ */
/* 日志                                                               */
/* ------------------------------------------------------------------ */

static int g_logfd = -1;
static unsigned g_logcount;

static void log_open(void) {
  /*
   * 只追加，不截断。卸载路径也会走到这里，保留已收集的诊断日志。
   */
  g_logfd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

/* 每 64 次才量一次大小，砍掉前半截再 O_APPEND 重开，避免长跑撑爆存储 */
static void log_trim(void) {
  if (++g_logcount % 64) return;
  struct stat st;
  if (fstat(g_logfd, &st) || st.st_size < 256 * 1024) return;
  int fd = open(LOG_FILE, O_RDWR);
  if (fd < 0) return;
  if (!ftruncate(fd, st.st_size / 2)) dup2(fd, g_logfd);
  close(fd);
}

static void logf_(const char *fmt, ...) {
  if (g_logfd < 0) return;
  char buf[512];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  int n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min,
                   tm.tm_sec);
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '\n';
  ssize_t w = write(g_logfd, buf, n);
  (void)w;
  log_trim();
}

#define log(...) logf_(__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* sysfs 基础读写                                                     */
/* ------------------------------------------------------------------ */

static int read_file(const char *path, char *buf, size_t len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, len - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  while (n > 0 && strchr(" \t\r\n", buf[n - 1])) buf[--n] = 0;
  return 0;
}

static long read_num(const char *path) {
  char b[64];
  if (read_file(path, b, sizeof b) || !isdigit((unsigned char)b[0])) return -1;
  return strtol(b, NULL, 10);
}

/* procfs 命令节点（limit_table）：一次写一整行，不截断。 */
static int write_text(const char *path, const char *s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  size_t len = strlen(s);
  ssize_t n = write(fd, s, len);
  close(fd);
  return (n == (ssize_t)len) ? 0 : -1;
}

/* 普通文件快照：需要截断旧内容。 */
static int write_text_file(const char *path, const char *s) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  size_t len = strlen(s);
  ssize_t n = write(fd, s, len);
  close(fd);
  return (n == (ssize_t)len) ? 0 : -1;
}

/* 有符号读数。battery/current_now 在充电时可能是负值（PJZ110 实测 -404），
 * 用 read_num 会把它当成"读失败"；深睡判据会因此永远不触发。 */
static int read_signed(const char *path, long *out) {
  char b[64];
  if (read_file(path, b, sizeof b)) return 0;
  char *p = b;
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p != '-' && !isdigit((unsigned char)*p)) return 0;
  char *end = NULL;
  long v = strtol(p, &end, 10);
  if (end == p) return 0;
  *out = v;
  return 1;
}

/* 这个核心在不在。
 *
 * 路径必须是 /sys/devices/system/cpu/cpuN/cpufreq/...，
 * **不能**写成 CPUFREQ "/cpuN/..."：
 * CPUFREQ 是 policy 的容器目录，里面只有 policyN/，没有 cpuN/。
 * 该节点挂在 policyN/ 和 cpuN/cpufreq/ 两处，唯独不在 cpuN/ 下，
 * 写成 CPUFREQ "/cpu%d/cpuinfo_max_freq" 拼出的路径内核不提供。
 *
 * 判据用 cpuinfo_max_freq 而不是"目录在不在"：
 * 目录存在不等于能读到值，判据要的是"这颗核有真实频率"。
 */
static int core_path(char *b, size_t n, int c, const char *leaf) {
  return snprintf(b, n, CPUBASE "/cpu%d/cpufreq/%s", c, leaf);
}

static int core_exists(int c) {
  char b[96];
  core_path(b, sizeof b, c, "cpuinfo_max_freq");
  return read_num(b) > 0;
}

/* 这颗核的 cpufreq 是不是符号链接。内核会建
 * /sys/devices/system/cpu/cpuN/cpufreq → …/cpufreq/policyM，
 * 读它比解析 related_cpus 可靠：related_cpus 的格式
 * （十进制列表 还是 十六进制掩码）各内核不统一，按十进制解析
 * 遇到掩码会全解析成 0，映射整个失效且照样不报错。 */
static int core_policy_of(int c) {
  char b[96], link[160];
  /* 路径**不能带尾斜杠**。
   * 模板结尾是 "/cpu%d/cpufreq/%s"，传空 leaf 拼出来末尾会多一个 /。
   * 按 POSIX 路径解析，结尾带斜杠时最后一个分量会被穿过符号链接去解析，
   * 该路径不再命名一个符号链接，readlink() 直接失败（EINVAL）。
   * 所以这里直接写死不带斜杠的完整路径。 */
  snprintf(b, sizeof b, CPUBASE "/cpu%d/cpufreq", c);
  ssize_t k = readlink(b, link, sizeof link - 1);
  if (k <= 0) return -1;
  link[k] = 0;
  const char *s = strrchr(link, '/');
  if (!s) return -1;
  if (strncmp(s + 1, "policy", 6)) return -1;
  return atoi(s + 7);
}

/* ── 逐核频率历史 ─────────────────────────────────────────────
 * 不是频率分布直方图。每 1 秒采一次「当前频率 + 这一秒的 CPU 占用」，
 * 旧样本左移、新样本放最右；页面画 8 根柱，最右是现在。
 * 高度 = 占用百分比；窗口里最高的一根标黄。
 * 占用来自 /proc/stat 的逐核 total/idle 差值；cores 每秒一个新进程，
 * 所以上一拍计数器和 8 个样本一起落到 run/cores.state。
 */
#define CORE_HIST_N 8
struct core_hist {
  long freq[CORE_HIST_N];   /* 旧 → 新 */
  long load[CORE_HIST_N];   /* 0~100 */
  int  n;
  long prev_total, prev_idle;
  int  have_prev;
};
static struct core_hist g_ch[MAX_CORES];

static int core_load_pct(long total, long idle, long pt, long pi) {
  long dt = total - pt, di = idle - pi;
  if (dt <= 0 || di < 0) return 0;
  long v = (dt - di) * 100 / dt;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  return (int)v;
}

/* 旧样本左移，新样本放最右；第一次直接铺满 8 格，
   避免页面刚打开时只有一根柱。 */
static void core_hist_push(struct core_hist *h, long freq, long load) {
  if (h->n <= 0) {
    for (int i = 0; i < CORE_HIST_N; i++) { h->freq[i] = freq; h->load[i] = load; }
    h->n = CORE_HIST_N;
    return;
  }
  int n = h->n < CORE_HIST_N ? h->n : CORE_HIST_N - 1;
  for (int i = 0; i < n; i++) {
    h->freq[i] = h->freq[i + 1];
    h->load[i] = h->load[i + 1];
  }
  h->freq[n] = freq;
  h->load[n] = load;
  if (h->n < CORE_HIST_N) h->n++;
}

static void core_hist_range(const struct core_hist *h, long *lo, long *hi) {
  *lo = 0; *hi = 0;
  for (int i = 0; i < h->n; i++) {
    long f = h->freq[i];
    if (f <= 0) continue;
    if (!*lo || f < *lo) *lo = f;
    if (f > *hi) *hi = f;
  }
}

/* 读 /proc/stat 的 cpuN 行：total（前 8 项和）/ idle（idle+iowait）。 */
static int core_stat_read(long *total, long *idle) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f) return 0;
  char line[256];
  int got = 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9') continue;
    int c = atoi(line + 3);
    if (c < 0 || c >= MAX_CORES) continue;
    long v[8] = {0};
    int k = sscanf(line, "cpu%d %ld %ld %ld %ld %ld %ld %ld %ld",
                   &c, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (k < 5) continue;
    long sum = 0;
    for (int i = 0; i < 8; i++) sum += v[i];
    total[c] = sum;
    idle[c] = v[3] + v[4];
    got++;
  }
  fclose(f);
  return got;
}

/* cores 每秒是一个新进程，内存里的窗口活不过一次调用。
 * 把上一拍读数和 5 格环落到 run/cores.state（先写 tmp 再 rename），
 * 下一次调用接着算增量；页面不开就没人调用，不会后台常驻采样。
 * 用单独的 lock 文件串行化 cores / export 两个入口。 */
static int g_core_lock_fd = -1;

static void core_state_lock(void) {
  mkdir(RUN_DIR, 0755);
  g_core_lock_fd = open(CORE_STATE_LOCK, O_RDWR | O_CREAT, 0644);
  if (g_core_lock_fd >= 0) flock(g_core_lock_fd, LOCK_EX);
}
static void core_state_unlock(void) {
  if (g_core_lock_fd >= 0) {
    flock(g_core_lock_fd, LOCK_UN);
    close(g_core_lock_fd);
    g_core_lock_fd = -1;
  }
}

static void core_state_load(void) {
  FILE *f = fopen(CORE_STATE_FILE, "r");
  if (!f) return;
  char line[4096];
  /* 页面关了很久再打开时，上一拍的 /proc/stat 基线已经过期；
     直接拿几小时的差值会算出一个假占用。超过 10 秒就重立。 */
  long stamp = 0;
  if (!fgets(line, sizeof line, f) || sscanf(line, "v3 %ld", &stamp) != 1 ||
      stamp <= 0) {
    fclose(f);
    return;
  }
  long gap = mono_ms() - stamp;
  if (gap < 0 || gap > 10000) {   /* gap<0 = 重启过，monotonic 归零 */
    fclose(f);
    return;
  }
  while (fgets(line, sizeof line, f)) {
    int cid, hp, n;
    long pt, pi;
    if (sscanf(line, "C %d %d %ld %ld %d", &cid, &hp, &pt, &pi, &n) != 5) continue;
    if (cid < 0 || cid >= MAX_CORES) continue;
    struct core_hist *h = &g_ch[cid];
    memset(h, 0, sizeof *h);
    h->have_prev = hp ? 1 : 0;
    h->prev_total = pt;
    h->prev_idle = pi;
    h->n = n < 0 ? 0 : (n > CORE_HIST_N ? CORE_HIST_N : n);
    if (!fgets(line, sizeof line, f) || strncmp(line, "F ", 2)) continue;
    if (sscanf(line, "F %ld %ld %ld %ld %ld %ld %ld %ld",
               &h->freq[0], &h->freq[1], &h->freq[2], &h->freq[3],
               &h->freq[4], &h->freq[5], &h->freq[6], &h->freq[7]) != 8) {
      h->n = 0;
      continue;
    }
    if (!fgets(line, sizeof line, f) || strncmp(line, "L ", 2)) { h->n = 0; continue; }
    if (sscanf(line, "L %ld %ld %ld %ld %ld %ld %ld %ld",
               &h->load[0], &h->load[1], &h->load[2], &h->load[3],
               &h->load[4], &h->load[5], &h->load[6], &h->load[7]) != 8) {
      h->n = 0;
      continue;
    }
  }
  fclose(f);
}

static void core_state_save(void) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp.%d", CORE_STATE_FILE, (int)getpid());
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  fprintf(f, "v3 %ld\n", mono_ms());
  for (int c = 0; c < MAX_CORES; c++) {
    struct core_hist *h = &g_ch[c];
    if (h->n <= 0 && !h->have_prev) continue;
    fprintf(f, "C %d %d %ld %ld %d\n", c, h->have_prev ? 1 : 0,
            h->prev_total, h->prev_idle, h->n);
    fprintf(f, "F %ld %ld %ld %ld %ld %ld %ld %ld\n",
            h->freq[0], h->freq[1], h->freq[2], h->freq[3],
            h->freq[4], h->freq[5], h->freq[6], h->freq[7]);
    fprintf(f, "L %ld %ld %ld %ld %ld %ld %ld %ld\n",
            h->load[0], h->load[1], h->load[2], h->load[3],
            h->load[4], h->load[5], h->load[6], h->load[7]);
  }
  fclose(f);
  rename(tmp, CORE_STATE_FILE);
}

static int write_num(const char *path, long val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  char b[32];
  int n = snprintf(b, sizeof b, "%ld\n", val);
  ssize_t w = write(fd, b, n);
  close(fd);
  return w == n ? 0 : -1;
}

static int write_str(const char *path, const char *val) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  size_t l = strlen(val);
  ssize_t w = write(fd, val, l);
  close(fd);
  return w == (ssize_t)l ? 0 : -1;
}

static int read_mode(const char *path) {
  struct stat st;
  if (stat(path, &st)) return 0644;
  return st.st_mode & 07777;
}

static void trim(char *s) {
  size_t l = strlen(s);
  while (l && strchr(" \t\r\n", s[l - 1])) s[--l] = 0;
  char *p = s;
  while (*p && isspace((unsigned char)*p)) memmove(p, p + 1, strlen(p));
}

/* ------------------------------------------------------------------ */
/* journal：写之前先记账，恢复失败也不丢，下次启动再试                 */
/* ------------------------------------------------------------------ */

static int journal_has(const char *file, const char *node) {
  FILE *f = fopen(file, "r");
  if (!f) return 0;
  char line[512], p[512];
  int found = 0;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%511s", p) == 1 && !strcmp(p, node)) {
      found = 1;
      break;
    }
  fclose(f);
  return found;
}

static void journal_add(const char *file, const char *node, long val, int mode) {
  FILE *f = fopen(file, "a");
  if (!f) return;
  fprintf(f, "%s %ld %o\n", node, val, mode);
  fclose(f);
}

/* 数值节点写入：先记账原值+权限，再写。val<0 表示只记不改 */
static int write_journaled(const char *file, const char *node, long val) {
  if (access(node, F_OK)) return -1;
  if (!journal_has(file, node)) {
    long orig = read_num(node);
    if (orig < 0) return -1;
    journal_add(file, node, orig, read_mode(node));
  }
  if (val < 0) return 0;
  chmod(node, 0644);
  return write_num(node, val);
}

/* 恢复 journal 中所有数值节点；任一失败保留文件，下次启动重试 */
static int journal_restore(const char *file) {
  if (access(file, F_OK)) return 0;
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  char line[512], node[512];
  long val;
  int mode, failed = 0, count = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "%511s %ld %o", node, &val, &mode) != 3) continue;
    chmod(node, 0644);
    if (write_num(node, val) || read_num(node) != val) failed = 1;
    chmod(node, mode);
    count++;
  }
  fclose(f);
  if (failed) {
    log("WARN 恢复未完成（%d 节点），保留快照下次重试", count);
    return -1;
  }
  unlink(file);
  log("已恢复 %d 个节点", count);
  return 0;
}

/*
 * 只还权限，不动数值。
 *
 * 权限 journal 里的值列是占位的 0 —— 它只用来记 mode。
 * 走 journal_restore 会把 0 真的写进节点：
 *   - scaling_max_freq 收到 "0" 被内核拒 → failed → 快照永不删除
 *   - max_pwrlevel 这种 0 是合法索引值的节点，会被真的写没
 * 所以权限还原必须单开一条路径。
 */
static int perms_restore(void) {
  if (access(PERM_JOURNAL, F_OK)) return 0;
  FILE *f = fopen(PERM_JOURNAL, "r");
  if (!f) return -1;
  char line[512], node[512];
  long val;
  int mode, failed = 0, count = 0;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "%511s %ld %o", node, &val, &mode) != 3) continue;
    /* 节点已经没了也算处理完，不然快照永远删不掉 */
    if (access(node, F_OK)) continue;
    if (chmod(node, (mode_t)mode)) failed = 1;
    else count++;
  }
  fclose(f);
  if (failed) {
    log("WARN 权限收回未完成，保留 perms.saved 下次重试");
    return -1;
  }
  unlink(PERM_JOURNAL);
  if (count) log("已收回 %d 个节点权限", count);
  return 0;
}

/* ------------------------------------------------------------------ */
/* SoC 芯片代号                                                        */
/* ------------------------------------------------------------------ */

/*
 * 顺序不能随便换：ro.soc.model 是最准的，但只在部分机型上存在；
 * ro.board.platform 覆盖面最广，值就是芯片代号（sm8550 / mt6893…）。
 * 都取不到就回机型名，总比空着好——WebUI 顶上要显示这一行。
 */

/* 按空格/逗号/制表符切词，取最后一个词并转小写。 */
static void last_word(const char *v, char *out, size_t n) {
  const char *t = v, *tok = NULL, *end = NULL;
  for (;;) {
    while (*t == ' ' || *t == ',' || *t == '\t') t++;
    if (!*t) break;
    tok = t; /* 记词的开头 */
    while (*t && *t != ' ' && *t != ',' && *t != '\t') t++;
    end = t; /* 词尾现在就得记：外层还会把 t 推过尾部分隔符 */
  }
  if (!tok) { snprintf(out, n, ""); return; }
  size_t i = 0;
  for (; tok + i < end && i + 1 < n; i++)
    out[i] = (char)tolower((unsigned char)tok[i]);
  out[i] = 0;
}

static void read_soc_name(char *out, size_t n) {
  static const char *props[] = {"ro.soc.model", "ro.board.platform",
                                "ro.chipname", "ro.product.model"};
  snprintf(out, n, "未知");
  for (size_t i = 0; i < sizeof props / sizeof *props; i++) {
    char v[128], w[128];
    if (__system_property_get(props[i], v) <= 0) continue;
    last_word(v, w, sizeof w);
    if (w[0]) { snprintf(out, n, "%s", w); return; }
  }
}

/* ------------------------------------------------------------------ */
/* CPU policy 探测与分档                                              */
/* ------------------------------------------------------------------ */

struct policy {
  char name[32];
  char dir[160];
  long hw_max;
  long cap;  /* 封顶值，已吸附到频点 */
  long pct;  /* 意图百分比 */
  int tier;  /* 用的哪一档：0=BIG 1=MID2 2=MID 3=LITTLE，WebUI 靠它对滑杆 */
  int pol_no; /* policy 号（atoi(name+6)）。反查"某档管哪几颗核"要拿它和
               * core_policy_of(c) 比，后者来自 readlink、不是数组下标；
               * 按 hw_max 去重后下标与 policy 号不再相等，须单独保存。 */
};

static int policy_index(const char *dir) {
  const char *s = strrchr(dir, '/');
  s = s ? s + 1 : dir;
  if (strncmp(s, "policy", 6)) return 999;
  return atoi(s + 6);
}

/* 从频点表文本里挑出 <= want 的最高频点。 */
static long snap_from_list(const char *list, long want) {
  char b[4096];
  snprintf(b, sizeof b, "%s", list);
  long best = 0;
  char *save = NULL;
  for (char *t = strtok_r(b, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) {
    if (!isdigit((unsigned char)*t)) continue;
    long f = strtol(t, NULL, 10);
    if (f > 0 && f <= want && f > best) best = f;
  }
  return best > 0 ? best : want;
}

/* 把期望值吸附到 <= 它的最高可用频点：封顶语义要求实际永远不超过意图 */
static long snap_freq(const struct policy *p, long want) {
  char b[4096], path[224];
  snprintf(path, sizeof path, "%s/scaling_available_frequencies", p->dir);
  if (read_file(path, b, sizeof b)) return want; /* 无频点表就用原值 */
  return snap_from_list(b, want);
}

/* 收集全部 policy 目录，**不去重**，并记下 policy 号。
 * detect_policies 去重是为了得到干净的簇阶梯（封顶只需一条代表），
 * 但"某档管哪几颗核"必须按未去重的列表反查：同一硬件上限的多个
 * policy 去重后只剩一个，另一个管的核心会整档丢失。 */
/* ── 芯片适配覆盖（override.conf）───────────────────────────────
 * 有的机器节点名、目录结构和常规不一样，自动探测一个都读不到，
 * 表现就是"装上模块但看不到核心表 / GPU 不限频"。这种机器靠人工适配：
 * 让对方在页面上「导出适配包」，按包里的现场路径改出一份 override.conf，
 * 放回去重启守护即可。
 *
 * 只在自动探测拿不到结果时才用它：正常机器不读这些值，
 * 覆盖也就永远不会盖掉实测出来的东西。
 *
 * 一行一条，认这四种键（其余行忽略，方便人写注释）：
 *   cpu-dir   <目录绝对路径> <硬件最高频>
 *   gpu-max   <节点路径>
 *   gpu-min   <节点路径>
 *   gpu-freqs <空格分隔的频率表>
 */
#define OVR_FILE SF_DATADIR "/override.conf"

static char ovr_dir[MAX_POLICY][224];
static long ovr_hw[MAX_POLICY];
static int ovr_n;
static char ovr_gpu_max[256], ovr_gpu_min[256], ovr_gpu_freqs[512];

static void ovr_load(void) {
  FILE *f = fopen(OVR_FILE, "r");
  if (!f) return;
  char line[800];
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "cpu-dir ", 8)) {
      if (ovr_n >= MAX_POLICY) continue;
      long hw = 0;
      if (sscanf(line + 8, "%223s %ld", ovr_dir[ovr_n], &hw) == 2 && hw > 0 &&
          !access(ovr_dir[ovr_n], F_OK)) {
        ovr_hw[ovr_n] = hw;
        ovr_n++;
      }
    } else if (!strncmp(line, "gpu-max ", 8)) {
      sscanf(line + 8, "%255[^\r\n]", ovr_gpu_max);
    } else if (!strncmp(line, "gpu-min ", 8)) {
      sscanf(line + 8, "%255[^\r\n]", ovr_gpu_min);
    } else if (!strncmp(line, "gpu-freqs ", 10)) {
      sscanf(line + 10, "%511[^\r\n]", ovr_gpu_freqs);
    }
  }
  fclose(f);
}

/* 自动探测一个 policy 都没拿到时的兜底。
 * 目录路径由人给出，所以不要求它一定在 /sys/devices/system/cpu/cpufreq 下面，
 * 也不要求目录名一定叫 policyN。 */
static int ovr_policies(struct policy *out) {
  int n = 0;
  for (int i = 0; i < ovr_n && n < MAX_POLICY; i++) {
    char cut[240], path[256];
    const char *base = strrchr(ovr_dir[i], '/');
    base = base ? base + 1 : ovr_dir[i];
    /* 逐核目录（…/cpu0/cpufreq）的末段每条都一样，直接拿来当名字，
       页面上就是一排重名的"cpufreq"。取上一段（cpu0）才分得清哪一簇。 */
    if (!strcmp(base, "cpufreq")) {
      size_t len = (size_t)(base - 1 - ovr_dir[i]);   /* 去掉末段，留 …/cpu0 */
      if (base == ovr_dir[i] || len >= sizeof cut) len = 0;
      snprintf(cut, sizeof cut, "%.*s", (int)len, ovr_dir[i]);
      const char *p2 = strrchr(cut, '/');
      base = p2 ? p2 + 1 : cut;
    }
    snprintf(out[n].dir, sizeof out[n].dir, "%s", ovr_dir[i]);
    snprintf(out[n].name, sizeof out[n].name, "%s", base);
    out[n].pol_no = policy_index(out[n].dir);
    snprintf(path, sizeof path, "%s/cpuinfo_max_freq", out[n].dir);
    out[n].hw_max = read_num(path);
    if (out[n].hw_max <= 0) out[n].hw_max = ovr_hw[i];
    if (out[n].hw_max > 0) n++;
  }
  return n;
}

static int detect_policies_raw(struct policy *out) {
  DIR *d = opendir(CPUFREQ);
  if (!d) return 0;
  char path[256];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < MAX_POLICY) {
    if (strncmp(e->d_name, "policy", 6)) continue;
    snprintf(out[n].dir, sizeof out[n].dir, "%s/%s", CPUFREQ, e->d_name);
    snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
    /* policy 号直接从目录名取，别用数组下标：下标会随目录顺序变，
     * 而 core_policy_of(c) 给的是 readlink 读出来的真实 policy 号，
     * 两者必须能对上，反查才成立。 */
    out[n].pol_no = atoi(e->d_name + 6);
    snprintf(path, sizeof path, "%s/cpuinfo_max_freq", out[n].dir);
    out[n].hw_max = read_num(path);
    if (out[n].hw_max > 0) n++;
  }
  closedir(d);
  /* 按 policy 号升序，让同样的机器每次导出顺序一致 */
  for (int i = 1; i < n; i++) {
    struct policy key = out[i];
    int j = i - 1;
    while (j >= 0 && policy_index(out[j].dir) > policy_index(key.dir)) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  return n;
}

/* 硬件最高频相同的 policy 只留一个，得到干净的簇阶梯 */
static int detect_policies(struct policy *out) {
  struct policy tmp[MAX_POLICY];
  int n = detect_policies_raw(tmp);
  if (!n) n = ovr_policies(tmp);   /* 一个都没探测到时才用适配覆盖 */
  int u = 0;
  for (int i = 0; i < n; i++) {
    int dup = 0;
    for (int j = 0; j < u; j++)
      if (tmp[j].hw_max == tmp[i].hw_max) dup = 1;
    if (!dup) out[u++] = tmp[i];
  }
  return u;
}

/* 簇阶梯：把全部 policy 的硬件上限去重后升序排好，m 是"不同上限的档数"。
   assign_pct 分档和 export 反查"某档管哪几颗核"共用同一份阶梯，
   以保证两处判据一致。 */
static int build_ladder(const struct policy *p, int n, long *ladder) {
  int m = 0;
  for (int i = 0; i < n; i++) {
    int seen = 0;
    for (int j = 0; j < m; j++)
      if (ladder[j] == p[i].hw_max) seen = 1;
    if (!seen) ladder[m++] = p[i].hw_max;
  }
  for (int i = 1; i < m; i++) {
    long k = ladder[i];
    int j = i - 1;
    while (j >= 0 && ladder[j] > k) {
      ladder[j + 1] = ladder[j];
      j--;
    }
    ladder[j + 1] = k;
  }
  return m;
}

/* 某个硬件上限属于哪一档，返回 0=BIG 1=MID2 2=MID 3=LITTLE（= tierkey）。
 * 判据与 assign_pct 共用同一函数，保证两处一致。
 *
 * m==2 的机器（小核 3533 + 超核 4320 这类两档布局）会**跳过**
 * MID2 和 MID 两档：最高和最低占 tier 0 和 1，
 * CPU_PCT_MID2 / CPU_PCT_MID 这两个配置键在这类机器上不生效——
 * 这两档没有对应的簇，不是保存失败。
 * 因此"这台机有几档"由 ntier 报出，页面对未落到的档显示（未检测到）。 */
static int tier_key_of(const long *ladder, int m, long hw_max) {
  int tier = 0;
  for (int j = 0; j < m; j++)
    if (ladder[j] == hw_max) tier = j;
  if (m <= 1) return 0;
  if (tier == m - 1) return 0;
  if (tier == 0) return 3;
  if (m >= 4 && tier == m - 2) return 1;
  return 2;
}

/* 按簇阶梯分档：最高档 big，次高 mid2，中间 mid，最低 little */
/* 生效封顶 = 用户设的值 + 实时偏移（adapt.c 每轮算好放在快照里）。
 * 充电态是"抬地板"：插着电发热由充电器承担，没必要压着；
 * 但它只作用于全局分档 —— 用户给某个应用单独设的档位是他的明确选择，
 * 插电不该替他改掉。温控和 PSI 叠在地板之上，所以热的时候插着电照样降。 */
static int eff_pct(int base, int per_app) {
  struct adapt a;
  adapt_snapshot(&a);
  if (!per_app && a.chg_on && a.chg_pct > base) base = a.chg_pct;
  /* 电量阶梯也属于"全局让路"，不盖用户给某个应用明确设的档位。 */
  int d = a.delta;
  if (per_app) d += a.batt_cut;
  base += d;
  if (base < 1) base = 1;
  if (base > 100) base = 100;
  return base;
}

static void assign_pct(struct policy *p, int n) {
  long ladder[MAX_POLICY];
  int m = build_ladder(p, n, ladder);
  for (int i = 0; i < n; i++) {
    int tier = 0;
    for (int j = 0; j < m; j++)
      if (ladder[j] == p[i].hw_max) tier = j;
    int pct, tierkey;
    tierkey = tier_key_of(ladder, m, p[i].hw_max);
    /* 键到值的下标和 tierkey 一样（cfg.pct 的下标就是 BIG/MID2/MID/LITTLE） */
    pct = cfg.pct[tierkey];
    if (pct < 1 || pct > 100) pct = 1;
    int eff = eff_pct(pct, 0);
    p[i].pct = eff;
    p[i].tier = tierkey;
    p[i].cap = snap_freq(&p[i], p[i].hw_max * eff / 100);
    log("  %-8s 档位%d/%d 硬件%ld 意图%d%% 生效%d%% 封顶%ld (%.1f%%)", p[i].name, tier,
        m - 1, p[i].hw_max, pct, eff, p[i].cap, 100.0 * p[i].cap / p[i].hw_max);
  }
}

/* ------------------------------------------------------------------ */
/* GPU 探测（高通 KGSL + MTK Mali）                                   */
/* ------------------------------------------------------------------ */

struct gpu {
  char max_node[256];
  int max_is_mhz;
  char min_node[256];
  int min_is_mhz;
  char pwr_node[256];
  long normal, max_hz, min_hz;
  long active;            /* 本轮真正要压到的频点；实时偏移会改它 */
  long tbl[MAX_NODE];     /* 降序频点表，吸附和 pwrlevel 都用同一份 */
  int ntbl;
  int pwr_normal;
  int ok;
  /* MTK gpufreqv2 limit_table 后端（没有 devfreq 节点的天玑机） */
  int mtk;
  int mtk_layer;
  int mtk_orig_ceiling, mtk_orig_floor;
  int mtk_dirty;
};

static struct gpu g_gpu;

static const char *gpu_max_candidates[] = {
    "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/max_freq",
    "/sys/class/devfreq/5000000.qcom,kgsl-3d0/max_freq",
    "/sys/class/devfreq/2c00000.qcom,kgsl-3d0/max_freq",
    "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq",
    "/sys/class/kgsl/kgsl-3d0/max_gpuclk",
    "/sys/class/kgsl/kgsl-3d0/max_clock_mhz",
    "/sys/class/devfreq/13000000.mali/max_freq",
    "/sys/class/devfreq/48000000.mali/max_freq",
    "/sys/class/devfreq/mtk-mali/max_freq",
    "/sys/class/devfreq/mali0/max_freq",
    "/sys/class/devfreq/gpufreq/max_freq",
    "/sys/class/misc/mali0/device/devfreq/max_freq",
    "/sys/devices/platform/soc/13000000.mali/devfreq/max_freq",
    "/sys/devices/platform/13000000.mali/devfreq/max_freq",
    "/sys/kernel/gpu/gpu_max_clock",
};

static const char *gpu_freq_candidates[] = {
    "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies",
    "/sys/class/kgsl/kgsl-3d0/devfreq/available_frequencies",
    "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/available_frequencies",
    "/sys/class/devfreq/5000000.qcom,kgsl-3d0/available_frequencies",
    "/sys/class/kgsl/kgsl-3d0/freq_table_mhz",
    "/sys/class/devfreq/3d00000.qcom,kgsl-3d0/freq_table_mhz",
    "/sys/class/devfreq/5000000.qcom,kgsl-3d0/freq_table_mhz",
    "/sys/class/devfreq/13000000.mali/available_frequencies",
    "/sys/class/devfreq/48000000.mali/available_frequencies",
    "/sys/class/devfreq/mtk-mali/available_frequencies",
    "/sys/class/devfreq/mali0/available_frequencies",
    "/sys/class/devfreq/gpufreq/available_frequencies",
    "/sys/class/misc/mali0/device/devfreq/available_frequencies",
    "/sys/devices/platform/soc/13000000.mali/devfreq/available_frequencies",
    "/sys/devices/platform/13000000.mali/devfreq/available_frequencies",
};

/* 读频率表并统一归一到 Hz；数字小于一万判为 MHz */
static int parse_freq_table(const char *path, long *out, int max) {
  char b[8192];
  if (read_file(path, b, sizeof b)) return 0;
  int n = 0, mhz_tbl = strstr(path, "freq_table_mhz") != NULL;
  long peak = 0;
  char *save = NULL;
  for (char *t = strtok_r(b, " \t\n,", &save); t; t = strtok_r(NULL, " \t\n,", &save)) {
    if (!isdigit((unsigned char)*t)) continue;
    long v = strtol(t, NULL, 10);
    if (v <= 0) continue;
    if (v > peak) peak = v;
    if (n < max) out[n++] = v;
  }
  if (!n) return 0;
  if (mhz_tbl || peak < 10000)
    for (int i = 0; i < n; i++) out[i] *= 1000000;
  return n;
}

/* ── MTK gpufreqv2 ─────────────────────────────────────────────── */
/* stack_working_opp_table 一行形如 "[00] freq: 1716000, volt: ..."；
 * 只取 freq: 后面的数，单位 kHz，归一到 Hz。 */
static int mtk_parse_opp_text(const char *text, long *out, int max) {
  if (!text) return 0;
  char *copy = strdup(text);
  if (!copy) return 0;
  int n = 0;
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *p = strstr(line, "freq:");
    if (!p) continue;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) continue;
    long v = strtol(p, NULL, 10);
    if (v <= 0) continue;
    if (v < 10000000L) v *= 1000;   /* MTK OPP 表用 kHz */
    if (n < max) out[n++] = v;
  }
  free(copy);
  return n;
}

static int mtk_parse_opp(const char *path, long *out, int max) {
  char b[16384];
  if (read_file(path, b, sizeof b)) return 0;
  return mtk_parse_opp_text(b, out, max);
}

/* 从 limit_table 读某一层的 ceiling/floor；找不到返回 0。 */
static int mtk_read_row(int id, int *ceil, int *floor) {
  char b[4096];
  if (read_file(MTK_LIMIT, b, sizeof b)) return 0;
  char *save = NULL;
  for (char *ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    int fid = -1, prio = 0, ce = -1, fl = -1, ce_en = 0, fe_en = 0;
    char name[64] = {0};
    if (sscanf(ln, "%d %63s %d %d %d %d %d",
               &fid, name, &prio, &ce, &fl, &ce_en, &fe_en) != 7)
      continue;
    if (fid != id) continue;
    *ceil = ce;
    *floor = fl;
    return 1;
  }
  return 0;
}

static int mtk_write_row(int id, int ceil, int floor) {
  char cmd[96];
  snprintf(cmd, sizeof cmd, "set %d %d %d\n", id, ceil, floor);
  return write_text(MTK_LIMIT, cmd);
}

static void mtk_save_snapshot(void) {
  char b[128];
  snprintf(b, sizeof b, "id=%d\nceil=%d\nfloor=%d\n",
           g_gpu.mtk_layer, g_gpu.mtk_orig_ceiling, g_gpu.mtk_orig_floor);
  if (write_text_file(MTK_GPU_SNAP, b))
    log("WARN MTK GPU 快照写入失败");
}

/* 正常退出/交权：把这一层写回原来的 ceiling/floor，删快照。 */
static void mtk_restore_row(void) {
  if (!g_gpu.mtk) return;
  mtk_write_row(g_gpu.mtk_layer, g_gpu.mtk_orig_ceiling, g_gpu.mtk_orig_floor);
  unlink(MTK_GPU_SNAP);
  g_gpu.mtk_dirty = 0;
}

/* 启动时发现上次没走正常退出：先把 limit_table 还原，再重新探测。 */
static void mtk_restore_snapshot(void) {
  char b[256];
  if (read_file(MTK_GPU_SNAP, b, sizeof b)) return;
  int id = -1, c = -1, f = -1;
  char *p;
  if ((p = strstr(b, "id="))) id = atoi(p + 3);
  if ((p = strstr(b, "ceil="))) c = atoi(p + 5);
  if ((p = strstr(b, "floor="))) f = atoi(p + 6);
  if (id >= 0 && mtk_write_row(id, c, f) == 0)
    log("MTK GPU: 按存档恢复 layer=%d %d/%d", id, c, f);
  unlink(MTK_GPU_SNAP);
}

static int detect_gpu(void) {
  /* 上次被 kill -9 留下的 cap 先还原，别把"37/56"当成原始行再存一遍。 */
  mtk_restore_snapshot();
  memset(&g_gpu, 0, sizeof g_gpu);
  long tbl[MAX_NODE];
  int n = 0;
  if (!access(MTK_LIMIT, F_OK) && !access(MTK_OPP, F_OK)) {
    n = mtk_parse_opp(MTK_OPP, tbl, MAX_NODE);
    if (n) {
      g_gpu.mtk = 1;
      g_gpu.mtk_layer = 1;   /* DEBUG 层：实测能压 PPM Ceiling，且不抢系统层 */
      log("GPU 频率表: MTK %s", MTK_OPP);
    }
  }
  for (size_t i = 0; !n && i < sizeof gpu_freq_candidates / sizeof *gpu_freq_candidates;
       i++) {
    n = parse_freq_table(gpu_freq_candidates[i], tbl, MAX_NODE);
    if (n) {
      log("GPU 频率表: %s", gpu_freq_candidates[i]);
      break;
    }
  }
  if (!n && ovr_gpu_freqs[0]) {
    char fb[512], *fsv = NULL;
    snprintf(fb, sizeof fb, "%s", ovr_gpu_freqs);
    for (char *t = strtok_r(fb, " \t", &fsv); t && n < MAX_NODE;
         t = strtok_r(NULL, " \t", &fsv)) {
      long v = strtol(t, NULL, 10);
      if (v > 0) tbl[n++] = v;
    }
    if (n) log("GPU 频率表来自适配覆盖");
  }
  if (!n) {
    log("WARN 未找到 GPU 频率表，跳过 GPU 限频");
    return -1;
  }

  g_gpu.max_hz = g_gpu.min_hz = tbl[0];
  for (int i = 1; i < n; i++) {
    if (tbl[i] > g_gpu.max_hz) g_gpu.max_hz = tbl[i];
    if (tbl[i] < g_gpu.min_hz) g_gpu.min_hz = tbl[i];
  }

  long want = g_gpu.max_hz * cfg.gpu_pct / 100;
  g_gpu.normal = 0;
  for (int i = 0; i < n; i++)
    if (tbl[i] <= want && tbl[i] > g_gpu.normal) g_gpu.normal = tbl[i];
  if (!g_gpu.normal) g_gpu.normal = g_gpu.min_hz;

  /* 降序排一遍：KGSL 的 max_pwrlevel 是索引，0 对应最高频 */
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (tbl[j] > tbl[i]) {
        long t = tbl[i];
        tbl[i] = tbl[j];
        tbl[j] = t;
      }
  memcpy(g_gpu.tbl, tbl, sizeof(long) * n);
  g_gpu.ntbl = n;
  if (g_gpu.mtk) {
    int c = -1, f = -1;
    if (!mtk_read_row(g_gpu.mtk_layer, &c, &f)) { c = -1; f = -1; }
    g_gpu.mtk_orig_ceiling = c;
    g_gpu.mtk_orig_floor = f;
    g_gpu.ok = 1;
    log("GPU: MTK gpufreqv2 layer=%d 原始 %d/%d 硬件%ld~%ldHz 封顶%ldHz",
        g_gpu.mtk_layer, c, f, g_gpu.min_hz, g_gpu.max_hz, g_gpu.normal);
    return 0;
  }
  if (!access("/sys/class/kgsl/kgsl-3d0/max_pwrlevel", F_OK)) {
    snprintf(g_gpu.pwr_node, sizeof g_gpu.pwr_node,
             "/sys/class/kgsl/kgsl-3d0/max_pwrlevel");
    for (int i = 0; i < n; i++)
      if (tbl[i] <= g_gpu.normal) {
        g_gpu.pwr_normal = i;
        break;
      }
  }

  for (size_t i = 0; i < sizeof gpu_max_candidates / sizeof *gpu_max_candidates;
       i++) {
    if (access(gpu_max_candidates[i], F_OK)) continue;
    snprintf(g_gpu.max_node, sizeof g_gpu.max_node, "%s", gpu_max_candidates[i]);
    g_gpu.max_is_mhz = strstr(g_gpu.max_node, "gpu_max_clock") ||
                       strstr(g_gpu.max_node, "max_clock_mhz");
    break;
  }

  if (!g_gpu.max_node[0] && ovr_gpu_max[0] && !access(ovr_gpu_max, F_OK)) {
    snprintf(g_gpu.max_node, sizeof g_gpu.max_node, "%s", ovr_gpu_max);
    g_gpu.max_is_mhz = strstr(g_gpu.max_node, "mhz") ||
                       strstr(g_gpu.max_node, "MHZ") ? 1 : 0;
    log("GPU 上限节点来自适配覆盖");
  }
  if (!g_gpu.min_node[0] && ovr_gpu_min[0] && !access(ovr_gpu_min, F_OK)) {
    snprintf(g_gpu.min_node, sizeof g_gpu.min_node, "%s", ovr_gpu_min);
    g_gpu.min_is_mhz = strstr(g_gpu.min_node, "mhz") ||
                       strstr(g_gpu.min_node, "MHZ") ? 1 : 0;
  }

  /* 配对下限节点：优先统一命名，否则按后缀替换 max→min */
  if (!access("/sys/kernel/gpu/gpu_min_clock", F_OK)) {
    snprintf(g_gpu.min_node, sizeof g_gpu.min_node,
             "/sys/kernel/gpu/gpu_min_clock");
    g_gpu.min_is_mhz = 1;
  } else if (g_gpu.max_node[0]) {
    static const struct {
      const char *suffix, *repl;
      int mhz;
    } pairs[] = {
        {"max_gpuclk", "min_gpuclk", 0},
        {"max_freq", "min_freq", 0},
        {"gpu_max_clock", "gpu_min_clock", 1},
        {"max_clock_mhz", "min_clock_mhz", 1},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
      char *hit = strstr(g_gpu.max_node, pairs[i].suffix);
      if (!hit) continue;
      char cand[256];
      snprintf(cand, sizeof cand, "%.*s%s%s", (int)(hit - g_gpu.max_node),
               g_gpu.max_node, pairs[i].repl, hit + strlen(pairs[i].suffix));
      if (!access(cand, F_OK)) {
        snprintf(g_gpu.min_node, sizeof g_gpu.min_node, "%s", cand);
        g_gpu.min_is_mhz = pairs[i].mhz;
      }
      break;
    }
  }

  if (!g_gpu.max_node[0] && !g_gpu.pwr_node[0]) {
    log("WARN 未找到可写 GPU 节点，跳过 GPU 限频");
    return -1;
  }
  g_gpu.ok = 1;
  log("GPU: 上限节点=%s(%s) 下限节点=%s 硬件%ld~%ldHz 封顶%ldHz%s", g_gpu.max_node,
      g_gpu.max_is_mhz ? "MHz" : "Hz", g_gpu.min_node[0] ? g_gpu.min_node : "无",
      g_gpu.min_hz, g_gpu.max_hz, g_gpu.normal, g_gpu.pwr_node[0] ? " +pwrlevel" : "");
  return 0;
}

/* 百分比吸附到不高于目标值的最高频点。和 detect_gpu 挑 normal 是同一套规则，
 * 只是目标值会随 PSI/温控偏移变化，不能只在加载配置时算一次。 */
static long gpu_snap(int pct) {
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  long want = g_gpu.max_hz * pct / 100;
  long best = 0;
  for (int i = 0; i < g_gpu.ntbl; i++)
    if (g_gpu.tbl[i] <= want && g_gpu.tbl[i] > best) best = g_gpu.tbl[i];
  return best ? best : g_gpu.min_hz;
}

/* 降序表里第一个不高于目标的档就是 max_pwrlevel 的索引。 */
static int gpu_pwr_for(long hz) {
  for (int i = 0; i < g_gpu.ntbl; i++)
    if (g_gpu.tbl[i] <= hz) return i;
  return g_gpu.ntbl > 0 ? g_gpu.ntbl - 1 : 0;
}

/* 按节点单位换算后写入；解锁时只认写成功，被厂商钳制就如实报告不绕路 */
static const char *write_gpu(long want_hz, int unlock) {
  long got;
  if (g_gpu.mtk) {
    int idx, fl;
    if (unlock) {
      idx = g_gpu.mtk_orig_ceiling;
      fl = g_gpu.mtk_orig_floor;
    } else {
      idx = gpu_pwr_for(want_hz);
      fl = cfg.gpu_min_lock ? g_gpu.ntbl - 1 : g_gpu.mtk_orig_floor;
    }
    if (mtk_write_row(g_gpu.mtk_layer, idx, fl) == 0) {
      int rc = -1, rf = -1;
      if (mtk_read_row(g_gpu.mtk_layer, &rc, &rf) && rc == idx && rf == fl) {
        if (unlock) {
          unlink(MTK_GPU_SNAP);
          g_gpu.mtk_dirty = 0;
          return "unlocked";
        }
        if (!g_gpu.mtk_dirty) {
          mtk_save_snapshot();
          g_gpu.mtk_dirty = 1;
        }
        return "capped";
      }
    }
    return "failed";
  }
  if (g_gpu.max_node[0]) {
    long val = g_gpu.max_is_mhz ? want_hz / 1000000 : want_hz;
    if (!write_journaled(NODES_JOURNAL, g_gpu.max_node, val)) {
      got = read_num(g_gpu.max_node);
      if (got > 0 && g_gpu.max_is_mhz) got *= 1000000;
      if (unlock) {
        if (got >= want_hz) return "unlocked";
        log("WARN GPU 解锁受限 期望%ldHz 实际%ldHz（厂商限制）", want_hz, got);
        return "restricted";
      }
      if (got > 0 && got <= want_hz) return "capped";
    }
  }
  /* pwrlevel 是索引不是频率，单独校验 */
  if (g_gpu.pwr_node[0]) {
    long val = unlock ? 0 : gpu_pwr_for(want_hz);
    if (!write_journaled(NODES_JOURNAL, g_gpu.pwr_node, val) &&
        read_num(g_gpu.pwr_node) == val)
      return "pwrlevel";
  }
  return "failed";
}

/* 把 GPU 下限钉回最低档，抵消厂商 boost 抬高下限造成的越界 */
static void gpu_min_to_floor(void) {
  /* MTK 的下限在同一条 set 命令里写；没有独立 min 节点。 */
  if (g_gpu.mtk) return;
  if (!cfg.gpu_min_lock || !g_gpu.min_node[0] || g_gpu.min_hz <= 0) return;
  long val = g_gpu.min_is_mhz ? g_gpu.min_hz / 1000000 : g_gpu.min_hz;
  if (write_journaled(NODES_JOURNAL, g_gpu.min_node, val)) return;
  long got = read_num(g_gpu.min_node);
  if (got > 0 && g_gpu.min_is_mhz) got *= 1000000;
  if (got > g_gpu.min_hz) log("RESTORE GPU 下限 %ldHz -> %ldHz", got, g_gpu.min_hz);
}

/* ------------------------------------------------------------------ */
/* 调速器                                                             */
/* ------------------------------------------------------------------ */

static struct policy g_pol[MAX_POLICY];
static int g_npol;

/* 厂商调速器一律不碰：hmbird / scx 这类自有调度会把写权限从调度器
 * 手里夺走，和"游戏态把权限交还系统"冲突。 */
static int is_vendor_gov(const char *g) {
  return !strncmp(g, "hmbird", 6) || !strncmp(g, "scx", 3);
}

static void apply_governor(const struct policy *p) {
  if (!cfg.governor[0]) return;
  char path[224], cur[64], avail[512], needle[64];
  snprintf(path, sizeof path, "%s/scaling_governor", p->dir);
  if (read_file(path, cur, sizeof cur)) return;
  if (!strcmp(cur, cfg.governor) || is_vendor_gov(cur)) return;
  snprintf(path, sizeof path, "%s/scaling_available_governors", p->dir);
  if (read_file(path, avail, sizeof avail)) return;
  snprintf(needle, sizeof needle, " %s ", cfg.governor);
  if (!strstr(avail, needle)) {
    log("WARN %s 不支持调速器 %s（可用: %s）", p->name, cfg.governor, avail);
    return;
  }
  /* 调速器是字符串，单独记一份账，恢复时按名字写回 */
  snprintf(path, sizeof path, "%s/scaling_governor", p->dir);
  if (!journal_has(GOV_JOURNAL, path)) {
    journal_add(GOV_JOURNAL, path, 0, read_mode(path));
    FILE *f = fopen(GOV_JOURNAL, "a");
    if (f) {
      fprintf(f, "%s %s\n", path, cur);
      fclose(f);
    }
  }
  chmod(path, 0644);
  if (write_str(path, cfg.governor))
    log("WARN %s 调速器写入失败 -> %s", p->name, cfg.governor);
  else
    log("  %s 调速器 -> %s", p->name, cfg.governor);
}

static void snapshot_governors(void) {
  unlink(GOV_JOURNAL);
  for (int i = 0; i < g_npol; i++) {
    char path[224], cur[64];
    snprintf(path, sizeof path, "%s/scaling_governor", g_pol[i].dir);
    if (read_file(path, cur, sizeof cur)) continue;
    journal_add(GOV_JOURNAL, path, 0, read_mode(path));
    FILE *f = fopen(GOV_JOURNAL, "a");
    if (f) {
      fprintf(f, "%s %s\n", path, cur);
      fclose(f);
    }
  }
}

static void restore_governors(void) {
  if (access(GOV_JOURNAL, F_OK)) return;
  FILE *f = fopen(GOV_JOURNAL, "r");
  if (!f) return;
  char line[512], node[512], gov[64];
  int mode, failed = 0, count = 0;
  while (fgets(line, sizeof line, f)) {
    /* govs.saved 里同一个节点有两行：
     *   "<path> 0 <mode>"     <- journal_add 记的权限（第三段是八进制）
     *   "<path> <governor>"   <- 记的调速器名字（只有两段）
     * 两行都要认，只按 sscanf==3 取会拿到权限那一行，
     * 把字面量 "0" 写进 scaling_governor，而名字那行被挡掉，
     * 恢复完的调速器是个 "0"，内核多半拒收。
     *
     *   三段 -> 只取权限，不写值（和 perms_restore 一个道理，
     *          max_pwrlevel 这种 0 是合法索引值）
     *   两段 -> 第二段就是名字，写回去
     */
    if (sscanf(line, "%511s %63s %o", node, gov, &mode) == 3) {
      /* 权限那一行：名字位置是占位的 0，不许写 */
      if (access(node, F_OK)) continue;
      if (chmod(node, (mode_t)mode)) failed = 1;
      else count++;
      continue;
    }
    if (sscanf(line, "%511s %63s", node, gov) != 2) continue;
    if (!gov[0] || strspn(gov, "01234567") == strlen(gov)) continue; /* 纯八进制数不是名字 */
    if (access(node, F_OK)) continue;
    /* 权限先记下来，写完再还回去。
     * 写之前读一次当前权限：这时权限行已经恢复成出厂值了。
     * 硬编码 0644 会盖掉它——出厂 scaling_governor 是 0664 的机型上
     * （system 组要写，perfd 换调速器靠它），模块一停、一卸载，
     * 厂商调度就再也写不动调速器了，而且一句日志都不会有。
     */
    int before = read_mode(node);
    chmod(node, 0644);
    if (write_str(node, gov)) failed = 1;
    else count++;
    if (chmod(node, before)) failed = 1;
  }
  fclose(f);
  if (failed) log("WARN 调速器恢复未完成，保留快照下次重试");
  else {
    unlink(GOV_JOURNAL);
    log("已恢复 %d 个调速器", count);
  }
}

/* ------------------------------------------------------------------ */
/* 游戏判定                                                           */
/* ------------------------------------------------------------------ */

static char g_pkgs[MAX_PKG][PKG_LEN];
static int g_npkg;
static long g_pkg_mtime = -1;

/* 按 mtime 判重，避免每轮都重读整个名单 */
static void load_game_pkgs(void) {
  struct stat st;
  if (stat(GAME_LIST, &st)) return;
  if (st.st_mtime == g_pkg_mtime) return;
  FILE *f = fopen(GAME_LIST, "r");
  if (!f) return;
  g_npkg = 0;
  char line[PKG_LEN];
  while (fgets(line, sizeof line, f) && g_npkg < MAX_PKG) {
    trim(line);
    if (!line[0] || *line == '#' || !strchr(line, '.')) continue;
    int dup = 0;
    for (int i = 0; i < g_npkg; i++)
      if (!strcmp(g_pkgs[i], line)) dup = 1;
    if (dup) continue;
    snprintf(g_pkgs[g_npkg], PKG_LEN, "%s", line);
    g_npkg++;
  }
  fclose(f);
  g_pkg_mtime = st.st_mtime;
  log("游戏名单已加载: %d 个包名", g_npkg);
}

/*
 * 抓一行里的前台包名。
 *
 * 真实 dumpsys 的这几种行，窗口 token 后面只有一个用户号：
 *   mCurrentFocus=Window{a1b2c3 u0 com.foo.bar/com.foo.bar.Main}
 *   mFocusedApp=AppWindowToken{u0 com.foo.bar/com.foo.bar.Main}
 *   mTopFullscreenOpaqueWindowState=Window{9f2b1a u0 com.foo.bar/...}
 * 按此格式一次匹配：越过 " u" 两位再读用户号，其后即为包名。
 */
static int grab_pkg(const char *line, char *out) {
  const char *p = strstr(line, " u");
  if (!p) return -1;
  p += 2; /* 越过 " u"，落在用户号上 */
  if (!isdigit((unsigned char)*p)) return -1;
  while (isdigit((unsigned char)*p)) p++;
  if (*p != ' ') return -1;
  p++;
  /*
   * p 现在指向包名首字符，包名一路到斜杠。
   * 必须向前找包名末尾，不能从 slash 反向扫——反向扫会先撞上
   * 包名前那个空格，end 恰等于 p，长度算成 0。
   */
  size_t l = 0;
  while (p[l] && p[l] != '/') l++;
  if (!p[l] || l >= PKG_LEN) return -1;
  /* 只接受包名允许的字符，杜绝把 Activity 路径或异常内容当成包名 */
  for (size_t i = 0; i < l; i++) {
    char c = p[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) return -1;
  }
  memcpy(out, p, l);
  out[l] = 0;
  return 0;
}

/*
 * 从 dumpsys 输出流里取前台包名。
 * 独立成函数，便于对同一段解析逻辑喂入 dumpsys 文本复用。
 */
static int fg_from_stream(FILE *f, char *out) {
  char line[2048];
  char focus[PKG_LEN] = {0}, app[PKG_LEN] = {0}, top[PKG_LEN] = {0};
  while (fgets(line, sizeof line, f)) {
    if (!focus[0] && strstr(line, "mCurrentFocus")) {
      /* grab_pkg 成功返回 0，失败返回 -1。失败才清空，成功必须保留。 */
      if (grab_pkg(line, focus)) focus[0] = 0;
    } else if (!app[0] && strstr(line, "mFocusedApp")) {
      if (grab_pkg(line, app)) app[0] = 0;
    } else if (!top[0] && strstr(line, "mTopFullscreenOpaqueWindowState")) {
      if (grab_pkg(line, top)) top[0] = 0;
    }
  }

  const char *pick = focus[0] ? focus : (app[0] ? app : top);
  /* 工具类浮窗在上面挡着时，改用它下面的全屏窗口 */
  if (pick && top[0] && (!strcmp(pick, "com.omarea.vtools") ||
                         !strcmp(pick, "bin.mt.plus") ||
                         !strcmp(pick, "bin.mt.plus.canary")))
    pick = top;
  if (!pick || !pick[0]) return -1;
  snprintf(out, PKG_LEN, "%s", pick);
  return 0;
}

/* 取前台包名。dumpsys 会卡，套 timeout，否则 pclose 一直等。 */
static int foreground_pkg(char *out) {
  FILE *f = popen("timeout 2 dumpsys window 2>/dev/null", "r");
  if (!f) return -1;
  int rc = fg_from_stream(f, out);
  pclose(f);
  return rc;
}

static int is_game(const char *pkg) {
  for (int i = 0; i < g_npkg; i++)
    if (!strcmp(g_pkgs[i], pkg)) return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* 分应用档位（app_modes.conf）                                        */
/* ------------------------------------------------------------------ */

/* 一行一条，值是以下三种之一：
 *   full        两者都交还系统（和游戏解锁同一条路，pct=MODE_FULL）
 *   80          只管 CPU；GPU 跟随全局，和加冒号之前的老写法完全一样
 *   80:40        CPU 和 GPU 各自封顶；任一边留空表示那一边跟随全局（80: / :40）
 *   keep        熄屏时不交还，继续按全局百分比巡检（挂机/编译场景）
 * 优先级：游戏名单 > 满血窗口（开机）> 应用专属 > 全局。
 * 熄屏策略见 SCREEN_POLICY；keep 只影响熄屏，不影响亮屏时的全局分档。 */
#define MODE_FULL (-1)
#define MODE_KEEP (-2)
struct app_mode {
  char pkg[PKG_LEN];
  int pct; /* MODE_FULL / MODE_KEEP / 0（CPU 跟随全局）或 1..100 */
  int gpu; /* 0 = GPU 跟随全局，否则 1..100 */
};
static struct app_mode g_modes[MAX_PKG];
static int g_nmode;
static long g_mode_mtime = -1;

/* 当前生效的应用档位：包名 + 值。空串 = 没有生效中的专属档位 */
static char g_app[PKG_LEN];
static int g_app_pct;
/* 应用专属 GPU 档位，0 = 跟随全局 GPU_PCT。full 走交还系统那条路，不在这里。 */
static int g_app_gpu;

/* 刚过去的那个采样周期里，核心或 GPU 身上戴的是哪个应用的专属档位（空 = 全局）。
 * 按包归因只认这个而不认"本轮读到的前台"：它是上一轮写下的，覆盖的正是 dt
 * 那一整段，切换瞬间的脏样本进不来。满血、游戏解锁、熄屏这些没帽的时段留空。 */
static char g_app_capped[PKG_LEN];
/* 刚过去那段戴的是全局帽时，前台是谁。和 g_app_capped 互斥，同一轮只会有一个
 * 非空：全局帽下的分布两层都要记，但归因的包必须来自上一轮，理由同上。 */
static char g_fg_global[PKG_LEN];

/* 读一个档位文件。路径做成参数，便于指向临时件而不改用户生效的配置。 */
static int load_app_modes_from(const char *path) {
  struct stat st;
  if (stat(path, &st)) { g_nmode = 0; g_mode_mtime = -1; return 0; }
  if (st.st_mtime == g_mode_mtime) return g_nmode;
  FILE *f = fopen(path, "r");
  if (!f) return g_nmode;
  g_nmode = 0;
  char line[512];
  while (fgets(line, sizeof line, f) && g_nmode < MAX_PKG) {
    trim(line);
    if (!line[0] || *line == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *pkg = line, *val = eq + 1;
    trim(pkg);
    trim(val);
    /* 值后面允许跟一段注释：学习写回来的那条长这样
     *   com.foo.bar=75  #learn
     * 截掉注释再判数字，否则整行会被当成坏行丢掉——表现是"页面上明明有一条
     * 档位，核心上却没戴帽"，最难查的就是这种静默失效。 */
    char *hash = strchr(val, '#');
    if (hash) { *hash = 0; trim(val); }
    /* 包名必须含点且字符合法：和 api.sh 的 is_package 同一套判据。
     * 守护这侧宽松了，就等于把"写进去的一条坏行"变成"读起来行为不明"。 */
    if (!pkg[0] || !strchr(pkg, '.') || strlen(pkg) >= PKG_LEN) continue;
    for (char *c = pkg; *c; c++)
      if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_') { pkg[0] = 0; break; }
    if (!pkg[0]) continue;
    int pct = 0, gpuv = 0;
    char *colon = strchr(val, ':');
    if (colon) {
      /* CPU 段可以为空（:40 = 只设 GPU），两段都不空时各自 1..100。 */
      *colon = 0;
      char *cp = val, *gp = colon + 1;
      trim(cp);
      trim(gp);
      if (!cp[0] && !gp[0]) continue;
      if (cp[0]) {
        if (cp[strspn(cp, "0123456789")]) continue;
        pct = atoi(cp);
        if (pct < 1 || pct > 100) continue;
      }
      if (gp[0]) {
        if (gp[strspn(gp, "0123456789")]) continue;
        gpuv = atoi(gp);
        if (gpuv < 1 || gpuv > 100) continue;
      }
    } else if (!strcmp(val, "full") || !strcmp(val, "FULL")) {
      pct = MODE_FULL;
    } else if (!strcmp(val, "keep") || !strcmp(val, "KEEP")) {
      pct = MODE_KEEP;      /* 只影响熄屏；亮屏仍按全局分档 */
    } else if (!val[0] || val[strspn(val, "0123456789")]) {
      continue;             /* 非纯数字 */
    } else {
      pct = atoi(val);
      if (pct < 1 || pct > 100) continue;
    }
    int dup = 0;
    for (int i = 0; i < g_nmode; i++)
      if (!strcmp(g_modes[i].pkg, pkg)) {
        g_modes[i].pct = pct;
        g_modes[i].gpu = gpuv;
        dup = 1;
        break;
      }
    if (dup) continue;      /* 同一包重复出现：取后一条，不再加一行 */
    snprintf(g_modes[g_nmode].pkg, PKG_LEN, "%s", pkg);
    g_modes[g_nmode].pct = pct;
    g_modes[g_nmode].gpu = gpuv;
    g_nmode++;
  }
  fclose(f);
  g_mode_mtime = st.st_mtime;
  log("分应用档位已加载: %d 条", g_nmode);
  return g_nmode;
}

static void load_app_modes(void) { load_app_modes_from(MODE_FILE); }

/* 命中返回 1 并给出 CPU/GPU 两段；gpu 的 0 表示跟随全局。 */
static int mode_lookup(const char *pkg, int *out, int *out_gpu) {
  if (!pkg || !pkg[0]) return 0;
  for (int i = 0; i < g_nmode; i++)
    if (!strcmp(g_modes[i].pkg, pkg)) {
      *out = g_modes[i].pct;
      if (out_gpu) *out_gpu = g_modes[i].gpu;
      return 1;
    }
  return 0;
}

/* 重算每个 policy 的封顶值。app_pct>0 时四档统一用它，0 表示回到全局分档。
 * assign_pct 每档都打一行日志，切应用太吵，所以这里只重算不打表。
 * tier 沿用 assign_pct 已经算好的档位归属（同一硬件上限的多个 policy
 * 要一起降，不能各算各的）。 */
static void set_caps(int app_pct, const char *why) {
  for (int i = 0; i < g_npol; i++) {
    int pct = app_pct > 0 ? app_pct : cfg.pct[g_pol[i].tier];
    if (pct < 1 || pct > 100) pct = 1;
    /* 专属档位也吃实时偏移（热了照样降），但不吃充电态那块地板 */
    pct = eff_pct(pct, app_pct > 0);
    g_pol[i].pct = pct;
    g_pol[i].cap = snap_freq(&g_pol[i], g_pol[i].hw_max * pct / 100);
  }
  if (app_pct > 0) log("  封顶重算（%s 专属档位 %d%%）", why, app_pct);
  else log("  封顶重算（%s）", why);
}

/* GPU 的生效值。只吃 PSI 借档 / 温控降档这两项偏移，不吃充电地板：
 * GPU_PCT 和应用 GPU 档都是用户明确设的上限，插电不该替他改。
 * 单独拆出来是为了自检能喂任意 delta，不必去改 adapt 的内部状态。 */
static long gpu_target_for(int base_pct, int delta) {
  int pct = (base_pct > 0 ? base_pct : cfg.gpu_pct) + delta;
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  return gpu_snap(pct);
}

static long gpu_target(void) {
  int base = g_app_gpu > 0 ? g_app_gpu : cfg.gpu_pct;
  int d = adapt_delta();
  /* 应用 GPU 档是用户明确设的，电量阶梯不往下盖它。 */
  if (g_app_gpu > 0) d += adapt_batt_cut();
  return gpu_target_for(base, d);
}

/* 把 GPU 上限写到当前目标值。force=0 时目标没变就不重复写。 */
static void apply_gpu_cap(int force) {
  if (!g_gpu.ok) return;
  long t = gpu_target();
  if (!force && t == g_gpu.active) return;
  write_gpu(t, 0);
  g_gpu.active = t;
}

/* GPU 稳态巡检：GPU Boost 会自己把上限顶回去，CPU 那条巡检管不到它。
 * 目标值和 apply_normal 用同一个 gpu_target()，温控/应用 GPU 档压下去后
 * 不会被静态 normal 顶回来。分应用档位分支和常态分支都要走这里；
 * 游戏/满血/熄屏是交权态，不能调。 */
static void gpu_patrol(void) {
  if (!g_gpu.ok) return;
  long t = gpu_target();
  long got = read_num(g_gpu.max_node);
  if (g_gpu.max_is_mhz && got > 0) got *= 1000000;
  if (got > t) write_gpu(t, 0);
  g_gpu.active = t;
  gpu_min_to_floor();
}

/* ------------------------------------------------------------------ */
/* 模式切换                                                           */
/* ------------------------------------------------------------------ */

/* 配置指纹：变了才需要重新探测分档，重启时无需 */
static uint64_t cfg_stamp(void) {
  struct stat st;
  if (stat(CFG_FILE, &st)) return 0;
  return (uint64_t)st.st_mtime * 1000000ULL + (uint64_t)st.st_size;
}

/*
 * force：配置变更或启动时为 1，无条件重写一遍。
 * 巡检逻辑是"厂商顶高了才压"，cur <= cap 就跳过；升档时新 cap 比当前值大，
 * 会落进跳过分支、节点停在旧低值，只有降档方向（cur > cap）正常写入。
 * GPU 的写入同样只在 changed 分支里。所以升档/改档时必须置 force。
 */
/* 建目录（含父级）。数据目录 /data/adb/soc_freqcap 不在解包范围内，
 * **首次运行时可能还不存在**（模块目录是解包出来的，数据目录不是），
 * 需要连父级一起建。 */
static void mkdir_p(const char *path) {
  char tmp[256];
  size_t n = strlen(path);
  if (n >= sizeof tmp) return;
  memcpy(tmp, path, n + 1);
  for (size_t i = 1; i <= n; i++) {
    if (tmp[i] == '/' || tmp[i] == 0) {
      char save = tmp[i];
      tmp[i] = 0;
      mkdir(tmp, 0700);          /* 已存在时失败无所谓，继续往下建 */
      tmp[i] = save;
    }
  }
}

/* 开机至今的秒数。用 /proc/uptime 而不是墙钟：
 * 开机阶段时间还要等 NTP 校准，date +%s 可能跳；
 * 而"开机后 60 秒"本来就是相对量，用单调的 uptime 才是原意。 */
static long uptime_sec(void) {
  char b[64];
  if (read_file("/proc/uptime", b, sizeof b)) return -1;
  double u = strtod(b, NULL);
  return u <= 0 ? -1 : (long)u;
}

/* 屏幕是否亮着。只在 SCREEN_UNCAP=1 时才被调用 ——
 * 默认关就一次 dumpsys 都不跑，不给巡检添开销。
 * 读不到（没有 dumpsys、超时、格式变了）一律当"亮着"：
 * 宁可照常限频，也不要在判断失败时把上限悄悄放开。 */
static int screen_awake(void) {
  FILE *f = popen("timeout 2 dumpsys power 2>/dev/null | grep -m1 mWakefulness", "r");
  if (!f) return 1;
  char line[256];
  int awake = 1, got = 0;
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, "Awake")) { awake = 1; got = 1; break; }
    if (strstr(line, "Asleep") || strstr(line, "Dozing")) { awake = 0; got = 1; break; }
  }
  int rc = pclose(f);
  if (!got || rc != 0) return 1;
  return awake;
}

/* 亮屏信号取值：blank 语义 0=亮、1=灭；backlight 语义 >0=亮、0=灭。
 * 返回 1=亮、0=灭、-1=读不到。 */
int screen_value_on(long v, int invert) {
  if (v < 0) return -1;
  return invert ? (v == 0) : (v > 0);
}

/* current_now 归一化成 mA。PJZ110 实测是 mA 且充电为负（与 AOSP 常见约定相反），
 * 所以这里先取绝对值；µA 的 ROM 再除以 1000。读不到由调用方传 valid=0。 */
long batt_current_ma(long raw, int unit_ma) {
  long a = raw < 0 ? -raw : raw;
  return unit_ma ? a : a / 1000;
}

/* 深度空闲纯步进。返回 0=不变、1=进入深睡、2=电流突增（让调用方做一次亮屏确认）。
 * ema 用 8 拍滑动；读不到时只清 hold，不用坏值改基线。
 * 进入条件：连续 hold_cycle 拍都低于 idle_ma。
 * 唤醒提示：比 EMA 基线高出 wake_delta（绝对阈值在低亮度深色屏上不可靠）。 */
int idle_step(long *ema, int *hold, long ma, int valid,
              int idle_ma, int hold_cycle, int wake_delta) {
  if (!valid) { *hold = 0; return 0; }
  if (*ema <= 0) { *ema = ma; *hold = 0; return 0; }
  int wake = (ma > *ema + wake_delta);
  *ema = (*ema * 7 + ma) / 8;
  if (wake) { *hold = 0; return 2; }
  if (ma < idle_ma) {
    if (++*hold >= hold_cycle) return 1;
  } else {
    *hold = 0;
  }
  return 0;
}

/* 挑一个亮屏信号：用户指定 > fb0/blank > 任意 backlight/brightness。
 * PJZ110/Android 17 上 fb0/blank 不存在，实测 backlight 0/1127 与
 * dumpsys power 的 Dozing/Awake 完全一致。 */
static void screen_state_init(void) {
  g_scr_path[0] = 0;
  g_scr_invert = 0;
  g_scr_bad = 0;
  g_scr_check = 0;
  if (cfg.screen_state_path[0]) {
    if (!access(cfg.screen_state_path, F_OK)) {
      snprintf(g_scr_path, sizeof g_scr_path, "%s", cfg.screen_state_path);
      g_scr_invert = strstr(g_scr_path, "blank") ? 1 : 0;
    } else {
      log("WARN 亮屏信号 %s 不存在，改为自动挑选", cfg.screen_state_path);
    }
  }
  if (!g_scr_path[0] && !access("/sys/class/graphics/fb0/blank", F_OK)) {
    snprintf(g_scr_path, sizeof g_scr_path, "%s", "/sys/class/graphics/fb0/blank");
    g_scr_invert = 1;
  }
  if (!g_scr_path[0]) {
    DIR *d = opendir("/sys/class/backlight");
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char t[224];
        snprintf(t, sizeof t, "/sys/class/backlight/%s/brightness", e->d_name);
        if (!access(t, F_OK)) {
          snprintf(g_scr_path, sizeof g_scr_path, "%s", t);
          g_scr_invert = 0;
          break;
        }
      }
      closedir(d);
    }
  }
  if (g_scr_path[0])
    log("亮屏信号: %s（%s）", g_scr_path,
        g_scr_invert ? "blank 0=亮" : "亮度>0=亮");
  else
    log("WARN 没有可用的亮屏信号，改用 dumpsys power 兜底");
}

static int screen_sig_read(void) {
  if (!g_scr_path[0]) return -1;
  return screen_value_on(read_num(g_scr_path), g_scr_invert);
}

/* ACTIVE 态判断是否已经熄屏。信号说灭时用 dumpsys 复核一次，
 * 防止某些 ROM 的亮度节点假归零；连续 3 次对不上就永久停用该文件。 */
static int screen_is_off_now(int force_check) {
  int sig = screen_sig_read();
  if (sig == 0) {
    if (screen_awake()) {
      g_scr_bad++;
      log("WARN 亮屏信号说灭但系统仍 Awake（%d/3）", g_scr_bad);
      if (g_scr_bad >= 3 && g_scr_path[0]) {
        g_scr_path[0] = 0;
        log("停用亮屏信号，改用 dumpsys power 兜底");
      }
      return 0;
    }
    g_scr_bad = 0;
    return 1;
  }
  if (sig < 0 && !force_check) return 0;   /* 暂时当亮屏，保守 */
  if (!force_check) return 0;
  return screen_awake() ? 0 : 1;           /* 每 6 轮兜底校验一次 */
}

/* SCREEN_OFF 态是否已经亮屏。主判据是 sysfs；读不到时每 6 轮问一次 dumpsys。 */
static int screen_wake_seen(void) {
  int sig = screen_sig_read();
  if (sig == 1) return 1;
  if (sig < 0 || ++g_scr_check >= 6) {
    g_scr_check = 0;
    return screen_awake();
  }
  return 0;
}

/* 开机满血窗口是否还在有效。service.sh 写的是"第几秒结束"（uptime 口径），
 * 到点由守护自己删文件并压回 —— 让 service.sh 去写 sysfs 不成立：
 * 它跑得很早，cpufreq 节点和厂商调度未必就绪，写失败还是静默的。 */
static int boot_boost_active(void) {
  long until = read_num(BOOT_BOOST_FILE);
  if (until <= 0) return 0;
  long up = uptime_sec();
  if (up < 0) return 0;                 /* 读不到 uptime 就当窗口不在，保守压回 */
  if (up >= until) { unlink(BOOT_BOOST_FILE); return 0; }
  return 1;
}

/* 只做 CPU 上限巡检，不碰 state/权限。apply_normal 和熄屏 keep 轻巡检共用。 */
static void caps_patrol(int force) {
  for (int i = 0; i < g_npol; i++) {
    char path[224];
    long cur;
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    cur = read_num(path);
    if (cur < 0) continue;
    /* 常态巡检：厂商把上限顶高了就压回去 */
    if (!force && cur <= g_pol[i].cap) continue;
    if (!write_journaled(NODES_JOURNAL, path, g_pol[i].cap)) {
      long got = read_num(path);
      if (force)
        log("  %-8s 上限 -> %ld%s", g_pol[i].name, got,
            got < g_pol[i].cap ? "（厂商已压更低）" : "");
      else
        log("RESTORE %s: %ld -> %ld", g_pol[i].name, cur, got);
    }
    apply_governor(&g_pol[i]);
  }
}

static void apply_normal(int force) {
  int changed = force || strcmp(g_state, "normal") != 0;
  if (changed) {
    log("→ 常态封顶");
    perms_restore(); /* 先收回游戏期交出去的写权限 */
    gpu_min_to_floor();
    apply_gpu_cap(1);
  }
  caps_patrol(changed);
  if (changed) {
    snprintf(g_state, sizeof g_state, "normal");
    snprintf(g_reason, sizeof g_reason, "-");
  }
}

/* 交权态（游戏 / 满血 / 熄屏）下不能抢回节点：游戏分支只在包名变化时
 * 重新 unlock_game，抢回来之后同一个游戏就再也交不出去，只能切出再切回。
 * 配置和偏移照常重算进内存，等真正回到常态时由 apply_normal 一次带上。 */
static int reclaim_caps_allowed(void) {
  return strcmp(g_state, "game") != 0 && strcmp(g_state, "full") != 0;
}

/*
 * 配置变了就吸收新值。返回 1 表示"这一轮要重算封顶并立刻生效"。
 * 这里**故意不**算封顶：生效值还要叠加实时偏移（插电地板 / 温控 / PSI），
 * 而偏移是主循环里 adapt_tick 之后才新鲜的。以前在这里直接 assign_pct +
 * apply_normal，用的还是上一拍的快照，真机上表现为"改一次配置先按旧地板
 * 写一遍节点、下一拍才纠正"。
 */
static int reload_if_changed(uint64_t seen) {
  uint64_t now = cfg_stamp();
  if (now == seen) return 0;
  log("检测到配置变更，重新计算封顶值");
  load_cfg();
  screen_state_init(); /* SCREEN_STATE_PATH / 策略变了要重新挑信号 */
  detect_gpu();        /* GPU 百分比变了要重新挑吸附频点 */
  return 1;
}

/* 交还系统的公共动作：值放到硬件最高 + 写权限交给厂商框架。
 * 游戏解锁、熄屏解除、开机满血窗口三处都走这里，共用一份实现，
 * 避免任一处漏掉权限交换导致厂商调度接管不了。 */
static void hand_back_full(const char *why) {
  log("→ %s", why);
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    if (!write_journaled(NODES_JOURNAL, path, snap_freq(&g_pol[i], g_pol[i].hw_max)))
      log("  %-8s -> 硬件最高 %ld", g_pol[i].name, g_pol[i].hw_max);
  }
  if (g_gpu.ok) {
    write_gpu(g_gpu.max_hz, 1);
    if (g_gpu.pwr_node[0]) write_journaled(NODES_JOURNAL, g_gpu.pwr_node, 0);
  }

  /*
   * 权限交换：值解锁之后再把写权限交给系统，厂商框架才能真正接管。
   * 只放开实际探测到的节点，不遍历全部候选路径。
   */
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    if (!access(path, F_OK)) {
      if (!journal_has(PERM_JOURNAL, path))
        journal_add(PERM_JOURNAL, path, 0, read_mode(path));
      chmod(path, 0666);
    }
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    if (!access(path, F_OK)) {
      if (!journal_has(PERM_JOURNAL, path))
        journal_add(PERM_JOURNAL, path, 0, read_mode(path));
      chmod(path, 0666);
    }
  }
  const char *nodes[] = {g_gpu.max_node, g_gpu.min_node, g_gpu.pwr_node};
  for (size_t i = 0; i < 3; i++) {
    if (!nodes[i][0] || access(nodes[i], F_OK)) continue;
    if (!journal_has(PERM_JOURNAL, nodes[i]))
      journal_add(PERM_JOURNAL, nodes[i], 0, read_mode(nodes[i]));
    chmod(nodes[i], 0666);
  }
  log("  CPU/GPU 节点写权限已交由系统接管");
  /* state 由调用方决定：游戏/满血应用是 game，熄屏交还是 full。
     hand_back_full 自己写死 game 会让熄屏状态显示成"游戏中"。 */
}

/* 游戏解锁路径的日志文案保持稳定：外部脚本按这一行判断解锁是否生效。 */
static void unlock_game(void) {
  char why[PKG_LEN + 24];
  snprintf(g_reason, sizeof g_reason, "game");
  snprintf(why, sizeof why, "游戏解锁并交还系统: %s", g_game);
  hand_back_full(why);
  snprintf(g_state, sizeof g_state, "game");
}

/* 进入熄屏轻巡检：只有常态封顶才需要交还；游戏/满血应用已经是交权态，
 * 保留原 state/reason，亮屏后不会先压一拍再重新解锁。 */
static void enter_screen_off(void) {
  if (g_screen_off) return;
  int mpct = 0;
  /* pkg=keep：只对熄屏那一刻的前台包生效，且只在常态封顶下有意义。 */
  int keep = !strcmp(g_state, "normal") && g_fg[0] &&
             mode_lookup(g_fg, &mpct, NULL) && mpct == MODE_KEEP;
  g_screen_off = 1;
  g_scr_check = 0;
  g_deep = 0;
  g_idle_ema = 0;
  g_idle_hold = 0;
  g_soff_keep = keep;
  snprintf(g_soff_pkg, sizeof g_soff_pkg, "%s", keep ? g_fg : "");
  g_app_capped[0] = 0;
  g_fg_global[0] = 0;
  if (keep) {
    /* 亮屏时 keep 不动分档；熄屏时把可能残留的应用档位收回全局，再做轻巡检。 */
    if (g_app_pct || g_app_gpu) {
      set_caps(0, "熄屏 keep");
      g_app_pct = 0;
      g_app_gpu = 0;
      apply_gpu_cap(1);
    }
    g_app[0] = 0;
    caps_patrol(0);
    gpu_patrol();
    log("进入熄屏轻巡检（%d s，保持封顶：%s）", cfg.soff_interval, g_fg);
    return;
  }
  if (!strcmp(g_state, "normal")) {
    g_app[0] = 0;
    g_app_pct = 0;
    g_app_gpu = 0;
    hand_back_full("熄屏解除限频，交还系统");
    snprintf(g_state, sizeof g_state, "full");
    snprintf(g_reason, sizeof g_reason, "screen");
  }
  log("进入熄屏轻巡检（%d s，不查前台/不采样）", cfg.soff_interval);
}

static void leave_screen_off(void) {
  if (!g_screen_off) return;
  g_screen_off = 0;
  g_scr_check = 0;
  g_deep = 0;
  g_idle_ema = 0;
  g_idle_hold = 0;
  g_soff_keep = 0;
  g_soff_pkg[0] = 0;
  log("亮屏，恢复常态巡检");
  /* 熄屏交还的才需要压回；游戏态/keep 态保持原样，交给主循环重新判定。 */
  if (!strcmp(g_state, "full")) {
    g_app[0] = 0;
    g_app_pct = 0;
    g_app_gpu = 0;
    apply_normal(1);
  }
}

/* ------------------------------------------------------------------ */
/* 配置解析：坏值一律回落到默认值，绝不让脏配置生效                   */
/* ------------------------------------------------------------------ */

static int cfg_get(const char *key, char *out) {
  FILE *f = fopen(CFG_FILE, "r");
  if (!f) return -1;
  char line[512], k[128];
  int found = -1;
  while (fgets(line, sizeof line, f)) {
    const char *s = line;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '#' || !*s) continue;
    if (sscanf(s, "%127[^= \t]=%511[^\n]", k, out) == 2 && !strcmp(k, key)) {
      found = 0;
      break;
    }
  }
  fclose(f);
  return found;
}

static int clampi(int v, int lo, int hi, int dflt) {
  return (v < lo || v > hi) ? dflt : v;
}

static void load_cfg(void) {
  static const char *keys[4] = {"CPU_PCT_BIG", "CPU_PCT_MID2", "CPU_PCT_MID",
                                "CPU_PCT_LITTLE"};
  static const int dflt[4] = {65, 64, 66, 65};
  char v[512];
  cfg.interval = 5;
  cfg.grace = 10;
  cfg.gpu_pct = 45;
  cfg.gpu_min_lock = 1;
  /* 熄屏策略：基准 keep（旧行为）。全新安装的 conf 写 uncap；
     升级用户没有 SCREEN_POLICY 时按旧 SCREEN_UNCAP 保持现状；
     两个键都没有的手写/极老配置按新装默认 uncap（见下面的映射）。 */
  cfg.screen_uncap = 0;
  cfg.screen_policy = SCREEN_KEEP;
  cfg.soff_interval = 15;
  cfg.screen_state_path[0] = 0;
  cfg.idle_current_ma = 250;
  cfg.idle_hold_cycle = 2;
  cfg.idle_interval = 30;
  cfg.wake_delta_ma = 150;
  snprintf(cfg.batt_current_path, sizeof cfg.batt_current_path,
           "/sys/class/power_supply/battery/current_now");
  cfg.batt_current_ma_unit = 0;
  cfg.boot_full_s = 60;
  cfg.learn = 1;
  cfg.learn_app = 0;
  /* 实时偏移的默认值。三条都默认开：装了模块的人要的就是"该压住、该让路时
     也让得出去"。每个来源各有上限，合计再有一对总闸（默认 +15 / -30），
     避免"每一项都合法、叠起来不可用"。 */
  cfg.ad.psi = 1;   cfg.ad.psi_gain = 10;  cfg.ad.psi_step = 5;
  cfg.ad.psi_hold = 30;  cfg.ad.psi_back = 15;
  cfg.ad.th = 1;    cfg.ad.th_high = 75;   cfg.ad.th_low = 68;
  cfg.ad.th_step = 5;   cfg.ad.th_max = 25;  cfg.ad.th_back = 20;
  cfg.ad.chg = 1;   cfg.ad.chg_pct = 100;
  cfg.ad.up_max = 15;    cfg.ad.down_max = 30;
  cfg.ad.batt = 0;  cfg.ad.batt_low = 20;  cfg.ad.batt_low_cut = 5;
  cfg.ad.batt_crit = 10; cfg.ad.batt_crit_cut = 10;
  for (int i = 0; i < 4; i++) cfg.pct[i] = dflt[i];

  if (cfg_get("INTERVAL", v) == 0) cfg.interval = clampi(atoi(v), 2, 60, 5);
  {
    int uncap_seen = 0;
    if (cfg_get("SCREEN_UNCAP", v) == 0) {
      cfg.screen_uncap = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? 1 : 0;
      uncap_seen = 1;
    }
    /* SCREEN_POLICY 优先；没有这个键时，旧 SCREEN_UNCAP=1 映射成 uncap。
       两个键都没有（手写/极老配置）按新装默认 uncap —— 单独把
       screen_uncap 的初值改成 1 不安全：显式 SCREEN_POLICY=keep 又没写
       SCREEN_UNCAP 的配置会因此走进上面那条旧的熄屏解锁分支。 */
    {
      int policy_seen = 0;
      if (cfg_get("SCREEN_POLICY", v) == 0) {
        if (!strcasecmp(v, "uncap")) { cfg.screen_policy = SCREEN_UNCAP; policy_seen = 1; }
        else if (!strcasecmp(v, "keep")) { cfg.screen_policy = SCREEN_KEEP; policy_seen = 1; }
      }
      if (!policy_seen)
        cfg.screen_policy = !uncap_seen ? SCREEN_UNCAP
                          : (cfg.screen_uncap ? SCREEN_UNCAP : SCREEN_KEEP);
    }
  }
  if (cfg_get("SOFF_INTERVAL", v) == 0)
    cfg.soff_interval = clampi(atoi(v), 5, 300, 15);
  if (cfg_get("SCREEN_STATE_PATH", v) == 0 && v[0] == '/') {
    char *e = v + strlen(v);
    while (e > v && isspace((unsigned char)e[-1])) *--e = 0;
    if (strlen(v) < sizeof cfg.screen_state_path)
      snprintf(cfg.screen_state_path, sizeof cfg.screen_state_path, "%s", v);
  }
  if (cfg_get("IDLE_CURRENT_MA", v) == 0) cfg.idle_current_ma = clampi(atoi(v), 20, 2000, 250);
  if (cfg_get("IDLE_HOLD_CYCLE", v) == 0) cfg.idle_hold_cycle = clampi(atoi(v), 1, 10, 2);
  if (cfg_get("IDLE_INTERVAL", v) == 0) cfg.idle_interval = clampi(atoi(v), 10, 300, 30);
  if (cfg_get("WAKE_DELTA_MA", v) == 0) cfg.wake_delta_ma = clampi(atoi(v), 20, 2000, 150);
  if (cfg_get("BATT_CURRENT_PATH", v) == 0 && v[0] == '/') {
    char *e = v + strlen(v);
    while (e > v && isspace((unsigned char)e[-1])) *--e = 0;
    if (strlen(v) < sizeof cfg.batt_current_path)
      snprintf(cfg.batt_current_path, sizeof cfg.batt_current_path, "%s", v);
  }
  if (cfg_get("BATT_CURRENT_UNIT", v) == 0)
    cfg.batt_current_ma_unit = !strcasecmp(v, "ma");
  if (cfg_get("LEARN", v) == 0)
    cfg.learn = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  /* 按应用学习默认关，所以判据和上面那条相反：只认显式的 1/true。
     它只在"这个应用已经有专属数字档位"时才攒得出样本，是给愿意细调的人用的。 */
  if (cfg_get("LEARN_APP", v) == 0)
    cfg.learn_app = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? 1 : 0;
  /* 开机满血窗口上限 600s：再长就不是"开机提速"而是"忘了限频"了，
     而且窗口越长，越容易盖过厂商开机的热管理。 */
  if (cfg_get("BOOT_FULL_S", v) == 0)
    cfg.boot_full_s = clampi(atoi(v), 0, 600, 60);
  if (cfg_get("GAME_GRACE_SEC", v) == 0)
    cfg.grace = clampi(atoi(v), cfg.interval, 300, 10);
  if (cfg_get("GPU_PCT", v) == 0)
    cfg.gpu_pct = clampi(atoi(v), 1, 100, 45);
  if (cfg_get("GPU_MIN_LOCK", v) == 0)
    cfg.gpu_min_lock = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("CPU_GOVERNOR", v) == 0) {
    trim(v);
    /* 只接受白名单内的名字，避免把任意字符串写进 sysfs */
    cfg.governor[0] = 0;
    if (v[0] && !strpbrk(v, "/ \t;|&$`\\()")) {
      for (char *c = v; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_') {
          v[0] = 0;
          break;
        }
      if (v[0]) snprintf(cfg.governor, sizeof cfg.governor, "%s", v);
    }
  }
  for (int i = 0; i < 4; i++)
    if (cfg_get(keys[i], v) == 0)
      cfg.pct[i] = clampi(atoi(v), 1, 100, dflt[i]);

  /* 实时偏移。区间故意收窄：这些数直接决定"最坏能比设定值宽/严多少"，
     放开了填会让模块看起来"没生效"或"压太狠"。 */
  if (cfg_get("PSI_ADAPT", v) == 0)
    cfg.ad.psi = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("PSI_GAIN", v) == 0) cfg.ad.psi_gain = clampi(atoi(v), 0, 30, 10);
  if (cfg_get("PSI_STEP", v) == 0) cfg.ad.psi_step = clampi(atoi(v), 1, 10, 5);
  if (cfg_get("PSI_HOLD", v) == 0) cfg.ad.psi_hold = clampi(atoi(v), 5, 600, 30);
  if (cfg_get("PSI_BACK", v) == 0) cfg.ad.psi_back = clampi(atoi(v), 5, 600, 15);
  if (cfg_get("TH_CAP", v) == 0)
    cfg.ad.th = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("TH_HIGH", v) == 0) cfg.ad.th_high = clampi(atoi(v), 35, 110, 75);
  if (cfg_get("TH_LOW", v) == 0)  cfg.ad.th_low = clampi(atoi(v), 30, 105, 68);
  if (cfg_get("TH_STEP", v) == 0) cfg.ad.th_step = clampi(atoi(v), 1, 10, 5);
  if (cfg_get("TH_MAX", v) == 0)  cfg.ad.th_max = clampi(atoi(v), 0, 60, 25);
  if (cfg_get("TH_BACK", v) == 0) cfg.ad.th_back = clampi(atoi(v), 5, 600, 20);
  if (cfg_get("CHG_UNCAP", v) == 0)
    cfg.ad.chg = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("CHG_PCT", v) == 0) cfg.ad.chg_pct = clampi(atoi(v), 20, 100, 100);
  if (cfg_get("ADAPT_UP_MAX", v) == 0) cfg.ad.up_max = clampi(atoi(v), 0, 100, 15);
  if (cfg_get("ADAPT_DOWN_MAX", v) == 0) cfg.ad.down_max = clampi(atoi(v), 0, 100, 30);
  if (cfg_get("BATT_STEP", v) == 0)
    cfg.ad.batt = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? 1 : 0;
  if (cfg_get("BATT_LOW", v) == 0) cfg.ad.batt_low = clampi(atoi(v), 5, 95, 20);
  if (cfg_get("BATT_LOW_CUT", v) == 0) cfg.ad.batt_low_cut = clampi(atoi(v), 0, 30, 5);
  if (cfg_get("BATT_CRIT", v) == 0) cfg.ad.batt_crit = clampi(atoi(v), 1, 95, 10);
  if (cfg_get("BATT_CRIT_CUT", v) == 0) cfg.ad.batt_crit_cut = clampi(atoi(v), 0, 30, 10);
  /* 两档必须错开，否则 10% 以下会同时叠两刀、数值不可预期。 */
  if (cfg.ad.batt_crit >= cfg.ad.batt_low) {
    cfg.ad.batt_crit = cfg.ad.batt_low - 5;
    if (cfg.ad.batt_crit < 1) cfg.ad.batt_crit = 1;
  }
  /* 恢复线必须低于触发线，否则温度在阈值附近时会逐档来回抖。
     填反了就贴着触发线往下留 5 度，而不是直接关掉温控。 */
  if (cfg.ad.th_low >= cfg.ad.th_high) cfg.ad.th_low = cfg.ad.th_high - 5;
  if (cfg.ad.th_low < 30) cfg.ad.th_low = 30;
}

/* ------------------------------------------------------------------ */
/* 状态发布                                                           */
/* ------------------------------------------------------------------ */

static void publish_status(void) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp", STATUS_FILE);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  /* 生效中的应用档位。"" 表示没有专属档位，走全局百分比。 */
  char mode[16], gmode[8];
  if (g_app_pct == MODE_FULL) snprintf(mode, sizeof mode, "full");
  else if (g_app_pct > 0) snprintf(mode, sizeof mode, "%d", g_app_pct);
  else mode[0] = 0;
  /* 空 = GPU 跟随全局；和 app_mode 分开报，前端不用去拆 70:40 这种串 */
  if (g_app_gpu > 0) snprintf(gmode, sizeof gmode, "%d", g_app_gpu);
  else gmode[0] = 0;
  /* fg 是"此刻前台是谁"，game_pkg 是"命中名单的那个"。
   * 两个必须分开：页面上"把当前前台应用设为满血"要的是 fg，
   * 而它大部分时候根本不是游戏（相机、地图、播放器）。
   * 只报 game_pkg 的话，非游戏态那半永远是空，按钮就永远点不动。 */
  fprintf(f, "state=%s\nscreen=%s\nsoff=%d\nsoff_keep=%d\ngame_pkg=%s\nreason=%s\n"
             "fg=%s\napp_mode=%s\napp_gpu=%s\npid=%d\nver=%s\n",
          g_state, g_deep ? "deep" : (g_screen_off ? "off" : "on"),
          g_deep ? cfg.idle_interval : cfg.soff_interval, g_soff_keep,
          g_game[0] ? g_game : "-", g_reason,
          g_fg[0] ? g_fg : "-", mode, gmode, getpid(), SF_VERSION);
  /* 实时偏移的三个来源一起发出去：页面上"实际比设定宽/严了多少"必须说得出是谁干的，
   * 只剩一个合计数就等于让人猜。读不到的传感器发 -1，页面据此不显示那一项。 */
  {
    struct adapt a;
    adapt_snapshot(&a);
    fprintf(f, "dlt=%d\nbatt=%d\nbcut=%d\nstall=%d\npsib=%d\navg10=%d\ntemp=%d\n"
               "chg=%d\npsi=%d\nth=%d\n",
            a.delta, a.capacity, a.batt_cut,
            a.have_psi ? a.stall : -1, a.base, a.avg10x100,
            a.temp_m > 0 ? a.temp_m / 1000 : -1, a.charging, a.psi_steps, a.th_steps);
  }
  fclose(f);
  rename(tmp, STATUS_FILE);
}

/* ------------------------------------------------------------------ */
/* 清理                                                               */
/* ------------------------------------------------------------------ */

static void cleanup(void) {
  if (!g_have_lock) return;
  log("停止，恢复启动前状态");
  learn_flush();
  restore_governors();
  /* 顺序：先还原数值，再收权限。反了会写不进需要放开才能写的节点 */
  if (g_gpu.mtk) mtk_restore_row();
  journal_restore(NODES_JOURNAL);
  perms_restore();
  unlink(STATE_FILE);
  unlink(STATUS_FILE);
  unlink(HEARTBEAT_FILE);
  unlink(LOCK_FILE);
  g_have_lock = 0;
}

static void on_signal(int sig) {
  (void)sig;
  /*
   * 信号处理里只置标志：恢复要写 sysfs，绝不能在这里做。
   * 标志在 sleep() 结束后才被看到，最长延迟一个巡检周期。
   */
  g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

/*
 * pid 是否确实是本守护。
 * pid 会被系统回收给别的进程，只 kill(pid,0) 不够——
 * 那样一次陈旧的锁文件就能让 stop/restart 打死一个无辜进程。
 */
static int pid_is_daemon(long pid) {
  if (pid <= 1) return 0;
  if (kill((pid_t)pid, 0)) return 0; /* 不存在 */
  char path[64], name[32];
  snprintf(path, sizeof path, "/proc/%ld/comm", pid);
  if (read_file(path, name, sizeof name)) return 0; /* 读不到就不信 */
  trim(name);
  return !strncmp(name, "socfreqd", 8);
}

/*
 * 守护是否真的在跑。
 * 不能只看锁文件在不在：被 kill -9 时不会执行 cleanup，锁文件会残留，
 * 那样看门狗/WebUI 会永远认为"有人在跑"。判据必须是 pid 还活着。
 */
static int daemon_alive(void) {
  char b[32];
  long pid;
  if (read_file(LOCK_FILE, b, sizeof b)) return 0;
  pid = atol(b);
  if (pid <= 1) return 0;
  /* kill(pid,0) 返回 0 表示进程存在；EPERM 表示存在但无权限，同样算活着 */
  if (kill((pid_t)pid, 0)) return 0;
  return 1;
}

static int acquire_lock(void) {
  /*
   * 残留锁自愈：上一实例被 kill -9 时不会走到 cleanup，锁文件会留下。
   * flock 已经随进程退出发放，所以拿得到锁；但要让内容反映真实 pid，
   * 否则 service.sh 与 WebUI 会读到陈旧 pid 而误判。
   */
  int stale = !daemon_alive();
  int fd = open(LOCK_FILE, O_RDWR | O_CREAT, 0644);
  if (fd < 0) return -1;
  if (flock(fd, LOCK_EX | LOCK_NB)) {
    close(fd);
    return -1;
  }
  if (!ftruncate(fd, 0)) {
    char b[32];
    int n = snprintf(b, sizeof b, "%d\n", getpid());
    ssize_t w = write(fd, b, n);
    (void)w;
    if (stale) log("清理残留锁文件（上次非正常退出）");
  }
  g_have_lock = 1;
  return 0; /* fd 故意不关：flock 随进程退出自动释放 */
}

/* 定义 SF_TEST 时不编 main，直接包含测试文件跑内部函数。 */
#ifdef SF_TEST
#include "test_real.c"
#else
/* 逐核状态。update=1 时把这一拍的增量推进 5 秒环（WebUI 每秒调 cores）；
 * update=0 只读窗口快照（export 每 5 秒被 status 调一次，不能再喂样本，
 * 否则 5 秒的大增量会把 1 秒的小样本挤掉）。ring 为空时先初始化一次。 */
static void dump_cores(int update) {
  core_state_lock();
  core_state_load();
  int dirty = 0;
  long st_total[MAX_CORES], st_idle[MAX_CORES];
  memset(st_total, 0, sizeof st_total);
  memset(st_idle, 0, sizeof st_idle);
  int have_stat = core_stat_read(st_total, st_idle) > 0;
  int cores[MAX_CORES];
  int nc = 0;
  for (int c = 0; c < MAX_CORES; c++)
    if (core_exists(c)) cores[nc++] = c;
  printf("ncores=%d\n", nc);
  for (int i = 0; i < nc; i++) {
    const int c = cores[i];
    char b[96];
    core_path(b, sizeof b, c, "cpuinfo_cur_freq");
    long cur = read_num(b);
    if (cur <= 0) {
      core_path(b, sizeof b, c, "scaling_cur_freq");
      cur = read_num(b);
    }
    printf("corehz_%d=%ld\n", c, cur > 0 ? cur : 0);

    core_path(b, sizeof b, c, "cpuinfo_max_freq");
    long maxf = read_num(b);
    printf("coremax_%d=%ld\n", c, maxf > 0 ? maxf : 0);

    core_path(b, sizeof b, c, "cpuinfo_min_freq");
    long minf = read_num(b);
    printf("coremin_%d=%ld\n", c, minf > 0 ? minf : 0);

    core_path(b, sizeof b, c, "scaling_max_freq");
    long tgt = read_num(b);
    if (tgt <= 0) tgt = maxf;
    printf("coretgt_%d=%ld\n", c, tgt > 0 ? tgt : 0);

    printf("corepol_%d=%d\n", c, core_policy_of(c));

    struct core_hist *h = &g_ch[c];
    int load = 0;
    if (update) {
      if (have_stat && h->have_prev)
        load = core_load_pct(st_total[c], st_idle[c], h->prev_total, h->prev_idle);
      core_hist_push(h, cur, load);
      if (have_stat) {
        h->prev_total = st_total[c];
        h->prev_idle = st_idle[c];
        h->have_prev = 1;
      }
      dirty = 1;
    } else if (h->n > 0) {
      load = (int)h->load[h->n - 1];
    }

    long lo = 0, hi = 0;
    core_hist_range(h, &lo, &hi);
    if (lo <= 0 && hi <= 0 && cur > 0) { lo = cur; hi = cur; }
    printf("coreload_%d=%d\n", c, load);
    printf("corefreqs_%d=", c);
    for (int k = 0; k < h->n; k++) printf("%ld%s", h->freq[k], k + 1 < h->n ? "," : "");
    printf("\n");
    printf("coreloads_%d=", c);
    for (int k = 0; k < h->n; k++) printf("%ld%s", h->load[k], k + 1 < h->n ? "," : "");
    printf("\n");
    printf("coreobsmin_%d=%ld\n", c, lo);
    printf("coreobsmax_%d=%ld\n", c, hi);
  }
  printf("coredet=");
  for (int i = 0; i < nc; i++) printf("%d%s", cores[i], i + 1 < nc ? "," : "");
  printf("\n");
  if (dirty) core_state_save();
  core_state_unlock();
}

int main(int argc, char **argv) {
  ovr_load();   /* 适配覆盖：只给后面"探测不到"那条路用，正常机器读不到这文件 */
  const char *cmd = argc > 1 ? argv[1] : "start";

  if (!strcmp(cmd, "version")) {
    printf("socfreqd %s\n", SF_VERSION);
    return 0;
  }
  if (!strcmp(cmd, "status")) {
    char b[256];
    read_file(STATUS_FILE, b, sizeof b);
    printf("%s", b[0] ? b : "state=unknown\n");
    return 0;
  }
  if (!strcmp(cmd, "uninstall")) {
    /*
     * 卸载：先停掉在跑的实例（它自己会恢复），再强制按 journal 兜底。
     * 守护被 kill -9 或看门狗没拉起时，这是唯一的清理路径。
     */
    char b[32];
    long pid = 0;
    if (!read_file(LOCK_FILE, b, sizeof b)) pid = atol(b);
    if (pid_is_daemon(pid) && pid != getpid()) {
      kill((pid_t)pid, SIGTERM);
      for (int i = 0; i < 50; i++) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        if (kill((pid_t)pid, 0)) break;
      }
      if (!kill((pid_t)pid, 0)) {
        kill((pid_t)pid, SIGKILL);
        nanosleep(&(struct timespec){0, 200000000}, NULL);
      }
    }
    log_open();
    log("卸载清理 pid=%d", getpid());
    g_have_lock = 1; /* 让 cleanup() 执行恢复 */
    cleanup();
    /*
     * 补一道兜底：只修"谁都写不动"的情况。
     * 不能无条件压成 0644 —— 出厂就是 0664/0666 的机型（system 组需要写）
     * 会被改得比原厂更严，反而弄坏厂商调度。
     */
    DIR *d = opendir(CPUFREQ);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        char path[256];
        if (strncmp(e->d_name, "policy", 6)) continue;
        snprintf(path, sizeof path, "%s/%s/scaling_max_freq", CPUFREQ, e->d_name);
        if ((read_mode(path) & 0222) == 0) chmod(path, 0644);
        snprintf(path, sizeof path, "%s/%s/scaling_governor", CPUFREQ, e->d_name);
        if ((read_mode(path) & 0222) == 0) chmod(path, 0644);
      }
      closedir(d);
    }
    unlink(PAUSE_FILE);
    printf("cleaned\n");
    return 0;
  }
  if (!strcmp(cmd, "stop") || !strcmp(cmd, "restart")) {
    /*
     * 只对确实持有 flock 的进程下手：读锁文件里的 pid，确认它还活着再发信号。
     * 拿不到锁说明没在跑，省掉一次无谓的系统调用。
     */
    char b[32];
    long pid = 0;
    if (!read_file(LOCK_FILE, b, sizeof b)) pid = atol(b);
    if (pid_is_daemon(pid) && pid != getpid()) {
      kill((pid_t)pid, SIGTERM);
      printf("sent SIGTERM to pid %ld\n", pid);
    } else {
      printf("not running\n");
    }
    if (!strcmp(cmd, "stop")) {
      /*
       * 先落 pause，否则看门狗 30 秒后就把守护复活了 ——
       * "停止"必须真的是停住。
       */
      int pfd = open(PAUSE_FILE, O_CREAT | O_WRONLY, 0644);
      if (pfd >= 0) close(pfd);
      printf("paused and stopped\n");
      return 0;
    }
    /* restart：必须清掉 pause，否则 start 侧的守卫会静默拒绝 */
    unlink(PAUSE_FILE);
    /* restart：等旧实例退出（它退出时要把节点恢复回原值）再接手，最多 5 秒 */
    if (pid > 1) {
      for (int i = 0; i < 50; i++) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
        if (!pid_is_daemon(pid)) break;
      }
    }
    /*
     * 守护循环是前台常驻的，所以必须交给子进程去跑，不能在这里 exec 自己：
     * 那样"重启守护"这条命令永远不返回，调它的 WebUI 请求、action.sh、
     * 适配包导入回执全被吊住，管理器那头只会看到超时。
     * setsid 让它脱离调用者的进程组——管理器掐掉请求进程组时不该顺带杀掉守护。
     */
    pid_t f = fork();
    if (f < 0) { perror("fork"); return 1; }
    if (f == 0) {
      setsid();
      int fd = open("/dev/null", O_RDWR);
      if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
      }
      execl(SF_MODDIR "/socfreqd", "socfreqd", "start", (char *)NULL);
      _exit(127);
    }
    printf("restart issued\n");
    return 0;
  }
  if (!strcmp(cmd, "export")) {
    /*
     * 给 WebUI 用的只读快照，输出 key=value 行。
     * 走 stdout 而不是一个 webroot 临时文件：省一次临时文件往返，
     * 也避免 WebUI 把文件写进模块目录。
     */
    load_cfg();
    struct policy p[MAX_POLICY];
    int n = detect_policies(p);
    assign_pct(p, n); /* 让 WebUI 显示的是当前百分比算出的封顶值 */
    /* 不加载的话 npkg 恒为 0，WebUI 的"游戏名单"计数就是错的 */
    load_game_pkgs();
    load_app_modes();
    printf("ver=%s\n", SF_VERSION);
    printf("nmode=%d\n", g_nmode);
    printf("alive=%d\n", daemon_alive());
    printf("paused=%d\n", !access(PAUSE_FILE, F_OK));
    printf("interval=%d\n", cfg.interval);
    printf("grace=%d\n", cfg.grace);
    printf("pct_big=%d\n", cfg.pct[0]);
    printf("pct_mid2=%d\n", cfg.pct[1]);
    printf("pct_mid=%d\n", cfg.pct[2]);
    printf("pct_little=%d\n", cfg.pct[3]);
    printf("gpu_pct=%d\n", cfg.gpu_pct);
    printf("gpu_min_lock=%d\n", cfg.gpu_min_lock);
    printf("screen_uncap=%d\n", cfg.screen_uncap);
    printf("screen_policy=%s\n", cfg.screen_policy == SCREEN_UNCAP ? "uncap" : "keep");
    printf("soff_interval=%d\n", cfg.soff_interval);
    printf("idle_interval=%d\nidle_current_ma=%d\nidle_hold=%d\nwake_delta=%d\n",
           cfg.idle_interval, cfg.idle_current_ma, cfg.idle_hold_cycle, cfg.wake_delta_ma);
    printf("batt_unit=%s\n", cfg.batt_current_ma_unit ? "ma" : "ua");
    printf("boot_full=%d\n", cfg.boot_full_s);
    printf("learn=%d\n", cfg.learn);
    /* 实时偏移：页面配置页要能显示、能改，所以和上面这些一样原样发出去 */
    printf("psi_adapt=%d\npsi_gain=%d\npsi_step=%d\n", cfg.ad.psi, cfg.ad.psi_gain, cfg.ad.psi_step);
    printf("th_cap=%d\nth_high=%d\nth_low=%d\nth_step=%d\nth_max=%d\n",
           cfg.ad.th, cfg.ad.th_high, cfg.ad.th_low, cfg.ad.th_step, cfg.ad.th_max);
    printf("chg_uncap=%d\nchg_pct=%d\n", cfg.ad.chg, cfg.ad.chg_pct);
    printf("governor=%s\n", cfg.governor[0] ? cfg.governor : "");
    printf("npkg=%d\n", g_npkg);
    char soc[128];
    read_soc_name(soc, sizeof soc);
    printf("soc=%s\n", soc);

    dump_cores(0);

    for (int i = 0; i < n; i++) {
      char path[256];
      long cur;
      snprintf(path, sizeof path, "%s/scaling_max_freq", p[i].dir);
      cur = read_num(path); /* 失败返回 -1，下面单独判 */
      char vbuf[64] = "0";
      if (cur > 0) snprintf(vbuf, sizeof vbuf, "%ld", cur);
      /* cpu_N=硬件最高频，cpucur_N=当前实际封顶。
       * WebUI 要拿这两个算"66% 到底是多少 MHz"，只给一个不够。 */
      printf("cpu_%s=%ld\n", p[i].name + 6, p[i].hw_max);
      printf("cpucur_%s=%s\n", p[i].name + 6, vbuf);
      printf("cpucap_%s=%ld\n", p[i].name + 6, p[i].cap);
      printf("cpupct_%s=%ld\n", p[i].name + 6, p[i].pct);
      /* 档位 → 配置键名。WebUI 拖滑杆时要知道自己改的是 BIG 还是 MID2，
       * 不给的话它只能反推，两个 policy 同百分比时会分错。 */
      static const char *tier_names[4] = {"CPU_PCT_BIG", "CPU_PCT_MID2",
                                          "CPU_PCT_MID", "CPU_PCT_LITTLE"};
      printf("cpukey_%s=%s\n", p[i].name + 6,
             tier_names[p[i].tier & 3]);
      /* 频点表原样导出去，让前端做和守护 snap_freq 一样的吸附，
       * 避免前端草稿态直接乘百分比得到的显示值和守护实际吸附值对不上。 */
      {
        char fb[4096], fpath[256];
        snprintf(fpath, sizeof fpath, "%s/scaling_available_frequencies", p[i].dir);
        if (read_file(fpath, fb, sizeof fb))
          printf("policyfreq_%s=%s\n", p[i].name + 6, fb);
      }
    }

    /* ── 每个档位具体管着哪几颗核 ──────────────────────────────────
     * cpukey_N 的 N 是 **policy 号**不是核心号，一个 policy 可管多颗核心，
     * 这里按档位反查它名下的所有 CPU 号，逐档报一行：
     *   coretier_CPU_PCT_BIG=6,7
     * 缺失的档位**也要报一行、值留空**（coretier_BIG= 而不是报 -1）：
     * 前端靠"这个键在不在且非空"判断有没有这一档，从而区分"未检测到"。
     */
    {
      const char *keys[4] = {"CPU_PCT_BIG", "CPU_PCT_MID2",
                              "CPU_PCT_MID", "CPU_PCT_LITTLE"};
      /* 阶梯要用**未去重**的 policy 列表算，否则同一硬件上限的多个
       * policy 只剩一个，另一个管的核心整档丢失。 */
      struct policy all[MAX_POLICY];
      int na = detect_policies_raw(all);
      long ladder[MAX_POLICY];
      int m = build_ladder(all, na, ladder);
      for (int t = 0; t < 4; t++) {
        int ncpu = 0;
        /* 缺档也要报一行、值留空：前端靠"这个键在不在且非空"判断
         * 有没有这一档，从而区分未落到的档和读不出来的情况。 */
        printf("coretier_%s=", keys[t]);
        for (int i = 0; i < na; i++) {
          if (tier_key_of(ladder, m, all[i].hw_max) != t) continue;
          for (int c = 0; c < MAX_CORES; c++) {
            if (!core_exists(c)) continue;
            if (core_policy_of(c) != all[i].pol_no) continue;
            printf("%s%d", ncpu ? "," : "", c);
            ncpu++;
          }
        }
        printf("\n");
      }
      /* 这台机实际有几档。前端靠它区分
       * "这台机没这一档"（m=2 时 MID/MID2 恒定落空）
       * 和"这一档在但没读到核心"。
       * 报的是**被用掉的档位数**，不是 m：
       * m=2 时 tier_key_of 只会返回 0 和 3，实际用掉 2 档。 */
      int used[4] = {0, 0, 0, 0};
      for (int i = 0; i < na; i++) used[tier_key_of(ladder, m, all[i].hw_max)] = 1;
      int nt = 0;
      for (int t = 0; t < 4; t++) nt += used[t];
      printf("ntier=%d\n", nt);
    }
    /* GPU 同样要硬件最高与当前封顶，否则 45% 对用户没意义 */
    detect_gpu();
    /*
     * export 是独立进程，g_gpu.active 永远是 0，不能直接报它。
     * 运行中的守护每轮把生效值的前提（state / app_gpu / dlt）写进 status，
     * 这里按同一张频点表重算；status 不存在（守护没跑）才退回设定值。
     * 交权态（游戏/满血/熄屏）GPU 是解开的，报硬件最高。
     */
    long gcap = g_gpu.normal;
    int gpu_dlt = 0;
    {
      FILE *sf = fopen(STATUS_FILE, "r");
      if (sf) {
        char ln[128];
        int state_seen = 0, normal = 0, agpu = 0, dlt = 0, bcut = 0;
        while (fgets(ln, sizeof ln, sf)) {
          if (!strncmp(ln, "state=", 6)) {
            state_seen = 1;
            normal = !strncmp(ln + 6, "normal", 6);
          } else if (!strncmp(ln, "app_gpu=", 8)) {
            agpu = atoi(ln + 8);
          } else if (!strncmp(ln, "dlt=", 4)) {
            dlt = atoi(ln + 4);
          } else if (!strncmp(ln, "bcut=", 5)) {
            bcut = atoi(ln + 5);
          }
        }
        fclose(sf);
        /* 草稿预览要按同一个偏移算，否则滑杆显示 50%→600、保存后却变成
         * 偏移后的 282，看起来像守护没按百分比走。 */
        gpu_dlt = dlt;
        if (state_seen && !normal) gcap = g_gpu.max_hz;
        else if (state_seen) {
          /* 应用 GPU 档不吃电量阶梯，和 CPU 专属档同一个口径。 */
          if (agpu > 0) dlt += bcut;
          gcap = gpu_target_for(agpu > 0 ? agpu : cfg.gpu_pct, dlt);
        }
      }
    }
    printf("gpu_max=%ld\n", g_gpu.max_hz);
    printf("gpu_cap=%ld\n", gcap);
    printf("gpu_dlt=%d\n", gpu_dlt);
    if (g_gpu.ok && g_gpu.ntbl > 0) {
      printf("gpufreq=");
      for (int i = 0; i < g_gpu.ntbl; i++)
        printf("%ld%s", g_gpu.tbl[i], i + 1 < g_gpu.ntbl ? " " : "");
      printf("\n");
    }
    printf("gpu_ok=%d\n", g_gpu.ok);
    /* max_hz / normal 恒为 Hz：detect_gpu 里 MHz 频点表已经 ×1e6 归一。
       不能用 max_is_mhz 来判断这两个数——那个标志说的是"往哪个节点写"时
       要不要 ×1e6，是写入单位，跟这两个值无关。混用会让选 MHz 节点的
       机型显示成 1200000 MHz，差 1000 倍。 */
    printf("gpu_unit=Hz\n");
    printf("gpu_kind=%s\n", g_gpu.mtk ? "MTK"
                              : strstr(g_gpu.max_node, "kgsl") ? "KGSL"
                              : strstr(g_gpu.max_node, "mali") ? "Mali" : "");
    return 0;
  }
  if (!strcmp(cmd, "cores")) {
    /* WebUI 每秒调一次：只读逐核频率/分布，不重新解析配置和温控。 */
    dump_cores(1);
    return 0;
  }
  if (!strcmp(cmd, "icon")) {
    /* 从 APK 取图标，输出 data URI。取不到就什么都不输出——
       页面据此退回首字母色块。名单里大部分包在手机上根本没装，
       这是常态而不是错误状态，报错会让整个名单页变成一个错误提示。
       argv[3] 可选：appinfo 已经确定该用哪张图时直接告诉它，
       省掉"猜"那一轮（资源名被缩短过的包靠猜会猜错，见 appinfo.c 头）。 */
    if (argc > 2) { icon_from_apk(argv[2], argc > 3 ? argv[3] : NULL); return 0; }
    return 0;
  }
  if (!strcmp(cmd, "iconpath")) {
    /* manifest 的 android:icon 声明的那张**位图**在 zip 里的路径。
       一行路径，取不到就一行空 —— 调用方据此决定要不要交给 icon 子命令。 */
    char path[200];
    path[0] = 0;
    if (argc > 2) appinfo_iconpath(argv[2], path, sizeof path);
    printf("%s\n", path);
    return 0;
  }
  if (!strcmp(cmd, "label")) {
    char name[128];
    name[0] = 0;
    if (argc > 2) appinfo_label(argv[2], name, sizeof name);
    printf("%s\n", name);
    return 0;
  }
  if (!strcmp(cmd, "labels")) {
    /* 批量：stdin 每行 "包名<TAB>APK 路径"，stdout 每行 "包名<TAB>应用名"。
       批量是因为一次读名字要重新走一遍中央目录，
       逐包 fork 几百次的开销比 IO 还大（见 appinfo.c 末尾）。 */
    return appinfo_labels_stdin();
  }
  if (!strcmp(cmd, "score")) {
    /* 纯函数式子命令：读 stdin、写 stdout、不碰任何节点、不需要 root。
     * api.sh 用它给 unzip 兜底挑图——挑图规则留在 C 里，
     * shell 那侧只负责把 `unzip -l` 的名字抠出来喂进来。 */
    return icon_score_stdin();
  }
  if (!strcmp(cmd, "probe")) {
    /* 只读探测：把将要施加的封顶打印出来，不写任何节点 */
    load_cfg();
    struct policy p[MAX_POLICY];
    int n = detect_policies(p);
    printf("=== cpufreq policy ===\n");
    for (int i = 0; i < n; i++) {
      char path[224];
      snprintf(path, sizeof path, "%s/scaling_max_freq", p[i].dir);
      printf("  %-8s hw=%-9ld cur=%-9ld\n", p[i].name, p[i].hw_max,
             read_num(path));
    }
    assign_pct(p, n);
    printf("=== GPU ===\n");
    detect_gpu();
    printf("  node=%s 最高=%ldHz 封顶=%ldHz\n", g_gpu.max_node[0] ? g_gpu.max_node : "无",
           g_gpu.max_hz, g_gpu.normal);
    return 0;
  }
  if (!access(PAUSE_FILE, F_OK)) {
    printf("paused\n");
    return 0;
  }

  /* 数据目录不在解包范围内（模块升级会重建模块目录），
     所以首次运行必须自己把它建出来；RUN_DIR 在它下面。 */
  mkdir_p(SF_DATADIR);
  mkdir_p(RUN_DIR);
  log_open();
  log("===== socfreqd %s 启动 pid=%d =====", SF_VERSION, getpid());

  if (acquire_lock()) {
    log("已有实例在运行，本次退出");
    return 0;
  }
  g_stop = 0;
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, SIG_IGN);

  load_cfg();
  g_npol = detect_policies(g_pol);
  if (!g_npol) {
    log("WARN 未探测到 cpufreq policy，本模块不工作");
    return 1;
  }
  assign_pct(g_pol, g_npol);
  detect_gpu();

  /*
   * 先按持久化快照回收上次崩溃留下的写入，再取新基准。
   * 所有数值写入都走 journal，所以这里恢复出来的就是真正的原始值。
   */
  journal_restore(NODES_JOURNAL);
  perms_restore();
  snapshot_governors();
  /* 上次崩溃时改过调速器的话，基准快照要记恢复后的值 */
  restore_governors();
  snapshot_governors();

  load_game_pkgs();
  load_app_modes();
  learn_init(LEARN_FILE, LEARN_APP_FILE);
  /* 传感器路径的钩子在这里读一次环境变量，之后每轮直接按路径取 */
  adapt_init();
  /* 顺序要有：先加载已有样本，再判开关。
     LEARN=0 时关掉的那一步会顺手删掉数据文件，"关闭学习"就等价于清空样本，
     反过来先删后读的话，磁盘上会留一份再也不会被覆盖的旧数据。 */
  learn_set_enabled(cfg.learn);
  screen_state_init();
  log("巡检 日常%d s / 游戏%d s / 熄屏%d s，退出宽限 %d s%s，熄屏策略=%s", cfg.interval,
      cfg.interval * 2, cfg.soff_interval, cfg.grace,
      cfg.learn ? "，学习采样开" : "",
      cfg.screen_policy == SCREEN_UNCAP ? "交还系统" : "保持封顶");
  apply_normal(1);
  heartbeat();

  long missing_since = 0; /* 前台丢失的起始秒，0 = 未丢失 */
  long ad_last = 0;       /* 上一次算实时偏移的时刻，用来量真实的巡检间隔 */
  uint64_t seen = cfg_stamp();
  for (; !g_stop;) {
    /* 每轮开头复位：这一轮只要偏移动过，后面几处 apply_normal 就得 force 写 */
    int adapt_changed = 0;
    /* pause 自查：手工 touch pause 与 WebUI 的"暂停"都在一个巡检周期内生效。
     * break 出去走 cleanup()，顺带还原频率、收回权限、删锁文件。 */
    if (!access(PAUSE_FILE, F_OK)) {
      log("检测到 pause，恢复并退出");
      break;
    }
    heartbeat();
    int cfg_changed = reload_if_changed(seen);
    seen = cfg_stamp();
    /* 页面上把 LEARN 改成 0 之后不必重启守护：这一行让下一个巡检周期就停采，
       并把已经采到的样本删掉（见 learn_set_enabled）。 */
    learn_set_enabled(cfg.learn);
    /* 熄屏轻巡检：SCREEN_OFF 下只读一个亮屏信号，不 popen dumpsys、
       不跑 PSI/温控/学习。SCREEN_POLICY=keep 时整段跳过，行为与旧版一致。 */
    if (cfg.screen_policy == SCREEN_UNCAP) {
      if (g_screen_off) {
        int woke = screen_wake_seen();
        if (!woke && !g_soff_keep) {
          long raw = 0;
          int valid = read_signed(cfg.batt_current_path, &raw);
          long ma = valid ? batt_current_ma(raw, cfg.batt_current_ma_unit) : 0;
          int r = idle_step(&g_idle_ema, &g_idle_hold, ma, valid,
                            cfg.idle_current_ma, cfg.idle_hold_cycle,
                            cfg.wake_delta_ma);
          if (r == 2) {
            /* 电流突增只当"可能亮屏"的提示，必须再用 dumpsys 确认；
               否则后台下载也会把深睡打断。 */
            if (screen_awake()) woke = 1;
            else g_deep = 0;
          } else if (r == 1 && !g_deep) {
            g_deep = 1;
            log("进入深度空闲（%d s 轻巡检，电流 %ldmA）", cfg.idle_interval, ma);
          } else if (g_deep && (!valid || ma >= cfg.idle_current_ma)) {
            g_deep = 0;
            log("电流回升，回到熄屏巡检");
          }
        }
        if (!woke) {
          if (g_soff_keep) {
            /* keep 应用：不交还，只把被厂商顶高的上限压回去。 */
            caps_patrol(0);
            gpu_patrol();
          }
          publish_status();
          sleep(g_deep ? cfg.idle_interval : cfg.soff_interval);
          continue;
        }
        leave_screen_off();
        missing_since = 0;
      }
      /* ACTIVE：先看 sysfs 信号，熄屏后就不必再跑一次 dumpsys window。
         信号说亮时每 60 轮（5 分钟）做一次 dumpsys power 兜底校验，
         信号说灭时立刻复核；熄屏态的无信号兜底仍是每 6 轮。 */
      int verify = (++g_scr_check >= 60);
      if (verify) g_scr_check = 0;
      if (screen_is_off_now(verify)) {
        enter_screen_off();
        missing_since = 0;
        publish_status();
        sleep(cfg.soff_interval);
        continue;
      }
    } else if (g_screen_off) {
      /* 策略被改回 keep：退出轻巡检，让下面的常态路径重新压回。 */
      g_screen_off = 0;
      g_deep = 0;
      g_soff_keep = 0;
      g_soff_pkg[0] = 0;
      g_idle_ema = 0;
      g_idle_hold = 0;
      if (!strcmp(g_state, "full")) apply_normal(1);
    }
    /* 实时偏移：每轮读一次 PSI / 最热温度传感器 / 充电态。
     * 生效值变了就把封顶重算一遍，并且这一轮必须 force 写 ——
     * 巡检的判断是 cur<=cap 就跳过，"放宽"这个方向正好落在跳过分支里，
     * 和换应用档位时是同一个坑。 */
    {
      long anow = mono_sec();
      int adt = ad_last ? (int)(anow - ad_last) : cfg.interval;
      if (adt < 1) adt = 1;
      if (adt > 120) adt = 120;          /* 被暂停/长眠隔久了：按上限算，别把整段当成一轮 */
      ad_last = anow;
      /* PSI 基线只在"常态、且没有偏移在生效"时更新。
         游戏/满血/熄屏那段时间帽是解开的、负载又高，让它进基线会把门槛抬高，
         回到日常后 PSI 护栏就变钝了；PSI 已经借出或温控正在压的时候也不更新，
         否则正在卡的这段会被当成新常态。插电只抬地板、不改 delta，照常更新。 */
      int base_ok = !strcmp(g_state, "normal") && adapt_delta() == 0;
      if (adapt_tick(&cfg.ad, adt, base_ok)) {
        struct adapt a;
        adapt_snapshot(&a);
        adapt_changed = 1;
        log("  实时偏移 %+d 个百分点（PSI %d 档 / 温控 %d 档 / %s）", a.delta,
            a.psi_steps, a.th_steps, a.chg_on ? "插电放宽" : "未插电");
      }
    }
    /* 配置和偏移都看过之后，统一在这里重算生效封顶。
     * 顺序是有意的：偏移的快照必须先新鲜，否则这里算出来的是旧地板。 */
    if (cfg_changed || adapt_changed) {
      assign_pct(g_pol, g_npol);
      /* 全局分档刚被重算，生效中的应用专属档位要盖回去，
       * 否则保存配置会让正在生效的专属档位静默退回全局百分比。 */
      if (g_app_pct > 0) set_caps(g_app_pct, g_app[0] ? g_app : "应用专属");
      if (reclaim_caps_allowed()) apply_normal(1);
    }

    /* 学习采样。eligible 判的是 g_state=="normal"，也就是**上一轮到这一轮之间**
     * 处于常态封顶——那才是刚过去这段 dt 的真实状态。游戏、开机满血窗口、
     * 熄屏解除、应用满血期间帽不在身上，采到的分布只会把建议往高了带。
     * 这些时段仍然要调一次：learn_tick 会只刷基线不累计，避免回来第一拍
     * 把整段解锁时间一次性算进封顶期的分布里。 */
    {
      const char *ld[MAX_POLICY]; int lt[MAX_POLICY];
      long lh[MAX_POLICY], lc[MAX_POLICY], lp[MAX_POLICY];
      int ln = 0;
      /* 样本里的"帽"必须记**配置意图**（CPU_PCT_* / GPU_PCT），不能记生效值：
         生效值里有充电地板和 PSI/温控偏移，插拔一次电源、抖一档，
         就会把已经攒了几小时的分布整档清零（表现就是"CPU 学习不见了"）。
         判据那边也是拿这个值跟 conf 比，意图才是两边的共同语言。 */
      int app_cap_active = g_app_capped[0] != 0;
      /* 只有开了按应用学习才把应用百分比写进样本；没开时那段仍按全局帽记账，
         否则一个 40% 的应用帽会被当成"全局改成了 40%"，整行样本作废。 */
      int app_layer = cfg.learn_app && app_cap_active;
      for (int i = 0; i < g_npol && ln < MAX_POLICY; i++) {
        ld[ln] = g_pol[i].dir; lt[ln] = g_pol[i].tier;
        lh[ln] = g_pol[i].hw_max; lc[ln] = g_pol[i].cap;
        lp[ln] = (app_layer && g_app_pct > 0) ? g_app_pct : cfg.pct[g_pol[i].tier];
        ln++;
      }
      /* GPU 当成第 5 档（tier 4）来采：判据、页面那一行、应用建议都走同一条路。
       * 只有 KGSL 那对表（gpu_clock_stats / gpu_available_frequencies）能采，
       * 读不到就干脆不给 GPU 这一行 —— 不拿 cur_freq 凑数，
       * 那玩意儿空闲时趴在最低档，会把分布做成"GPU 永远用不满"的假结论。 */
      if (g_gpu.ok && ln < MAX_POLICY &&
          !access(GPU_STATS_FILE, F_OK)) {
        ld[ln] = GPU_LEARN_DIR; lt[ln] = 4;
        lh[ln] = g_gpu.max_hz;
        /* 生效值而不是配置值：应用专属 GPU 档和实时偏移都会改它 */
        lc[ln] = g_gpu.active > 0 ? g_gpu.active : g_gpu.normal;
        /* 但样本要记住"百分比意图"：应用专属 GPU 档下采的样本不能让全局
           GPU 建议拿去用（advice 会按这个值过滤）。 */
        lp[ln] = (app_layer && g_app_gpu > 0) ? g_app_gpu : cfg.gpu_pct;
        ln++;
      }
      /* 采样只在"帽正好是用户设的那一道"时累计。
       * 插电抬到 100%、温控压了两档、PSI 借了 5 个点的时候，
       * 核心身上的是别的封顶，那份分布回答不了"设定值这道帽紧不紧"，
       * 混进去会把建议往错的方向带（顶帽率虚高 → 建议放宽，或反之）。
       * 代价是插着电的时段不攒样本：宁可慢，也不要给出偏掉的建议。
       *
       * 戴的是应用专属档位时同一份分布只记到按应用那一层，两层互斥：
       * 拿"75% 应用帽下的表现"去建议全局值，两层会互相把对方带偏。
       * 戴的是全局帽时反过来：同一段分布既回答全局那道帽紧不紧，
       * 也回答这个前台包在全局帽下过得怎么样，两层都记。 */
      struct adapt av;
      adapt_snapshot(&av);
      int adapt_clean = !av.delta && !av.chg_on;
      /* 有没有应用专属帽，和开没开按应用学习是两件事：没开学习时那段分布
         仍然不属于全局帽，不能记进全局行（否则一开抖音就清掉全局 CPU 样本）。 */
      int app_now = cfg.learn && cfg.learn_app && app_cap_active;
      /* 归属来自上一轮，覆盖的正是刚过去的 dt。应用专属帽优先——
         那一轮全局层本来就不采；否则用全局帽下的前台包。 */
      const char *lpkg = "";
      int lown = 0;
      if (cfg.learn_app) {
        if (app_now) { lpkg = g_app_capped; lown = 1; }
        else if (g_fg_global[0]) lpkg = g_fg_global;
      }
      learn_tick(!strcmp(g_state, "normal") && adapt_clean && !app_cap_active,
                 lpkg, lown, ld, lt, lh, lc, lp, ln);
      /* 本轮还没走到"要不要戴某个应用的档位"那一步，先清空：
       * 只有下面真的应用了数字档位的那一支会把它写回来。 */
      g_app_capped[0] = 0;
      g_fg_global[0] = 0;
    }
    char fg[PKG_LEN] = {0};
    load_game_pkgs();
    load_app_modes();
    int game = foreground_pkg(fg) == 0 && g_npkg && is_game(fg);
    /* 前台包名无条件记下来（命中名单与否都要）。
     * 只在游戏态记的话，页面上"把当前前台设为满血"在相机、地图这些
     * 最需要它的场景里永远是空的。 */
    if (fg[0]) snprintf(g_fg, sizeof g_fg, "%s", fg);
    else g_fg[0] = 0;

    /* 满血窗口：开机后前 N 秒、或（可选）熄屏期间。
     * 顺序有讲究——先算 boot 再算 screen，screen 那条带 dumpsys 开销，
     * 只有开了 SCREEN_UNCAP 才会去读。 */
    int boost = 0;
    const char *bwhy = "-";
    if (cfg.boot_full_s > 0 && boot_boost_active()) { boost = 1; bwhy = "boot"; }
    else if (cfg.screen_policy == SCREEN_KEEP && cfg.screen_uncap &&
             !screen_awake())                        { boost = 1; bwhy = "screen"; }

    if (game) {
      missing_since = 0;
      if (strcmp(g_game, fg)) {
        snprintf(g_game, sizeof g_game, "%s", fg);
        unlock_game();
      } else {
        gpu_min_to_floor();
      }
      publish_status();
      sleep(cfg.interval * 2);
      continue;
    }

    if (boost) {
      missing_since = 0;
      if (strcmp(g_state, "game") || strcmp(g_reason, bwhy)) {
        snprintf(g_reason, sizeof g_reason, "%s", bwhy);
        hand_back_full(strcmp(bwhy, "boot") ? "熄屏解除限频，交还系统"
                                            : "开机满血窗口，暂不封顶");
        snprintf(g_state, sizeof g_state, "game");
      } else {
        gpu_min_to_floor();
      }
      publish_status();
      sleep(cfg.interval);
      continue;
    }

    /* 分应用档位。优先级 游戏名单 > 满血窗口 > 应用专属 > 全局百分比：
     * 熄屏解除是用户主动开的全局开关，屏幕都灭了还守着某个应用的 80%
     * 没有意义，所以排在 boost 后面。 */
    int mpct = 0, mgpu = 0;
    int has_mode = (!game && !boost && fg[0]) ? mode_lookup(fg, &mpct, &mgpu) : 0;
    int is_full = has_mode && mpct == MODE_FULL;
    /* keep 只影响熄屏：亮屏时按没有专属档位处理，继续走全局分档/学习。 */
    if (has_mode && mpct == MODE_KEEP) has_mode = 0;
    int want_pct = (has_mode && !is_full && mpct > 0) ? mpct : 0;
    int want_gpu = (has_mode && !is_full) ? mgpu : 0;

    if (is_full) {
      missing_since = 0;
      snprintf(g_fg, sizeof g_fg, "%s", fg);
      /* full 两者都交还；把生效值标成 full/0，离开时才会按全局重新算。
       * 状态里的 app_mode 也因此能报出 full（以前这一支从来没写过它）。 */
      g_app_pct = MODE_FULL;
      g_app_gpu = 0;
      if (strcmp(g_app, fg) || strcmp(g_state, "game") || strcmp(g_reason, "app")) {
        snprintf(g_app, sizeof g_app, "%s", fg);
        snprintf(g_reason, sizeof g_reason, "app");
        hand_back_full("应用设为满血，交还系统");
        snprintf(g_state, sizeof g_state, "game");
      } else {
        gpu_min_to_floor();
      }
      publish_status();
      sleep(cfg.interval);
      continue;
    }

    /* 封顶档位要不要换：只在数值变了时重算。
     * 换了必须 force 写一遍——巡检的判断是 cur<=cap 就跳过，
     * 从 70% 的应用切到 80% 的应用时新 cap 比当前值大，
     * 正好落在"不用管"的分支里，于是永远停在旧应用的下限上。 */
    int caps_changed = (want_pct != g_app_pct);
    int gpu_changed = (want_gpu != g_app_gpu);
    if (caps_changed) {
      set_caps(want_pct, want_pct ? fg : "全局分档");
      g_app_pct = want_pct;
    }
    g_app_gpu = want_gpu;
    snprintf(g_app, sizeof g_app, "%s", has_mode ? fg : "");
    /* GPU 只换了应用档位、CPU 没换时也要立刻写，巡检只往回压、不会主动升 */
    if (gpu_changed) {
      if (want_gpu > 0) log("  封顶重算（%s 专属 GPU %d%%）", fg, want_gpu);
      else log("  GPU 回到全局 %d%%", cfg.gpu_pct);
      apply_gpu_cap(1);
    }
    /* 这个应用戴着任何一道专属帽（CPU 或 GPU）时，这一轮就记在它名下（下一轮采样读它）。
     * GPU-only 档（:40）也要标：那段 GPU 驻留是在应用帽下采的，不能算进全局样本。 */
    if ((want_pct > 0 || want_gpu > 0) && fg[0])
      snprintf(g_app_capped, sizeof g_app_capped, "%s", fg);

    if (has_mode) {
      missing_since = 0;
      apply_normal(caps_changed || adapt_changed);
      gpu_patrol();     /* 这一支提前 continue，不巡检的话 Boost 会把 40% 顶回去 */
      publish_status();
      sleep(cfg.interval);
      continue;
    }

    /* 熄屏 / 开机窗口结束：直接压回，不吃"退出宽限"。
     * 那个宽限是给"切后台、弹窗造成前台包名短暂丢失"用的，
     * 满血窗口是明确到点的，多等 10 秒只会让人以为没生效。 */
    if (!strcmp(g_state, "game") && strcmp(g_reason, "game")) {
      /* 这一支同时兜"满血窗口到期"和"离开满血应用"两种：
       * 都是已交还系统要压回封顶，但原因不同，日志要分得开——
       * 排查时"应用档位没生效"和"开机窗口没结束"是完全不同的方向。 */
      log("%s，压回常态封顶", !strcmp(g_reason, "app") ? "离开满血应用"
                                                       : "满血窗口结束");
      g_game[0] = 0;
      g_app[0] = 0;
      apply_normal(1);
    }

    if (!strcmp(g_state, "game")) {
      /* 迟滞：切后台/弹窗造成的短暂丢失不立刻收回 */
      if (!missing_since) {
        missing_since = mono_sec();
        log("  游戏离开前台，%d s 内仍保持解锁", cfg.grace);
      } else if (mono_sec() - missing_since >= cfg.grace) {
        g_game[0] = 0;
        log("游戏已离开前台，复位到常态封顶");
        apply_normal(1);
      }
    } else {
      missing_since = 0;
      /*
       * 稳态巡检：CPU 上限也必须压回来。
       * 厂商侧（perfd / fpsgo / 热管控）会把 scaling_max_freq 抬回硬件最高，
       * 这里不调 apply_normal 就永远没人管，只能等改配置或进出游戏才恢复。
       * force=0：状态没变，走 RESTORE 分支，不重复做"→ 常态封顶"那套。
       */
      /* force 带上 caps_changed：封顶档位这一轮刚换过（离开专属应用回到全局），
       * 走巡检的 cur<=cap 就跳过这条，新（更高）的 cap 就永远写不上去。
       * adapt_changed 同理，而且更常见：PSI 借出额度、或凉下来还档，都是升档。 */
      apply_normal(caps_changed || adapt_changed);
      gpu_patrol();
      /* 这一轮落在全局帽的稳态巡检上：接下来的 dt 就是这个包在全局帽下。
         下一轮顶上的 learn_tick 用它做按应用归属；游戏宽限、满血、专属档位
         那几支都提前 continue 了，不会写到这里。 */
      if (fg[0]) snprintf(g_fg_global, sizeof g_fg_global, "%s", fg);
    }
    publish_status();
    sleep(cfg.interval);
  }

  cleanup();
  return 0;
}
#endif /* !SF_TEST */