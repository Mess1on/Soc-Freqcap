/* socfreqd.c — Soc智能限频 守护进程 */
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
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdint.h>
#include <sys/system_properties.h>
#include <unistd.h>

/* icon.c：取应用图标，与限频逻辑无关，只在 icon 子命令里调用。 */
int icon_from_apk(const char *apk, const char *want);
/* score 子命令：stdin 收条目名，stdout 打 "分数\t名字"，给 api.sh 的 */
int icon_score_stdin(void);
/* appinfo.c：从 APK 里读「应用名」和「声明的桌面图标路径」。 */
int appinfo_label(const char *apk, char *out, size_t osz);
int appinfo_iconpath(const char *apk, char *out, size_t osz);
int appinfo_labels_stdin(void);
/* adapt.c：三个实时传感器（PSI / 温度 / 充电态）合成一个"封顶偏移"。 */
#include "adapt.h"

#ifndef SF_MODDIR
#define SF_MODDIR "/data/adb/modules/soc_freqcap"
#endif
/* 可变状态的家。**不能放在模块目录里**：管理器升级模块会重建 */
#ifndef SF_DATADIR
#define SF_DATADIR "/data/adb/soc_freqcap"
#endif
#ifndef SF_VERSION
#define SF_VERSION "2.4 Beta3"
#endif

/* policy 的容器目录。里面只有 policyN/ 子目录，没有 cpuN/。 */
#define CPUFREQ "/sys/devices/system/cpu/cpufreq"
/* 核心的容器目录。逐核路径是 CPUBASE "/cpuN/cpufreq/..."。 */
#define CPUBASE "/sys/devices/system/cpu"
#define RUN_DIR   SF_DATADIR "/run"
/* 运行态文件放 /dev 内存盘：心跳/状态/逐核窗口每轮都在写，落在 F2FS */
#define TMP_RUN   "/dev/soc_freqcap"
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
/* 分应用档位：pkg=full / pkg=60 / pkg=60:40。优先级 应用专属 > 全局百分比。 */
#define MODE_FILE  SF_DATADIR "/app_modes.conf"
#define CAT_FILE   SF_DATADIR "/app_categories.conf"
#define CAT_DEFAULTS SF_DATADIR "/defaults/app_categories.conf"
#define CAT_FALLBACK SF_MODDIR "/app_categories.conf"
#define STATUS_FILE TMP_RUN "/status"
#define HEARTBEAT_FILE TMP_RUN "/heartbeat"
#define CORE_STATE_FILE TMP_RUN "/cores.state"
#define CORE_STATE_LOCK TMP_RUN "/cores.lock"
/* MTK 天玑（gpufreqv2）：没有 GPU devfreq，上限走 limit_table 的层约束。 */
#define MTK_LIMIT   "/proc/gpufreqv2/limit_table"
#define MTK_OPP     "/proc/gpufreqv2/stack_working_opp_table"
#define MTK_GPU_SNAP RUN_DIR "/gpu_mtk.saved"

/* CPU 封顶的第二道锁（MTK PPM / 高通 msm_performance） */
#define PPM_DIR        "/proc/ppm"
#define PPM_ENABLED    "/proc/ppm/enabled"
#define PPM_MAX_NODE   "/proc/ppm/policy/hard_userlimit_max_cpu_freq"
#define PPM_MIN_NODE   "/proc/ppm/policy/hard_userlimit_min_cpu_freq"
#define PPM_JOURNAL    RUN_DIR "/ppm.saved"
#define PPM_LAST       TMP_RUN "/ppm.last"

#define MSM_MAX_NODE   "/sys/module/msm_performance/parameters/cpu_max_freq"
#define MSM_MAX_ALT    "/sys/kernel/msm_performance/parameters/cpu_max_freq"
#define MSM_MIN_NODE   "/sys/module/msm_performance/parameters/cpu_min_freq"
#define MSM_MIN_ALT    "/sys/kernel/msm_performance/parameters/cpu_min_freq"
#define MSM_JOURNAL    RUN_DIR "/msm.saved"
#define MSM_LAST       TMP_RUN "/msm.last"

/* 老 MTK（无 devfreq max 节点）的 GPU 上限：写的是 OPP index，不是频率。 */
#define MTK_GED_UPBOUND "/sys/kernel/ged/hal/custom_upbound_gpu_freq"
#define MTK_OLD_OPP     "/proc/gpufreq/gpufreq_opp_dump"

enum {
  CAP_BE_AUTO = 0,
  CAP_BE_SYSFS,
  CAP_BE_PPM,
  CAP_BE_MSM,
};

#define MAX_POLICY 16
/* 核心数上限。8 核是当前主流（骁龙 8 Elite 到 8 核，天玑 9400 也 8+1）。 */
#define MAX_CORES 16
#define MAX_NODE 64
#define MAX_PKG 512
#define PKG_LEN 256

/* 配置与全局状态                                                     */

#define SCREEN_KEEP  0   /* 熄屏不改变封顶，但按亮屏全量路径巡检（旧行为） */
#define SCREEN_UNCAP 1   /* 熄屏交还系统，轻巡检 */
#define SCREEN_CAP   2   /* 熄屏保持封顶，轻巡检（只压回被顶高的上限） */
#define FSM_STATES   6

struct cfg {
  int interval;        /* 常态巡检间隔秒 */
  /* 调度策略：0=限频（固定百分比），1=智能（按负载状态给目标）。 */
  int strategy;
  int grace;           /* 游戏退出宽限秒 */
  /* 动态封顶（「Boost 与压制」页，默认关）：一个开关 = 完整压制—— */
  int suppress;        /* 1 = 动态封顶（1s 快拍动态天花板） */
  int sup_style;       /* 0 = 均衡（地板 80%），1 = 省电（地板 70%、降档更快） */
  int gov_up_us[3];    /* 升频限速 µs：0=BIG 1=MID 2=LITTLE（100~50000） */
  int gov_down_us[3];  /* 降频限速 µs，同上 */
  /* 智能模式切前台首拍预热档：0=关，1=轻载(65%)，2=中载(85%)，3=重载(100%)。 */
  int app_switch_tier;
  /* FSM 六态参数；0 = 用内置默认。group 0=BIG 1=其他核。 */
  int fsm_cpu[FSM_STATES][2];
  int fsm_min[FSM_STATES][2];
  int fsm_gpu[FSM_STATES];
  int fsm_cpu_hi_in, fsm_cpu_hi_out, fsm_cpu_mod_in, fsm_cpu_mod_out;
  int fsm_gpu_game_in, fsm_gpu_game_out, fsm_gpu_hi_in, fsm_gpu_hi_out;
  int fsm_up_s, fsm_down_s, fsm_pair_s;
  int fsm_warmup_s, fsm_dwell_heavy_s, fsm_dwell_game_s;
  int fsm_dwell_sustain_s, fsm_thermal_hold_s;
  int fsm_psi_mod, fsm_psi_hi, fsm_skin_hi, fsm_skin_lo, fsm_adpf_hi;
  int fsm_hr_soft, fsm_hr_hard;
  int pct[4];          /* big / mid2 / mid / little 封顶百分比 */
  /* 进阶最低频：每个簇的 scaling_min_freq 下限，百分比；0 = 跟随系统。 */
  int min_pct[4];
  /* M 簇封顶下限（Hz）：只作用于最低频档（2 簇机型的 LITTLE/M 簇）。 */
  int cap_floor_m;
  int gpu_pct;         /* GPU 封顶百分比 */
  int gpu_min_lock;    /* 1 = 把 GPU 下限压回最低档（devfreq 一路，非关 boost） */
  int gpu_boost_off;   /* 1 = 关掉 GPU boost 源（dcvs_boost / touch_wake / devfreq_boost） */
  int bg_light;        /* 1 = 后台分组常驻轻压制（与动态封顶无关） */
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
  char governor[32];   /* 日常调速器，空 = 不干预 */
  int big_min;         /* 1 = 超大核压到最低频（不关闭核心） */
  int big_offline;     /* 1 = 常态直接关掉超大核，游戏/满血时上线 */
  int cpu_min_lock;    /* 1 = 常态把各簇下限压到 cpuinfo_min（有节流+退避） */
  /* CPU 封顶后端：auto=有 PPM/msm 节点就加第二道锁；sysfs=只写 sysfs */
  int cap_backend;
  /* 实时偏移（adapt.c）。都是"百分点"，只加在生效值上，不写回配置。 */
  struct adapt_cfg ad;
};

static struct cfg cfg;
/* 智能模式独占封顶：FSM 开了就不再叠动态封顶。 */
static int suppress_on(void) {
  return cfg.suppress && cfg.strategy != 1;
}
static char g_state[16] = "normal";
/* 交还系统的原因：game / screen / boot / app。 */
static char g_reason[16] = "-";
static char g_game[PKG_LEN];
/* 本轮读到的前台包名（不代表命中游戏名单）。 */
static char g_fg[PKG_LEN];
/* 熄屏轻巡检（SCREEN_POLICY=uncap）。g_screen_off 与封顶状态分开 */
static int g_screen_off;
static char g_scr_path[192];  /* 亮屏信号文件；空 = 没有可用文件 */
static int g_scr_invert;      /* 1 = blank 语义（0=亮）；0 = 亮度（>0=亮） */
static int g_scr_bad;         /* 信号与 dumpsys 不符的次数；连续 3 次就停用 */
static int g_scr_fallback;    /* 1 = 已降级为 dumpsys，配置重载不再重新启用坏节点 */
static char g_scr_cfg_seen[192]; /* 上次 SCREEN_STATE_PATH，区分"配置没变" */
static int g_scr_check;       /* 兜底校验计数 */
static int g_soff_th;         /* 熄屏温控节拍：每 4 拍采一次 */
/* P0b：熄屏深睡。g_deep 只影响周期和电流判定；亮屏信号每轮仍读。 */
static int g_deep;
static long g_idle_ema;
static int g_idle_hold;
static int g_soff_keep;       /* 熄屏瞬间的快照应用是 pkg=keep */
static char g_soff_pkg[PKG_LEN];
static int g_have_lock;
static int g_lock_fd = -1;

static void cpu_min_patrol(void);
static void cpu_min_refresh(void);
static void cpu_min_restore_user(void);
static void cpu_min_restore(void);
static void big_offline_apply(void);
static void big_offline_restore(const char *why);
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

/* 看门狗心跳：每轮巡检写一次 mtime。service.sh 只 stat 这个文件 */
static long g_hb_at;
static void heartbeat(void) {
  long now = mono_sec();
  if (g_hb_at && now - g_hb_at < 60) return;
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp", HEARTBEAT_FILE);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  fprintf(f, "%ld\n", now);
  fclose(f);
  rename(tmp, HEARTBEAT_FILE);
  g_hb_at = now;
}

/* 日志                                                               */

static int g_logfd = -1;
static unsigned g_logcount;

static void log_open(void) {
  
  g_logfd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

/* 每 64 次才量一次大小，超 256KB 就砍掉旧的一半，避免长跑撑爆存储。 */
static void log_trim(void) {
  if (++g_logcount % 64) return;
  struct stat st;
  if (fstat(g_logfd, &st) || st.st_size < 256 * 1024) return;
  char tmp[sizeof LOG_FILE + 8];
  snprintf(tmp, sizeof tmp, "%s.trim", LOG_FILE);
  int in = open(LOG_FILE, O_RDONLY);
  if (in < 0) return;
  off_t skip = st.st_size / 2;
  if (lseek(in, skip, SEEK_SET) != skip) { close(in); return; }
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { close(in); return; }
  char buf[4096];
  ssize_t r = 0;
  while ((r = read(in, buf, sizeof buf)) > 0) {
    if (write(out, buf, r) != r) { r = -1; break; }
  }
  close(in);
  if (close(out) != 0 || r < 0) { unlink(tmp); return; }
  if (rename(tmp, LOG_FILE) != 0) { unlink(tmp); return; }
  close(g_logfd);
  g_logfd = -1;
  log_open();
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

/* sysfs 基础读写                                                     */

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

/* 配置文件指纹：秒级 mtime 会让"同一秒连写两次"漏重载；带上纳秒 */
static uint64_t file_stamp(const char *path) {
  struct stat st;
  if (stat(path, &st)) return 0;
  uint64_t ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ULL +
                (uint64_t)st.st_mtim.tv_nsec;
  return ns ^ ((uint64_t)st.st_size << 32) ^ (uint64_t)st.st_ino;
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

/* 有符号读数。battery/current_now 在充电时可能是负值（PJZ110 实测 -404） */
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

/* 这个核心在不在。 */
static int core_path(char *b, size_t n, int c, const char *leaf) {
  return snprintf(b, n, CPUBASE "/cpu%d/cpufreq/%s", c, leaf);
}

static int core_exists(int c) {
  char b[96];
  core_path(b, sizeof b, c, "cpuinfo_max_freq");
  return read_num(b) > 0;
}

/* 这颗核的 cpufreq 是不是符号链接。内核会建 */
static int core_policy_of(int c) {
  char b[96], link[160];
  /* 路径**不能带尾斜杠**。 */
  snprintf(b, sizeof b, CPUBASE "/cpu%d/cpufreq", c);
  ssize_t k = readlink(b, link, sizeof link - 1);
  if (k <= 0) return -1;
  link[k] = 0;
  const char *s = strrchr(link, '/');
  if (!s) return -1;
  if (strncmp(s + 1, "policy", 6)) return -1;
  return atoi(s + 7);
}

/* 逐核频率历史 */
#define CORE_HIST_N 5
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

/* 旧样本左移，新样本放最右；第一次直接铺满 5 格 */
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

/* cores 每秒是一个新进程，内存里的窗口活不过一次调用。 */
/* 运行态文件已迁到 /dev/soc_freqcap；旧路径留软链 */
static void tmp_link(const char *target, const char *link) {
  char cur[192];
  ssize_t n = readlink(link, cur, sizeof cur - 1);
  if (n > 0) {
    cur[n] = 0;
    if (!strcmp(cur, target)) return;
  }
  unlink(link);
  symlink(target, link);
}

static void tmp_runtime_init(void) {
  mkdir(TMP_RUN, 0700);
  mkdir(RUN_DIR, 0755);
  tmp_link(HEARTBEAT_FILE, RUN_DIR "/heartbeat");
  tmp_link(STATUS_FILE, RUN_DIR "/status");
}

static int g_core_lock_fd = -1;

static void core_state_lock(void) {
  mkdir(TMP_RUN, 0700);
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
  /* 页面关了很久再打开时，上一拍的 /proc/stat 基线已经过期 */
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
    if (sscanf(line, "F %ld %ld %ld %ld %ld",
               &h->freq[0], &h->freq[1], &h->freq[2], &h->freq[3],
               &h->freq[4]) != 5) {
      h->n = 0;
      continue;
    }
    if (!fgets(line, sizeof line, f) || strncmp(line, "L ", 2)) { h->n = 0; continue; }
    if (sscanf(line, "L %ld %ld %ld %ld %ld",
               &h->load[0], &h->load[1], &h->load[2], &h->load[3],
               &h->load[4]) != 5) {
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
    fprintf(f, "F %ld %ld %ld %ld %ld\n",
            h->freq[0], h->freq[1], h->freq[2], h->freq[3], h->freq[4]);
    fprintf(f, "L %ld %ld %ld %ld %ld\n",
            h->load[0], h->load[1], h->load[2], h->load[3], h->load[4]);
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

/* journal：写之前先记账，恢复失败也不丢，下次启动再试                 */

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

/* 取 journal 里这个节点记的原始权限；没有返回 -1。 */
static int journal_mode(const char *file, const char *node) {
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  char line[512], p[512];
  long v;
  unsigned m;
  int ret = -1;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%511s %ld %o", p, &v, &m) == 3 && !strcmp(p, node)) {
      ret = (int)m;
      break;
    }
  fclose(f);
  return ret;
}

/* 取 journal 里记的原始值；没记过返回 0。 */
static int journal_value(const char *file, const char *node, long *out) {
  FILE *f = fopen(file, "r");
  if (!f) return 0;
  char line[512], p[512];
  long v;
  unsigned m;
  int ret = 0;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "%511s %ld %o", p, &v, &m) == 3 && !strcmp(p, node)) {
      *out = v;
      ret = 1;
      break;
    }
  fclose(f);
  return ret;
}

static void journal_add(const char *file, const char *node, long val, int mode) {
  FILE *f = fopen(file, "a");
  if (!f) return;
  fprintf(f, "%s %ld %o\n", node, val, mode);
  fflush(f);
  fsync(fileno(f));
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
  /* 先还 max/普通节点，最后再还 min：内核要求 min <= max */
  for (int pass = 0; pass < 2; pass++) {
    rewind(f);
    while (fgets(line, sizeof line, f)) {
      if (sscanf(line, "%511s %ld %o", node, &val, &mode) != 3) continue;
      int is_min = strstr(node, "/scaling_min_freq") ||
                   strstr(node, "/min_freq");
      if ((pass == 1) != !!is_min) continue;
      chmod(node, 0644);
      if (write_num(node, val) || read_num(node) != val) failed = 1;
      chmod(node, mode);
      count++;
    }
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

/* 只还权限，不动数值。 */
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

/* SoC 芯片代号                                                        */

/* 顺序不能随便换：ro.soc.model 是最准的，但只在部分机型上存在 */

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

/* CPU policy 探测与分档                                              */

struct policy {
  char name[32];
  char dir[160];
  long hw_max;
  long hw_min;  /* cpuinfo_min_freq：熄屏态压下限用 */
  /* 厂商出厂/原始 scaling_max_freq。普通态和智能模式的百分比都以它为顶 */
  long vendor_max;
  long cap;  /* 封顶值，已吸附到频点 */
  long cat_cap; /* 应用分类上限，0=未命中；只在写入时取更低值 */
  long pct;  /* 意图百分比（已含实时偏移前的策略目标） */
  int  tpct; /* 策略目标：cap 下 = CPU_PCT_*，smart 下 = 状态机给的值 */

  int tier;  /* 用的哪一档：0=BIG 1=MID2 2=MID 3=LITTLE，WebUI 靠它对滑杆 */
  int pol_no; /* policy 号（atoi(name+6)）。反查"某档管哪几颗核"要拿它和
               * core_policy_of(c) 比，后者来自 readlink、不是数组下标
               * 按 hw_max 去重后下标与 policy 号不再相等，须单独保存。 */
  /* 1 = 该簇节点暂不可读（核心离线/热插拔）：巡检跳过、不刷错误日志 */
  int offline;
  /* 最近一次模块写入的意图值和实际回读值。force 升帽前用它判断 */
  long last_cap;
  long last_got;
  /* 上一次"写下去没生效"的目标与回读值。内核实际下限可能比 */
  long unreach_cap;
  long unreach_got;
  long unreach_at;
  /* 第二道锁（PPM/MSM）：用哪条后端、PPM cluster 号、最后写入值和连续失败数。 */
  int be;          /* CAP_BE_SYSFS / CAP_BE_PPM / CAP_BE_MSM */
  int ppm_idx;     /* PPM cluster 号，-1 = 不使用 */
  long be_last;    /* 最后写入后端的值；0 = 还没有写过 */
  int be_fail;     /* 连续写失败次数，达到阈值就停用该簇后端 */
};

static long cap_with_cat(const struct policy *p, long want);
static int clampi(int v, int lo, int hi, int dflt);

static int policy_index(const char *dir) {
  const char *s = strrchr(dir, '/');
  s = s ? s + 1 : dir;
  if (strncmp(s, "policy", 6)) return 999;
  return atoi(s + 6);
}

/* 百分比按硬件最高频算，再把结果压回厂商原始上限以内。 */
static long cap_from_pct(const struct policy *p, int pct) {
  long want = p->hw_max * pct / 100;
  if (p->vendor_max > 0 && want > p->vendor_max) want = p->vendor_max;
  return want;
}

/* 交还系统时写回的厂商上限；没有记录才退回硬件最高频。 */
static long cap_base(const struct policy *p) {
  if (p->vendor_max > 0 && p->vendor_max < p->hw_max) return p->vendor_max;
  return p->hw_max;
}

static void policy_vendor_max_init(struct policy *p) {
  char path[256];
  snprintf(path, sizeof path, "%s/scaling_max_freq", p->dir);
  long v = 0;
  if (journal_value(NODES_JOURNAL, path, &v) && v > 0) {
    p->vendor_max = v;
  } else {
    p->vendor_max = read_num(path);
  }
  if (p->vendor_max <= 0) p->vendor_max = p->hw_max;
  if (p->hw_max > 0 && p->vendor_max > p->hw_max) p->vendor_max = p->hw_max;
}

/* 从频点表文本里挑出 <= want 的最高频点。 */
/* 只返回真实频点；没有 <= want 的候选就返回 0。给 snap_freq 合并 */
static long snap_best_from_list(const char *list, long want) {
  char b[4096];
  snprintf(b, sizeof b, "%s", list);
  long best = 0;
  char *save = NULL;
  for (char *t = strtok_r(b, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) {
    if (!isdigit((unsigned char)*t)) continue;
    long f = strtol(t, NULL, 10);
    if (f > 0 && f <= want && f > best) best = f;
  }
  return best;
}

/* 兼容旧自检：没有候选时保留原值。snap_freq 自己用 snap_best_from_list。 */
static long __attribute__((unused)) snap_from_list(const char *list, long want) {
  long v = snap_best_from_list(list, want);
  return v > 0 ? v : want;
}

/* 把期望值吸附到 <= 它的最高可用频点：封顶语义要求实际永远不超过意图。 */
static long snap_freq(const struct policy *p, long want) {
  if (p->hw_max > 0 && want >= p->hw_max) return p->hw_max;
  char b[4096], path[224];
  long best = 0;
  snprintf(path, sizeof path, "%s/scaling_available_frequencies", p->dir);
  if (!read_file(path, b, sizeof b)) best = snap_best_from_list(b, want);
  snprintf(path, sizeof path, "%s/scaling_boost_frequencies", p->dir);
  if (!read_file(path, b, sizeof b)) {
    long v = snap_best_from_list(b, want);
    if (v > best) best = v;
  }
  return best > 0 ? best : want;
}

/* 只返回 >= want 的最小真实频点；没有就在调用处回退。 */
static long snap_up_best_from_list(const char *list, long want) {
  char b[4096];
  snprintf(b, sizeof b, "%s", list);
  long best = 0;
  char *save = NULL;
  for (char *t = strtok_r(b, " \t\n", &save); t; t = strtok_r(NULL, " \t\n", &save)) {
    if (!isdigit((unsigned char)*t)) continue;
    long f = strtol(t, NULL, 10);
    if (f >= want && (best == 0 || f < best)) best = f;
  }
  return best;
}

/* 静置上限要“向上对齐”真实频点：960MHz 不一定每颗芯片都有 */
static long snap_up_freq(const struct policy *p, long want) {
  if (want <= 0) return 0;
  if (p->hw_max > 0 && want > p->hw_max) want = p->hw_max;
  char b[4096], path[224];
  long best = 0;
  snprintf(path, sizeof path, "%s/scaling_available_frequencies", p->dir);
  if (!read_file(path, b, sizeof b)) best = snap_up_best_from_list(b, want);
  snprintf(path, sizeof path, "%s/scaling_boost_frequencies", p->dir);
  if (!read_file(path, b, sizeof b)) {
    long v = snap_up_best_from_list(b, want);
    if (v > 0 && (best == 0 || v < best)) best = v;
  }
  if (!best) best = p->hw_max > 0 ? p->hw_max : want;
  if (p->cap > 0 && best > p->cap) best = snap_freq(p, p->cap);
  return best;
}

/* M 簇封顶下限：只作用于最低频档（2 簇机的 LITTLE/M 簇）。 */
static long cap_floor_m_for(const struct policy *p) {
  if (cfg.cap_floor_m <= 0 || (p->tier & 3) != 3) return 0;
  long f = cfg.cap_floor_m;
  if (p->hw_max > 0 && f > p->hw_max) f = p->hw_max;
  return f;
}

static long cap_floor_apply(const struct policy *p, long cap) {
  long f = cap_floor_m_for(p);
  return (f > 0 && cap < f) ? f : cap;
}

/* 收集全部 policy 目录，**不去重**，并记下 policy 号。 */
/* 芯片适配覆盖（override.conf） */
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

/* 自动探测一个 policy 都没拿到时的兜底。 */
static int ovr_policies(struct policy *out) {
  int n = 0;
  for (int i = 0; i < ovr_n && n < MAX_POLICY; i++) {
    char cut[240], path[256];
    const char *base = strrchr(ovr_dir[i], '/');
    base = base ? base + 1 : ovr_dir[i];
    /* 逐核目录（…/cpu0/cpufreq）的末段每条都一样，直接拿来当名字 */
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
    snprintf(path, sizeof path, "%s/cpuinfo_min_freq", out[n].dir);
    out[n].hw_min = read_num(path);
    if (out[n].hw_max > 0) {
      policy_vendor_max_init(&out[n]);
      n++;
    }
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
    /* policy 号直接从目录名取，别用数组下标：下标会随目录顺序变 */
    out[n].pol_no = atoi(e->d_name + 6);
    snprintf(path, sizeof path, "%s/cpuinfo_max_freq", out[n].dir);
    out[n].hw_max = read_num(path);
    snprintf(path, sizeof path, "%s/cpuinfo_min_freq", out[n].dir);
    out[n].hw_min = read_num(path);
    if (out[n].hw_max > 0) {
      policy_vendor_max_init(&out[n]);
      n++;
    }
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

/* 不能用 hw_max 去重来省事：同频但独立的 policy（例如部分 4+4 机型的 */
static int detect_policies(struct policy *out) {
  struct policy tmp[MAX_POLICY];
  int n = detect_policies_raw(tmp);
  if (!n) n = ovr_policies(tmp);   /* 一个都没探测到时才用适配覆盖 */
  for (int i = 0; i < n; i++) out[i] = tmp[i];
  return n;
}

/* 簇阶梯：把全部 policy 的硬件上限去重后升序排好，m 是"不同上限的档数"。 */
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

/* 某个硬件上限属于哪一档，返回 0=BIG 1=MID2 2=MID 3=LITTLE（= tierkey）。 */
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

/* 智能模式：负载状态机 */
/* 各档占"用户天花板"的比例：重载用满，其余依次收紧。 */
enum {
  SMART_DEEP_IDLE = 0, SMART_LIGHT_IDLE, SMART_MODERATE,
  SMART_HEAVY, SMART_SUSTAINED, SMART_GAMING, SMART_STATE_COUNT
};

static const int kSmartTickSec = 2;
static const int kSmartUpWindow = 2;
static const int kSmartDownWindow = 5;
static const int kSmartPairWindow = 3;
static const int kSmartWarmupTicks = 23;
static const int kSmartThermalHoldTicks = 90;
static const int kSmartDwellHeavy = 4;
static const int kSmartDwellGaming = 13;
static const int kSmartDwellSustained = 12;

static const int kSmartFrac[SMART_STATE_COUNT] = {
  45, 65, 85, 100, 80, 100,
};
static const int kSmartFracM2Low[SMART_STATE_COUNT] = { 55, 75, 90, 100, 85, 100 };
static const int kSmartFracM2Big[SMART_STATE_COUNT] = { 40, 60, 80, 100, 70, 100 };
/* 状态 GPU 天花板：只和现有全局/分类/应用 GPU 取更低，不抬帽。 */
static const int kSmartGpuPct[SMART_STATE_COUNT] = { 40, 45, 60, 85, 80, 100 };
static int fsm_ticks_from_s(int sec) {
  return sec > 0 ? (sec + kSmartTickSec - 1) / kSmartTickSec : 0;
}
/* assign_pct 每拍都会用同一份阶梯算 m；存下来给 smart_pct 用。 */
static int g_ladder_m;

struct smart_state {
  int  state;          /* 四档输出：0 空闲 / 1 轻载 / 2 中载 / 3 重载 */
  int  pending;
  int  pend_n;
  long switches;
  int  fsm;            /* 六态核心，决定百分比 */
  int  fsm_pending;
  int  fsm_pend_n;
  long fsm_enter_tick;
  long session_tick;
  long tick;
  int  wait;
  char reason[24];
  int  gaming_streak;
  int  cpu_est;
};
static struct smart_state g_sm;
static int g_sm_fast_n;

/* 每簇平均占用里最忙的一簇。全系统 /proc/stat 是 8 核求和 */
static int cluster_max_load(const int *sum, const int *cnt, int n) {
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (cnt[i] <= 0) continue;
    int avg = sum[i] / cnt[i];
    if (avg > best) best = avg;
  }
  return best;
}

static long g_cpu_prev_total[MAX_CORES], g_cpu_prev_idle[MAX_CORES];
static int g_cpu_prev_ok;
static long g_cpu_prev_ms;
static int g_cpu_overall = -1;
/* 超大核（最高阶梯、tierkey==0）那一簇的平均占用；-1 = 无效，不向下钳。 */
static int g_prime_busy = -1;
/* 超大核自己的有效档：跟 smart_target 用同一套迟滞，避免占用在 8/30/60 */
static int g_prime_state = -1;
/* 1 = 本圈 smart_tick 走切前台预热（亮屏 / cap→smart，不靠 fg_changed）。 */
static int g_smart_warmup;
#ifdef SF_TEST
static int g_test_busy = -2;  /* -2 = 走真实 cpu_busy_pct */
static int g_test_half = -1;  /* -1 = 走真实 clusters_below_half */
static int g_test_gpu = -1;
static int g_test_temp = -1;
static int g_test_screen = 1;
static int g_test_fg_not_game = 0;
static long g_test_now_ms = -1;
#endif
/* 分类状态在文件后段才定义；智能 FSM 只读它判断"前台不是游戏"。 */
static int g_cat_idx;

static int prime_cluster_busy(const int *sum, const int *cnt);

static long cpu_busy_now_ms(void) {
#ifdef SF_TEST
  if (g_test_now_ms >= 0) return g_test_now_ms;
#endif
  return mono_ms();
}

/* T = max(2×INTERVAL, 10) 秒，返回毫秒。 */
static long cpu_busy_stale_ms(int interval_s) {
  int t = interval_s * 2;
  if (t < 10) t = 10;
  return (long)t * 1000;
}

static int cpu_busy_gap_stale(long dt_ms, int interval_s) {
  return dt_ms < 0 || dt_ms > cpu_busy_stale_ms(interval_s);
}

/* 首次或长间隙：调用方重立计数器并返回 -1。 */
static int cpu_busy_must_rearm(long now_ms) {
  if (!g_cpu_prev_ok) {
    g_cpu_prev_ms = now_ms;
    return 1;
  }
  if (cpu_busy_gap_stale(now_ms - g_cpu_prev_ms, cfg.interval)) {
    g_cpu_prev_ms = now_ms;
    return 1;
  }
  return 0;
}

static void cpu_busy_invalidate(void) {
  g_cpu_prev_ok = 0;
  g_prime_busy = -1;
  g_prime_state = -1;
}

/* 有效档：仅 m!=1 且 tierkey==0（超大核）时按本簇占用向下钳。 */
static int smart_effective_state(int tierkey, int global_state,
                                int prime_state, int m) {
  int state = global_state;
  if (state < 0 || state >= SMART_STATE_COUNT) state = SMART_DEEP_IDLE;
  if (state >= SMART_SUSTAINED) return state;
  if (m == 1 || (tierkey & 3) != 0) return state;
  if (prime_state < 0 || prime_state > 3) return state;
  return prime_state < state ? prime_state : state;
}

static void smart_reset_idle(void) {
  g_sm.state = 0;
  g_sm.pending = 0;
  g_sm.pend_n = 0;
  g_sm_fast_n = 0;
  g_sm.fsm = SMART_DEEP_IDLE;
  g_sm.fsm_pending = SMART_DEEP_IDLE;
  g_sm.fsm_pend_n = 0;
  g_sm.fsm_enter_tick = 0;
  g_sm.session_tick = 0;
  g_sm.wait = 0;
  g_sm.reason[0] = 0;
  g_sm.gaming_streak = 0;
  g_sm.cpu_est = -1;
  cpu_busy_invalidate();
}

/* 仅 smart + 保持封顶路径把档收到空闲。返回 1 表示已清状态。 */
static int smart_on_enter_screen_off(int keep) {
  if (cfg.strategy != 1 || !keep) return 0;
  smart_reset_idle();
  return 1;
}

static void smart_request_warmup(void) {
  g_smart_warmup = 0;
  if (cfg.strategy == 1 && cfg.app_switch_tier > 0 && strcmp(g_state, "game") != 0)
    g_smart_warmup = 1;
}

static void smart_on_leave_screen_off(void) {
  cpu_busy_invalidate();
  smart_request_warmup();
}

/* STRATEGY 边沿：重置状态机与占用基线；切入 smart 时再按规则请求预热。 */
static void smart_on_strategy_edge(int prev, int now) {
  (void)prev;
  smart_reset_idle();
  if (now == 1) smart_request_warmup();
}


/* 采样一次；返回最忙簇的平均占用百分比。 */
static int cpu_busy_pct_ex(int skip_cpu0) {
  long total[MAX_CORES], idle[MAX_CORES];
  memset(total, 0, sizeof total);
  memset(idle, 0, sizeof idle);
  if (!core_stat_read(total, idle)) return -1;
  long now = cpu_busy_now_ms();
  if (cpu_busy_must_rearm(now)) {
    memcpy(g_cpu_prev_total, total, sizeof total);
    memcpy(g_cpu_prev_idle, idle, sizeof idle);
    g_cpu_prev_ok = 1;
    g_prime_busy = -1;
    return -1;
  }
  int sum[MAX_CORES] = {0}, cnt[MAX_CORES] = {0};
  int all_sum = 0, all_cnt = 0;
  /* CPU0 常年被系统/输入/WALT 占用，拿它判"簇忙不忙"会让静置永远进不了 */
  int skip0 = 0;
  if (skip_cpu0) {
    for (int c = 1; c < MAX_CORES; c++)
      if (total[c] > 0) { skip0 = 1; break; }
  }
  for (int c = 0; c < MAX_CORES; c++) {
    if (total[c] <= 0) continue;
    if (c == 0 && skip0) {
      g_cpu_prev_total[c] = total[c];
      g_cpu_prev_idle[c] = idle[c];
      continue;
    }
    int pol = core_policy_of(c);
    if (pol < 0 || pol >= MAX_CORES) continue;
    /* 计数器没动 = 这颗核 offline/没 tick，不能当成"0% 占用"拉低簇均值 */
    if (total[c] <= g_cpu_prev_total[c]) {
      g_cpu_prev_total[c] = total[c];
      g_cpu_prev_idle[c] = idle[c];
      continue;
    }
    int load = core_load_pct(total[c], idle[c],
                             g_cpu_prev_total[c], g_cpu_prev_idle[c]);
    sum[pol] += load;
    all_sum += load;
    all_cnt++;
    cnt[pol]++;
    g_cpu_prev_total[c] = total[c];
    g_cpu_prev_idle[c] = idle[c];
  }
  g_cpu_prev_ms = now;
  g_cpu_overall = all_cnt ? all_sum / all_cnt : -1;
  g_prime_busy = prime_cluster_busy(sum, cnt);
  return cluster_max_load(sum, cnt, MAX_CORES);
}

static int cpu_busy_pct(void) { return cpu_busy_pct_ex(0); }

static long g_gpu_busy_prev, g_gpu_busy_total_prev;
static int g_gpu_busy_prev_ok;

/* GPU 占用：优先百分比节点，其次 KGSL 计数差分，最后 MTK 的 util。 */
static int gpu_busy_pct(void) {
  char b[64];
  if (!read_file("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", b, sizeof b)) {
    int v = atoi(b);
    if (v >= 0 && v <= 100) return v;
  }
  if (!read_file("/sys/class/kgsl/kgsl-3d0/gpubusy", b, sizeof b)) {
    long busy = 0, total = 0;
    if (sscanf(b, "%ld %ld", &busy, &total) == 2) {
      if (!g_gpu_busy_prev_ok || busy < g_gpu_busy_prev ||
          total <= g_gpu_busy_total_prev) {
        g_gpu_busy_prev = busy;
        g_gpu_busy_total_prev = total;
        g_gpu_busy_prev_ok = 1;
        return -1;
      }
      long db = busy - g_gpu_busy_prev;
      long dt = total - g_gpu_busy_total_prev;
      g_gpu_busy_prev = busy;
      g_gpu_busy_total_prev = total;
      if (dt > 0) return (int)(db * 100 / dt);
    }
  }
  if (!read_file("/sys/kernel/ged/hal/gpu_utilization", b, sizeof b)) {
    int v = atoi(b);
    if (v >= 0 && v <= 100) return v;
  }
  return -1;
}

/* 皮肤/外壳温：只有物理 shell/skin/back 传感器才算；找不到就返回 -1。 */
static char g_skin_path[192];
static int g_skin_probed;
static int norm_temp_c(long v) {
  if (v <= 0) return -1;
  if (v > 100000) v /= 1000;
  else if (v > 1000) v /= 100;
  else if (v > 200) v /= 10;
  return v >= 5 && v <= 120 ? (int)v : -1;
}
static int skin_temp_c(void) {
  if (!g_skin_probed) {
    g_skin_probed = 1;
    int best = 99;
    for (int z = 0; z < 64; z++) {
      char tp[192], type[64] = {0};
      snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/type", z);
      if (read_file(tp, type, sizeof type)) continue;
      for (char *q = type; *q; q++) *q = (char)tolower((unsigned char)*q);
      if (strstr(type, "cpu") || strstr(type, "gpu") || strstr(type, "soc") ||
          strstr(type, "batt") || strstr(type, "trip")) continue;
      int prio = strstr(type, "shell") ? 1
               : (strstr(type, "skin") || strstr(type, "cover")) ? 2
               : (strstr(type, "back") || strstr(type, "frame")) ? 3 : 0;
      if (!prio || prio >= best) continue;
      snprintf(tp, sizeof tp, "/sys/class/thermal/thermal_zone%d/temp", z);
      if (norm_temp_c(read_num(tp)) < 0) continue;
      snprintf(g_skin_path, sizeof g_skin_path, "%s", tp);
      best = prio;
      if (prio == 1) break;
    }
  }
  return g_skin_path[0] ? norm_temp_c(read_num(g_skin_path)) : -1;
}

/* PSI CPU some avg10，直接读，不依赖实时偏移是否启用。 */
static int psi_cpu_pct(void) {
  char b[192];
  if (read_file("/proc/pressure/cpu", b, sizeof b)) return -1;
  const char *p = strstr(b, "some");
  if (!p || !(p = strstr(p, "avg10="))) return -1;
  double v = atof(p + 6);
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  return (int)(v + 0.5);
}

/* ADPF 热余量：无 companion 写入时忽略；新鲜度 30s。 */
static int adpf_headroom(void) {
  const char *p = SF_DATADIR "/adpf";
  struct stat st;
  if (stat(p, &st) || (long)time(NULL) - (long)st.st_mtime > 30) return -1;
  long v = read_num(p);
  return v >= 0 && v <= 100 ? (int)v : -1;
}

static int g_fsm_psi = -1, g_fsm_skin = -1, g_fsm_adpf = -1;

/* 纯函数：busy% + 当前档 → 目标档。单测直接喂数。 */
static int smart_target(int cur, int busy) {
  if (busy < 0) return cur;
  int want;
  if (busy >= 60) want = 3;
  else if (busy >= 30) want = 2;
  else if (busy >= 8) want = 1;
  else want = 0;
  if (want < cur) {
    int lo = (cur == 3) ? 55 : (cur == 2) ? 25 : 4;
    if (busy > lo) want = cur;          /* 没跌破退档阈值：不动 */
  }
  return want;
}

/* 超大核自己的档位：与全局同一套阈值和迟滞，避免占用在 8/30/60 */
static int smart_prime_state_step(int prev, int busy) {
  if (busy < 0) return prev;
  if (prev < 0) prev = 3;
  return smart_target(prev, busy);
}

/* 这一档、这个 tier 的目标百分比（已按用户设的天花板夹过）。 */
static int smart_pct_of(int state, int ceil) {
  if (state < 0 || state >= SMART_STATE_COUNT) state = SMART_DEEP_IDLE;
  if (ceil < 1) ceil = 1;
  if (ceil > 100) ceil = 100;
  int v = ceil * kSmartFrac[state] / 100;
  return v < 1 ? 1 : v;
}

/* 带拓扑的版本：m==2 时超大核与主簇分开缩放。 */
static int smart_pct_tier(int tierkey, int m) {
  int state = smart_effective_state(tierkey, g_sm.fsm, g_prime_state, m);
  int ceil = cfg.pct[tierkey & 3];
  if (ceil < 1) ceil = 1;
  if (ceil > 100) ceil = 100;
  int ov = cfg.fsm_cpu[state][((tierkey & 3) == 0) ? 0 : 1];
  if (ov > 0) {
    int v = ceil * ov / 100;
    return v < 1 ? 1 : v;
  }
  if (m != 2) return smart_pct_of(state, ceil);   /* m=1/3/4 走通用比例 */
  const int *tab = ((tierkey & 3) == 0) ? kSmartFracM2Big : kSmartFracM2Low;
  int v = ceil * tab[state] / 100;
  return v < 1 ? 1 : v;
}

static int smart_pct(int tierkey) {
  return smart_pct_tier(tierkey, g_ladder_m);
}

/* 每拍推进一次。app_switch=1 表示这一拍检测到前台包名换了。 */
static int freq_below_half(long cur, long cap) {
  return cur > 0 && cap > 0 && cur * 2 <= cap;
}

static int clusters_below_half(void);

static const char *smart_state_cn(int s) {
  static const char *const n[SMART_STATE_COUNT] = {
    "深度空闲", "浅空闲", "中载", "重载", "持续高载", "游戏"
  };
  return n[s < 0 || s >= SMART_STATE_COUNT ? SMART_DEEP_IDLE : s];
}

static void smart_fsm_enter(int s, const char *why) {
  g_sm.fsm = s;
  snprintf(g_sm.reason, sizeof g_sm.reason, "%s", why ? why : "");
  g_sm.fsm_pending = s;
  g_sm.fsm_pend_n = 0;
  g_sm.fsm_enter_tick = g_sm.tick;
  g_sm.wait = 0;
}

/* 六态核心的期望状态。temp/gpu/cpu 任一读不到就按 -1 跳过该指标。 */
static int smart_fsm_desired(int cpu, int gpu, int temp, int fg_not_game,
                             int screen_on) {
  if (!screen_on) return SMART_DEEP_IDLE;
  int cur = g_sm.fsm;
  int cpu_hi_in = cfg.fsm_cpu_hi_in > 0 ? cfg.fsm_cpu_hi_in : 80;
  int cpu_hi_out = cfg.fsm_cpu_hi_out > 0 ? cfg.fsm_cpu_hi_out : 70;
  int cpu_mod_in = cfg.fsm_cpu_mod_in > 0 ? cfg.fsm_cpu_mod_in : 60;
  int cpu_mod_out = cfg.fsm_cpu_mod_out > 0 ? cfg.fsm_cpu_mod_out : 45;
  int cpu_heavy_th = cur >= SMART_HEAVY ? cpu_hi_out : cpu_hi_in;
  int cpu_mod_th = cur >= SMART_MODERATE ? cpu_mod_out : cpu_mod_in;
  int cpu_heavy = cpu >= 0 && cpu >= cpu_heavy_th;
  int cpu_mod = cpu >= 0 && cpu >= cpu_mod_th;
  if (g_fsm_psi >= 0 && cfg.fsm_psi_hi > 0 && g_fsm_psi >= cfg.fsm_psi_hi)
    cpu_heavy = 1;
  else if (g_fsm_psi >= 0 && cfg.fsm_psi_mod > 0 && g_fsm_psi >= cfg.fsm_psi_mod)
    cpu_mod = 1;
  int cpu_some = cpu >= 0 && cpu >= 50;
  int gpu_game_in = cfg.fsm_gpu_game_in > 0 ? cfg.fsm_gpu_game_in : 90;
  int gpu_game_out = cfg.fsm_gpu_game_out > 0 ? cfg.fsm_gpu_game_out : 78;
  int gpu_hi_in = cfg.fsm_gpu_hi_in > 0 ? cfg.fsm_gpu_hi_in : 80;
  int gpu_hi_out = cfg.fsm_gpu_hi_out > 0 ? cfg.fsm_gpu_hi_out : 65;
  int gpu_heavy_th = cur >= SMART_HEAVY ? gpu_hi_out : gpu_hi_in;
  int gpu_heavy = gpu >= 0 && gpu >= gpu_heavy_th;
  int gaming_by_gpu = gpu >= gpu_game_in && !fg_not_game;

  if (gaming_by_gpu) {
    if (g_sm.gaming_streak < 6) g_sm.gaming_streak++;
  } else if (gpu >= 0 && gpu < gpu_game_out) {
    g_sm.gaming_streak = 0;
  }

  int d;
  if (gaming_by_gpu)
    d = g_sm.gaming_streak >= 6 ? SMART_GAMING : SMART_HEAVY;
  else if (cpu_heavy || (gpu_heavy && cpu_some))
    d = SMART_HEAVY;
  else if (cpu_mod || gpu_heavy)
    d = SMART_MODERATE;
  else
    d = SMART_LIGHT_IDLE;

  /* 预热期不吃热降档；GAMING 不因温度降级。 */
  int warm_ticks = fsm_ticks_from_s(cfg.fsm_warmup_s > 0 ? cfg.fsm_warmup_s : kSmartWarmupTicks * kSmartTickSec);
  int warming = g_sm.session_tick > 0 &&
                g_sm.tick - g_sm.session_tick < warm_ticks;
  int skin_hot = cfg.fsm_skin_hi > 0 && g_fsm_skin >= cfg.fsm_skin_hi;
  int skin_hold = cfg.fsm_skin_lo > 0 && g_fsm_skin >= cfg.fsm_skin_lo;
  int adpf_hot = cfg.fsm_adpf_hi > 0 && g_fsm_adpf >= cfg.fsm_adpf_hi;
  if (!warming && d == SMART_HEAVY &&
      ((cfg.ad.th && temp >= 0 && temp >= cfg.ad.th_high) || skin_hot || adpf_hot))
    d = SMART_SUSTAINED;
  else if (!warming && cur == SMART_SUSTAINED &&
           ((cfg.ad.th && temp >= 0 && temp >= cfg.ad.th_low) || skin_hold || adpf_hot))
    d = SMART_SUSTAINED;
  return d;
}

static int smart_tick_ex(int busy, int gpu, int temp, int screen_on,
                         int app_switch, int below_half, int fg_not_game) {
  g_sm.tick++;
  g_sm.wait = 0;
  if (screen_on && g_sm.session_tick == 0) g_sm.session_tick = g_sm.tick;
  if (!screen_on) g_sm.session_tick = 0;

  int cpu = busy;
  if (busy >= 0) {
    g_sm.cpu_est = (g_sm.cpu_est < 0 || busy >= g_sm.cpu_est)
                     ? busy : (g_sm.cpu_est + busy) / 2;
    cpu = g_sm.cpu_est;
  }

  int want = smart_fsm_desired(cpu, gpu, temp, fg_not_game, screen_on);
  int changed = 0, forced = 0;

  if (app_switch && cfg.app_switch_tier > 0) {
    int target = cfg.app_switch_tier == 1 ? SMART_LIGHT_IDLE
               : cfg.app_switch_tier == 2 ? SMART_MODERATE : SMART_HEAVY;
    if (want > target) target = want;
    if (target > g_sm.fsm) {
      smart_fsm_enter(target, "预热");
      g_sm_fast_n = 0;
      changed = forced = 1;
      log("智能模式：切前台，预热到%s（大核 %d%% / 其余 %d%%）",
          smart_state_cn(g_sm.fsm), smart_pct(0), smart_pct(3));
    }
  }

  if (!forced && want <= SMART_LIGHT_IDLE && g_sm.fsm >= SMART_MODERATE &&
      below_half) {
    if (++g_sm_fast_n >= 2) {
      smart_fsm_enter(want, "静置");
      g_sm_fast_n = 0;
      changed = forced = 1;
      log("智能模式：静置且各簇低于半帽 → %s（大核 %d%% / 其余 %d%%）",
          smart_state_cn(g_sm.fsm), smart_pct(0), smart_pct(3));
    }
  } else if (!forced) {
    g_sm_fast_n = 0;
  }

  if (!forced) {
    if (want != g_sm.fsm_pending) {
      g_sm.fsm_pending = want;
      g_sm.fsm_pend_n = 0;
    } else {
      g_sm.fsm_pend_n++;
    }
    int up_window = fsm_ticks_from_s(cfg.fsm_up_s > 0 ? cfg.fsm_up_s : kSmartUpWindow * kSmartTickSec);
    int down_window = fsm_ticks_from_s(cfg.fsm_down_s > 0 ? cfg.fsm_down_s : kSmartDownWindow * kSmartTickSec);
    int pair_window = fsm_ticks_from_s(cfg.fsm_pair_s > 0 ? cfg.fsm_pair_s : kSmartPairWindow * kSmartTickSec);
    if (up_window < 1) up_window = 1;
    if (down_window < 1) down_window = 1;
    if (pair_window < 1) pair_window = 1;
    int window = want > g_sm.fsm ? up_window : down_window;
    if ((g_sm.fsm == SMART_MODERATE &&
         (want == SMART_HEAVY || want == SMART_SUSTAINED)) ||
        ((g_sm.fsm == SMART_HEAVY || g_sm.fsm == SMART_SUSTAINED) &&
         want == SMART_MODERATE))
      window = pair_window;

    int can_leave = 1;
    int thermal_hold = (cfg.ad.th && temp >= 0 && temp >= cfg.ad.th_low) ||
                       (cfg.fsm_skin_lo > 0 && g_fsm_skin >= cfg.fsm_skin_lo) ||
                       (cfg.fsm_adpf_hi > 0 && g_fsm_adpf >= cfg.fsm_adpf_hi);
    if (g_sm.fsm == SMART_SUSTAINED && want < SMART_SUSTAINED &&
        thermal_hold &&
        g_sm.tick - g_sm.fsm_enter_tick <
          (fsm_ticks_from_s(cfg.fsm_thermal_hold_s > 0 ? cfg.fsm_thermal_hold_s : kSmartThermalHoldTicks * kSmartTickSec)))
      can_leave = 0;
    if (can_leave && want < g_sm.fsm) {
      int dwell = g_sm.fsm == SMART_HEAVY
                    ? fsm_ticks_from_s(cfg.fsm_dwell_heavy_s > 0 ? cfg.fsm_dwell_heavy_s : kSmartDwellHeavy * kSmartTickSec)
                  : g_sm.fsm == SMART_GAMING
                    ? fsm_ticks_from_s(cfg.fsm_dwell_game_s > 0 ? cfg.fsm_dwell_game_s : kSmartDwellGaming * kSmartTickSec)
                  : g_sm.fsm == SMART_SUSTAINED
                    ? fsm_ticks_from_s(cfg.fsm_dwell_sustain_s > 0 ? cfg.fsm_dwell_sustain_s : kSmartDwellSustained * kSmartTickSec)
                    : 0;
      if (dwell && g_sm.tick - g_sm.fsm_enter_tick < dwell) can_leave = 0;
    }
    if (want != g_sm.fsm)
      g_sm.wait = window > g_sm.fsm_pend_n ? window - g_sm.fsm_pend_n : 1;
    if (can_leave && g_sm.fsm_pend_n >= window && want != g_sm.fsm) {
      const char *why = want == SMART_SUSTAINED ? "热降" :
                        (want > g_sm.fsm ? "升载" : "回载");
      smart_fsm_enter(want, why);
      g_sm.switches++;
      changed = 1;
    }
  }

  g_sm.state = g_sm.fsm;
  g_sm.pending = g_sm.fsm;
  g_sm.pend_n = 0;
  if (changed)
    log("智能模式：负载 %d%% / GPU %d%% → %s（大核 %d%% / 其余 %d%%）",
        busy, gpu, smart_state_cn(g_sm.fsm), smart_pct(0), smart_pct(3));
  return changed;
}

static int smart_tick(int app_switch) {
  if (g_smart_warmup) {
    app_switch = 1;
    g_smart_warmup = 0;
  }
  int preheat = app_switch && cfg.app_switch_tier > 0;
  if (preheat) g_prime_state = -1;
#ifdef SF_TEST
  if (g_test_busy != -2)
    return smart_tick_ex(g_test_busy, g_test_gpu, g_test_temp, g_test_screen,
                         app_switch, g_test_half >= 0 ? g_test_half : 0,
                         g_test_fg_not_game);
#endif
  int busy = cpu_busy_pct();
  int cpu = g_cpu_overall >= 0 ? g_cpu_overall : busy;
  if (busy >= 0 && !preheat)
    g_prime_state = smart_prime_state_step(g_prime_state, g_prime_busy);
  int below = g_sm.fsm >= SMART_MODERATE ? clusters_below_half() : 0;
  struct adapt a;
  adapt_snapshot(&a);
  int temp = (cfg.ad.th && a.temp_m > 0) ? a.temp_m / 1000 : -1;
  g_fsm_psi = psi_cpu_pct();
  g_fsm_skin = skin_temp_c();
  g_fsm_adpf = adpf_headroom();
  return smart_tick_ex(cpu, gpu_busy_pct(), temp, !g_screen_off,
                       app_switch, below, g_cat_idx >= 0);
}

/* 按簇阶梯分档：最高档 big，次高 mid2，中间 mid，最低 little */
/* 生效封顶 = 用户设的值 + 实时偏移（adapt.c 每轮算好放在快照里）。 */
/* 生效偏移。智能模式自己按负载调目标，不再叠 PSI 的向上借额度 */
static int effective_delta(const struct adapt *a) {
  if (cfg.strategy != 1) return a->delta;
  /* 智能模式：温控交给 FSM，实时偏移只保留电量收紧。 */
  int d = -a->batt_cut;
  if (d > cfg.ad.up_max) d = cfg.ad.up_max;
  if (d < -cfg.ad.down_max) d = -cfg.ad.down_max;
  return d;
}

static int eff_pct(int base, int per_app) {
  struct adapt a;
  adapt_snapshot(&a);
  /* 电量阶梯也属于"全局让路"，不盖用户给某个应用明确设的档位。 */
  int d = effective_delta(&a);
  if (per_app) d += a.batt_cut;
  /* 充电地板只抬"放宽侧"：本拍有收紧（温控/电量）时不抬。 */
  /* 熄屏不吃充电地板：夜里插电是"发热 + 待机"的主场景，白天插电才要性能。 */
  if (!per_app && !g_screen_off && a.chg_on && a.chg_pct > base && d >= 0)
    base = a.chg_pct;
  base += d;
  if (base < 1) base = 1;
  if (base > 100) base = 100;
  return base;
}

static void assign_pct(struct policy *p, int n) {
  long ladder[MAX_POLICY];
  int m = build_ladder(p, n, ladder);
  g_ladder_m = m;
  for (int i = 0; i < n; i++) {
    int tier = 0;
    for (int j = 0; j < m; j++)
      if (ladder[j] == p[i].hw_max) tier = j;
    int pct, tierkey;
    tierkey = tier_key_of(ladder, m, p[i].hw_max);
    /* 键到值的下标和 tierkey 一样（cfg.pct 的下标就是 BIG/MID2/MID/LITTLE） */
    pct = (cfg.strategy == 1) ? smart_pct(tierkey) : cfg.pct[tierkey];
    /* 超大核压到最低频：不关闭核心，只把 BIG 簇的目标放到 1%。 */
    int big_min_hit = (cfg.big_min && tierkey == 0);
    if (big_min_hit) pct = 1;
    if (pct < 1 || pct > 100) pct = 1;
    p[i].tpct = pct;
    /* big_min 的目标就是最低频：充电地板、PSI 借档都不许把它抬起来 */
    int eff = big_min_hit ? 1 : eff_pct(pct, 0);
    p[i].pct = eff;
    p[i].tier = tierkey;
    long vendor = p[i].vendor_max;
    p[i].cap = cap_floor_apply(&p[i], snap_freq(&p[i], cap_from_pct(&p[i], eff)));
    log("  %-8s 档位%d/%d 厂商%ld 硬件%ld 意图%d%% 生效%d%% 封顶%ld (%.1f%%)",
        p[i].name, tier, m - 1, vendor, p[i].hw_max, pct, eff,
        p[i].cap, p[i].hw_max > 0 ? 100.0 * p[i].cap / p[i].hw_max : 0.0);
  }
}

/* GPU 探测（高通 KGSL + MTK Mali）                                   */

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
  /* 老 MTK：GED custom_upbound_gpu_freq 写 OPP index，不是频率 */
  int ged;
  int opidx[MAX_NODE];
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

/* MTK gpufreqv2 */
/* stack_working_opp_table 一行形如 "[00] freq: 1716000, volt: ..." */
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

/* 老 MTK 的 OPP dump：一行形如 "[07] freq = 900000, volt = 75000"。 */
static int mtk_old_parse_opp_text(const char *text, long *freq, int *opidx, int max) {
  if (!text) return 0;
  char *copy = strdup(text);
  if (!copy) return 0;
  char *save = NULL;
  int n = 0;
  for (char *ln = strtok_r(copy, "\n", &save); ln && n < max;
       ln = strtok_r(NULL, "\n", &save)) {
    char *p = strchr(ln, '[');
    if (!p) continue;
    int idx = atoi(p + 1);
    char *f = strstr(ln, "freq");
    if (!f) continue;
    f = strchr(f, '=');
    if (!f) continue;
    f++;
    while (*f == ' ' || *f == '\t') f++;
    if (!isdigit((unsigned char)*f)) continue;
    long v = strtol(f, NULL, 10);
    if (v <= 0) continue;
    if (v < 10000000L) v *= 1000;   /* kHz -> Hz */
    freq[n] = v;
    opidx[n] = idx;
    n++;
  }
  free(copy);
  return n;
}

static int mtk_old_parse_opp(const char *path, long *freq, int *opidx, int max) {
  char b[16384];
  if (read_file(path, b, sizeof b)) return 0;
  return mtk_old_parse_opp_text(b, freq, opidx, max);
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

/* readonly=1：只探测，不碰任何节点。 */
static int detect_gpu(int readonly) {
  /* 上次被 kill -9 留下的 cap 先还原，别把"37/56"当成原始行再存一遍。 */
  if (!readonly) mtk_restore_snapshot();
  memset(&g_gpu, 0, sizeof g_gpu);
  long tbl[MAX_NODE];
  int opidx[MAX_NODE] = {0};
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
  /* 老 MTK：没有 devfreq 频率表时，用 gpufreq_opp_dump + GED 上限。 */
  if (!n && !access(MTK_OLD_OPP, F_OK) && !access(MTK_GED_UPBOUND, F_OK)) {
    n = mtk_old_parse_opp(MTK_OLD_OPP, tbl, opidx, MAX_NODE);
    if (n) {
      g_gpu.ged = 1;
      log("GPU 频率表: MTK 老 OPP %s（GED 上限）", MTK_OLD_OPP);
    }
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
        int ti = opidx[i];
        opidx[i] = opidx[j];
        opidx[j] = ti;
      }
  memcpy(g_gpu.tbl, tbl, sizeof(long) * n);
  memcpy(g_gpu.opidx, opidx, sizeof(int) * n);
  g_gpu.ntbl = n;
  if (g_gpu.ged) {
    snprintf(g_gpu.max_node, sizeof g_gpu.max_node, "%s", MTK_GED_UPBOUND);
    g_gpu.ok = 1;
    log("GPU: MTK 老 GED 上限=%s 硬件%ld~%ldHz 封顶%ldHz",
        g_gpu.max_node, g_gpu.min_hz, g_gpu.max_hz, g_gpu.normal);
    return 0;
  }
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

/* 百分比吸附到不高于目标值的最高频点。和 detect_gpu 挑 normal 是同一套规则 */
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

/* GED custom_upbound_gpu_freq 收的是 OPP 序号，不是频率。 */
static int gpu_ged_idx_for(long hz) {
  for (int i = 0; i < g_gpu.ntbl; i++)
    if (g_gpu.tbl[i] <= hz) return g_gpu.opidx[i];
  return -1;
}

static long gpu_ged_freq_of(int idx) {
  for (int i = 0; i < g_gpu.ntbl; i++)
    if (g_gpu.opidx[i] == idx) return g_gpu.tbl[i];
  return -1;
}

/* 按节点单位换算后写入；解锁时只认写成功，被厂商钳制就如实报告不绕路 */
static const char *write_gpu(long want_hz, int unlock) {
  long got;
  if (g_gpu.ged) {
    long val;
    if (unlock) {
      if (!journal_value(NODES_JOURNAL, g_gpu.max_node, &val)) val = 0;
    } else {
      int idx = gpu_ged_idx_for(want_hz);
      if (idx < 0) return "failed";
      val = idx;
    }
    if (!write_journaled(NODES_JOURNAL, g_gpu.max_node, val)) {
      long got_idx = read_num(g_gpu.max_node);
      if (unlock) {
        if (got_idx == val) return "unlocked";
        log("WARN GPU 解锁受限 期望 OPP %ld 实际 %ld", val, got_idx);
        return "restricted";
      }
      long got_hz = gpu_ged_freq_of((int)got_idx);
      if (got_hz > 0 && got_hz <= want_hz) return "capped";
    }
    return "failed";
  }
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

/* 把 GPU 下限钉回最低档，压回被 boost/触摸抬高的地板。 */
static long g_gmin_writes;   /* 真正写节点的次数，status 里报出来用于验收 */
/* 初值 -600：mono_sec() 从开机起算，初值 0 会让开机头 10 分钟一条 RESTORE 都不出。 */
static long g_gmin_log_at = -600;   /* 上一行 RESTORE 日志的时刻：写进去又被盖回时不刷屏 */

static void gpu_min_to_floor(void) {
  /* MTK 的下限在同一条 set 命令里写；没有独立 min 节点。 */
  if (g_gpu.mtk) return;
  if (!cfg.gpu_min_lock || !g_gpu.min_node[0] || g_gpu.min_hz <= 0) return;
  long cur = read_num(g_gpu.min_node);
  if (cur > 0 && g_gpu.min_is_mhz) cur *= 1000000;
  if (cur <= 0) return;                 /* 读不到就别写，免得把下限写成 0 */
  if (cur <= g_gpu.min_hz) return;      /* 已经在下限：这一拍什么都不做 */
  long val = g_gpu.min_is_mhz ? g_gpu.min_hz / 1000000 : g_gpu.min_hz;
  if (write_journaled(NODES_JOURNAL, g_gpu.min_node, val)) return;
  g_gmin_writes++;
  long got = read_num(g_gpu.min_node);
  if (got > 0 && g_gpu.min_is_mhz) got *= 1000000;
  /* 写完还高于下限才算异常（被厂商钳制）。"highest floor wins" 的内核上 */
  if (got > g_gpu.min_hz) {
    long now = mono_sec();
    if (now - g_gmin_log_at >= 600) {
      g_gmin_log_at = now;
      log("RESTORE GPU 下限 %ldHz -> %ldHz", got, g_gpu.min_hz);
    }
  }
}

/* GPU Boost 源                                                       */

/* 「禁止 GPU Boost」不是改频率帽，而是把厂商抬频的源头关掉 */
#define MTK_BOOST_NODE "/sys/kernel/ged/hal/custom_boost_gpu_freq"
#define MTK_OPP_V1     "/proc/gpufreq/gpufreq_opp_dump"

static const char *g_boost_nodes[] = {
    "/sys/class/kgsl/kgsl-3d0/dcvs_boost",
    "/sys/class/kgsl/kgsl-3d0/touch_wake",
    "/sys/class/kgsl/kgsl-3d0/force_no_nap",
    "/sys/module/devfreq_boost/parameters/devfreq_input_boost_duration_ms",
    "/sys/module/devfreq_boost/parameters/devfreq_wake_boost_duration_ms",
};

/* 高通 KGSL 空闲计时器：默认 80ms，即停止渲染后 GPU 还要空转 80ms */
#define KGSL_IDLE_TIMER "/sys/class/kgsl/kgsl-3d0/idle_timer"
#define KGSL_IDLE_MS   48

static void kgsl_idle_trim(void) {
  if (!cfg.gpu_boost_off) return;
  if (access(KGSL_IDLE_TIMER, F_OK)) return;
  long orig;
  if (journal_value(NODES_JOURNAL, KGSL_IDLE_TIMER, &orig)) {
    if (orig <= KGSL_IDLE_MS) return;   /* 出厂就比目标短：不升回去 */
  } else {
    long cur = read_num(KGSL_IDLE_TIMER);
    if (cur <= 0 || cur <= KGSL_IDLE_MS) return;
  }
  if (read_num(KGSL_IDLE_TIMER) == KGSL_IDLE_MS) return;
  write_journaled(NODES_JOURNAL, KGSL_IDLE_TIMER, KGSL_IDLE_MS);
}

/* 天玑的 boost 节点要的是 OPP 索引，而索引排序各内核不一定一致 */
static int mtk_opp_lowest_index_text(const char *text) {
  if (!text) return -1;
  char *copy = strdup(text);
  if (!copy) return -1;
  int best_idx = -1;
  long best_hz = 0;
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *lb = strchr(line, '[');
    if (!lb) continue;
    char *end = NULL;
    long idx = strtol(lb + 1, &end, 10);
    if (end == lb + 1 || *end != ']' || idx < 0 || idx > 255) continue;
    char *f = strstr(line, "freq");        /* v2 是 freq:，v1 是 freq = */
    if (!f) continue;
    f += 4;
    while (*f == ':' || *f == '=' || *f == ' ' || *f == '\t') f++;
    if (!isdigit((unsigned char)*f)) continue;
    long v = strtol(f, NULL, 10);
    if (v <= 0) continue;
    if (v < 10000000L) v *= 1000;          /* kHz -> Hz */
    if (best_idx < 0 || v < best_hz) { best_hz = v; best_idx = (int)idx; }
  }
  free(copy);
  return best_idx;
}

static int mtk_boost_lowest_index(void) {
  char b[16384];
  if (!read_file(MTK_OPP, b, sizeof b)) return mtk_opp_lowest_index_text(b);
  if (!read_file(MTK_OPP_V1, b, sizeof b)) return mtk_opp_lowest_index_text(b);
  return -1;
}

static void boost_restore_path(const char *p) {
  if (access(p, F_OK)) return;
  long orig;
  if (!journal_value(NODES_JOURNAL, p, &orig)) return;
  if (read_num(p) == orig) return;
  write_num(p, orig);
  int m = journal_mode(NODES_JOURNAL, p);
  if (m >= 0) chmod(p, (mode_t)m);
}

static void boost_off_apply(void) {
  if (!cfg.gpu_boost_off) return;
  int n = 0;
  for (size_t i = 0; i < sizeof g_boost_nodes / sizeof *g_boost_nodes; i++) {
    const char *p = g_boost_nodes[i];
    if (access(p, F_OK)) continue;
    if (read_num(p) == 0) continue;          /* 已经是关的：不重复写 */
    if (!write_journaled(NODES_JOURNAL, p, 0)) n++;
  }
  /* 天玑：把 boost 目标改成最低频档。表读不到就不写，不猜索引。 */
  if (!access(MTK_BOOST_NODE, F_OK)) {
    static int idx = -2;                     /* -2 = 还没算过 */
    if (idx == -2) idx = mtk_boost_lowest_index();
    long cur = read_num(MTK_BOOST_NODE);
    if (idx >= 0 && cur >= 0 && cur != idx) {
      if (!write_journaled(NODES_JOURNAL, MTK_BOOST_NODE, idx)) n++;
    }
  }
  kgsl_idle_trim();
  if (n) log("GPU Boost 已关闭（%d 个节点）", n);
}

/* 只还原模块真正改过的节点（账本里有才动），常态/交权都会调。 */
/* 旧版本/崩溃残留的 boost 节点可能仍是 0，账本没了就没人还原。 */
static void touch_boost_guard(void) {
  static const struct { const char *path; long want; } n[] = {
      {"/sys/class/kgsl/kgsl-3d0/touch_wake", 1},
      {"/sys/class/kgsl/kgsl-3d0/dcvs_boost", 1},
      {"/proc/sys/walt/input_boost/input_boost_ms", 100},
      {"/proc/sys/walt/input_boost_ms", 100},
  };
  int fixed = 0;
  for (size_t i = 0; i < sizeof n / sizeof *n; i++) {
    const char *p = n[i].path;
    if (access(p, F_OK) || read_num(p) != 0) continue;
    long orig;
    if (journal_value(NODES_JOURNAL, p, &orig) && orig == 0) continue;
    if (write_num(p, n[i].want) == 0) fixed++;
  }
  if (fixed) log("游戏前恢复触摸 boost（%d 个节点）", fixed);
}

static void boost_off_restore(void) {
  for (size_t i = 0; i < sizeof g_boost_nodes / sizeof *g_boost_nodes; i++)
    boost_restore_path(g_boost_nodes[i]);
  boost_restore_path(MTK_BOOST_NODE);
  boost_restore_path(KGSL_IDLE_TIMER);
}

/* CPU Boost 中和（walt 输入/调度拉频源） */
static const char *g_cpu_boost_nodes[] = {
    /* 内核把 input_boost 收进子目录（一加 13 / ColorOS 16 实测） */
    "/proc/sys/walt/input_boost/input_boost_ms",
    /* 其他内核的平铺布局 */
    "/proc/sys/walt/input_boost_ms",
    "/proc/sys/walt/sched_per_task_boost",
    "/proc/sys/walt/sched_boost",
};

static long g_boost_log_at = -600;

static void suppress_boost_apply(void) {
  int n = 0;
  for (size_t i = 0; i < sizeof g_cpu_boost_nodes / sizeof *g_cpu_boost_nodes; i++) {
    const char *p = g_cpu_boost_nodes[i];
    if (access(p, F_OK)) continue;
    if (read_num(p) == 0) continue;
    if (!write_journaled(NODES_JOURNAL, p, 0)) n++;
  }
  if (n) {
    long now = mono_sec();
    if (now - g_boost_log_at >= 600) {
      g_boost_log_at = now;
      log("CPU Boost 已中和（%d 个 walt 节点）", n);
    }
  }
}

static void suppress_boost_restore(void) {
  for (size_t i = 0; i < sizeof g_cpu_boost_nodes / sizeof *g_cpu_boost_nodes; i++)
    boost_restore_path(g_cpu_boost_nodes[i]);
}

/* 动态封顶的三档：0=最优解，1=风格中间档，2=风格地板。 */
#define SUP_OPT     0
#define SUP_MID_LV  1
#define SUP_FLOOR_LV 2
static int g_sup_level;     /* 0=最优解，1=风格中间档，2=风格地板 */
static int sup_profile_level(void);

/* 任务组的 uclamp.min：把前台/系统的"最低性能请求"压到 0。 */
static const char *g_sup_uclamp_nodes[] = {
    "/dev/cpuctl/top-app/cpu.uclamp.min",
    "/dev/cpuctl/system/cpu.uclamp.min",
    "/dev/cpuctl/cpu/cpu.uclamp.min",
    "/dev/cpuctl/foreground/cpu.uclamp.min",
    "/dev/cpuctl/background/cpu.uclamp.min",
};

static void suppress_uclamp_apply(void) {
  int n = 0;
  for (size_t i = 0; i < sizeof g_sup_uclamp_nodes / sizeof *g_sup_uclamp_nodes; i++) {
    const char *p = g_sup_uclamp_nodes[i];
    if (access(p, F_OK)) continue;
    if (read_num(p) == 0) continue;
    if (!write_journaled(NODES_JOURNAL, p, 0)) n++;
  }
  if (n) log("调度下限已压低（%d 个 uclamp 节点）", n);
}

static void suppress_uclamp_restore(void) {
  for (size_t i = 0; i < sizeof g_sup_uclamp_nodes / sizeof *g_sup_uclamp_nodes; i++)
    boost_restore_path(g_sup_uclamp_nodes[i]);
}

/* 字符串账本：uclamp.max 回读是 "max"/"50.00"，cpuset 是 "0-5" */
static int str_journal_has(const char *file, const char *path) {
  FILE *f = fopen(file, "r");
  if (!f) return 0;
  char ln[640], p[512], v[64];
  int mode, hit = 0;
  while (fgets(ln, sizeof ln, f)) {
    if (sscanf(ln, "%511s %63s %o", p, v, &mode) != 3) continue;
    if (!strcmp(p, path)) { hit = 1; break; }
  }
  fclose(f);
  return hit;
}

static int str_journal_add(const char *file, const char *path, const char *val,
                           int mode) {
  FILE *f = fopen(file, "a");
  if (!f) return -1;
  fprintf(f, "%s %s %o\n", path, val, mode);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  return 0;
}

static int str_write_journaled(const char *file, const char *path,
                               const char *val) {
  if (access(path, F_OK)) return -1;
  if (!str_journal_has(file, path)) {
    char cur[64] = {0};
    if (read_file(path, cur, sizeof cur) || !cur[0]) return -1;
    if (str_journal_add(file, path, cur, read_mode(path))) return -1;
  }
  char now[64] = {0};
  if (!read_file(path, now, sizeof now) && !strcmp(now, val)) return 0;
  chmod(path, 0644);
  return write_str(path, val);
}

static void str_restore_all(const char *file, const char *tag) {
  if (access(file, F_OK)) return;
  FILE *f = fopen(file, "r");
  if (!f) return;
  char ln[640], path[512], val[64];
  int mode, failed = 0, count = 0;
  while (fgets(ln, sizeof ln, f)) {
    if (sscanf(ln, "%511s %63s %o", path, val, &mode) != 3) continue;
    if (access(path, F_OK)) { failed = 1; continue; }
    chmod(path, 0644);
    char now[64] = {0};
    if (write_str(path, val) || read_file(path, now, sizeof now) ||
        strcmp(now, val))
      failed = 1;
    chmod(path, (mode_t)mode);
    count++;
  }
  fclose(f);
  if (failed) {
    log("WARN %s 未完全还原，保留账本下次重试", tag);
    return;
  }
  unlink(file);
  log("%s 已还原 %d 个节点", tag, count);
}

/* 任务组 uclamp.max：动态封顶的第二层，只压上限、不抬下限。 */
#define UCLAMP_JOURNAL RUN_DIR "/uclamp.saved"
static const struct {
  const char *path;
  int optimal;
  int mid;
  int floor;
} g_sup_umax_nodes[] = {
    { "/dev/cpuctl/top-app/cpu.uclamp.max", 85, 70, 60 },
    { "/dev/cpuctl/foreground/cpu.uclamp.max", 65, 50, 40 },
    { "/dev/cpuctl/dex2oat/cpu.uclamp.max", 45, 30, 20 },
    /* background / system-background 已拆成常驻项（BG_LIGHT） */
};

static int uclamp_scale_of_text(const char *b) {
  if (!b || !b[0]) return 0;
  if (!strcmp(b, "max")) return 100;
  char *end;
  double v = strtod(b, &end);
  if (end == b) return 0;
  return v > 100.0 ? 1024 : 100;
}

static int uclamp_max_scale(const char *path) {
  char b[64] = {0};
  if (read_file(path, b, sizeof b)) return 0;
  trim(b);
  return uclamp_scale_of_text(b);
}

static int uclamp_val_eq(const char *a, const char *b) {
  if (!strcmp(a, b)) return 1;
  char *ea, *eb;
  double da = strtod(a, &ea), db = strtod(b, &eb);
  if (ea == a || eb == b) return 0;
  double d = da > db ? da - db : db - da;
  return d < 0.01;
}

static int uclamp_max_write(const char *path, const char *val) {
  if (access(path, F_OK)) return -1;
  if (!str_journal_has(UCLAMP_JOURNAL, path)) {
    char cur[64] = {0};
    if (read_file(path, cur, sizeof cur) || !cur[0]) return -1;
    if (str_journal_add(UCLAMP_JOURNAL, path, cur, read_mode(path))) return -1;
  }
  char now[64] = {0};
  if (!read_file(path, now, sizeof now) && uclamp_val_eq(now, val)) return 0;
  chmod(path, 0644);
  return write_str(path, val);
}

static void suppress_uclamp_max_apply(void) {
  static int scale_mem[sizeof g_sup_umax_nodes / sizeof *g_sup_umax_nodes];
  static long log_at = -600;
  int n = 0;
  for (size_t i = 0; i < sizeof g_sup_umax_nodes / sizeof *g_sup_umax_nodes; i++) {
    const char *p = g_sup_umax_nodes[i].path;
    if (access(p, F_OK)) continue;
    if (!scale_mem[i]) scale_mem[i] = uclamp_max_scale(p);
    int scale = scale_mem[i];
    if (!scale) continue;
    int prof = sup_profile_level();
    int pct = prof == SUP_FLOOR_LV ? g_sup_umax_nodes[i].floor
            : prof == SUP_MID_LV   ? g_sup_umax_nodes[i].mid
                                   : g_sup_umax_nodes[i].optimal;
    char want[24];
    if (scale == 100) snprintf(want, sizeof want, "%d", pct);
    else snprintf(want, sizeof want, "%ld", (pct * 1024L + 50) / 100);
    if (!uclamp_max_write(p, want)) n++;
  }
  if (n) {
    long now = mono_sec();
    if (now - log_at >= 600) {
      log_at = now;
      log("动态封顶：uclamp 上限已压（%d 个节点）", n);
    }
  }
}

static void suppress_uclamp_max_restore(void) {
  str_restore_all(UCLAMP_JOURNAL, "uclamp.max");
}

/* 后台分组常驻轻压制（与动态封顶解耦） */
#define BGCG_JOURNAL RUN_DIR "/bgcgroup.saved"

static const struct { const char *path; int pct; } g_bg_umax[] = {
    { "/dev/cpuctl/background/cpu.uclamp.max", 50 },
    { "/dev/cpuctl/system-background/cpu.uclamp.max", 60 },
};

/* 读 "0-3,5" 成位图；格式不认识返回 0（调用处不动节点）。 */
static int cpulist_parse(const char *s, unsigned char *set, int n) {
  memset(set, 0, (size_t)n);
  int any = 0;
  const char *p = s;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\t') p++;
    if (!*p) break;
    if (!isdigit((unsigned char)*p)) return 0;
    char *end;
    long a = strtol(p, &end, 10);
    if (end == p) return 0;
    long b = a;
    p = end;
    if (*p == '-') {
      p++;
      if (!isdigit((unsigned char)*p)) return 0;
      b = strtol(p, &end, 10);
      if (end == p) return 0;
      p = end;
    }
    if (a < 0 || b < a || b >= n) return 0;
    for (long i = a; i <= b; i++) set[i] = 1;
    any = 1;
    if (*p && *p != ',' && *p != ' ' && *p != '\n' && *p != '\t') return 0;
  }
  return any;
}

static void cpulist_format(const unsigned char *set, int n, char *out, size_t cap) {
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < n;) {
    if (!set[i]) { i++; continue; }
    int j = i;
    while (j + 1 < n && set[j + 1]) j++;
    int w = (i == j) ? snprintf(out + o, cap - o, "%s%d", o ? "," : "", i)
                     : snprintf(out + o, cap - o, "%s%d-%d", o ? "," : "", i, j);
    if (w < 0 || (size_t)w >= cap - o) { out[0] = 0; return; }
    o += (size_t)w;
    i = j + 1;
  }
}

/* 后台掩码 = 现有掩码 ∩ 最低频簇。某台厂商已经把 background 压到 */
static int bg_cpus_mask(char *out, size_t n) {
  if (g_ladder_m < 2) return 0;
  struct policy raw[MAX_POLICY];
  int nr = detect_policies_raw(raw);
  if (nr < 2) return 0;
  long low = raw[0].hw_max;
  int low_no = raw[0].pol_no;
  for (int i = 1; i < nr; i++)
    if (raw[i].hw_max > 0 && raw[i].hw_max < low) {
      low = raw[i].hw_max;
      low_no = raw[i].pol_no;
    }
  unsigned char lowset[MAX_CORES], cur[MAX_CORES], res[MAX_CORES];
  memset(lowset, 0, sizeof lowset);
  for (int c = 0; c < MAX_CORES; c++) {
    if (!core_exists(c)) continue;
    if (core_policy_of(c) == low_no) lowset[c] = 1;
  }
  char b[128] = {0};
  if (read_file("/dev/cpuset/background/cpus", b, sizeof b)) return 0;
  trim(b);
  if (!cpulist_parse(b, cur, MAX_CORES)) return 0;
  int any = 0;
  for (int c = 0; c < MAX_CORES; c++) {
    res[c] = lowset[c] && cur[c];
    any |= res[c];
  }
  if (!any) return 0;
  cpulist_format(res, MAX_CORES, out, n);
  return out[0] != 0;
}

static void bg_light_restore(void) {
  str_restore_all(BGCG_JOURNAL, "后台分组");
}

/* uclamp 回读是 "50.00"/"max"，字符串账本按原样比会永远“不相等”。 */
static int bg_uclamp_write(const char *path, const char *val) {
  if (access(path, F_OK)) return -1;
  if (!str_journal_has(BGCG_JOURNAL, path)) {
    char cur[64] = {0};
    if (read_file(path, cur, sizeof cur) || !cur[0]) return -1;
    if (str_journal_add(BGCG_JOURNAL, path, cur, read_mode(path))) return -1;
  }
  char now[64] = {0};
  if (!read_file(path, now, sizeof now)) {
    trim(now);
    if (uclamp_val_eq(now, val)) return 0;
  }
  chmod(path, 0644);
  return write_str(path, val) == 0 ? 1 : -1;
}

static void bg_light_apply(void) {
  if (!cfg.bg_light) { bg_light_restore(); return; }
  int n = 0;
  for (size_t i = 0; i < sizeof g_bg_umax / sizeof *g_bg_umax; i++) {
    const char *p = g_bg_umax[i].path;
    if (access(p, F_OK)) continue;
    int scale = uclamp_max_scale(p);
    if (!scale) continue;
    char want[24];
    if (scale == 100) snprintf(want, sizeof want, "%d", g_bg_umax[i].pct);
    else snprintf(want, sizeof want, "%ld", (g_bg_umax[i].pct * 1024L + 50) / 100);
    /* 只降不升：厂商已经把上限压得比目标更低就保持原样。 */
    char cur[64] = {0};
    if (!read_file(p, cur, sizeof cur)) {
      trim(cur);
      if (strcmp(cur, "max")) {
        char *ce, *we;
        double d = strtod(cur, &ce);
        double target = strtod(want, &we);
        if (ce != cur && we != want && d < target) continue;
      }
    }
    if (bg_uclamp_write(p, want) > 0) n++;
  }
  char mask[64];
  if (bg_cpus_mask(mask, sizeof mask)) {
    const char *p = "/dev/cpuset/background/cpus";
    char cur[64] = {0};
    int same = !read_file(p, cur, sizeof cur);
    if (same) {
      trim(cur);
      same = !strcmp(cur, mask);
    }
    if (!same && !str_write_journaled(BGCG_JOURNAL, p, mask)) n++;
  }
  if (n) log("后台分组已收口（%d 个节点）", n);
}

/* cpuset 前后台分区（随动态封顶的中间/地板档） */
#define CPUSET_JOURNAL RUN_DIR "/cpuset.saved"
#define CPUSET_GROUPS  4
static const char *g_cpuset_dirs[CPUSET_GROUPS] = {
    "top-app", "foreground", "background", "system-background",
};
static char g_cpuset_mask[3][CPUSET_GROUPS][64];
static unsigned g_cpuset_seen[3];

static int cpuset_group_idx(const char *s) {
  if (!strcmp(s, "top")) return 0;
  if (!strcmp(s, "fg")) return 1;
  if (!strcmp(s, "bg")) return 2;
  if (!strcmp(s, "sysbg")) return 3;
  return -1;
}

/* 只接受 0-7 / 0-1,5-6 这类 CPU 列表；数据文件写错就整行忽略。 */
static int cpuset_mask_valid(const char *s) {
  if (!s || !*s) return 0;
  const char *p = s;
  while (*p) {
    char *end;
    long a = strtol(p, &end, 10);
    if (end == p || a < 0 || a > 1023) return 0;
    p = end;
    if (*p == '-') {
      p++;
      long b = strtol(p, &end, 10);
      if (end == p || b < a || b > 1023) return 0;
      p = end;
    }
    if (*p == ',') {
      p++;
      if (!*p) return 0;
      continue;
    }
    if (*p) return 0;
  }
  return 1;
}

/* cpuset_<top|fg|bg|sysbg>_lv1/lv2=<mask> */
static int cpuset_parse_line(const char *line, int *level, int *group,
                             char *mask, size_t n) {
  if (!line || strncmp(line, "cpuset_", 7)) return 0;
  const char *p = line + 7;
  const char *sep = strstr(p, "_lv");
  if (!sep || sep == p || (sep[3] != '0' && sep[3] != '1' && sep[3] != '2') ||
      sep[4] != '=')
    return 0;
  char g[16];
  size_t gl = (size_t)(sep - p);
  if (gl >= sizeof g) return 0;
  memcpy(g, p, gl);
  g[gl] = 0;
  int gi = cpuset_group_idx(g);
  if (gi < 0) return 0;
  const char *v = sep + 5;
  if (!cpuset_mask_valid(v)) return 0;
  snprintf(mask, n, "%s", v);
  *level = sep[3] - '0';
  *group = gi;
  return 1;
}

static int cpuset_write_journaled(const char *path, const char *mask) {
  if (!cpuset_mask_valid(mask)) return -1;
  return str_write_journaled(CPUSET_JOURNAL, path, mask);
}

static int cpuset_apply_level(int level) {
  static long log_at = -600;
  if (level < 0 || level > 2) return 0;
  int n = 0;
  for (int g = 0; g < CPUSET_GROUPS; g++) {
    /* background 由常驻项 BG_LIGHT 单独管（只收窄到最低簇） */
    if (g == 2) continue;
    if (!(g_cpuset_seen[level] & (1u << g))) continue;
    char path[160];
    snprintf(path, sizeof path, "/dev/cpuset/%s/cpus", g_cpuset_dirs[g]);
    if (access(path, F_OK)) continue;
    if (!cpuset_write_journaled(path, g_cpuset_mask[level][g])) n++;
  }
  if (n) {
    long now = mono_sec();
    if (now - log_at >= 600) {
      log_at = now;
      log("动态封顶：cpuset 分区已应用（%d 个节点）", n);
    }
  }
  return n;
}

static void cpuset_restore_all(void) {
  str_restore_all(CPUSET_JOURNAL, "cpuset");
}

/* 调速器                                                             */

static struct policy g_pol[MAX_POLICY];
static int g_npol;

static int prime_cluster_busy(const int *sum, const int *cnt) {
  if (g_npol <= 0) return -1;
  long best_hw = -1;
  int best_pol = -1;
  for (int i = 0; i < g_npol; i++) {
    if (g_pol[i].hw_max > best_hw) {
      best_hw = g_pol[i].hw_max;
      best_pol = g_pol[i].pol_no;
    }
  }
  if (best_pol < 0 || best_pol >= MAX_CORES || cnt[best_pol] <= 0) return -1;
  return sum[best_pol] / cnt[best_pol];
}

/* 智能模式快降判据：每个簇的当前频率都低于自己封顶的一半。 */
static int clusters_below_half(void) {
  if (g_npol <= 0) return 0;
  for (int i = 0; i < g_npol; i++) {
    if (g_pol[i].cap <= 0) return 0;
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_cur_freq", g_pol[i].dir);
    if (!freq_below_half(read_num(path), g_pol[i].cap)) return 0;
  }
  return 1;
}
/* 未去重的原始 policy 列表：hispeed 数据按 cluster 序号写 */
static struct policy g_polraw[MAX_POLICY];
static int g_nraw;

/* CPU 封顶第二道锁（MTK PPM / 高通 msm_performance） */
static int g_ppm_ok;        /* /proc/ppm 存在、enabled、节点可读 */
static int g_ppm_enabled;   /* /proc/ppm/enabled 里读到 enabled */
static int g_ppm_lines;     /* PPM 硬限节点里读到的 cluster 行数 */
static int g_msm_ok;        /* msm_performance 节点可读可写 */
static char g_msm_max_path[128];

static long g_ppm_orig[MAX_POLICY];
static long g_ppm_last[MAX_POLICY];
static unsigned g_ppm_has_orig, g_ppm_has_last;  /* bit idx */
static char g_msm_orig[1024], g_msm_last[1024];
static int g_msm_has_orig, g_msm_has_last;

/* 0 和 -1 在 PPM/msm 里都表示“没有限制”，比较时当成无穷大。 */
static long be_eff(long v) { return v > 0 ? v : (1L << 60); }

static const char *cap_backend_name(int be) {
  switch (be) {
    case CAP_BE_PPM: return "ppm";
    case CAP_BE_MSM: return "msm";
    default: return "sysfs";
  }
}

/* 从 PPM 硬限节点里找某个 cluster 的当前值。返回 1=找到，0=没有该行 */
static int ppm_read_value(int idx, long *out) {
  char b[2048];
  if (read_file(PPM_MAX_NODE, b, sizeof b)) return -1;
  char *save = NULL;
  for (char *ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    int id = -1;
    long v = 0;
    if (sscanf(ln, "%d %ld", &id, &v) == 2 && id == idx) {
      *out = v;
      return 1;
    }
  }
  return 0;
}

static int ppm_count_lines(void) {
  char b[2048];
  if (read_file(PPM_MAX_NODE, b, sizeof b)) return 0;
  int n = 0;
  char *save = NULL;
  for (char *ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    int id = -1;
    long v = 0;
    if (sscanf(ln, "%d %ld", &id, &v) == 2) n++;
  }
  return n;
}

static int ppm_write_value(int idx, long val) {
  char b[64];
  snprintf(b, sizeof b, "%d %ld\n", idx, val);
  return write_text(PPM_MAX_NODE, b);
}

/* ppm.saved：每个 cluster 的原值。只在第一次写某个 cluster 时落盘 */
static void ppm_journal_save(void) {
  char b[1024];
  b[0] = 0;
  size_t n = 0;
  for (int i = 0; i < MAX_POLICY; i++) {
    if (!(g_ppm_has_orig & (1u << i))) continue;
    if (n >= sizeof b) break;
    int w = snprintf(b + n, sizeof b - n, "%d %ld\n", i, g_ppm_orig[i]);
    if (w < 0) break;
    n += (size_t)w;
    if (n >= sizeof b) break;
  }
  char tmp[sizeof PPM_JOURNAL + 8];
  snprintf(tmp, sizeof tmp, "%s.tmp", PPM_JOURNAL);
  if (write_text_file(tmp, b) == 0) rename(tmp, PPM_JOURNAL);
}

static void ppm_journal_load(void) {
  memset(g_ppm_orig, 0, sizeof g_ppm_orig);
  g_ppm_has_orig = 0;
  FILE *f = fopen(PPM_JOURNAL, "r");
  if (!f) return;
  int idx;
  long v;
  while (fscanf(f, "%d %ld", &idx, &v) == 2)
    if (idx >= 0 && idx < MAX_POLICY) {
      g_ppm_orig[idx] = v;
      g_ppm_has_orig |= 1u << idx;
    }
  fclose(f);
}

/* ppm.last 放在 tmpfs：每次写 PPM 都更新，崩溃后还能判断“这个值是不是 */
static void ppm_last_save(void) {
  char b[1024];
  b[0] = 0;
  size_t n = 0;
  for (int i = 0; i < MAX_POLICY; i++) {
    if (!(g_ppm_has_last & (1u << i))) continue;
    if (n >= sizeof b) break;
    int w = snprintf(b + n, sizeof b - n, "%d %ld\n", i, g_ppm_last[i]);
    if (w < 0) break;
    n += (size_t)w;
    if (n >= sizeof b) break;
  }
  char tmp[sizeof PPM_LAST + 8];
  snprintf(tmp, sizeof tmp, "%s.tmp", PPM_LAST);
  if (write_text_file(tmp, b) == 0) rename(tmp, PPM_LAST);
}

static void ppm_last_load(void) {
  memset(g_ppm_last, 0, sizeof g_ppm_last);
  g_ppm_has_last = 0;
  FILE *f = fopen(PPM_LAST, "r");
  if (!f) return;
  int idx;
  long v;
  while (fscanf(f, "%d %ld", &idx, &v) == 2)
    if (idx >= 0 && idx < MAX_POLICY) {
      g_ppm_last[idx] = v;
      g_ppm_has_last |= 1u << idx;
    }
  fclose(f);
}

static void ppm_note_orig(int idx, long orig) {
  if (g_ppm_has_orig & (1u << idx)) return;
  g_ppm_orig[idx] = orig;
  g_ppm_has_orig |= 1u << idx;
  ppm_journal_save();
}

static void ppm_note_last(int idx, long val) {
  g_ppm_last[idx] = val;
  g_ppm_has_last |= 1u << idx;
  ppm_last_save();
}

/* 恢复 PPM：clean=1 时连原值账本一起删（停用/卸载） */
static int ppm_restore_all(int clean) {
  int failed = 0, restored = 0;
  for (int i = 0; i < MAX_POLICY; i++) {
    if (!(g_ppm_has_orig & (1u << i))) continue;
    long cur = 0;
    int rr = ppm_read_value(i, &cur);
    if (rr < 0) {
      failed = 1;
      continue;
    }
    int ours = 0;
    if (g_ppm_has_last & (1u << i)) {
      ours = (rr == 1 && cur == g_ppm_last[i]);
    } else {
      /* 重启后 tmpfs 的 last 没了：我们只会把上限压低，所以“当前比原值低” */
      ours = (rr == 1 && be_eff(cur) < be_eff(g_ppm_orig[i]));
    }
    if (!ours) {
      if (g_ppm_has_last & (1u << i)) {
        log("PPM %d: 保留厂商值 %ld（模块最后写入 %ld）",
            i, cur, g_ppm_last[i]);
        g_ppm_has_last &= ~(1u << i);
      }
      continue;
    }
    if (ppm_write_value(i, g_ppm_orig[i]) == 0) {
      long got = 0;
      if (ppm_read_value(i, &got) == 1 && be_eff(got) == be_eff(g_ppm_orig[i])) {
        restored++;
        g_ppm_has_last &= ~(1u << i);
        log("PPM %d: 已恢复原值 %ld", i, g_ppm_orig[i]);
      } else {
        failed = 1;
      }
    } else {
      failed = 1;
    }
  }
  if (restored) ppm_last_save();
  if (clean) {
    if (failed) {
      log("WARN PPM 恢复未完成，保留 ppm.saved 下次重试");
    } else {
      unlink(PPM_JOURNAL);
      unlink(PPM_LAST);
      g_ppm_has_orig = 0;
      g_ppm_has_last = 0;
    }
  }
  return failed ? -1 : 0;
}

static void ppm_apply(struct policy *p, long want) {
  if (!p || p->be != CAP_BE_PPM || p->ppm_idx < 0 || p->be_fail >= 3) return;
  long cur = 0;
  int rr = ppm_read_value(p->ppm_idx, &cur);
  if (rr < 0) {
    if (p->be_fail++ == 0)
      log("PPM %d: 读取失败，本簇回退 sysfs", p->ppm_idx);
    return;
  }
  if (rr == 0) {
    /* 节点里没有这一行：先按“无限制”记原值，写入后再看能不能读到。 */
    cur = -1;
  }
  if (p->be_last <= 0) ppm_note_orig(p->ppm_idx, cur);
  int ours = 0;
  if (cur == want) {
    ppm_note_last(p->ppm_idx, cur);
    p->be_last = cur;
    return;
  }
  if (be_eff(cur) > be_eff(want)) {
    ours = 1;                       /* 当前比目标高：压回去 */
  } else if (p->be_last > 0 && cur == p->be_last && want > p->be_last) {
    ours = 1;                       /* 是自己的帽，允许放宽 */
  }
  if (!ours) return;                /* 厂商压得更低：保留 */
  if (ppm_write_value(p->ppm_idx, want)) {
    if (p->be_fail++ == 0)
      log("PPM %d: 写入 %ld 失败，本簇回退 sysfs", p->ppm_idx, want);
    return;
  }
  long got = 0;
  int gr = ppm_read_value(p->ppm_idx, &got);
  if (gr != 1 || be_eff(got) > be_eff(want)) {
    if (p->be_fail++ == 0)
      log("PPM %d: 写入 %ld 未生效（回读 %ld）", p->ppm_idx, want, got);
    return;
  }
  p->be_fail = 0;
  p->be_last = got;
  ppm_note_last(p->ppm_idx, got);
}

/* 高通 msm_performance */
static void msm_journal_save(void) {
  char tmp[sizeof MSM_JOURNAL + 8];
  snprintf(tmp, sizeof tmp, "%s.tmp", MSM_JOURNAL);
  if (write_text_file(tmp, g_msm_orig) == 0) rename(tmp, MSM_JOURNAL);
}

static void msm_last_save(void) {
  char tmp[sizeof MSM_LAST + 8];
  snprintf(tmp, sizeof tmp, "%s.tmp", MSM_LAST);
  if (write_text_file(tmp, g_msm_last) == 0) rename(tmp, MSM_LAST);
}

static void msm_journal_load(void) {
  g_msm_has_orig = !access(MSM_JOURNAL, F_OK);
  if (g_msm_has_orig) {
    if (read_file(MSM_JOURNAL, g_msm_orig, sizeof g_msm_orig))
      g_msm_orig[0] = 0;
  }
  g_msm_has_last = !access(MSM_LAST, F_OK);
  if (g_msm_has_last) {
    if (read_file(MSM_LAST, g_msm_last, sizeof g_msm_last))
      g_msm_last[0] = 0;
  }
}

static int msm_read_current(char *buf, size_t cap) {
  if (read_file(g_msm_max_path, buf, cap)) {
    buf[0] = 0;
    return access(g_msm_max_path, F_OK) == 0 ? 0 : -1;
  }
  return 0;
}

/* 厂商 CPU 上限 / 本机硬件上限；只做诊断，不抢写。 */
static int g_hr_dead_n;
static int vendor_headroom(int *valid, int *soft, int *hard) {
  *valid = 0; *soft = 0; *hard = 0;
  const char *path = g_msm_max_path[0] ? g_msm_max_path : MSM_MAX_NODE;
  if (access(path, F_OK) && !access(MSM_MAX_ALT, F_OK)) path = MSM_MAX_ALT;
  if (access(path, F_OK)) return -1;
  char b[1024];
  if (read_file(path, b, sizeof b)) return -1;
  long minf = 0;
  char *save = NULL;
  for (char *tok = strtok_r(b, " \t\n", &save); tok;
       tok = strtok_r(NULL, " \t\n", &save)) {
    int c = -1; long f = 0;
    if (sscanf(tok, "%d:%ld", &c, &f) == 2 && c >= 0 && f > 0 &&
        (!minf || f < minf)) minf = f;
  }
  long hw = 0;
  for (int i = 0; i < g_npol; i++)
    if (g_pol[i].hw_max > hw) hw = g_pol[i].hw_max;
  int hr = (hw > 0 && minf > 0) ? (int)(minf * 100 / hw) : 100;
  if (hr > 100) hr = 100;
  if (hr >= 100) {
    if (g_hr_dead_n < 10) g_hr_dead_n++;
    if (g_hr_dead_n >= 10) return hr;
  } else {
    g_hr_dead_n = 0;
  }
  *valid = 1;
  *soft = hr < (cfg.fsm_hr_soft > 0 ? cfg.fsm_hr_soft : 70);
  *hard = hr < (cfg.fsm_hr_hard > 0 ? cfg.fsm_hr_hard : 45);
  return hr;
}

/* msm 的写语义是“列出的 CPU 改值，没列出的保持原样”。 */
static int msm_build_restore(char *out, size_t cap) {
  long ov[MAX_CORES];
  int oh[MAX_CORES] = {0};
  char *save = NULL;
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", g_msm_orig);
  for (char *tok = strtok_r(tmp, " \t\n", &save); tok;
       tok = strtok_r(NULL, " \t\n", &save)) {
    int c = -1;
    long f = 0;
    if (sscanf(tok, "%d:%ld", &c, &f) == 2 && c >= 0 && c < MAX_CORES) {
      ov[c] = f;
      oh[c] = 1;
    }
  }
  size_t n = 0;
  out[0] = 0;
  for (int c = 0; c < MAX_CORES; c++) {
    char p[64];
    snprintf(p, sizeof p, CPUBASE "/cpu%d", c);
    if (access(p, F_OK)) continue;
    if (n >= cap) break;
    int w = snprintf(out + n, cap - n, "%s%d:%ld", n ? " " : "", c,
                     oh[c] ? ov[c] : 0L);
    if (w < 0) break;
    n += (size_t)w;
    if (n >= cap) break;
  }
  return n > 0;
}

/* 只撤自己写的；clean=1 时删账本。 */
static int msm_restore_all(int clean) {
  int failed = 0;
  if (g_msm_has_orig) {
    char cur[1024];
    if (msm_read_current(cur, sizeof cur) == 0) {
      int ours = g_msm_has_last && !strcmp(cur, g_msm_last);
      if (ours) {
        char payload[1024];
        if (!msm_build_restore(payload, sizeof payload) ||
            write_text(g_msm_max_path, payload)) {
          failed = 1;
        } else {
          char got[1024];
          if (msm_read_current(got, sizeof got) || strcmp(got, payload)) {
            failed = 1;
          } else {
            g_msm_has_last = 0;
            log("msm: 已恢复原值（未列出的 CPU 写 0 解除）");
          }
        }
      } else if (g_msm_has_last) {
        log("msm: 保留厂商值");
        g_msm_has_last = 0;
      }
    } else {
      failed = 1;
    }
  }
  if (clean) {
    if (failed) {
      log("WARN msm 恢复未完成，保留 msm.saved 下次重试");
    } else {
      unlink(MSM_JOURNAL);
      unlink(MSM_LAST);
      g_msm_has_orig = 0;
      g_msm_has_last = 0;
      g_msm_orig[0] = 0;
      g_msm_last[0] = 0;
    }
  } else if (!failed) {
    g_msm_has_last = 0;
    msm_last_save();
  }
  return failed ? -1 : 0;
}

/* 本 policy 管的 CPU 列表（用未去重的 raw policy 补全同频簇）。 */
static int msm_cluster_cpus(const struct policy *p, int *out, int max) {
  int n = 0;
  for (int r = 0; r < g_nraw; r++) {
    if (g_polraw[r].hw_max != p->hw_max) continue;
    for (int c = 0; c < MAX_CORES && n < max; c++)
      if (core_policy_of(c) == g_polraw[r].pol_no) out[n++] = c;
  }
  if (!n) {
    for (int c = 0; c < MAX_CORES && n < max; c++)
      if (core_policy_of(c) == p->pol_no) out[n++] = c;
  }
  return n;
}

static void msm_apply(struct policy *p, long want) {
  if (!p || p->be != CAP_BE_MSM || !g_msm_ok || p->be_fail >= 3) return;
  int cpus[MAX_CORES];
  int nc = msm_cluster_cpus(p, cpus, MAX_CORES);
  if (!nc) return;

  char cur[1024];
  if (msm_read_current(cur, sizeof cur)) {
    p->be_fail++;
    return;
  }
  long mf[MAX_CORES];
  int mh[MAX_CORES] = {0};
  int parsed = 0;
  char *save = NULL;
  for (char *tok = strtok_r(cur, " \t\n", &save); tok;
       tok = strtok_r(NULL, " \t\n", &save)) {
    int c = -1;
    long f = 0;
    if (sscanf(tok, "%d:%ld", &c, &f) == 2 && c >= 0 && c < MAX_CORES) {
      mf[c] = f;
      mh[c] = 1;
      parsed++;
    }
  }
  if (cur[0] && !parsed) {
    if (p->be_fail++ == 0)
      log("msm: 现有内容格式不识别，停用本簇后端");
    return;
  }
  for (int i = 0; i < nc; i++) {
    mf[cpus[i]] = want;
    mh[cpus[i]] = 1;
  }
  char payload[1024];
  payload[0] = 0;
  size_t n = 0;
  for (int c = 0; c < MAX_CORES; c++) {
    if (!mh[c]) continue;
    if (n >= sizeof payload) break;
    int w = snprintf(payload + n, sizeof payload - n, "%s%d:%ld",
                     n ? " " : "", c, mf[c]);
    if (w < 0) break;
    n += (size_t)w;
    if (n >= sizeof payload) break;
  }
  if (!n || !strcmp(payload, cur)) return;
  if (!g_msm_has_orig) {
    snprintf(g_msm_orig, sizeof g_msm_orig, "%s", cur);
    g_msm_has_orig = 1;
    msm_journal_save();
  }
  if (write_text(g_msm_max_path, payload)) {
    if (p->be_fail++ == 0) log("msm: 写入失败，本簇回退 sysfs");
    return;
  }
  char got[1024];
  if (msm_read_current(got, sizeof got) || strcmp(got, payload)) {
    if (p->be_fail++ == 0) log("msm: 写入未生效（回读 %s）", got);
    return;
  }
  p->be_fail = 0;
  p->be_last = want;
  snprintf(g_msm_last, sizeof g_msm_last, "%s", payload);
  g_msm_has_last = 1;
  msm_last_save();
}

static void be_apply_max(struct policy *p, long want) {
  if (!p || want <= 0) return;
  if (p->be == CAP_BE_PPM) ppm_apply(p, want);
  else if (p->be == CAP_BE_MSM) msm_apply(p, want);
}

/* 交权（游戏/熄屏）时把第二道锁放掉。只撤自己写的，厂商值不动。 */
static void be_handback(void) {
  if (g_ppm_ok) ppm_restore_all(0);
  if (g_msm_ok) msm_restore_all(0);
}

/* 停用/卸载/崩溃重启：连账本一起清。 */
static void be_restore_cleanup(void) {
  if (g_ppm_ok) ppm_restore_all(1);
  if (g_msm_ok) msm_restore_all(1);
}

/* 探测节点；不写任何东西。 */
static void cap_backend_detect(void) {
  g_ppm_ok = 0;
  g_ppm_enabled = 0;
  g_ppm_lines = 0;
  g_msm_ok = 0;
  g_msm_max_path[0] = 0;

  if (!access(PPM_DIR, F_OK) && !access(PPM_MAX_NODE, F_OK)) {
    char b[128];
    if (!read_file(PPM_ENABLED, b, sizeof b) && strstr(b, "enabled"))
      g_ppm_enabled = 1;
    if (g_ppm_enabled) {
      g_ppm_lines = ppm_count_lines();
      g_ppm_ok = 1;
    }
  }
  const char *mx = NULL;
  if (!access(MSM_MAX_NODE, R_OK | W_OK)) {
    mx = MSM_MAX_NODE;
  } else if (!access(MSM_MAX_ALT, R_OK | W_OK)) {
    mx = MSM_MAX_ALT;
  }
  if (mx) {
    snprintf(g_msm_max_path, sizeof g_msm_max_path, "%s", mx);
    g_msm_ok = 1;
  }
}

/* 启动时选后端并清掉上次崩溃留下的 PPM/msm 帽。 */
static void cap_backend_init(void) {
  cap_backend_detect();
  char soc[64];
  read_soc_name(soc, sizeof soc);
  int is_mtk = !strncasecmp(soc, "mt", 2);
  for (int i = 0; i < g_npol; i++) {
    g_pol[i].be = CAP_BE_SYSFS;
    g_pol[i].ppm_idx = -1;
    g_pol[i].be_last = 0;
    g_pol[i].be_fail = 0;
  }
  int want = cfg.cap_backend;
  if (want == CAP_BE_AUTO) {
    /* auto 只自动启用 MTK PPM。高通 msm_performance 的写语义是 */
    if (g_ppm_ok) want = CAP_BE_PPM;
    else {
      want = CAP_BE_SYSFS;
      if (is_mtk) {
        if (!access(PPM_DIR, F_OK)) {
          if (!g_ppm_enabled)
            log("PPM 未启用：/proc/ppm/enabled 不是 enabled（或读不到），只走 sysfs");
          else if (access(PPM_MAX_NODE, F_OK))
            log("PPM 未启用：hard_userlimit_max_cpu_freq 节点不存在，只走 sysfs");
        } else {
          log("PPM 未启用：/proc/ppm 目录不存在，只走 sysfs");
        }
      }
    }
  }
  if (want == CAP_BE_PPM && !g_ppm_ok) {
    log("PPM 不可用（目录/开关/节点不满足），回退 sysfs");
    want = CAP_BE_SYSFS;
  }
  if (want == CAP_BE_MSM && !g_msm_ok) {
    log("msm_performance 节点不可用，回退 sysfs");
    want = CAP_BE_SYSFS;
  }
  /* auto 下簇数对不上就不冒险：PPM cluster 号是顺序假设，错一个会压错簇。 */
  if (want == CAP_BE_PPM && cfg.cap_backend == CAP_BE_AUTO &&
      g_ppm_lines > 0 && g_ppm_lines != g_npol) {
    log("PPM 簇数 %d 与 policy 数 %d 不一致，auto 回退 sysfs",
        g_ppm_lines, g_npol);
    want = CAP_BE_SYSFS;
  }
  /* 先把上次崩溃留下的后端帽清掉，不管这次选哪个后端。 */
  int ppm_stale_failed = 0, msm_stale_failed = 0;
  if (g_ppm_ok) {
    ppm_journal_load();
    ppm_last_load();
    if (g_ppm_has_orig && g_ppm_has_last) {
      if (ppm_restore_all(1)) ppm_stale_failed = 1;
    } else if (g_ppm_has_orig) {
      /* 只有原值账本、没有 tmpfs 的 last：基本是重启过，PPM 已复位。 */
      unlink(PPM_JOURNAL);
      unlink(PPM_LAST);
      g_ppm_has_orig = 0;
    }
  }
  if (g_msm_ok) {
    msm_journal_load();
    if (g_msm_has_orig && g_msm_has_last) {
      if (msm_restore_all(1)) msm_stale_failed = 1;
    } else if (g_msm_has_orig) {
      unlink(MSM_JOURNAL);
      unlink(MSM_LAST);
      g_msm_has_orig = 0;
      g_msm_has_last = 0;
    }
  }
  if (want == CAP_BE_PPM && ppm_stale_failed) {
    log("PPM 旧帽未还原，回退 sysfs（账本保留，下次再试）");
    want = CAP_BE_SYSFS;
  }
  if (want == CAP_BE_MSM && msm_stale_failed) {
    log("msm 旧帽未还原，回退 sysfs（账本保留，下次再试）");
    want = CAP_BE_SYSFS;
  }
  if (want == CAP_BE_PPM) {
    for (int i = 0; i < g_npol; i++) {
      g_pol[i].be = CAP_BE_PPM;
      g_pol[i].ppm_idx = i;
    }
  } else if (want == CAP_BE_MSM) {
    for (int i = 0; i < g_npol; i++) g_pol[i].be = CAP_BE_MSM;
  }
  if (want == CAP_BE_PPM) {
    char map[256];
    map[0] = 0;
    size_t n = 0;
    for (int i = 0; i < g_npol; i++) {
      if (n >= sizeof map) break;
      int w = snprintf(map + n, sizeof map - n, "%s%s:%d",
                       n ? " " : "", g_pol[i].name, g_pol[i].ppm_idx);
      if (w < 0) break;
      n += (size_t)w;
      if (n >= sizeof map) break;
    }
    log("CPU 封顶后端：MTK PPM（%d 簇，%s）", g_npol, map);
  } else if (want == CAP_BE_MSM) {
    log("CPU 封顶后端：高通 msm_performance（%s）", g_msm_max_path);
  } else {
    log("CPU 封顶后端：sysfs（未启用 PPM/msm 第二道锁）");
  }
}

/* probe 用：只报告，不改 g_pol，也不恢复任何东西。 */
static void cap_backend_report(const struct policy *p, int n) {
  cap_backend_detect();
  int want = cfg.cap_backend;
  if (want == CAP_BE_AUTO) {
    if (g_ppm_ok && (g_ppm_lines == 0 || g_ppm_lines == n)) want = CAP_BE_PPM;
    else want = CAP_BE_SYSFS;
  }
  printf("capbe_cfg=%s\n", cfg.cap_backend == CAP_BE_AUTO ? "auto"
                            : cap_backend_name(cfg.cap_backend));
  printf("capbe=%s\n", cap_backend_name(want));
  printf("ppm_dir=%d\nppm_enabled=%d\nppm_node=%d\nppm_lines=%d\n",
         !access(PPM_DIR, F_OK), g_ppm_enabled, !access(PPM_MAX_NODE, F_OK),
         g_ppm_lines);
  {
    char raw[1024];
    if (read_file(PPM_MAX_NODE, raw, sizeof raw)) raw[0] = 0;
    for (char *c = raw; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    printf("ppm_raw_max=%s\n", raw);
  }
  printf("msm_ok=%d\nmsm_node=%s\n", g_msm_ok, g_msm_ok ? g_msm_max_path : "-");
  if (g_msm_ok) {
    char raw[1024];
    if (read_file(g_msm_max_path, raw, sizeof raw)) raw[0] = 0;
    for (char *c = raw; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    printf("msm_raw_max=%s\n", raw);
  }
  printf("ppm_map=");
  for (int i = 0; i < n; i++)
    printf("%s%s:%d", i ? " " : "", p[i].name, (want == CAP_BE_PPM) ? i : -1);
  printf("\n");
}

/* 厂商调速器一律不碰：hmbird / scx 这类自有调度会把写权限从调度器 */
static int is_vendor_gov(const char *g) {
  return !strncmp(g, "hmbird", 6) || !strncmp(g, "scx", 3);
}

/* 升频限速（调度器通用调参） */
static int gov_param_path(int i, const char *param, char *out, size_t cap) {
  char gov[32] = {0};
  char gp[256];
  snprintf(gp, sizeof gp, "%s/scaling_governor", g_pol[i].dir);
  if (read_file(gp, gov, sizeof gov)) return 0;
  trim(gov);
  if (!gov[0] || is_vendor_gov(gov)) return 0;
  snprintf(out, cap, "%s/%s/%s", g_pol[i].dir, gov, param);
  if (!access(out, F_OK)) return 1;
  snprintf(out, cap, "%s/%s", g_pol[i].dir, param);
  return !access(out, F_OK);
}

/* 档位 → 限速参数下标：0=超大核 1=大核（MID2/MID 共用） 2=小核 */
static int gov_rate_idx(int tier) {
  if ((tier & 3) == 0) return 0;
  if ((tier & 3) == 3) return 2;
  return 1;
}

/* 配置里的 GOV_* 是“手动基准”：还是旧默认或均衡默认时，跟随压制风格 */
static void gov_rate_for(int ri, int *up, int *down) {
  static const int old_up[3] = {3000, 2000, 1000};
  static const int old_dn[3] = {1000, 1500, 2000};
  static const int bal_up[3] = {3000, 2000, 1500};
  static const int bal_dn[3] = {800, 800, 500};
  static const int sav_up[3] = {5000, 3000, 2000};
  static const int sav_dn[3] = {500, 500, 500};
  int u = cfg.gov_up_us[ri], d = cfg.gov_down_us[ri];
  int auto_u = (u == old_up[ri] || u == bal_up[ri]);
  int auto_d = (d == old_dn[ri] || d == bal_dn[ri]);
  *up = auto_u ? (cfg.sup_style ? sav_up[ri] : bal_up[ri]) : u;
  *down = auto_d ? (cfg.sup_style ? sav_dn[ri] : bal_dn[ri]) : d;
}

static void suppress_smooth_apply(void) {
  int n = 0;
  for (int i = 0; i < g_npol; i++) {
    char path[320];
    int ri = gov_rate_idx(g_pol[i].tier);
    int up, down;
    gov_rate_for(ri, &up, &down);
    static const char *const params[] = { "up_rate_limit_us", "down_rate_limit_us" };
    for (size_t k = 0; k < sizeof params / sizeof *params; k++) {
      if (!gov_param_path(i, params[k], path, sizeof path)) continue;
      long want = k == 0 ? up : down;
      if (read_num(path) == want) continue;   /* 已是目标值：不重复写 */
      if (!write_journaled(NODES_JOURNAL, path, want)) n++;
    }
  }
  if (n) {
    int u0, d0, u1, d1, u2, d2;
    gov_rate_for(0, &u0, &d0);
    gov_rate_for(1, &u1, &d1);
    gov_rate_for(2, &u2, &d2);
    log("调速器限速已应用（%d 个节点；超/大/小 %d-%d/%d-%d/%d-%dµs，%s）", n,
        u0, d0, u1, d1, u2, d2, cfg.sup_style ? "省电" : "均衡");
  }
}

static void suppress_smooth_restore(void) {
  for (int i = 0; i < g_npol; i++) {
    char path[320];
    static const char *const params[] = { "up_rate_limit_us", "down_rate_limit_us" };
    for (size_t k = 0; k < sizeof params / sizeof *params; k++) {
      if (!gov_param_path(i, params[k], path, sizeof path)) continue;
      boost_restore_path(path);
    }
  }
}

/* 逐芯片调速器工作点参数（gov_[lv1_|lv2_]<tier>_<节点名>=数字） */
#define GOV_TIER_N     4
#define GOV_LEVELS     3
#define GOV_KV_MAX     12
#define GOV_PARAM_MAX  48
struct gov_kv { char param[GOV_PARAM_MAX]; long val; };
static struct gov_kv g_govkv[GOV_LEVELS][GOV_TIER_N][GOV_KV_MAX];
static int g_govkn[GOV_LEVELS][GOV_TIER_N];
/* 按 cluster 序号（policy 号升序）的写法：gov_lv1_cl0_hispeed_freq=… */
static struct gov_kv g_govcl[GOV_LEVELS][MAX_POLICY][GOV_KV_MAX];
static int g_govcln[GOV_LEVELS][MAX_POLICY];
static int g_gov_have;
static char g_gov_applied[128][128];
static int  g_gov_applied_n;
static long g_gov_log_at;   /* 调速器工作点写入日志的 60s 限流时刻 */

/* 纯函数：解析一行 gov_[lv1_|lv2_]<tier>_<param>=<数字>。 */
static int gov_parse_line(const char *line, int *level, int *tier, char *param,
                          size_t pn, long *val) {
  static const char *const tiers[GOV_TIER_N] = { "big", "mid2", "mid", "little" };
  if (!line || strncmp(line, "gov_", 4)) return 0;
  const char *rest = line + 4;
  int lv = 0;
  if (!strncmp(rest, "lv0_", 4)) { lv = 0; rest += 4; }
  else if (!strncmp(rest, "lv1_", 4)) { lv = 1; rest += 4; }
  else if (!strncmp(rest, "lv2_", 4)) { lv = 2; rest += 4; }
  for (int t = 0; t < GOV_TIER_N; t++) {
    size_t tl = strlen(tiers[t]);
    if (strncmp(rest, tiers[t], tl) || rest[tl] != '_') continue;
    const char *p = rest + tl + 1;
    const char *eq = strchr(p, '=');
    if (!eq || eq == p) return 0;
    size_t pl = (size_t)(eq - p);
    if (pl >= pn) return 0;
    for (size_t c = 0; c < pl; c++)
      if (!isalnum((unsigned char)p[c]) && p[c] != '_') return 0;
    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;
    if (!isdigit((unsigned char)*v)) return 0;
    char num[24]; size_t ni = 0;
    while (isdigit((unsigned char)v[ni]) && ni < sizeof num - 1) ni++;
    /* 值必须是纯数字：85:1344000 这类 target_loads 表不能被截成 85 写进节点。 */
    while (v[ni] == ' ' || v[ni] == '\t' || v[ni] == '\r' || v[ni] == '\n') ni++;
    if (v[ni] != 0) return 0;
    char tmp[24];
    memcpy(tmp, v, ni); tmp[ni] = 0;
    memcpy(param, p, pl); param[pl] = 0;
    *val = strtol(tmp, NULL, 10);
    *level = lv;
    *tier = t;
    return 1;
  }
  return 0;
}

/* 纯函数：gov_[lv0_|lv1_|lv2_]cl<N>_<param>=<数字>。 */
static int gov_cl_parse_line(const char *line, int *level, int *cl, char *param,
                             size_t pn, long *val) {
  if (!line || strncmp(line, "gov_", 4)) return 0;
  const char *rest = line + 4;
  int lv = 0;
  if (!strncmp(rest, "lv0_", 4)) { lv = 0; rest += 4; }
  else if (!strncmp(rest, "lv1_", 4)) { lv = 1; rest += 4; }
  else if (!strncmp(rest, "lv2_", 4)) { lv = 2; rest += 4; }
  if (strncmp(rest, "cl", 2)) return 0;
  char *end;
  long idx = strtol(rest + 2, &end, 10);
  if (end == rest + 2 || idx < 0 || idx >= MAX_POLICY || *end != '_') return 0;
  const char *p = end + 1;
  const char *eq = strchr(p, '=');
  if (!eq || eq == p) return 0;
  size_t pl = (size_t)(eq - p);
  if (pl >= pn) return 0;
  for (size_t c = 0; c < pl; c++)
    if (!isalnum((unsigned char)p[c]) && p[c] != '_') return 0;
  const char *v = eq + 1;
  while (*v == ' ' || *v == '\t') v++;
  if (!isdigit((unsigned char)*v)) return 0;
  char num[24]; size_t ni = 0;
  while (isdigit((unsigned char)v[ni]) && ni < sizeof num - 1) ni++;
  while (v[ni] == ' ' || v[ni] == '\t' || v[ni] == '\r' || v[ni] == '\n') ni++;
  if (v[ni] != 0) return 0;
  memcpy(num, v, ni); num[ni] = 0;
  memcpy(param, p, pl); param[pl] = 0;
  *val = strtol(num, NULL, 10);
  *level = lv;
  *cl = (int)idx;
  return 1;
}

/* 从 adapt.conf 读 gov_/cpuset_ 行。装机脚本按 #match 选中的那份会落到 */
static void suppress_gov_load(void) {
  char soc[64], path[256];
  read_soc_name(soc, sizeof soc);
  for (char *c = soc; *c; c++) *c = (char)tolower((unsigned char)*c);
  snprintf(path, sizeof path, SF_DATADIR "/adapt.conf");
  FILE *f = fopen(path, "r");
  if (!f) {
    snprintf(path, sizeof path, SF_MODDIR "/adapt/%s.conf", soc);
    f = fopen(path, "r");
    if (!f && strlen(soc) > 1 && soc[strlen(soc) - 1] == 'p') {
      /* SM8750P / SM8850P 这类地区后缀：没有独立文件就吃基础型号的数据。 */
      soc[strlen(soc) - 1] = 0;
      snprintf(path, sizeof path, SF_MODDIR "/adapt/%s.conf", soc);
      f = fopen(path, "r");
    }
  }
  if (!f) {
    log("调速器工作点：%s 无适配数据，不调节", soc);
    return;
  }
  char line[512];
  int cpuset_groups = 0;
  while (fgets(line, sizeof line, f)) {
    char *h = strchr(line, '#');
    if (h) *h = 0;
    trim(line);   /* CRLF 文件：cpuset 掩码会把行尾 \r 当成非法字符 */
    int lv, group; char mask[64];
    if (cpuset_parse_line(line, &lv, &group, mask, sizeof mask)) {
      snprintf(g_cpuset_mask[lv][group], sizeof g_cpuset_mask[lv][group],
               "%s", mask);
      g_cpuset_seen[lv] |= 1u << group;
      cpuset_groups++;
      continue;
    }
    int cl; char cparam[GOV_PARAM_MAX]; long cval;
    if (gov_cl_parse_line(line, &lv, &cl, cparam, sizeof cparam, &cval)) {
      int dup = 0;
      for (int k = 0; k < g_govcln[lv][cl]; k++)
        if (!strcmp(g_govcl[lv][cl][k].param, cparam)) dup = 1;
      if (dup || g_govcln[lv][cl] >= GOV_KV_MAX) continue;
      snprintf(g_govcl[lv][cl][g_govcln[lv][cl]].param, GOV_PARAM_MAX, "%s",
               cparam);
      g_govcl[lv][cl][g_govcln[lv][cl]].val = cval;
      g_govcln[lv][cl]++;
      g_gov_have = 1;
      continue;
    }
    int tier; char param[GOV_PARAM_MAX]; long val;
    if (!gov_parse_line(line, &lv, &tier, param, sizeof param, &val)) continue;
    int dup = 0;
    for (int k = 0; k < g_govkn[lv][tier]; k++)
      if (!strcmp(g_govkv[lv][tier][k].param, param)) dup = 1;
    if (dup || g_govkn[lv][tier] >= GOV_KV_MAX) continue;
    snprintf(g_govkv[lv][tier][g_govkn[lv][tier]].param, GOV_PARAM_MAX, "%s", param);
    g_govkv[lv][tier][g_govkn[lv][tier]].val = val;
    g_govkn[lv][tier]++;
    g_gov_have = 1;
  }
  fclose(f);
  if (g_gov_have) {
    int total = 0;
    for (int lv = 0; lv < GOV_LEVELS; lv++) {
      for (int t = 0; t < GOV_TIER_N; t++) total += g_govkn[lv][t];
      for (int c = 0; c < MAX_POLICY; c++) total += g_govcln[lv][c];
    }
    log("调速器工作点：%s 适配含 %d 组参数（随动态封顶档位生效）", soc, total);
  }
  if (cpuset_groups)
    log("cpuset 分区：%s 适配含 %d 个掩码（随动态封顶档位生效）",
        soc, cpuset_groups);
}

static long sup_cap_for(const struct policy *p);

/* hispeed_ratio = 千分比：把对位 JSON 的 hispeed/该模式上限 */
static long hispeed_from_ratio(long cap, long ratio) {
  if (ratio <= 0) return 0;
  if (cap <= 0) return -1;
  return cap * ratio / 1000;
}

static void suppress_gov_apply(void) {
  if (!suppress_on() || !g_gov_have) return;
  int lv = sup_profile_level();
  if (lv < 0) lv = 0;
  if (lv >= GOV_LEVELS) lv = GOV_LEVELS - 1;
  /* 省电静态低功耗档：hispeed 交给缩放封顶，保持 0 */
  int static_floor = cfg.sup_style && g_sup_level == SUP_FLOOR_LV;
  int n = 0;
  for (int i = 0; i < g_npol; i++) {
    int tier = g_pol[i].tier;
    if (tier < 0 || tier >= GOV_TIER_N) continue;
    char gov[32] = {0};
    char gp[256];
    snprintf(gp, sizeof gp, "%s/scaling_governor", g_pol[i].dir);
    if (read_file(gp, gov, sizeof gov)) continue;
    trim(gov);
    if (!gov[0] || is_vendor_gov(gov)) continue;
    /* hispeed 高于本档封顶等于没写：跳过整簇，避免把无效工作点 */
    long cur_cap = cap_with_cat(&g_pol[i], sup_cap_for(&g_pol[i]));
    int skip_tier = 0;
    if (!static_floor) {
      for (int k = 0; k < g_govkn[lv][tier]; k++) {
        if (!strcmp(g_govkv[lv][tier][k].param, "hispeed_freq") &&
            cur_cap > 0 && g_govkv[lv][tier][k].val >= cur_cap) {
          skip_tier = 1;
          break;
        }
      }
    }
    if (skip_tier) continue;
    for (int k = 0; k < g_govkn[lv][tier]; k++) {
      char path[320];
      const char *param = g_govkv[lv][tier][k].param;
      long val = g_govkv[lv][tier][k].val;
      if (static_floor && !strcmp(param, "hispeed_freq")) val = 0;
      snprintf(path, sizeof path, "%s/%s/%s", g_pol[i].dir, gov, param);
      if (access(path, F_OK))
        snprintf(path, sizeof path, "%s/%s", g_pol[i].dir, param);
      if (access(path, F_OK)) continue;       /* 该内核没有此节点：跳过 */
      if (read_num(path) == val) continue;    /* 已是目标值 */
      if (!write_journaled(NODES_JOURNAL, path, val)) {
        n++;
        if (g_gov_applied_n < 128) {
          snprintf(g_gov_applied[g_gov_applied_n], 128, "%s", path);
          g_gov_applied_n++;
        }
      }
    }
  }
  /* cluster 写法：直接对原始 policy 列表下单，同频簇也不会丢。 */
  for (int i = 0; i < g_nraw; i++) {
    if (!g_govcln[lv][i]) continue;
    struct policy *rp = &g_polraw[i];
    char gov[32] = {0};
    char gp[256];
    snprintf(gp, sizeof gp, "%s/scaling_governor", rp->dir);
    if (read_file(gp, gov, sizeof gov)) continue;
    trim(gov);
    if (!gov[0] || is_vendor_gov(gov)) continue;
    struct policy *rep = NULL;
    long cur_cap = 0;
    for (int j = 0; j < g_npol; j++)
      if (g_pol[j].hw_max == rp->hw_max) {
        rep = &g_pol[j];
        cur_cap = sup_cap_for(rep);
        break;
      }
    char sp[320];
    long cur_max = 0, cur_min = 0;
    snprintf(sp, sizeof sp, "%s/scaling_max_freq", rp->dir);
    cur_max = read_num(sp);
    snprintf(sp, sizeof sp, "%s/scaling_min_freq", rp->dir);
    cur_min = read_num(sp);
    int skip = 0;
    if (!static_floor) {
      for (int k = 0; k < g_govcln[lv][i]; k++) {
        const char *param = g_govcl[lv][i][k].param;
        long val = g_govcl[lv][i][k].val;
        if (!strcmp(param, "hispeed_freq") &&
            ((cur_cap > 0 && val >= cur_cap) ||
             (cur_min > 0 && val < cur_min))) {
          skip = 1;
          break;
        }
      }
    }
    if (skip) continue;
    for (int k = 0; k < g_govcln[lv][i]; k++) {
      char path[320];
      const char *param = g_govcl[lv][i][k].param;
      long val = g_govcl[lv][i][k].val;
      const char *node = param;
      if (static_floor &&
          (!strcmp(param, "hispeed_freq") || !strcmp(param, "hispeed_ratio"))) {
        node = "hispeed_freq";
        val = 0;
      } else if (!strcmp(param, "hispeed_ratio")) {
        node = "hispeed_freq";
        long base = cur_max > 0 ? cur_max : cur_cap;
        long r = hispeed_from_ratio(base, val);
        if (r < 0) continue;
        if (val > 0) {
          if (!rep) continue;
          r = snap_freq(rep, r);
          if (r <= rep->hw_min) continue;  /* 落到硬件最低以下：跳过 */
          if (cur_min > 0 && r < cur_min) continue; /* 低于当前下限：跳过 */
        }
        val = r;
      }
      snprintf(path, sizeof path, "%s/%s/%s", rp->dir, gov, node);
      if (access(path, F_OK)) snprintf(path, sizeof path, "%s/%s", rp->dir, node);
      if (access(path, F_OK)) continue;
      if (read_num(path) == val) continue;
      if (!write_journaled(NODES_JOURNAL, path, val)) {
        n++;
        if (g_gov_applied_n < 128) {
          snprintf(g_gov_applied[g_gov_applied_n], 128, "%s", path);
          g_gov_applied_n++;
        }
      }
    }
  }
  if (n) {
    /* 有的 ROM/perfd 会周期性重置 hispeed 等工作点；写入必须继续做 */
    long now = mono_sec();
    if (now - g_gov_log_at >= 60) {
      g_gov_log_at = now;
      log("调速器工作点已应用（%d 个节点）", n);
    }
  }
}

static void suppress_gov_restore(void) {
  for (int i = 0; i < g_gov_applied_n; i++) boost_restore_path(g_gov_applied[i]);
  g_gov_applied_n = 0;
}

/* 档位附加层：uclamp.max + cpuset + 分级 hispeed 一起随 g_sup_level 换。 */
static int g_sup_extra_level = -1;

static void suppress_extra_restore(void) {
  suppress_uclamp_max_restore();
  cpuset_restore_all();
  suppress_gov_restore();
  g_sup_extra_level = -1;
}

static void suppress_extra_apply(void) {
  if (!suppress_on()) {
    if (g_sup_extra_level >= 0) suppress_extra_restore();
    return;
  }
  int prof = sup_profile_level();
  if (g_sup_extra_level != prof) {
    suppress_uclamp_max_restore();
    cpuset_restore_all();
    suppress_gov_restore();
    g_sup_extra_level = prof;
  }
  suppress_uclamp_max_apply();
  cpuset_apply_level(prof);
  suppress_gov_apply();
}

/* 荣耀/华为/三星的 cpufreq 只认自家调度策略：把 scaling_governor 切成 */
static int governor_locked(void) {
  static int cached = -1;
  if (cached < 0) {
    char b[64] = "", m[64] = "";
    __system_property_get("ro.product.brand", b);
    __system_property_get("ro.product.manufacturer", m);
    cached = !strncasecmp(b, "honor", 5) || !strncasecmp(m, "honor", 5) ||
             !strncasecmp(b, "huawei", 6) || !strncasecmp(m, "huawei", 6) ||
             !strncasecmp(b, "samsung", 7) || !strncasecmp(m, "samsung", 7);
  }
  return cached;
}

/* 在空白分隔的列表里整词查找。 */
static int token_has(const char *list, const char *word) {
  size_t n = strlen(word);
  if (!n) return 0;
  const char *p = list;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    const char *s = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if ((size_t)(p - s) == n && !strncmp(s, word, n)) return 1;
  }
  return 0;
}

static void apply_governor(const struct policy *p) {
  if (!cfg.governor[0]) return;
  if (p->offline) return;   /* 簇离线：节点读不到，别做无效写入 */
  if (governor_locked()) {
    static int warned = 0;
    if (!warned) {
      log("  荣耀/华为/三星内核不切换调速器，CPU_GOVERNOR 已忽略");
      warned = 1;
    }
    return;
  }
  char path[224], cur[64], avail[512];
  snprintf(path, sizeof path, "%s/scaling_governor", p->dir);
  if (read_file(path, cur, sizeof cur)) return;
  if (!strcmp(cur, cfg.governor) || is_vendor_gov(cur)) return;
  snprintf(path, sizeof path, "%s/scaling_available_governors", p->dir);
  if (read_file(path, avail, sizeof avail)) return;
  if (!token_has(avail, cfg.governor)) {
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

/* keep=1：只把名字写回出厂值，账本留着（交权用，回常态还要按用户选择再写一次） */
static int restore_governors_core(int keep) {
  if (access(GOV_JOURNAL, F_OK)) return 0;
  FILE *f = fopen(GOV_JOURNAL, "r");
  if (!f) return 0;
  char line[512], node[512], gov[64];
  int mode, failed = 0, count = 0;
  while (fgets(line, sizeof line, f)) {
    /* govs.saved 里同一个节点有两行 */
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
    /* 权限先记下来，写完再还回去。 */
    int before = read_mode(node);
    chmod(node, 0644);
    if (write_str(node, gov)) failed = 1;
    else count++;
    if (chmod(node, before)) failed = 1;
  }
  fclose(f);
  if (failed) {
    log("WARN 调速器恢复未完成，保留快照下次重试");
    return -1;
  }
  if (!keep) {
    unlink(GOV_JOURNAL);
    log("已恢复 %d 个调速器", count);
  }
  return count;
}

static void restore_governors(void) { (void)restore_governors_core(0); }

/* 游戏判定                                                           */

static char g_pkgs[MAX_PKG][PKG_LEN];
static int g_npkg;
static uint64_t g_pkg_stamp;

/* 按纳秒级指纹判重，避免每轮都重读整个名单 */
static void load_game_pkgs(void) {
  uint64_t stamp = file_stamp(GAME_LIST);
  if (!stamp || stamp == g_pkg_stamp) return;
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
  g_pkg_stamp = stamp;
  log("游戏名单已加载: %d 个包名", g_npkg);
}

/* 抓一行里的前台包名。 */
/* 没有 " u0" 的 ROM（部分 realme / ColorOS）也抓一次 */
static int grab_pkg_relaxed(const char *line, char *out) {
  const char *p = line;
  while ((p = strchr(p, '/'))) {
    const char *s = p;
    while (s > line) {
      char c = s[-1];
      if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == ':')) break;
      s--;
    }
    size_t l = (size_t)(p - s);
    if (l) {
      for (size_t i = 0; i < l; i++)
        if (s[i] == ':') { l = i; break; }   /* 分身/多开：剥掉 :process */
      int dot = 0, ok = 1;
      for (size_t i = 0; i < l; i++) {
        char c = s[i];
        if (c == '.') dot = 1;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) { ok = 0; break; }
      }
      if (ok && dot && l >= 4 && l < PKG_LEN) {
        memcpy(out, s, l);
        out[l] = 0;
        return 0;
      }
    }
    p++;
  }
  return -1;
}

static int grab_pkg(const char *line, char *out) {
  const char *p = strstr(line, " u");
  if (!p) return grab_pkg_relaxed(line, out);
  p += 2; /* 越过 " u"，落在用户号上 */
  if (!isdigit((unsigned char)*p)) return grab_pkg_relaxed(line, out);
  while (isdigit((unsigned char)*p)) p++;
  /* 有些 ROM 的窗口行是 u0a123（用户号 + app id），不是 u0。 */
  if (*p == 'a') {
    p++;
    if (!isdigit((unsigned char)*p)) return grab_pkg_relaxed(line, out);
    while (isdigit((unsigned char)*p)) p++;
  }
  if (*p != ' ') return grab_pkg_relaxed(line, out);
  p++;
  
  size_t l = 0;
  while (p[l] && p[l] != '/' && p[l] != ':') l++;
  if (!p[l] || l >= PKG_LEN) return grab_pkg_relaxed(line, out);
  /* 只接受包名允许的字符，杜绝把 Activity 路径或异常内容当成包名 */
  for (size_t i = 0; i < l; i++) {
    char c = p[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_'))
      return grab_pkg_relaxed(line, out);
  }
  memcpy(out, p, l);
  out[l] = 0;
  return 0;
}

/* 游戏助手/浮窗：ColorOS/realme 的风驰、游戏空间、游戏助手会把焦点抢到 */
static int is_game_overlay(const char *p) {
  return p && (strstr(p, "gamespace") || strstr(p, "gameassist") ||
               strstr(p, "gameopt") || strstr(p, "gamecenter") ||
               strstr(p, "oplus.games") || strstr(p, "coloros.games"));
}

static int is_game(const char *pkg);

/* 从 dumpsys 输出流里取前台包名。 */
static int fg_from_stream(FILE *f, char *out) {
  char line[2048];
  char focus[PKG_LEN] = {0}, app[PKG_LEN] = {0};
  char resumed[PKG_LEN] = {0}, top[PKG_LEN] = {0};
  char game[PKG_LEN] = {0};
  while (fgets(line, sizeof line, f)) {
    int kind = 0;
    if (strstr(line, "mCurrentFocus")) kind = 1;
    else if (strstr(line, "mFocusedApp")) kind = 2;
    else if (strstr(line, "mResumedActivity") ||
             strstr(line, "topResumedActivity")) kind = 3;
    else if (strstr(line, "mTopFullscreenOpaqueWindowState")) kind = 4;
    if (!kind) continue;
    char tmp[PKG_LEN] = {0};
    if (grab_pkg(line, tmp)) continue;
    /* 每一条窗口行都参与“游戏候选”判断：录屏/虚拟显示会把游戏放到 */
    if (!game[0] && is_game(tmp) && !is_game_overlay(tmp))
      snprintf(game, sizeof game, "%s", tmp);
    if (kind == 1 && !focus[0]) snprintf(focus, sizeof focus, "%s", tmp);
    else if (kind == 2 && !app[0]) snprintf(app, sizeof app, "%s", tmp);
    else if (kind == 3 && !resumed[0]) snprintf(resumed, sizeof resumed, "%s", tmp);
    else if (kind == 4 && !top[0]) snprintf(top, sizeof top, "%s", tmp);
  }

  const char *pick = focus[0] ? focus
                   : (app[0] ? app
                   : (resumed[0] ? resumed : top));
  /* 小窗/浮窗抢焦点，但全屏窗口仍是游戏：游戏没退，不要按小窗包名 */
  if (pick && game[0] && top[0] && is_game(top) && !is_game(pick))
    pick = top;
  /* 工具类浮窗在上面挡着时，优先用它下面真正的游戏，其次才是全屏窗口 */
  if (pick && (!strcmp(pick, "com.omarea.vtools") ||
               !strcmp(pick, "bin.mt.plus") ||
               !strcmp(pick, "bin.mt.plus.canary"))) {
    if (game[0]) pick = game;
    else if (top[0]) pick = top;
  }
  /* 厂商游戏助手/风驰浮窗抢焦点时，改看它下面的游戏进程 */
  if (pick && is_game_overlay(pick)) {
    if (game[0]) pick = game;
    else if (app[0] && !is_game_overlay(app)) pick = app;
    else if (top[0] && !is_game_overlay(top)) pick = top;
    else if (resumed[0] && !is_game_overlay(resumed)) pick = resumed;
  }
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

/* 前台探测节流 */
#define FG_PIDS_MAX 64
#define FG_FORCE_S  60   /* 集合一直不变也每 60 秒强探一次（防同进程内切焦点） */
#define FG_FAIL_S   10   /* 探失败后 10 秒内不重复砸 dumpsys */

/* 可观测性：每拍一次 tick，只有真的跑了 dumpsys 才 +1。 */
static long g_tick_n;
static long g_fg_probe_n;
static long g_fg_pids[FG_PIDS_MAX];
static int  g_fg_npid;
static char g_fg_cache[PKG_LEN];
static int  g_fg_cache_ok;
static long g_fg_probed_at;

/* 纯函数：空格/换行分隔的 PID 列表 → 升序数组。返回个数。单测直接喂字符串。 */
static int parse_pid_list(char *b, long *out, int max) {
  int c = 0;
  for (char *p = b; *p && c < max; ) {
    while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
    if (!*p) break;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) break;
    if (v > 0) out[c++] = v;
    p = end;
  }
  for (int i = 1; i < c; i++) {
    long k = out[i];
    int j = i - 1;
    while (j >= 0 && out[j] > k) { out[j + 1] = out[j]; j--; }
    out[j + 1] = k;
  }
  return c;
}

/* 读 top-app 的 PID 集合。返回个数；-1 = 读不到（调用处回落到每拍探测）。 */
static int topapp_pids(long *out, int max) {
  int fd = open("/dev/cpuset/top-app/cgroup.procs", O_RDONLY);
  if (fd < 0) return -1;
  static char b[2048];
  ssize_t n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0) return -1;
  b[n] = 0;
  return parse_pid_list(b, out, max);
}

static int same_pids(const long *a, int na, const long *b, int nb) {
  if (na != nb) return 0;
  for (int i = 0; i < na; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* 带节流的前台探测。语义与 foreground_pkg 相同：0 = 拿到包名，-1 = 没拿到。 */
static int foreground_pkg_throttled_ex(char *out, int force) {
  long pids[FG_PIDS_MAX];
  int np = topapp_pids(pids, FG_PIDS_MAX);
  long now = mono_sec();
  int changed = 1;
  if (np >= 0) {
    changed = !same_pids(pids, np, g_fg_pids, g_fg_npid);
    for (int i = 0; i < np; i++) g_fg_pids[i] = pids[i];
    g_fg_npid = np;
  }
  long ttl = g_fg_cache_ok ? FG_FORCE_S : FG_FAIL_S;
  if (!force && !changed && g_fg_probed_at > 0 && now - g_fg_probed_at < ttl) {
    if (!g_fg_cache_ok) return -1;      /* 上一拍也没探到：别每拍都砸一次 */
    snprintf(out, PKG_LEN, "%s", g_fg_cache);
    return 0;
  }
  int rc = foreground_pkg(out);
  g_fg_probe_n++;
  g_fg_probed_at = now ? now : 1;
  if (rc == 0) {
    snprintf(g_fg_cache, sizeof g_fg_cache, "%s", out);
    g_fg_cache_ok = 1;
  } else {
    g_fg_cache[0] = 0;
    g_fg_cache_ok = 0;
  }
  return rc;
}

static int foreground_pkg_throttled(char *out) {
  return foreground_pkg_throttled_ex(out, 0);
}

/* 包名比较：忽略进程后缀（:xxx）。部分游戏的前台窗口报的是 */
static int pkg_base_eq(const char *a, const char *b) {
  size_t la = strcspn(a, ":");
  size_t lb = strcspn(b, ":");
  return la == lb && !strncmp(a, b, la);
}

static int is_game(const char *pkg) {
  for (int i = 0; i < g_npkg; i++)
    if (pkg_base_eq(g_pkgs[i], pkg)) return 1;
  return 0;
}

/* 分应用档位（app_modes.conf）                                        */

/* 一行一条，值是以下三种之一 */
#define MODE_FULL (-1)
#define MODE_KEEP (-2)
#define MODE_SOFF (-3)   /* 熄屏时交还，亮屏仍按全局 */
struct app_mode {
  char pkg[PKG_LEN];
  int pct;    /* MODE_FULL / MODE_KEEP / MODE_SOFF / 0（CPU 跟随全局）或 1..100 */
  int big;    /* 0 = 跟随 pct/全局；否则 BIG 簇 1..100 */
  int little; /* 0 = 跟随 pct/全局；否则 LITTLE 簇 1..100 */
  int gpu;    /* 0 = GPU 跟随全局，否则 1..100 */
};
static struct app_mode g_modes[MAX_PKG];
static int g_nmode;
static uint64_t g_mode_stamp;

/* 当前生效的应用档位：包名 + 值。空串 = 没有生效中的专属档位 */
static char g_app[PKG_LEN];
static int g_app_pct;
static int g_app_big;
static int g_app_little;
/* 应用专属 GPU 档位，0 = 跟随全局 GPU_PCT。full 走交还系统那条路，不在这里。 */
static int g_app_gpu;

/* 读一个档位文件。路径做成参数，便于指向临时件而不改用户生效的配置。 */
static int load_app_modes_from(const char *path) {
  uint64_t stamp = file_stamp(path);
  if (!stamp) { g_nmode = 0; g_mode_stamp = 0; return 0; }
  if (stamp == g_mode_stamp) return g_nmode;
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
    /* 值后面允许跟一段注释：学习写回来的那条长这样 */
    char *hash = strchr(val, '#');
    if (hash) { *hash = 0; trim(val); }
    /* 包名必须含点且字符合法：和 api.sh 的 is_package 同一套判据。 */
    if (!pkg[0] || !strchr(pkg, '.') || strlen(pkg) >= PKG_LEN) continue;
    for (char *c = pkg; *c; c++)
      if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_') { pkg[0] = 0; break; }
    if (!pkg[0]) continue;
    int pct = 0, gpuv = 0, bigv = 0, litv = 0;
    char *colon = strchr(val, ':');
    char *gpu_part = NULL;
    if (colon) {
      *colon = 0;
      gpu_part = colon + 1;
      trim(val);
      trim(gpu_part);
    }
    if (!val[0] && (!gpu_part || !gpu_part[0])) continue;
    char *slash = strchr(val, '/');
    if (slash) {
      /* big/little：只给 BIG 和 LITTLE 两档；MID/MID2 跟随全局。 */
      *slash = 0;
      char *bp = val, *lp = slash + 1;
      trim(bp); trim(lp);
      if (!bp[0] || !lp[0]) continue;
      if (bp[strspn(bp, "0123456789")] || lp[strspn(lp, "0123456789")]) continue;
      bigv = atoi(bp);
      litv = atoi(lp);
      if (bigv < 1 || bigv > 100 || litv < 1 || litv > 100) continue;
    } else if (!val[0]) {
      /* :40 = 只设 GPU，CPU 四档全部跟随全局 */
    } else if (!strcmp(val, "full") || !strcmp(val, "FULL")) {
      if (gpu_part) continue;   /* full 不带 GPU 段 */
      pct = MODE_FULL;
    } else if (!strcmp(val, "keep") || !strcmp(val, "KEEP")) {
      if (gpu_part) continue;
      pct = MODE_KEEP;      /* 只影响熄屏；亮屏仍按全局分档 */
    } else if (!strcmp(val, "soff") || !strcmp(val, "SOFF")) {
      if (gpu_part) continue;
      pct = MODE_SOFF;      /* 熄屏时交还系统；亮屏仍按全局分档 */
    } else if (val[strspn(val, "0123456789")]) {
      continue;             /* 非纯数字 */
    } else {
      pct = atoi(val);
      if (pct < 1 || pct > 100) continue;
    }
    if (gpu_part && gpu_part[0]) {
      if (gpu_part[strspn(gpu_part, "0123456789")]) continue;
      gpuv = atoi(gpu_part);
      if (gpuv < 1 || gpuv > 100) continue;
    }
    int dup = 0;
    for (int i = 0; i < g_nmode; i++)
      if (!strcmp(g_modes[i].pkg, pkg)) {
        g_modes[i].pct = pct;
        g_modes[i].big = bigv;
        g_modes[i].little = litv;
        g_modes[i].gpu = gpuv;
        dup = 1;
        break;
      }
    if (dup) continue;      /* 同一包重复出现：取后一条，不再加一行 */
    snprintf(g_modes[g_nmode].pkg, PKG_LEN, "%s", pkg);
    g_modes[g_nmode].pct = pct;
    g_modes[g_nmode].big = bigv;
    g_modes[g_nmode].little = litv;
    g_modes[g_nmode].gpu = gpuv;
    g_nmode++;
  }
  fclose(f);
  g_mode_stamp = stamp;
  log("分应用档位已加载: %d 条", g_nmode);
  return g_nmode;
}

static void load_app_modes(void) { load_app_modes_from(MODE_FILE); }

/* 应用分类：视频/社交/阅读。分类上限只压不抬，写入时与全局/应用档取更低。 */
enum { CAT_VIDEO = 0, CAT_SOCIAL = 1, CAT_READING = 2, CAT_COUNT = 3 };
static const char *const g_cat_names[CAT_COUNT] = {"video", "social", "reading"};
static int g_cat_cpu[CAT_COUNT][4] = {
  {44, 44, 44, 44}, {55, 55, 55, 55}, {50, 50, 50, 50}};
static int g_cat_gpu[CAT_COUNT] = {0, 0, 0};
struct cat_pkg { char pkg[PKG_LEN]; int cat; };
static struct cat_pkg g_catpkgs[MAX_PKG];
static int g_ncatpkg;
static int g_cat_idx = -1;
static int g_cat_gpu_cur;
static uint64_t g_cat_stamp;
static uint64_t g_cat_def_stamp;

static int cat_id(const char *s) {
  for (int i = 0; i < CAT_COUNT; i++)
    if (!strcasecmp(s, g_cat_names[i])) return i;
  return -1;
}

static void cat_parse_file(const char *path, int with_packages) {
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    trim(line);
    if (!line[0] || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k == ' ' || *k == '\t') k++;
    while (*v == ' ' || *v == '\t') v++;
    char *ke = k + strlen(k);
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
    char *ve = v + strlen(v);
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
    char *us = strchr(k, '_');
    if (!us) {
      int cat = cat_id(k);
      if (with_packages && cat >= 0 && strchr(v, '.') && g_ncatpkg < MAX_PKG) {
        snprintf(g_catpkgs[g_ncatpkg].pkg, PKG_LEN, "%s", v);
        g_catpkgs[g_ncatpkg].cat = cat;
        g_ncatpkg++;
      }
      continue;
    }
    *us = 0;
    int cat = cat_id(k);
    const char *field = us + 1;
    if (cat < 0) continue;
    if (!strcmp(field, "gpu")) {
      g_cat_gpu[cat] = clampi(atoi(v), 0, 100, 0);
    } else if (!strcmp(field, "cpu")) {
      int pct = clampi(atoi(v), 0, 100, g_cat_cpu[cat][0]);
      for (int t = 0; t < 4; t++) g_cat_cpu[cat][t] = pct;
    } else if (!strcmp(field, "cpu_big")) {
      g_cat_cpu[cat][0] = clampi(atoi(v), 0, 100, g_cat_cpu[cat][0]);
    } else if (!strcmp(field, "cpu_mid2")) {
      g_cat_cpu[cat][1] = clampi(atoi(v), 0, 100, g_cat_cpu[cat][1]);
    } else if (!strcmp(field, "cpu_mid")) {
      g_cat_cpu[cat][2] = clampi(atoi(v), 0, 100, g_cat_cpu[cat][2]);
    } else if (!strcmp(field, "cpu_little")) {
      g_cat_cpu[cat][3] = clampi(atoi(v), 0, 100, g_cat_cpu[cat][3]);
    }
  }
  fclose(f);
}

static void cat_load_paths(const char *user, const char *defs) {
  for (int c = 0; c < CAT_COUNT; c++) {
    int d = c == CAT_VIDEO ? 44 : c == CAT_SOCIAL ? 55 : 50;
    for (int t = 0; t < 4; t++) g_cat_cpu[c][t] = d;
  }
  g_cat_gpu[CAT_VIDEO] = g_cat_gpu[CAT_SOCIAL] = g_cat_gpu[CAT_READING] = 0;
  g_ncatpkg = 0;
  /* defaults 只提供按芯片算好的数值；包名归属仍以用户文件为准。 */
  if (defs) cat_parse_file(defs, 0);
  if (user) cat_parse_file(user, 1);
}

static void load_app_categories(void) {
  const char *path = CAT_FILE;
  uint64_t stamp = file_stamp(path);
  if (!stamp) { path = CAT_FALLBACK; stamp = file_stamp(path); }
  uint64_t dstamp = file_stamp(CAT_DEFAULTS);
  if (stamp == g_cat_stamp && dstamp == g_cat_def_stamp) return;
  g_cat_stamp = stamp;
  g_cat_def_stamp = dstamp;
  cat_load_paths(stamp ? path : NULL, dstamp ? CAT_DEFAULTS : NULL);
}

static int cat_lookup(const char *pkg) {
  if (!pkg || !pkg[0]) return -1;
  for (int i = 0; i < g_ncatpkg; i++)
    if (pkg_base_eq(g_catpkgs[i].pkg, pkg)) return g_catpkgs[i].cat;
  return -1;
}

static long cap_with_cat(const struct policy *p, long want) {
  if (p->cat_cap > 0 && p->cat_cap < want) return p->cat_cap;
  return want;
}

static void category_apply(const char *pkg) {
  int cat = cat_lookup(pkg);
  g_cat_idx = cat;
  g_cat_gpu_cur = cat >= 0 ? g_cat_gpu[cat] : 0;
  for (int i = 0; i < g_npol; i++) {
    g_pol[i].cat_cap = 0;
    int t = g_pol[i].tier & 3;
    int pct = cat >= 0 ? g_cat_cpu[cat][t] : 0;
    if (pct > 0) g_pol[i].cat_cap = cap_from_pct(&g_pol[i], pct);
  }
}

static void category_clear(void) {
  g_cat_idx = -1;
  g_cat_gpu_cur = 0;
  for (int i = 0; i < g_npol; i++) g_pol[i].cat_cap = 0;
}

/* 命中返回 1 并给出 CPU 统一值 / BIG / LITTLE / GPU 四段 */
static int mode_lookup(const char *pkg, int *out, int *out_big, int *out_little,
                       int *out_gpu) {
  if (!pkg || !pkg[0]) return 0;
  for (int i = 0; i < g_nmode; i++)
    if (pkg_base_eq(g_modes[i].pkg, pkg)) {
      if (out) *out = g_modes[i].pct;
      if (out_big) *out_big = g_modes[i].big;
      if (out_little) *out_little = g_modes[i].little;
      if (out_gpu) *out_gpu = g_modes[i].gpu;
      return 1;
    }
  return 0;
}

/* 重算每个 policy 的封顶值。app_pct>0 = 四档统一；app_big/app_little>0 = */
static void set_caps_tiers(int app_pct, int app_big, int app_little, const char *why) {
  for (int i = 0; i < g_npol; i++) {
    int t = g_pol[i].tier & 3;
    int pct, explicit = 0, big_min_hit = 0;
    if (t == 0 && app_big > 0) {
      pct = app_big;
      explicit = 1;
    } else if (t == 3 && app_little > 0) {
      pct = app_little;
      explicit = 1;
    } else if (app_pct > 0) {
      pct = app_pct;
      explicit = 1;
    } else {
      pct = cfg.pct[t];
    }
    /* 超大核省电：全局 BIG 没被应用单独指定时压到最低频 */
    if (!explicit && cfg.big_min && t == 0) {
      pct = 1;
      explicit = 1;
      big_min_hit = 1;
    }
    if (pct < 1 || pct > 100) pct = 1;
    /* 专属档位也吃实时偏移（热了照样降），但不吃充电态那块地板 */
    pct = big_min_hit ? 1 : eff_pct(pct, explicit);
    g_pol[i].pct = pct;
    g_pol[i].cap = cap_floor_apply(&g_pol[i],
                                   snap_freq(&g_pol[i], cap_from_pct(&g_pol[i], pct)));
  }
  if (app_pct > 0 || app_big > 0 || app_little > 0)
    log("  封顶重算（%s 专属档位 %d/%d/%d）", why, app_pct, app_big, app_little);
  else log("  封顶重算（%s）", why);
}

static void set_caps(int app_pct, const char *why) {
  set_caps_tiers(app_pct, 0, 0, why);
}

/* GPU 的生效值。只吃温控与电量阶梯两项收紧偏移，PSI 不参与 */
static long gpu_target_for(int base_pct, int delta) {
  int pct = (base_pct > 0 ? base_pct : cfg.gpu_pct) + delta;
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  return gpu_snap(pct);
}

static long gpu_target(void) {
  int base = g_app_gpu > 0 ? g_app_gpu : cfg.gpu_pct;
  if (g_cat_gpu_cur > 0 && g_cat_gpu_cur < base) base = g_cat_gpu_cur;
  if (cfg.strategy == 1) {
    int cap = cfg.fsm_gpu[g_sm.fsm] > 0 ? cfg.fsm_gpu[g_sm.fsm]
                                         : kSmartGpuPct[g_sm.fsm];
    if (cap > 0 && cap < base) base = cap;
  }
  struct adapt a;
  adapt_snapshot(&a);
  /* 充电地板同样抬 GPU：只抬 CPU 不抬 GPU，插电打游戏仍会被 GPU 上限 */
  /* GPU 只吃"收紧"项：温控 + 电量阶梯。PSI 是 CPU 排队信号 */
  int d = -(a.th_steps * cfg.ad.th_step) - a.batt_cut;
  if (cfg.strategy == 1) d = -a.batt_cut;
  /* 应用 GPU 档是用户明确设的，电量阶梯不往下盖它。 */
  if (g_app_gpu > 0) d += a.batt_cut;
  /* 和 CPU 同一口径：有收紧时不抬充电地板 */
  if (g_app_gpu <= 0 && !g_screen_off && a.chg_on && a.chg_pct > base && d >= 0)
    base = a.chg_pct;
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

/* GPU 稳态巡检：厂商 boost/调频会把上限顶回去，CPU 那条巡检管不到它。 */
static void gpu_patrol(void) {
  if (!g_gpu.ok) return;
  long t = gpu_target();
  long got = read_num(g_gpu.max_node);
  if (g_gpu.ged) got = gpu_ged_freq_of((int)got);
  else if (g_gpu.max_is_mhz && got > 0) got *= 1000000;
  if (got > t) write_gpu(t, 0);
  g_gpu.active = t;
  gpu_min_to_floor();
  /* 厂商 powerhal / 调试口可能把 idle_timer 改回 80：每轮补一次 */
  kgsl_idle_trim();
}

/* 动态封顶（持续负载驱动的三档天花板） */
#define SUP_FALL       20    /* busy<20：计入降档 */
#define SUP_MIN_TICKS  5     /* 到地板且低载，连续这么多拍才压下限 */
#define SUP_RELEASE_FLOOR 60 /* 地板→中间：busy 连续过线才放 */
#define SUP_RELEASE_MID   75 /* 中间→最优解：要求更高，避免两级连跳 */
#define SUP_STATIC_TICKS   3
#define SUP_LEVEL_HOLD_S   6 /* 换档后先稳这么久，再允许继续升/降 */
#define SUP_STATIC_HZ  960000L /* 省电风格静置上限：对齐到真实频点 */
/* 风格参数（SUP_STYLE）：均衡 = 地板 80%、中间 90%；省电 = 地板 960MHz */
static int sup_floor(void)      { return cfg.sup_style ? 70 : 80; }
static int sup_mid(void)        { return cfg.sup_style ? 85 : 90; }
static int sup_fall_ticks(void) { return cfg.sup_style ? 2 : 3; }

static int g_sup_fall_n;    /* 连续低载计数 */
static int g_sup_up_n;      /* 连续高载计数：2 拍才放一档，滤掉滑动抖动 */
static int g_sup_min_n;     /* 到底后连续空闲计数 */
static int g_sup_min_on;    /* 下限已压 */
static int g_sup_floor_busy_n; /* 连续高载计数（地板要求更久） */
static long g_sup_level_at; /* 当前档位进入时刻，换档后先稳住 */
static int g_sup_last_busy = -1; /* 最近一次压制负载样本，仅 status 诊断用 */
static int g_sup_chg_pause; /* 插电暂停中（插电不封顶策略） */
static long g_sup_reapply_at; /* 上次补写 boost/uclamp 的时刻 */
/* 内核钳制记忆：写 max 被夹回（目标低于该簇实际下限）时记住回读值 */
static long g_sup_stuck[MAX_POLICY];
static long g_sup_stuck_at[MAX_POLICY];

static int sup_pct(void) {
  if (g_sup_level == SUP_FLOOR_LV) return sup_floor();
  if (g_sup_level == SUP_MID_LV) return sup_mid();
  return 100;
}

/* 附加层（uclamp.max / cpuset / hispeed）用哪一档 */
static int sup_profile_level(void) {
  if (!cfg.sup_style) return g_sup_level;
  return g_sup_level <= SUP_OPT ? SUP_MID_LV : SUP_FLOOR_LV;
}

/* 省电风格的“静置地板”：不是 cap 的 70%，而是绝对 960MHz 起步 */
static long sup_static_floor_for(const struct policy *p) {
  long f = SUP_STATIC_HZ;
  if (p->hw_min > f) f = p->hw_min;
  if (p->cap > 0 && f > p->cap) f = p->cap;
  f = snap_up_freq(p, f);
  if (f <= 0) f = p->cap;
  return cap_floor_apply(p, f);
}

static long sup_cap_for(const struct policy *p) {
  if (g_sup_level <= SUP_OPT) return p->cap;
  if (cfg.sup_style && g_sup_level == SUP_FLOOR_LV)
    return sup_static_floor_for(p);
  /* 按用户设定的百分比继续收；BIG_MIN 已经把 cap 放到硬件最低时 */
  long want = p->cap * sup_pct() / 100;
  if (p->hw_min > 0 && want < p->hw_min) want = p->hw_min;
  return cap_floor_apply(p, snap_freq(p, want));
}

/* 用户设的进阶最低频；0 = 没设，返回 0。目标夹在 hw_min 与当前封顶之间 */
static long user_min_for(const struct policy *p) {
  int pct = cfg.min_pct[p->tier & 3];
  if (pct < 1) return 0;
  long want = p->hw_max * pct / 100;
  want = snap_freq(p, want);
  if (p->hw_min > 0 && want < p->hw_min) want = p->hw_min;
  if (p->cap > 0 && want > p->cap) want = p->cap;
  return want;
}

/* 模块自己的下限目标：用户设了就用用户的，否则用硬件最低。 */
static long min_floor_for(const struct policy *p) {
  long u = user_min_for(p);
  return u > 0 ? u : p->hw_min;
}

/* 下限压放（记账）。on=0 时只还原本策略压过的，不动系统后来的改写。 */
static void sup_min_clamp(int on) {
  for (int i = 0; i < g_npol; i++) {
    if (g_pol[i].hw_min <= 0) continue;
    char path[256];
    long target = (cfg.sup_style && g_sup_level == SUP_FLOOR_LV)
                      ? g_pol[i].hw_min
                      : min_floor_for(&g_pol[i]);
    if (target <= 0) continue;
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    if (on) {
      long cur = read_num(path);
      if (cur <= 0) continue;
      if (cur != target && !write_journaled(NODES_JOURNAL, path, target))
        g_sup_min_on = 1;
      /* 下限已经/刚刚被放松，之前因 min>max 留下的"写不进"退避要立刻作废 */
      g_sup_stuck[i] = 0;
    } else if (g_sup_min_on) {
      boost_restore_path(path);
    }
  }
  if (!on) g_sup_min_on = 0;
}

/* 退出压制——只清标志与还原下限，不动天花板 */
static void sup_cancel(void) {
  g_sup_level = 0;
  g_sup_fall_n = 0;
  g_sup_up_n = 0;
  g_sup_min_n = 0;
  g_sup_floor_busy_n = 0;
  g_sup_level_at = 0;
  g_sup_reapply_at = 0;
  if (g_sup_min_on) sup_min_clamp(0);
}

/* 退出压制并回到生效封顶：留在常态的暂停点（熄屏、配置关闭）用。 */
static void sup_exit(void) {
  sup_cancel();
  suppress_boost_restore();
  suppress_smooth_restore();
  suppress_gov_restore();
  suppress_uclamp_restore();
  suppress_extra_restore();
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    long want = cap_with_cat(&g_pol[i], g_pol[i].cap);
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    long cur = read_num(path);
    if (cur < 0) continue;          /* 簇离线：节点读不到，跳过 */
    if (want > 0 && cur != want) {
      if (!write_journaled(NODES_JOURNAL, path, want)) {
        long got = read_num(path);
        g_pol[i].last_cap = want;
        g_pol[i].last_got = got > 0 ? got : want;
        be_apply_max(&g_pol[i], want);
      }
    }
  }
}

/* 每个快拍调一次（亮屏常态尾部，1s 粒度）。 */
static void suppress_tick(void) {
  if (!suppress_on()) return;
  long nows = mono_sec();
  /* 系统在切前台/亮屏时会把 input_boost_ms 恢复成 100，手指一滑就重新抬频。 */
  suppress_boost_apply();
  if (!g_sup_reapply_at || nows - g_sup_reapply_at >= 60) {
    suppress_uclamp_apply();
    g_sup_reapply_at = nows;
  }
  /* 逐簇贴顶会把"刚好停在 cap"误判成饥饿：静置档的 cap 本身就是 */
  int busy = cpu_busy_pct_ex(1);
  if (busy < 0) busy = SUP_FALL;           /* 无样本：按"不升不压"处理 */
  g_sup_last_busy = busy;
  int hungry = 0;
  int i;
  if (g_sup_level_at == 0) g_sup_level_at = nows;
  int stable = nows - g_sup_level_at >= SUP_LEVEL_HOLD_S;
  int static_floor = cfg.sup_style && g_sup_level == SUP_FLOOR_LV;
  if (g_sup_level >= SUP_MID_LV) {
    int release_busy = static_floor ? SUP_RELEASE_FLOOR : SUP_RELEASE_MID;
    if (stable && busy >= release_busy) {
      if (g_sup_floor_busy_n < 5) g_sup_floor_busy_n++;
    } else {
      g_sup_floor_busy_n = 0;
    }
    hungry = g_sup_floor_busy_n >=
             (static_floor ? SUP_STATIC_TICKS : 2);
  } else {
    g_sup_floor_busy_n = 0;
  }
  if (hungry) {
    /* 连续高载才放一档：单次滑动/切屏的瞬时顶帽不把天花板拉回满频。 */
    if (g_sup_up_n < 2) {
      g_sup_up_n++;
    } else {
      if (g_sup_level == SUP_FLOOR_LV) {
        g_sup_level = SUP_MID_LV;
        g_sup_level_at = nows;
        g_sup_floor_busy_n = 0;
        log("动态封顶：持续高载，先放到 %d%%", sup_pct());
      } else if (g_sup_level == SUP_MID_LV) {
        g_sup_level = SUP_OPT;
        g_sup_level_at = nows;
        g_sup_floor_busy_n = 0;
        log("动态封顶：持续高载，放开到最优解");
      }
      g_sup_up_n = 0;
    }
    g_sup_fall_n = 0;
    g_sup_min_n = 0;
    if (g_sup_min_on) {
      sup_min_clamp(0);
      log("动态封顶：活动恢复，下限还原");
    }
  } else if (busy < SUP_FALL) {
    g_sup_up_n = 0;
    /* 无饥饿且占用低：先连续 N 拍降到中间档，再连续 N 拍降到地板。 */
    if (g_sup_fall_n < sup_fall_ticks()) g_sup_fall_n++;
    else if (g_sup_level == SUP_OPT) {
      g_sup_level = SUP_MID_LV;
      g_sup_level_at = nows;
      g_sup_fall_n = 0;
      log("动态封顶：持续低载，先收到 %d%%", sup_pct());
    } else if (g_sup_level == SUP_MID_LV) {
      g_sup_level = SUP_FLOOR_LV;
      g_sup_level_at = nows;
      g_sup_floor_busy_n = 0;
      g_sup_fall_n = 0;
      if (cfg.sup_style)
        log("动态封顶：持续低载，收到静置档 960MHz");
      else
        log("动态封顶：持续低载，收到地板 %d%%", sup_pct());
    }
    /* 到地板且低载：压下限（抖音这类静置场景的空闲大头）。 */
    if (g_sup_level == SUP_FLOOR_LV) {
      if (g_sup_min_n < SUP_MIN_TICKS) g_sup_min_n++;
      else if (!g_sup_min_on) {
        sup_min_clamp(1);
        log("动态封顶：静置空闲，下限压到硬件最低");
      }
    }
  } else {
    /* 负载在释放线以下：保持当前档不升不降 */
    g_sup_fall_n = 0;
    g_sup_up_n = 0;
  }
  /* 天花板写入：只写与当前值不同的拍（level 0 时等价于 1s 巡检压回）。 */
  for (i = 0; i < g_npol; i++) {
    char path[256];
    long want = cap_with_cat(&g_pol[i], sup_cap_for(&g_pol[i]));
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    long cur = read_num(path);
    /* 回读值不等于当初写不进去的那个值，或已经达到目标：退避前提已消失。 */
    if (g_sup_stuck[i] && cur > 0 &&
        (cur != g_sup_stuck[i] || cur == want))
      g_sup_stuck[i] = 0;
    if (g_sup_stuck[i] && want <= g_sup_stuck[i] &&
        nows - g_sup_stuck_at[i] < 600)
      continue;                              /* 已知写不进去：退避 */
    if (cur > 0 && cur != want) {
      if (!write_journaled(NODES_JOURNAL, path, want)) {
        long got = read_num(path);
        if (got == cur && got != want) {     /* 写了没动：被内核夹住 */
          g_sup_stuck[i] = got;
          g_sup_stuck_at[i] = nows;
        } else {
          g_sup_stuck[i] = 0;                /* 写进去了（或值变了） */
        }
        g_pol[i].last_cap = want;
        g_pol[i].last_got = got > 0 ? got : want;
        be_apply_max(&g_pol[i], want);
      }
    }
  }
  /* 档位换了就换 uclamp.max / cpuset / hispeed；同一档内幂等补写 */
  suppress_extra_apply();
}

/* 模式切换                                                           */

/* 配置指纹：变了才需要重新探测分档，重启时无需 */
static uint64_t cfg_stamp(void) {
  return file_stamp(CFG_FILE);
}

/* force：配置变更或启动时为 1，无条件重写一遍。 */
/* 建目录（含父级）。数据目录 /data/adb/soc_freqcap 不在解包范围内 */
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

/* 开机至今的秒数。用 /proc/uptime 而不是墙钟 */
static long uptime_sec(void) {
  char b[64];
  if (read_file("/proc/uptime", b, sizeof b)) return -1;
  double u = strtod(b, NULL);
  return u <= 0 ? -1 : (long)u;
}

/* 屏幕是否亮着。只在 SCREEN_UNCAP=1 时才被调用 —— */
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

/* 亮屏信号取值：blank 语义 0=亮、1=灭；backlight 语义 >0=亮、0=灭。 */
int screen_value_on(long v, int invert) {
  if (v < 0) return -1;
  return invert ? (v == 0) : (v > 0);
}

/* current_now 归一化成 mA。PJZ110 实测是 mA 且充电为负（与 AOSP 常见约定相反） */
long batt_current_ma(long raw, int unit_ma) {
  long a = raw < 0 ? -raw : raw;
  return unit_ma ? a : a / 1000;
}

static long batt_current_ma_now(void) {
  long raw;
  if (!read_signed(cfg.batt_current_path, &raw)) return -1;
  return batt_current_ma(raw, cfg.batt_current_ma_unit);
}

/* 深度空闲纯步进。返回 0=不变、1=进入深睡、2=电流突增（让调用方做一次亮屏确认）。 */
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

/* 挑一个亮屏信号：用户指定 > fb0/blank > 任意 backlight/brightness。 */
static void screen_state_init(void) {
  char want[192];
  snprintf(want, sizeof want, "%s", cfg.screen_state_path);
  /* 已经因为假归零降级过，且用户没改 SCREEN_STATE_PATH：保持 dumpsys 兜底。 */
  if (g_scr_fallback && !strcmp(want, g_scr_cfg_seen)) {
    g_scr_path[0] = 0;
    g_scr_invert = 0;
    g_scr_check = 0;
    log("亮屏信号继续使用 dumpsys 兜底（配置未变）");
    return;
  }
  snprintf(g_scr_cfg_seen, sizeof g_scr_cfg_seen, "%s", want);
  g_scr_fallback = 0;
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
  /* 优先已知可用的节点：三星 One UI 的 panel0-backlight 才是真值 */
  if (!g_scr_path[0]) {
    static const char *const pref[] = {
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/backlight/panel0-backlight/actual_brightness",
        "/sys/class/leds/lcd-backlight/brightness",
        "/sys/class/backlight/panel/brightness",
        "/sys/class/backlight/panel/actual_brightness",
        NULL
    };
    for (int i = 0; pref[i]; i++) {
      if (access(pref[i], F_OK)) continue;
      snprintf(g_scr_path, sizeof g_scr_path, "%s", pref[i]);
      g_scr_invert = 0;
      break;
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
  else {
    g_scr_fallback = 1;
    log("WARN 没有可用的亮屏信号，改用 dumpsys power 兜底");
  }
}

static int screen_sig_read(void) {
  if (!g_scr_path[0]) return -1;
  return screen_value_on(read_num(g_scr_path), g_scr_invert);
}

/* ACTIVE 态判断是否已经熄屏。信号说灭时用 dumpsys 复核一次 */
static int screen_is_off_now(int force_check) {
  int sig = screen_sig_read();
  if (sig == 0) {
    if (screen_awake()) {
      g_scr_bad++;
      log("WARN 亮屏信号说灭但系统仍 Awake（%d/3）", g_scr_bad);
      if (g_scr_bad >= 3 && g_scr_path[0]) {
        g_scr_path[0] = 0;
        g_scr_fallback = 1;
        log("停用亮屏信号，改用 dumpsys power 兜底（重载配置也不再启用）");
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

/* 开机满血窗口是否还在有效。service.sh 写的是"第几秒结束"（uptime 口径） */
static int boot_boost_active(void) {
  long until = read_num(BOOT_BOOST_FILE);
  if (until <= 0) return 0;
  long up = uptime_sec();
  if (up < 0) return 0;                 /* 读不到 uptime 就当窗口不在，保守压回 */
  if (up >= until) { unlink(BOOT_BOOST_FILE); return 0; }
  return 1;
}

/* 超大核离线：只关最高频簇，游戏/满血时先把核拉回在线。 */
#define BIG_OFF_JOURNAL RUN_DIR "/bigoffline.saved"
static int g_big_offline_on;

static int big_cpu_list(int *out, int max) {
  static int ids[MAX_CORES];
  static int n = -1;
  if (n < 0) {
    n = 0;
    struct policy all[MAX_POLICY];
    int na = detect_policies_raw(all);
    long ladder[MAX_POLICY];
    int m = na > 0 ? build_ladder(all, na, ladder) : 0;
    for (int c = 0; c < MAX_CORES; c++) {
      if (!core_exists(c)) continue;
      int pol = core_policy_of(c);
      if (pol < 0) continue;
      for (int i = 0; i < na; i++) {
        if (all[i].pol_no != pol) continue;
        if (tier_key_of(ladder, m, all[i].hw_max) == 0 && n < MAX_CORES)
          ids[n++] = c;
        break;
      }
    }
  }
  int k = n > max ? max : n;
  memcpy(out, ids, (size_t)k * sizeof *out);
  return k;
}

static int cpu_online_path(int c, char *out, size_t n) {
  return snprintf(out, n, CPUBASE "/cpu%d/online", c);
}

static int boot_completed_now(void) {
  char b[92] = "";
  return __system_property_get("sys.boot_completed", b) > 0 && b[0] == '1';
}

static void big_offline_restore(const char *why) {
  int had = access(BIG_OFF_JOURNAL, F_OK) == 0;
  if (!had && !g_big_offline_on) return;
  if (had) str_restore_all(BIG_OFF_JOURNAL, "超大核离线");
  g_big_offline_on = 0;

  int ids[MAX_CORES];
  int nb = big_cpu_list(ids, MAX_CORES);
  if (had && nb > 0) {
    /* 等内核把核拉回 online 再交还频率，最多等 2.5s。 */
    for (int try = 0; try < 25; try++) {
      int all = 1;
      for (int i = 0; i < nb; i++) {
        char path[96];
        cpu_online_path(ids[i], path, sizeof path);
        if (read_num(path) != 1) { all = 0; break; }
      }
      if (all) break;
      usleep(100000);
    }
  }
  int off = 0;
  for (int i = 0; i < nb; i++) {
    char path[96];
    cpu_online_path(ids[i], path, sizeof path);
    if (read_num(path) == 0) off++;
  }
  if (off) log("WARN 超大核离线：%s后仍有 %d 个核未上线", why, off);
  else log("超大核离线：%s，超大核已上线", why);
}

static void big_offline_apply(void) {
  static long log_at = -600, fail_at = 0;
  if (!cfg.big_offline) {
    if (g_big_offline_on) big_offline_restore("关闭");
    return;
  }
  if (!boot_completed_now() || boot_boost_active()) return;

  int ids[MAX_CORES];
  int nb = big_cpu_list(ids, MAX_CORES);
  if (nb <= 0) return;

  int total = 0, big_online = 0;
  for (int c = 0; c < MAX_CORES; c++) {
    char path[96];
    cpu_online_path(c, path, sizeof path);
    if (!access(path, F_OK)) {
      if (read_num(path) == 1) total++;
    } else if (core_exists(c)) {
      total++;  /* 没有 online 节点的小核按常在线算 */
    }
  }
  for (int i = 0; i < nb; i++) {
    char path[96];
    cpu_online_path(ids[i], path, sizeof path);
    if (read_num(path) == 1) big_online++;
  }
  /* 单 policy 机器会把所有核判成 BIG：至少留一颗在线，绝不整机关核。 */
  if (total <= 0 || big_online >= total) {
    static int warned;
    if (!warned) {
      warned = 1;
      log("WARN 超大核离线：关掉后没有剩余在线核，跳过");
    }
    return;
  }

  int changed = 0, failed = 0;
  for (int i = nb - 1; i >= 0; i--) {
    char path[96];
    cpu_online_path(ids[i], path, sizeof path);
    long cur = read_num(path);
    if (cur != 1) continue;
    str_write_journaled(BIG_OFF_JOURNAL, path, "0");
    if (read_num(path) == 0) changed++;
    else failed++;
  }

  long now = mono_sec();
  if (changed) {
    g_big_offline_on = 1;
    if (now - log_at >= 600) {
      log_at = now;
      log("超大核离线：已关闭 %d 个核%s", changed,
          failed ? "（部分内核拒绝 offline）" : "");
    }
  } else if (failed && now - fail_at >= 300) {
    fail_at = now;
    log("WARN 超大核离线：内核拒绝 offline，%d 个核仍在线", failed);
  }
}

/* 只做 CPU 上限巡检，不碰 state/权限。apply_normal 和熄屏 keep 轻巡检共用。 */
static long g_capfloor_log_at = -600;  /* 初值 -600：开机头 10 分钟也能打第一条 */
/* 厂商 perf/热控每隔几秒把 max 顶回去时，巡检每拍都会写一次 */
static long g_restore_log_at[MAX_POLICY];

static void caps_patrol(int force) {
  for (int i = 0; i < g_npol; i++) {
    char path[224];
    long cur;
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    cur = read_num(path);
    if (cur < 0) {
      /* 核心全离线时 policy 节点会读不到：静默跳过，回来那拍自动重压。 */
      if (!g_pol[i].offline) {
        g_pol[i].offline = 1;
        log("  %-8s 节点暂不可读（核心离线/热插拔），暂停写入", g_pol[i].name);
      }
      continue;
    }
    if (g_pol[i].offline) {
      g_pol[i].offline = 0;
      log("  %-8s 已恢复，重新压回封顶", g_pol[i].name);
    }
    long want = cap_with_cat(&g_pol[i], g_pol[i].cap);
    /* 常态巡检：厂商把上限顶高了就压回去 */
    if (!force && cur <= want) continue;
    /* force 升帽时先确认当前值就是模块上一轮写的那个。 */
    if (force && cur <= want) {
      /* 厂商在模块写完后把上限压得更低：保留厂商值，不在 force 里抢回去。 */
      if (g_pol[i].last_got > 0 && cur < g_pol[i].last_got) continue;
      if (!(g_pol[i].last_got > 0 && cur == g_pol[i].last_got &&
            want > g_pol[i].last_cap))
        continue;
    }
    long now = mono_sec();
    /* 同一目标、同一回读值刚刚已经证明写不动：10 分钟内不再重试。 */
    if (want == g_pol[i].unreach_cap && cur == g_pol[i].unreach_got &&
        now - g_pol[i].unreach_at < 600)
      continue;
    /* 内核可能把允许下限锁在 cpuinfo_min 之上（本机 1,017,600）。 */
    char mpath[224];
    snprintf(mpath, sizeof mpath, "%s/scaling_min_freq", g_pol[i].dir);
    long floor = read_num(mpath);
    if (floor > 0 && want < floor) {
      if (now - g_capfloor_log_at >= 600) {
        g_capfloor_log_at = now;
        log("%s 封顶目标 %ld 低于内核下限 %ld，按 %ld 生效", g_pol[i].name,
            want, floor, floor);
      }
      want = floor;
    }
    if (cur == want) {          /* 写下去也不会变，别做无效写入 */
      g_pol[i].last_cap = want;
      g_pol[i].last_got = cur;
      g_pol[i].unreach_cap = 0;
      g_pol[i].unreach_got = 0;
      g_pol[i].unreach_at = 0;
      continue;
    }
    if (!write_journaled(NODES_JOURNAL, path, want)) {
      long got = read_num(path);
      g_pol[i].last_cap = want;
      g_pol[i].last_got = got;
      be_apply_max(&g_pol[i], want);
      if (got > 0 && got == cur && got != want) {
        /* 写下去没动：内核真实下限比读到的 min 更高，记住这对值 */
        if (force) {
          char gov[32] = {0}, gp[256];
          snprintf(gp, sizeof gp, "%s/scaling_governor", g_pol[i].dir);
          if (!read_file(gp, gov, sizeof gov)) trim(gov);
          log("  %-8s 上限 -> %ld%s%s", g_pol[i].name, got,
              got < want ? "（厂商已压更低）" : "（写入未生效）",
              (got >= want && is_vendor_gov(gov)) ? "，厂商调速器接管" : "");
        }
        else if (now - g_capfloor_log_at >= 600) {
          g_capfloor_log_at = now;
          log("%s 上限被内核夹在 %ld，按 %ld 生效", g_pol[i].name, got, got);
        }
        g_pol[i].unreach_cap = want;
        g_pol[i].unreach_got = got;
        g_pol[i].unreach_at = now;
      } else {
        g_pol[i].unreach_cap = 0;
        g_pol[i].unreach_got = 0;
        g_pol[i].unreach_at = 0;
        if (force)
          log("  %-8s 上限 -> %ld%s", g_pol[i].name, got,
              got < want ? "（厂商已压更低）" : "");
        else if (now - g_restore_log_at[i] >= 60) {
          g_restore_log_at[i] = now;
          log("RESTORE %s: %ld -> %ld（厂商顶回后已压回，60s 内同类不再刷）",
              g_pol[i].name, cur, got);
        }
      }
    }
    apply_governor(&g_pol[i]);
  }
}

static void apply_normal(int force) {
  int changed = force || strcmp(g_state, "normal") != 0;
  if (changed) {
    log("→ 常态封顶");
    perms_restore(); /* 先收回游戏期交出去的写权限 */
    /* 交权时调速器被还原成出厂值；回常态要把用户选的那个再写回去。 */
    for (int i = 0; i < g_npol; i++) apply_governor(&g_pol[i]);
    /* 开关关掉时也要走一次还原：用户可能刚在页面上把它关掉。 */
    if (cfg.gpu_boost_off) boost_off_apply();
    else boost_off_restore();
    /* 后台分组常驻收口：与动态封顶无关，交权态已在 hand_back_full 还原。 */
    if (cfg.bg_light) bg_light_apply();
    else bg_light_restore();
    /* 动态封顶开启时：walt 拉频源中和 + 升频限速 + 调速器工作点参数 */
    if (suppress_on()) {
      if (!g_sup_chg_pause) {
        suppress_boost_apply();
        suppress_smooth_apply();
        suppress_uclamp_apply();
        suppress_extra_apply();
      } else {
        suppress_boost_restore();
        suppress_smooth_restore();
        suppress_gov_restore();
        suppress_uclamp_restore();
        suppress_extra_restore();
      }
    } else {
      suppress_boost_restore();
      suppress_smooth_restore();
      suppress_gov_restore();
      suppress_uclamp_restore();
      suppress_extra_restore();
    }
    gpu_min_to_floor();
    apply_gpu_cap(1);
  }
  caps_patrol(changed);
  if (changed) cpu_min_refresh();
  if (!g_screen_off) cpu_min_patrol();
  big_offline_apply();
  if (changed) {
    snprintf(g_state, sizeof g_state, "normal");
    snprintf(g_reason, sizeof g_reason, "-");
  }
}

/* 交权态（游戏 / 满血 / 熄屏）下不能抢回节点：游戏分支只在包名变化时 */
static int reclaim_caps_allowed(void) {
  return strcmp(g_state, "game") != 0 && strcmp(g_state, "full") != 0;
}

/* 配置变了就吸收新值。返回 1 表示"这一轮要重算封顶并立刻生效"。 */
static int reload_if_changed(uint64_t seen) {
  uint64_t now = cfg_stamp();
  if (now == seen) return 0;
  char prev_gov[32];
  snprintf(prev_gov, sizeof prev_gov, "%s", cfg.governor);
  log("检测到配置变更，重新计算封顶值");
  int prev_strategy = cfg.strategy;
  int prev_big_off = cfg.big_offline;
  load_cfg();
  if (cfg.strategy != prev_strategy)
    smart_on_strategy_edge(prev_strategy, cfg.strategy);
  if (cfg.big_offline != prev_big_off) {
    if (!cfg.big_offline) big_offline_restore("关闭");
    else if (!strcmp(g_state, "normal")) big_offline_apply();
  }
  screen_state_init(); /* SCREEN_STATE_PATH / 策略变了要重新挑信号 */
  detect_gpu(0);        /* GPU 百分比变了要重新挑吸附频点 */
  /* 从某个调速器改成"不干预"：不能只是停止写入，要把模块改过的名字写回 */
  if (prev_gov[0] && !cfg.governor[0]) {
    if (!access(GOV_JOURNAL, F_OK)) restore_governors();
    else log("WARN 调速器快照缺失，无法恢复原值");
  }
  return 1;
}

/* 交还系统的公共动作：值放到硬件最高 + 写权限交给厂商框架。 */
/* 交还写权限：优先还原 NODES journal 里记的出厂权限，没有才用 0666。 */
static void hand_back_mode(const char *path) {
  if (access(path, F_OK)) return;
  int orig = journal_mode(NODES_JOURNAL, path);
  if (!journal_has(PERM_JOURNAL, path))
    journal_add(PERM_JOURNAL, path, 0, orig >= 0 ? orig : read_mode(path));
  chmod(path, orig >= 0 ? (mode_t)orig : 0666);
}

static void hand_back_full(const char *why) {
  log("→ %s", why);
  /* 先放掉 PPM/msm 第二道锁，否则 sysfs 写回硬件最高也会被厂商硬限压着。 */
  be_handback();
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    long cur = read_num(path);
    if (cur < 0) continue;   /* 簇离线：别往不存在的节点写 */
    /* 只撤模块自己写的帽：current 等于模块最近一次目标或设定帽，才写回硬件最高。 */
    long own = g_pol[i].last_cap > 0 ? g_pol[i].last_cap : g_pol[i].cap;
    if (cur > 0 && cur != own && cur != g_pol[i].cap) {
      log("  %-8s 保留厂商上限 %ld（模块帽 %ld）", g_pol[i].name, cur, own);
      continue;
    }
    if (!write_journaled(NODES_JOURNAL, path, snap_freq(&g_pol[i], cap_base(&g_pol[i]))))
      log("  %-8s -> 厂商上限 %ld", g_pol[i].name, cap_base(&g_pol[i]));
  }
  if (g_gpu.ok) {
    if (g_gpu.ged) {
      /* GED 节点读回的是 OPP 序号，不能和 Hz 的 active 直接比 */
      write_gpu(g_gpu.max_hz, 1);
    } else {
      long gcur = read_num(g_gpu.max_node);
      if (g_gpu.max_is_mhz && gcur > 0) gcur *= 1000000;
      /* 同理：厂商已经把 GPU 上限压得更低时，交权不要再把它抬回硬件最高。 */
      if (gcur > 0 && g_gpu.active > 0 && gcur != g_gpu.active) {
        log("  GPU 保留厂商上限 %ldHz", gcur);
      } else {
        write_gpu(g_gpu.max_hz, 1);
      }
    }
    if (g_gpu.pwr_node[0]) write_journaled(NODES_JOURNAL, g_gpu.pwr_node, 0);
  }

  
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_max_freq", g_pol[i].dir);
    hand_back_mode(path);
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    hand_back_mode(path);
  }
  const char *nodes[] = {g_gpu.max_node, g_gpu.min_node, g_gpu.pwr_node};
  for (size_t i = 0; i < 3; i++) {
    if (!nodes[i][0]) continue;
    hand_back_mode(nodes[i]);
  }
  /* 调速器也属于"官方调度"：模块改过就还原成接管前的名字，没改过则不动。 */
  restore_governors_core(1);
  /* GPU Boost 开关是常态省电项，交权态一律放回去。 */
  boost_off_restore();
  if (g_gpu.min_node[0]) boost_restore_path(g_gpu.min_node);
  touch_boost_guard();
  /* 后台分组常驻收口也属于常态省电项，交权态还干净原值。 */
  bg_light_restore();
  /* 动态封顶带的 walt 拉频源中和与升频限速：交权给厂商干净的原值。 */
  suppress_boost_restore();
  suppress_smooth_restore();
  /* 工作点参数：交权后系统可能切换调速器，这里一并还原。 */
  suppress_gov_restore();
  suppress_uclamp_restore();
  /* uclamp.max / cpuset / 分级 hispeed 也属于动态封顶，交权时一起还。 */
  suppress_extra_restore();
  /* 动态封顶的下限与标志一并清理（天花板由上面的满频写入处理）。 */
  sup_cancel();
  /* CPU 下限是常态省电项：交权（游戏/满血/熄屏解除）时还给系统原值。 */
  cpu_min_restore();
  log("  CPU/GPU 节点写权限已交由系统接管");
  /* state 由调用方决定：游戏/满血应用是 game，熄屏交还是 full。 */
}

/* 游戏解锁路径的日志文案保持稳定：外部脚本按这一行判断解锁是否生效。 */
static void unlock_game(void) {
  char why[PKG_LEN + 24];
  big_offline_restore("游戏");
  snprintf(g_reason, sizeof g_reason, "game");
  snprintf(why, sizeof why, "游戏解锁并交还系统: %s", g_game);
  hand_back_full(why);
  snprintf(g_state, sizeof g_state, "game");
}

/* 熄屏专属：把每簇的 scaling_min_freq 压到下限目标（用户最低或硬件最低）。 */
static void cpu_min_to_floor(void) {
  int n = 0;
  for (int i = 0; i < g_npol; i++) {
    if (g_pol[i].hw_min <= 0) continue;
    long target = min_floor_for(&g_pol[i]);
    if (target <= 0) continue;
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    long cur = read_num(path);
    if (cur <= 0 || cur == target) continue;
    if (write_journaled(NODES_JOURNAL, path, target)) continue;
    /* 写下去回读不符：可能是内核把下限锁在更高处，也可能是用户最低 */
    long got = read_num(path);
    if (got > 0 && got != target) {
      log("熄屏：%s 下限目标 %ld，实际 %ld", g_pol[i].name, target, got);
    } else {
      n++;
    }
  }
  if (n) log("熄屏：CPU 下限已压到目标（%d 个簇）", n);
}

/* 亮屏常态的 CPU 下限巡检（CPU_MIN_LOCK）。 */
static long g_min_writes, g_min_last, g_min_fail_at;
static int  g_min_fails;
/* 每簇的下限战况：累计被夹回 9 次（约 3 轮退避）判定"系统把下限钉死" */
static int  g_min_pfails[MAX_POLICY];
static int  g_min_giveup[MAX_POLICY];
static long g_min_pinned[MAX_POLICY];
static long g_min_target[MAX_POLICY];
static long g_min_give_at;

/* 配置刚改过：清掉 30 秒节流，下一拍立刻按新下限写一次。 */
static void cpu_min_refresh(void) {
  g_min_last = 0;
}

/* 只还原本模块按“进阶最低频/下限锁”压过的簇，不动 suppression 或厂商 */
static void cpu_min_restore_user(void) {
  for (int i = 0; i < g_npol; i++) {
    if (g_min_target[i] <= 0) continue;
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    boost_restore_path(path);
    g_min_target[i] = 0;
  }
}

static void cpu_min_patrol(void) {
  int user_any = 0, fsm_any = 0;
  for (int i = 0; i < g_npol; i++)
    if (cfg.min_pct[g_pol[i].tier & 3] > 0) { user_any = 1; break; }
  if (cfg.strategy == 1)
    for (int g = 0; g < 2; g++)
      if (cfg.fsm_min[g_sm.fsm][g] > 0) { fsm_any = 1; break; }
  /* 动态封顶开启时，硬件最低由压制的低载静置接管；但用户明确设了 */
  if (suppress_on() && !user_any) return;
  if (!cfg.cpu_min_lock && !user_any && !fsm_any) {
    for (int i = 0; i < g_npol; i++) {
      if (g_min_target[i] <= 0) continue;
      char path[256];
      snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
      boost_restore_path(path);
      g_min_target[i] = 0;
    }
    return;
  }
  /* 省电静态档的下限已由压制改为硬件最低；巡检这时再写用户下限 */
  if (suppress_on() && cfg.sup_style && g_sup_level == SUP_FLOOR_LV) return;
  long now = mono_sec();
  if (g_min_fails >= 3) {
    if (now - g_min_fail_at < 600) return;
    /* 退避到期必须清零，否则 g_min_fails 永远 >=3，之后每拍都写 */
    g_min_fails = 0;
    g_min_fail_at = 0;
  }
  if (g_min_last && now - g_min_last < 30) return;
  for (int i = 0; i < g_npol; i++) {
    if (g_pol[i].hw_min <= 0) continue;
    long user_target = user_min_for(&g_pol[i]);
    int grp = (g_pol[i].tier & 3) == 0 ? 0 : 1;
    int fm = cfg.strategy == 1 ? cfg.fsm_min[g_sm.fsm][grp] : 0;
    long fsm_target = fm > 0 ? g_pol[i].hw_max * fm / 100 : 0;
    if (fsm_target > 0) {
      if (fsm_target > g_pol[i].cap) fsm_target = g_pol[i].cap;
      if (fsm_target < g_pol[i].hw_min) fsm_target = g_pol[i].hw_min;
    }
    if (fsm_target <= 0 && user_target <= 0 && !cfg.cpu_min_lock) {
      if (g_min_target[i] > 0) {
        char path[256];
        snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
        boost_restore_path(path);
        g_min_target[i] = 0;
      }
      continue;
    }
    long target = fsm_target > 0 ? fsm_target
                 : user_target > 0 ? user_target : g_pol[i].hw_min;
    if (target != g_min_target[i]) {
      g_min_target[i] = target;
      g_min_giveup[i] = 0;
      g_min_pfails[i] = 0;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    long cur = read_num(path);
    if (cur <= 0 || cur == target) continue;
    if (g_min_giveup[i]) {
      /* 钉死簇：30 分钟复探一次，其余时间跳过。 */
      if (now - g_min_give_at < 1800) continue;
      g_min_give_at = now;
    }
    if (write_journaled(NODES_JOURNAL, path, target)) continue;
    g_min_writes++;
    g_min_last = now;
    long got = read_num(path);
    /* 用户最低频只要“不低于目标”就算成功；CPU_MIN_LOCK 的下限是最低频 */
    int ok = got == target || ((user_target > 0 || fsm_target > 0) && got > target);
    if (!ok) {
      if (++g_min_pfails[i] >= 9 && !g_min_giveup[i]) {
        g_min_giveup[i] = 1;
        g_min_pinned[i] = got;
        g_min_give_at = now;
        log("CPU 下限：%s 被系统钉在 %ld，本启动不再尝试（30 分钟复探一次）",
            g_pol[i].name, got);
      }
      int before = g_min_fails;
      if (++g_min_fails >= 3) {
        g_min_fail_at = now;
        /* 一轮里多个簇先后被夹回时只在刚跨过阈值那一下打一行 */
        if (before < 3)
          log("CPU 下限目标 %ld（%s）连续 3 次没达成，暂停 10 分钟",
              target, g_pol[i].name);
      }
    } else {
      g_min_fails = 0;
      g_min_pfails[i] = 0;
      if (g_min_giveup[i]) {
        g_min_giveup[i] = 0;
        log("CPU 下限：%s 系统已放开，恢复下限巡检", g_pol[i].name);
      }
    }
  }
}

static void cpu_min_restore(void) {
  for (int i = 0; i < g_npol; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/scaling_min_freq", g_pol[i].dir);
    long orig;
    if (!journal_value(NODES_JOURNAL, path, &orig)) continue;
    long target = orig;
    /* 账本里的"原值"可能已经是厂商瞬时 boost（本机 policy6 是 1,958,400）。 */
    if (g_pol[i].cap > 0 && g_pol[i].hw_min > 0 && orig >= g_pol[i].cap) {
      target = g_pol[i].hw_min;
      log("%s 下限原值 %ld 会顶住封顶 %ld，按硬件最低 %ld 还原",
          g_pol[i].name, orig, g_pol[i].cap, target);
    }
    if (read_num(path) == target) continue;
    write_num(path, target);
    int m = journal_mode(NODES_JOURNAL, path);
    if (m >= 0) chmod(path, (mode_t)m);
  }
}

/* 进入熄屏轻巡检：只有常态封顶才需要交还；游戏/满血应用已经是交权态 */
/* 熄屏时放宽定时器松弛度，让 30/60s 的巡检唤醒与系统 Alarm/基带事件合并 */
static void set_timer_slack(unsigned long ns) {
  (void)prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0);
}

static void enter_screen_off(void) {
  if (g_screen_off) return;
  int mpct = 0, mbig = 0, mlit = 0;
  /* 熄屏白名单优先：pkg=soff 的应用在前台时，熄屏照样交还系统 */
  int have_mode = !strcmp(g_state, "normal") && g_fg[0] &&
                  mode_lookup(g_fg, &mpct, &mbig, &mlit, NULL);
  int soff_full = have_mode && mpct == MODE_SOFF;
  int global_cap = cfg.screen_policy == SCREEN_CAP && !strcmp(g_state, "normal") &&
                   !soff_full;
  int keep = global_cap || (have_mode && mpct == MODE_KEEP);
  g_screen_off = 1;
  set_timer_slack(3000000000UL);
  g_scr_check = 0;
  g_deep = 0;
  g_idle_ema = 0;
  g_idle_hold = 0;
  g_soff_th = 0;
  /* 动态封顶同步收口：保持封顶的路径把天花板交还生效封顶 */
  if (keep) sup_exit();
  else sup_cancel();
  g_soff_keep = keep;
  snprintf(g_soff_pkg, sizeof g_soff_pkg, "%s",
           keep && !global_cap ? g_fg : "");
  /* smart + 保持封顶（global cap 或分应用 keep）：收到空闲并重立基线。 */
  if (keep) smart_on_enter_screen_off(1);
  if (keep) {
    if (global_cap) {
      /* 交权态/PSI 额度不能带到熄屏；下限只在熄屏压，亮屏还原。 */
      adapt_cancel();
      cpu_min_to_floor();
    }
    /* 亮屏时 keep 不动分档；熄屏时把可能残留的应用档位收回全局，再做轻巡检。 */
    if (g_app_pct || g_app_big || g_app_little || g_app_gpu) {
      if (global_cap) assign_pct(g_pol, g_npol);
      else set_caps(0, "熄屏 keep");
      g_app_pct = 0;
      g_app_big = 0;
      g_app_little = 0;
      g_app_gpu = 0;
      apply_gpu_cap(1);
    }
    g_app[0] = 0;
    /* 充电地板在熄屏时不参与（eff_pct/gpu_target 已按 g_screen_off 跳过） */
    if (global_cap) assign_pct(g_pol, g_npol);
    caps_patrol(0);
    gpu_patrol();
    log("进入熄屏轻巡检（%d s，保持封顶%s）", cfg.soff_interval,
        global_cap ? "，不吃充电地板" : "");
    return;
  }
  if (!strcmp(g_state, "normal")) {
    g_app[0] = 0;
    g_app_pct = 0;
    g_app_big = 0;
    g_app_little = 0;
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
  set_timer_slack(50000UL);
  g_scr_check = 0;
  g_deep = 0;
  g_idle_ema = 0;
  g_idle_hold = 0;
  g_soff_keep = 0;
  g_soff_th = 0;
  sup_exit();
  g_soff_pkg[0] = 0;
  log("亮屏，恢复常态巡检");
  /* 重立占用基线；smart 且预热档打开且非游戏时，本圈 smart_tick 只升不降预热。 */
  smart_on_leave_screen_off();
  /* cap：下限还原、封顶重算（充电地板重新生效）。 */
  if (cfg.screen_policy == SCREEN_CAP && !strcmp(g_state, "normal")) {
    cpu_min_restore();
    apply_normal(1);
  }
  /* 熄屏交还的才需要压回；游戏态/keep 态保持原样，交给主循环重新判定。 */
  if (!strcmp(g_state, "full")) {
    g_app[0] = 0;
    g_app_pct = 0;
    g_app_big = 0;
    g_app_little = 0;
    g_app_gpu = 0;
    apply_normal(1);
  }
}

/* 配置解析：坏值一律回落到默认值，绝不让脏配置生效                   */

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

/* cfg_get 的 %[^\n] 要求值至少一个字符，空值会被当成键不存在。 */
static int cfg_key_present(const char *key) {
  FILE *f = fopen(CFG_FILE, "r");
  if (!f) return 0;
  char line[512], k[128];
  int found = 0;
  while (fgets(line, sizeof line, f)) {
    const char *s = line;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '#' || !*s) continue;
    if (sscanf(s, "%127[^= \t]", k) == 1 && !strcmp(k, key)) {
      found = 1;
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
  cfg.grace = 30;
  cfg.gpu_pct = 45;
  cfg.gpu_min_lock = 1;
  cfg.gpu_boost_off = 1;
  cfg.bg_light = 1;
  cfg.big_min = 0;
  cfg.big_offline = 0;
  cfg.cpu_min_lock = 0;
  cfg.cap_backend = CAP_BE_AUTO;
  /* 熄屏策略：基准 keep（旧行为）。全新安装的 conf 写 uncap */
  cfg.screen_uncap = 0;
  /* 熄屏轻巡检（保持封顶 + 30s 轻巡检）是正收益，固定默认开；页面不再 */
  cfg.screen_policy = SCREEN_CAP;
  cfg.soff_interval = 15;
  cfg.screen_state_path[0] = 0;
  cfg.idle_current_ma = 250;
  cfg.idle_hold_cycle = 2;
  cfg.strategy = 0;
  cfg.suppress = 0;
  cfg.sup_style = 0;
  cfg.app_switch_tier = 2;
  memset(cfg.fsm_cpu, 0, sizeof cfg.fsm_cpu);
  memset(cfg.fsm_min, 0, sizeof cfg.fsm_min);
  memset(cfg.fsm_gpu, 0, sizeof cfg.fsm_gpu);
  cfg.fsm_cpu_hi_in = cfg.fsm_cpu_hi_out = 0;
  cfg.fsm_cpu_mod_in = cfg.fsm_cpu_mod_out = 0;
  cfg.fsm_gpu_game_in = cfg.fsm_gpu_game_out = 0;
  cfg.fsm_gpu_hi_in = cfg.fsm_gpu_hi_out = 0;
  cfg.fsm_up_s = cfg.fsm_down_s = cfg.fsm_pair_s = 0;
  cfg.fsm_warmup_s = cfg.fsm_dwell_heavy_s = cfg.fsm_dwell_game_s = 0;
  cfg.fsm_dwell_sustain_s = cfg.fsm_thermal_hold_s = 0;
  cfg.fsm_psi_mod = 30; cfg.fsm_psi_hi = 50;
  cfg.fsm_skin_hi = 46; cfg.fsm_skin_lo = 42;
  cfg.fsm_adpf_hi = 85;
  cfg.fsm_hr_soft = 70; cfg.fsm_hr_hard = 45;
  /* 按簇给不同的升降频节奏。这组等于旧默认/均衡基准，gov_rate_for() */
  cfg.gov_up_us[0] = 3000; cfg.gov_down_us[0] = 1000;
  cfg.gov_up_us[1] = 2000; cfg.gov_down_us[1] = 1500;
  cfg.gov_up_us[2] = 1000; cfg.gov_down_us[2] = 2000;
  cfg.idle_interval = 30;
  cfg.wake_delta_ma = 150;
  snprintf(cfg.batt_current_path, sizeof cfg.batt_current_path,
           "/sys/class/power_supply/battery/current_now");
  cfg.batt_current_ma_unit = 0;
  cfg.boot_full_s = 60;
  /* 默认不碰调速器：部分厂商内核（荣耀/华为）切到 conservative 会硬重启。 */
  cfg.governor[0] = 0;
  /* 实时偏移的默认值。插电放宽默认开（电从充电器来，没必要压着） */
  cfg.ad.psi = 1;   cfg.ad.psi_gain = 5;   cfg.ad.psi_step = 5;
  cfg.ad.psi_hold = 30;  cfg.ad.psi_back = 15;
  cfg.ad.th = 0;    cfg.ad.th_high = 75;   cfg.ad.th_low = 68;
  cfg.ad.th_step = 5;   cfg.ad.th_max = 25;  cfg.ad.th_back = 20;
  cfg.ad.chg = 1;   cfg.ad.chg_pct = 100;
  cfg.ad.up_max = 15;    cfg.ad.down_max = 30;
  cfg.ad.batt = 0;  cfg.ad.batt_low = 20;  cfg.ad.batt_low_cut = 5;
  cfg.ad.batt_crit = 10; cfg.ad.batt_crit_cut = 10;
  for (int i = 0; i < 4; i++) cfg.pct[i] = dflt[i];
  for (int i = 0; i < 4; i++) cfg.min_pct[i] = 0;
  cfg.cap_floor_m = 0;

  if (cfg_get("INTERVAL", v) == 0) cfg.interval = clampi(atoi(v), 2, 60, 5);
  {
    /* 旧键仍读：只给 keep 策略下那条旧的熄屏解锁分支用，不再影响策略选择。 */
    if (cfg_get("SCREEN_UNCAP", v) == 0)
      cfg.screen_uncap = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? 1 : 0;
    /* 没有 SCREEN_POLICY 的手写/极老配置一律按 cap（轻巡检常开） */
    if (cfg_get("SCREEN_POLICY", v) == 0) {
      if (!strcasecmp(v, "uncap")) cfg.screen_policy = SCREEN_UNCAP;
      else if (!strcasecmp(v, "keep")) cfg.screen_policy = SCREEN_KEEP;
      else if (!strcasecmp(v, "cap") || !strcasecmp(v, "cap_off"))
        cfg.screen_policy = SCREEN_CAP;
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
  if (cfg_get("STRATEGY", v) == 0)
    cfg.strategy = !strcasecmp(v, "smart") ? 1 : 0;
  /* 开机满血窗口上限 600s：再长就不是"开机提速"而是"忘了限频"了 */
  if (cfg_get("BOOT_FULL_S", v) == 0)
    cfg.boot_full_s = clampi(atoi(v), 0, 600, 60);
  if (cfg_get("GAME_GRACE_SEC", v) == 0)
    cfg.grace = clampi(atoi(v), cfg.interval, 300, 30);
  if (cfg_get("SUPPRESS", v) == 0)
    cfg.suppress = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("SUP_STYLE", v) == 0)
    cfg.sup_style = !strcasecmp(v, "sav") ? 1 : 0;
  if (cfg_get("APP_SWITCH_TIER", v) == 0)
    cfg.app_switch_tier = clampi(atoi(v), 0, 3, 2);
  {
    static const char *const st[FSM_STATES] = {
      "DEEP", "LIGHT", "MOD", "HEAVY", "SUSTAIN", "GAME"
    };
    static const char *const grp[2] = { "BIG", "OTHER" };
    char key[64];
    for (int s = 0; s < FSM_STATES; s++) {
      for (int g = 0; g < 2; g++) {
        snprintf(key, sizeof key, "FSM_CPU_%s_%s", st[s], grp[g]);
        if (cfg_get(key, v) == 0) cfg.fsm_cpu[s][g] = clampi(atoi(v), 0, 100, 0);
        snprintf(key, sizeof key, "FSM_MIN_%s_%s", st[s], grp[g]);
        if (cfg_get(key, v) == 0) cfg.fsm_min[s][g] = clampi(atoi(v), 0, 100, 0);
      }
      snprintf(key, sizeof key, "FSM_GPU_%s", st[s]);
      if (cfg_get(key, v) == 0) cfg.fsm_gpu[s] = clampi(atoi(v), 0, 100, 0);
    }
    if (cfg_get("FSM_CPU_HI_IN", v) == 0) cfg.fsm_cpu_hi_in = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_CPU_HI_OUT", v) == 0) cfg.fsm_cpu_hi_out = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_CPU_MOD_IN", v) == 0) cfg.fsm_cpu_mod_in = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_CPU_MOD_OUT", v) == 0) cfg.fsm_cpu_mod_out = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_GPU_GAME_IN", v) == 0) cfg.fsm_gpu_game_in = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_GPU_GAME_OUT", v) == 0) cfg.fsm_gpu_game_out = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_GPU_HI_IN", v) == 0) cfg.fsm_gpu_hi_in = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_GPU_HI_OUT", v) == 0) cfg.fsm_gpu_hi_out = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_UP_S", v) == 0) cfg.fsm_up_s = clampi(atoi(v), 2, 120, 0);
    if (cfg_get("FSM_DOWN_S", v) == 0) cfg.fsm_down_s = clampi(atoi(v), 2, 300, 0);
    if (cfg_get("FSM_PAIR_S", v) == 0) cfg.fsm_pair_s = clampi(atoi(v), 2, 300, 0);
    if (cfg_get("FSM_WARMUP_S", v) == 0) cfg.fsm_warmup_s = clampi(atoi(v), 0, 600, 0);
    if (cfg_get("FSM_DWELL_HEAVY_S", v) == 0) cfg.fsm_dwell_heavy_s = clampi(atoi(v), 0, 600, 0);
    if (cfg_get("FSM_DWELL_GAME_S", v) == 0) cfg.fsm_dwell_game_s = clampi(atoi(v), 0, 600, 0);
    if (cfg_get("FSM_DWELL_SUSTAIN_S", v) == 0) cfg.fsm_dwell_sustain_s = clampi(atoi(v), 0, 600, 0);
    if (cfg_get("FSM_THERMAL_HOLD_S", v) == 0) cfg.fsm_thermal_hold_s = clampi(atoi(v), 0, 1800, 0);
    if (cfg_get("FSM_PSI_MOD", v) == 0) cfg.fsm_psi_mod = clampi(atoi(v), 0, 100, 0);
    if (cfg_get("FSM_PSI_HI", v) == 0) cfg.fsm_psi_hi = clampi(atoi(v), 0, 100, 0);
    if (cfg_get("FSM_SKIN_HI", v) == 0) cfg.fsm_skin_hi = clampi(atoi(v), 0, 120, 0);
    if (cfg_get("FSM_SKIN_LO", v) == 0) cfg.fsm_skin_lo = clampi(atoi(v), 0, 120, 0);
    if (cfg_get("FSM_ADPF_HI", v) == 0) cfg.fsm_adpf_hi = clampi(atoi(v), 0, 100, 0);
    if (cfg_get("FSM_HR_SOFT", v) == 0) cfg.fsm_hr_soft = clampi(atoi(v), 1, 100, 0);
    if (cfg_get("FSM_HR_HARD", v) == 0) cfg.fsm_hr_hard = clampi(atoi(v), 1, 100, 0);
  }
  {
    /* 分簇限速；某簇没写就用旧键（旧版是三簇统一值）。 */
    static const char *const upk[3] = {
      "GOV_UP_BIG", "GOV_UP_MID", "GOV_UP_LITTLE",
    };
    static const char *const dnk[3] = {
      "GOV_DOWN_BIG", "GOV_DOWN_MID", "GOV_DOWN_LITTLE",
    };
    int have_up[3] = {0, 0, 0}, have_dn[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
      if (cfg_get(upk[i], v) == 0) {
        cfg.gov_up_us[i] = clampi(atoi(v), 100, 50000, cfg.gov_up_us[i]);
        have_up[i] = 1;
      }
      if (cfg_get(dnk[i], v) == 0) {
        cfg.gov_down_us[i] = clampi(atoi(v), 100, 50000, cfg.gov_down_us[i]);
        have_dn[i] = 1;
      }
    }
    int old_up = -1, old_dn = -1;
    if (cfg_get("GOV_UP_US", v) == 0)
      old_up = clampi(atoi(v), 100, 50000, 1000);
    if (cfg_get("GOV_DOWN_US", v) == 0)
      old_dn = clampi(atoi(v), 100, 50000, 1000);
    for (int i = 0; i < 3; i++) {
      if (!have_up[i] && old_up >= 0) cfg.gov_up_us[i] = old_up;
      if (!have_dn[i] && old_dn >= 0) cfg.gov_down_us[i] = old_dn;
    }
  }
  if (cfg_get("GPU_PCT", v) == 0)
    cfg.gpu_pct = clampi(atoi(v), 1, 100, 45);
  /* GPU 下限锁定和 Boost 源中和已合并成一个开关。旧配置里可能只有 */
  int gbo_seen = 0, gml_seen = 0, gbo_val = 1, gml_val = 1;
  if (cfg_get("GPU_BOOST_OFF", v) == 0) {
    gbo_seen = 1;
    gbo_val = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  }
  if (cfg_get("GPU_MIN_LOCK", v) == 0) {
    gml_seen = 1;
    gml_val = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  }
  cfg.gpu_boost_off = gbo_seen ? gbo_val : (gml_seen ? gml_val : 1);
  cfg.gpu_min_lock = cfg.gpu_boost_off;
  if (cfg_get("BG_LIGHT", v) == 0)
    cfg.bg_light = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
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
  } else if (cfg_key_present("CPU_GOVERNOR")) {
    /* "不干预"写成空值：保留空，运行时/退出时按快照还原原调速器。 */
    cfg.governor[0] = 0;
  }
  if (cfg_get("BIG_MIN", v) == 0)
    cfg.big_min = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("BIG_OFFLINE", v) == 0)
    cfg.big_offline = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("CPU_MIN_LOCK", v) == 0)
    cfg.cpu_min_lock = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("CAP_BACKEND", v) == 0) {
    trim(v);
    if (!strcasecmp(v, "sysfs")) cfg.cap_backend = CAP_BE_SYSFS;
    else if (!strcasecmp(v, "ppm")) cfg.cap_backend = CAP_BE_PPM;
    else if (!strcasecmp(v, "msm")) cfg.cap_backend = CAP_BE_MSM;
    else cfg.cap_backend = CAP_BE_AUTO;
  }
  for (int i = 0; i < 4; i++)
    if (cfg_get(keys[i], v) == 0)
      cfg.pct[i] = clampi(atoi(v), 1, 100, dflt[i]);
  {
    static const char *const mkeys[4] = {
      "CPU_MIN_PCT_BIG", "CPU_MIN_PCT_MID2",
      "CPU_MIN_PCT_MID", "CPU_MIN_PCT_LITTLE",
    };
    for (int i = 0; i < 4; i++)
      if (cfg_get(mkeys[i], v) == 0)
        cfg.min_pct[i] = clampi(atoi(v), 0, 100, 0);
  }
  if (cfg_get("CPU_FLOOR_M", v) == 0)
    cfg.cap_floor_m = clampi(atoi(v), 0, 10000000, 0);

  /* 实时偏移。区间故意收窄：这些数直接决定"最坏能比设定值宽/严多少" */
  if (cfg_get("PSI_ADAPT", v) == 0)
    cfg.ad.psi = (!strcmp(v, "0") || !strcasecmp(v, "false")) ? 0 : 1;
  if (cfg_get("PSI_GAIN", v) == 0) cfg.ad.psi_gain = clampi(atoi(v), 0, 30, 5);
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
  /* 恢复线必须低于触发线，否则温度在阈值附近时会逐档来回抖。 */
  if (cfg.ad.th_low >= cfg.ad.th_high) cfg.ad.th_low = cfg.ad.th_high - 5;
  if (cfg.ad.th_low < 30) cfg.ad.th_low = 30;
  /* 0 = 整条环路关。GAIN/MAX 连一个 STEP 都凑不齐时按"关"处理 */
  if (cfg.ad.psi_gain > 0 && cfg.ad.psi_gain < cfg.ad.psi_step) {
    log("WARN PSI_GAIN=%d 小于 PSI_STEP=%d，PSI 护栏停用（填 0 或至少一档）",
        cfg.ad.psi_gain, cfg.ad.psi_step);
    cfg.ad.psi_gain = 0;
  }
  if (cfg.ad.th_max > 0 && cfg.ad.th_max < cfg.ad.th_step) {
    log("WARN TH_MAX=%d 小于 TH_STEP=%d，温控收紧停用（填 0 或至少一档）",
        cfg.ad.th_max, cfg.ad.th_step);
    cfg.ad.th_max = 0;
  }
}

/* 状态发布                                                           */

static char g_status_cache[8192];
static size_t g_status_len;

/* 往固定缓冲区追加；状态很短，不需要动态分配。 */
static size_t ps_append(char *b, size_t cap, size_t n, const char *fmt, ...) {
  if (n >= cap) return n;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(b + n, cap - n, fmt, ap);
  va_end(ap);
  if (w < 0) return n;
  size_t add = (size_t)w;
  if (add > cap - n - 1) add = cap - n - 1;
  return n + add;
}

static const char *policy_source(void) {
  if (!strcmp(g_state, "game")) return g_reason[0] ? g_reason : "game";
  if (g_app_pct == MODE_FULL) return "app_full";
  if (g_app_pct > 0 || g_app_big > 0 || g_app_little > 0 || g_app_gpu > 0)
    return "app";
  if (g_cat_idx >= 0) return "category";
  if (cfg.strategy == 1) return "fsm";
  if (suppress_on()) return "suppress";
  return "global";
}
static const char *gpu_policy_source(void) {
  if (g_app_gpu > 0) return "app";
  if (g_cat_gpu_cur > 0) return "category";
  if (cfg.strategy == 1) return "fsm";
  return "global";
}

static void publish_status(void) {
  char buf[8192];
  size_t n = 0;
  /* 生效中的应用档位。"" 表示没有专属档位，走全局百分比。 */
  char mode[16], gmode[8];
  if (g_app_pct == MODE_FULL) snprintf(mode, sizeof mode, "full");
  else if (g_app_big > 0 || g_app_little > 0)
    snprintf(mode, sizeof mode, "%d/%d", g_app_big, g_app_little);
  else if (g_app_pct > 0) snprintf(mode, sizeof mode, "%d", g_app_pct);
  else mode[0] = 0;
  /* 空 = GPU 跟随全局；和 app_mode 分开报，前端不用去拆 70:40 这种串 */
  if (g_app_gpu > 0) snprintf(gmode, sizeof gmode, "%d", g_app_gpu);
  else gmode[0] = 0;
  /* fg 是"此刻前台是谁"，game_pkg 是"命中名单的那个"。 */
  /* 这台机实际用掉几档（1..4）。api.sh 的按应用建议要拿它算"没设档位的包" */
  int ntier = 0;
  {
    int used[4] = {0, 0, 0, 0};
    for (int i = 0; i < g_npol; i++)
      if (g_pol[i].tier >= 0 && g_pol[i].tier < 4) used[g_pol[i].tier] = 1;
    for (int t = 0; t < 4; t++) ntier += used[t];
  }
  int cmin_paused = cfg.cpu_min_lock && g_min_fails >= 3 &&
                    mono_sec() - g_min_fail_at < 600;
  int cmin_give = 0;
  for (int i = 0; i < g_npol; i++) cmin_give += g_min_giveup[i];
  const char *cat_name = g_cat_idx >= 0 ? g_cat_names[g_cat_idx] : "-";
  int cat_cpu = g_cat_idx >= 0 ? g_cat_cpu[g_cat_idx][0] : 0;
  int cat_gpu = g_cat_idx >= 0 ? g_cat_gpu[g_cat_idx] : 0;
  n = ps_append(buf, sizeof buf, n,
             "state=%s\nscreen=%s\nsoff=%d\nsoff_keep=%d\ngame_pkg=%s\nreason=%s\n"
             "fg=%s\napp_mode=%s\napp_big=%d\napp_little=%d\napp_gpu=%s\n"
             "pid=%d\nver=%s\nntier=%d\ncapbe=%s\n"
             "strat=%d\nsmart=%d\nfsm=%d\nprime=%d\nsup=%d\ntick=%ld\nfgprobe=%ld\ngminw=%ld\n"
             "cminw=%ld\ncminf=%d\ncminpaused=%d\ncming=%d\ncat=%s\ncatcpu=%d\ncatgpu=%d\n",
          g_state, g_deep ? "deep" : (g_screen_off ? "off" : "on"),
          g_deep ? cfg.idle_interval : cfg.soff_interval, g_soff_keep,
          g_game[0] ? g_game : "-", g_reason,
          g_fg[0] ? g_fg : "-", mode, g_app_big, g_app_little, gmode,
          getpid(), SF_VERSION, ntier,
          g_npol > 0 ? cap_backend_name(g_pol[0].be) : "sysfs",
          cfg.strategy, g_sm.fsm, g_sm.fsm, g_prime_state,
          suppress_on() ? sup_pct() : 0,
          g_tick_n, g_fg_probe_n, g_gmin_writes,
          g_min_writes, g_min_fails, cmin_paused, cmin_give,
          cat_name, cat_cpu, cat_gpu);
  n = ps_append(buf, sizeof buf, n, "src=%s\ngsrc=%s\nfsmrs=%s\nfsmw=%d\n",
                policy_source(), gpu_policy_source(),
                g_sm.reason[0] ? g_sm.reason : "-", g_sm.wait);
  n = ps_append(buf, sizeof buf, n,
             "supup=%d\nsupfall=%d\nsupminn=%d\nsupbusy=%d\nsupfloor=%d\n"
             "supstuck0=%ld\nsupstuck1=%ld\nsupstuck2=%ld\nsupstuck3=%ld\n",
             g_sup_up_n, g_sup_fall_n, g_sup_min_n, g_sup_last_busy,
             g_sup_floor_busy_n,
             MAX_POLICY > 0 ? g_sup_stuck[0] : 0,
             MAX_POLICY > 1 ? g_sup_stuck[1] : 0,
             MAX_POLICY > 2 ? g_sup_stuck[2] : 0,
             MAX_POLICY > 3 ? g_sup_stuck[3] : 0);
  /* 实时偏移的三个来源一起发出去：页面上"实际比设定宽/严了多少"必须说得出是谁干的 */
  {
    struct adapt a;
    adapt_snapshot(&a);
    long cur_ma = batt_current_ma_now();
    long volt_uv = read_num("/sys/class/power_supply/battery/voltage_now");
    long pw_mw = (cur_ma > 0 && volt_uv > 0)
                   ? cur_ma * volt_uv / 1000000 : -1;
    int psiv = psi_cpu_pct();
    int hrv = 0, hrs = 0, hrh = 0;
    int hr = vendor_headroom(&hrv, &hrs, &hrh);
    n = ps_append(buf, sizeof buf, n,
               "dlt=%d\nbatt=%d\nbcut=%d\nstall=%d\npsib=%d\navg10=%d\npsiv=%d\ntemp=%d\n"
               "skin=%d\nadpf=%d\nhr=%d\nhrv=%d\nhrs=%d\nhrh=%d\n"
               "cur=%ld\npw=%ld\nchg=%d\npsi=%d\nth=%d\npsifrz=%d\n",
            effective_delta(&a), a.capacity, a.batt_cut,
            (a.have_psi && a.psi_valid) ? a.stall : -1, a.base, a.avg10x100, psiv,
            a.temp_m > 0 ? a.temp_m / 1000 : -1, skin_temp_c(), adpf_headroom(),
            hr, hrv, hrs, hrh, cur_ma, pw_mw,
            a.charging, a.psi_steps,
            cfg.strategy == 1 ? 0 : a.th_steps,
            a.base_frozen);
  }
  /* 熄屏轻巡检周期长、内容经常一模一样；一样就不落盘，省掉一次 */
  if (g_screen_off && access(STATUS_FILE, F_OK) == 0 && g_status_len &&
      n == g_status_len && !memcmp(buf, g_status_cache, n))
    return;
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s.tmp", STATUS_FILE);
  FILE *out = fopen(tmp, "w");
  if (!out) return;
  int ok = fwrite(buf, 1, n, out) == n;
  ok = (fclose(out) == 0) && ok;
  if (!ok || rename(tmp, STATUS_FILE) != 0) {
    unlink(tmp);
    return;
  }
  memcpy(g_status_cache, buf, n);
  g_status_len = n;
}

/* 清理                                                               */

static void cleanup(void) {
  if (!g_have_lock) return;
  log("停止，恢复启动前状态");
  restore_governors();
  /* 顺序：先还原数值，再收权限。反了会写不进需要放开才能写的节点 */
  if (g_gpu.mtk) mtk_restore_row();
  /* 第二道锁先放掉；否则 sysfs 写回原值会被厂商硬限压着。 */
  be_restore_cleanup();
  journal_restore(NODES_JOURNAL);
  cpuset_restore_all();             /* 字符串账本：cpuset 原掩码 */
  suppress_uclamp_max_restore();    /* 字符串账本：uclamp.max 原值 */
  bg_light_restore();               /* 字符串账本：后台分组原值 */
  big_offline_restore("退出");       /* 先让核回在线再收权限 */
  perms_restore();
  unlink(STATE_FILE);
  unlink(STATUS_FILE);
  unlink(HEARTBEAT_FILE);
  /* 锁文件只清空、不删除：删了会让下一个实例锁到新的 inode */
  if (g_lock_fd >= 0) {
    if (!ftruncate(g_lock_fd, 0)) lseek(g_lock_fd, 0, SEEK_SET);
  }
  g_have_lock = 0;
}

static void on_signal(int sig) {
  (void)sig;
  
  g_stop = 1;
}

/* main                                                               */

/* pid 是否确实是本守护。 */
static int pid_is_daemon(long pid) {
  if (pid <= 1) return 0;
  if (kill((pid_t)pid, 0)) return 0; /* 不存在 */
  char path[64], name[32];
  snprintf(path, sizeof path, "/proc/%ld/comm", pid);
  if (read_file(path, name, sizeof name)) return 0; /* 读不到就不信 */
  trim(name);
  return !strncmp(name, "socfreqd", 8);
}

/* 守护是否真的在跑。 */
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

/* 锁文件里有没有"上次的 pid"。干净退出会把它清空，不算残留。 */
static int lock_has_pid(void) {
  char b[32];
  if (read_file(LOCK_FILE, b, sizeof b)) return 0;
  return atol(b) > 1;
}

static int acquire_lock(void) {
  
  int stale = lock_has_pid() && !daemon_alive();
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
  g_lock_fd = fd;
  g_have_lock = 1;
  /* fd 故意不关：flock 随进程退出自动释放；cleanup 只清空内容，不删文件。 */
  return 0;
}

/* 定义 SF_TEST 时不编 main，直接包含测试文件跑内部函数。 */
#ifdef SF_TEST
#include "test_real.c"
#else
/* 逐核状态。update=1 时把这一拍的增量推进 5 秒环（WebUI 每秒调 cores） */
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
    core_path(b, sizeof b, c, "scaling_cur_freq");
    long cur = read_num(b);
    if (cur <= 0) {
      core_path(b, sizeof b, c, "cpuinfo_cur_freq");
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
    
    load_cfg();
    struct policy p[MAX_POLICY];
    int n = detect_policies(p);
    /* 智能模式的档位活在守护进程里，export 是独立进程：不读 status 的话 */
    {
      FILE *sf = fopen(STATUS_FILE, "r");
      if (sf) {
        char ln[128];
        while (fgets(ln, sizeof ln, sf)) {
          if (!strncmp(ln, "smart=", 6)) {
            g_sm.fsm = clampi(atoi(ln + 6), 0, SMART_STATE_COUNT - 1,
                              g_sm.fsm);
            g_sm.state = g_sm.fsm;
          } else if (!strncmp(ln, "fsm=", 4)) {
            g_sm.fsm = clampi(atoi(ln + 4), 0, SMART_STATE_COUNT - 1,
                              SMART_LIGHT_IDLE);
            g_sm.state = g_sm.fsm;
          } else if (!strncmp(ln, "prime=", 6))
            g_prime_state = clampi(atoi(ln + 6), -1, 3, -1);
        }
        fclose(sf);
      }
    }
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
    printf("gpu_boost_off=%d\n", cfg.gpu_boost_off);
    printf("big_min=%d\n", cfg.big_min);
    printf("bigoffline=%d\n", cfg.big_offline);
    printf("cpu_min_lock=%d\n", cfg.cpu_min_lock);
    printf("cpu_floor_m=%d\n", cfg.cap_floor_m);
    printf("screen_uncap=%d\n", cfg.screen_uncap);
    printf("screen_policy=%s\n", cfg.screen_policy == SCREEN_UNCAP ? "uncap"
                              : cfg.screen_policy == SCREEN_CAP ? "cap" : "keep");
    printf("soff_interval=%d\n", cfg.soff_interval);
    printf("idle_interval=%d\nidle_current_ma=%d\nidle_hold=%d\nwake_delta=%d\n",
           cfg.idle_interval, cfg.idle_current_ma, cfg.idle_hold_cycle, cfg.wake_delta_ma);
    printf("batt_unit=%s\n", cfg.batt_current_ma_unit ? "ma" : "ua");
    printf("boot_full=%d\n", cfg.boot_full_s);
    /* 实时偏移：页面配置页要能显示、能改，所以和上面这些一样原样发出去 */
    printf("psi_adapt=%d\npsi_gain=%d\npsi_step=%d\n", cfg.ad.psi, cfg.ad.psi_gain, cfg.ad.psi_step);
    printf("th_cap=%d\nth_high=%d\nth_low=%d\nth_step=%d\nth_max=%d\n",
           cfg.ad.th, cfg.ad.th_high, cfg.ad.th_low, cfg.ad.th_step, cfg.ad.th_max);
    printf("chg_uncap=%d\nchg_pct=%d\n", cfg.ad.chg, cfg.ad.chg_pct);
    printf("governor=%s\n", cfg.governor[0] ? cfg.governor : "");
    printf("gov_locked=%d\n", governor_locked());
    printf("npkg=%d\n", g_npkg);
    char soc[128];
    read_soc_name(soc, sizeof soc);
    printf("soc=%s\n", soc);
    cap_backend_report(p, n);

    dump_cores(0);

    for (int i = 0; i < n; i++) {
      char path[256];
      long cur;
      snprintf(path, sizeof path, "%s/scaling_max_freq", p[i].dir);
      cur = read_num(path); /* 失败返回 -1，下面单独判 */
      char vbuf[64] = "0";
      if (cur > 0) snprintf(vbuf, sizeof vbuf, "%ld", cur);
      /* cpu_N=硬件最高频，cpucur_N=当前实际封顶。 */
      printf("cpu_%s=%ld\n", p[i].name + 6, p[i].hw_max);
      printf("cpucur_%s=%s\n", p[i].name + 6, vbuf);
      printf("cpucap_%s=%ld\n", p[i].name + 6, p[i].cap);
      printf("cpupct_%s=%ld\n", p[i].name + 6, p[i].pct);
      /* 档位 → 配置键名。WebUI 拖滑杆时要知道自己改的是 BIG 还是 MID2 */
      static const char *tier_names[4] = {"CPU_PCT_BIG", "CPU_PCT_MID2",
                                          "CPU_PCT_MID", "CPU_PCT_LITTLE"};
      printf("cpukey_%s=%s\n", p[i].name + 6,
             tier_names[p[i].tier & 3]);
      /* 频点表原样导出去，让前端做和守护 snap_freq 一样的吸附 */
      {
        char fb[4096], fpath[256];
        fb[0] = 0;
        snprintf(fpath, sizeof fpath, "%s/scaling_available_frequencies", p[i].dir);
        /* read_file 返回 0 才是成功；写反了就会把栈残值当频点表吐给前端。 */
        if (!read_file(fpath, fb, sizeof fb))
          printf("policyfreq_%s=%s\n", p[i].name + 6, fb);
      }
    }

    /* 每个档位具体管着哪几颗核 */
    {
      const char *keys[4] = {"CPU_PCT_BIG", "CPU_PCT_MID2",
                              "CPU_PCT_MID", "CPU_PCT_LITTLE"};
      /* 阶梯要用**未去重**的 policy 列表算，否则同一硬件上限的多个 */
      struct policy all[MAX_POLICY];
      int na = detect_policies_raw(all);
      long ladder[MAX_POLICY];
      int m = build_ladder(all, na, ladder);
      for (int t = 0; t < 4; t++) {
        int ncpu = 0;
        /* 缺档也要报一行、值留空：前端靠"这个键在不在且非空"判断 */
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
      /* 这台机实际有几档。前端靠它区分 */
      int used[4] = {0, 0, 0, 0};
      for (int i = 0; i < na; i++) used[tier_key_of(ladder, m, all[i].hw_max)] = 1;
      int nt = 0;
      for (int t = 0; t < 4; t++) nt += used[t];
      printf("ntier=%d\n", nt);
    }
    /* GPU 同样要硬件最高与当前封顶，否则 45% 对用户没意义 */
    detect_gpu(1);
    
    long gcap = g_gpu.normal;
    int gpu_dlt = 0;
    {
      FILE *sf = fopen(STATUS_FILE, "r");
      if (sf) {
        char ln[128];
        int state_seen = 0, normal = 0, agpu = 0, bcut = 0, chg = 0, th = 0;
        while (fgets(ln, sizeof ln, sf)) {
          if (!strncmp(ln, "state=", 6)) {
            state_seen = 1;
            normal = !strncmp(ln + 6, "normal", 6);
          } else if (!strncmp(ln, "app_gpu=", 8)) {
            agpu = atoi(ln + 8);
          } else if (!strncmp(ln, "bcut=", 5)) {
            bcut = atoi(ln + 5);
          } else if (!strncmp(ln, "chg=", 4)) {
            chg = atoi(ln + 4);
          } else if (!strncmp(ln, "th=", 3)) {
            th = atoi(ln + 3);
          }
        }
        fclose(sf);
        /* 草稿预览要按同一个偏移算，否则滑杆显示 50%→600、保存后却变成 */
        int gdlt = -(th * cfg.ad.th_step) - bcut;
        if (agpu > 0) gdlt += bcut;
        gpu_dlt = gdlt;
        if (state_seen && !normal) gcap = g_gpu.max_hz;
        else if (state_seen) {
          /* 应用 GPU 档不吃电量阶梯/充电地板，和 CPU 专属档同一个口径。 */
          int baseg = agpu > 0 ? agpu : cfg.gpu_pct;
          if (agpu <= 0 && chg == 1 && cfg.ad.chg && cfg.ad.chg_pct > baseg &&
              gdlt >= 0)
            baseg = cfg.ad.chg_pct;
          gcap = gpu_target_for(baseg, gdlt);
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
    /* max_hz / normal 恒为 Hz：detect_gpu 里 MHz 频点表已经 ×1e6 归一。 */
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
    /* 从 APK 取图标，输出 data URI。取不到就什么都不输出—— */
    if (argc > 2) { icon_from_apk(argv[2], argc > 3 ? argv[3] : NULL); return 0; }
    return 0;
  }
  if (!strcmp(cmd, "iconpath")) {
    /* manifest 的 android:icon 声明的那张**位图**在 zip 里的路径。 */
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
    /* 批量：stdin 每行 "包名<TAB>APK 路径"，stdout 每行 "包名<TAB>应用名"。 */
    return appinfo_labels_stdin();
  }
  if (!strcmp(cmd, "score")) {
    /* 纯函数式子命令：读 stdin、写 stdout、不碰任何节点、不需要 root。 */
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
      printf("  %-8s hw=%-9ld vendor=%-9ld cur=%-9ld\n", p[i].name, p[i].hw_max,
             p[i].vendor_max, read_num(path));
    }
    assign_pct(p, n);
    printf("=== 计算封顶 ===\n");
    for (int i = 0; i < n; i++)
      printf("  %-8s cap=%-9ld (%ld%%)\n", p[i].name, p[i].cap, p[i].pct);
    printf("cpu_floor_m=%d\n", cfg.cap_floor_m);
    printf("=== cap backend ===\n");
    cap_backend_report(p, n);
    printf("=== GPU ===\n");
    detect_gpu(1);
    printf("  node=%s 最高=%ldHz 封顶=%ldHz\n", g_gpu.max_node[0] ? g_gpu.max_node : "无",
           g_gpu.max_hz, g_gpu.normal);
    return 0;
  }
  if (!strcmp(cmd, "probe_min")) {
    /* 下限探测：写一次 cpuinfo_min、立刻读回、再写回原值。 */
    load_cfg();
    struct policy p[MAX_POLICY];
    int n = detect_policies(p);
    printf("=== CPU 下限（写一次立刻还原）===\n");
    for (int i = 0; i < n; i++) {
      char path[256];
      snprintf(path, sizeof path, "%s/scaling_min_freq", p[i].dir);
      long cur = read_num(path);
      long want = p[i].hw_min;
      if (want <= 0 || cur <= 0) {
        printf("  %-8s 读不到当前值/硬件最低，跳过\n", p[i].name);
        continue;
      }
      if (cur <= want) {
        printf("  %-8s 已在硬件最低 %ld\n", p[i].name, cur);
        continue;
      }
      if (write_num(path, want)) {
        printf("  %-8s %ld -> %ld 写入失败（内核拒收）\n", p[i].name, cur, want);
        continue;
      }
      long got = read_num(path);
      write_num(path, cur);
      long back = read_num(path);
      printf("  %-8s %ld -> %ld %s（已还原 %ld）\n", p[i].name, cur, got,
             got == want ? "可写" : "被夹到别的值", back);
    }
    return 0;
  }
  if (!access(PAUSE_FILE, F_OK)) {
    printf("paused\n");
    return 0;
  }

  /* 数据目录不在解包范围内（模块升级会重建模块目录） */
  mkdir_p(SF_DATADIR);
  mkdir_p(RUN_DIR);
  tmp_runtime_init();
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
  g_nraw = detect_policies_raw(g_polraw);
  if (!g_nraw) {
    memcpy(g_polraw, g_pol, sizeof g_pol);
    g_nraw = g_npol;
  }
  for (int i = 0; i < g_npol; i++) { g_pol[i].last_cap = 0; g_pol[i].last_got = 0; }
  if (!g_npol) {
    log("WARN 未探测到 cpufreq policy，本模块不工作");
    return 1;
  }
  cap_backend_init();
  assign_pct(g_pol, g_npol);
  detect_gpu(0);
  suppress_gov_load();   /* adapt/<soc>.conf 的 gov_ 行：有数据才调节工作点 */

  
  journal_restore(NODES_JOURNAL);
  cpuset_restore_all();             /* 上次被 kill -9 时留下的 cpuset 原掩码 */
  suppress_uclamp_max_restore();    /* 上次被 kill -9 时留下的 uclamp.max 原值 */
  bg_light_restore();               /* 上次被 kill -9 时留下的后台分组原值 */
  big_offline_restore("上次退出");   /* 上次异常退出时留下的离线核 */
  perms_restore();
  restore_governors();
  if (access(GOV_JOURNAL, F_OK))
    log("WARN 调速器快照未还原，保留到下次启动重试");
  else
    snapshot_governors();

  load_game_pkgs();
  load_app_modes();
  /* 传感器路径的钩子在这里读一次环境变量，之后每轮直接按路径取 */
  adapt_init();
  screen_state_init();
  log("调度策略=%s", cfg.strategy ? "智能（状态机）" : "限频（固定百分比）");
  int active_iv = cfg.strategy == 1 ? kSmartTickSec : cfg.interval;
  log("巡检 日常%d s%s / 游戏%d s / 熄屏%d s，退出宽限 %d s，熄屏策略=%s",
      active_iv, cfg.strategy == 1 ? "（智能）" : "", cfg.interval * 2,
      cfg.soff_interval, cfg.grace,
      cfg.screen_policy == SCREEN_UNCAP ? "交还系统"
      : cfg.screen_policy == SCREEN_CAP ? "保持封顶（轻巡检）" : "保持封顶");
  apply_normal(1);
  heartbeat();

  long missing_since = 0; /* 前台丢失的起始秒，0 = 未丢失 */
  long ad_last = 0;       /* 上一次算实时偏移的时刻，用来量真实的巡检间隔 */
  uint64_t seen = cfg_stamp();
  for (; !g_stop;) {
    /* 每轮开头复位：这一轮只要偏移动过，后面几处 apply_normal 就得 force 写 */
    int adapt_changed = 0;
    /* pause 自查：手工 touch pause 与 WebUI 的"暂停"都在一个巡检周期内生效。 */
    if (!access(PAUSE_FILE, F_OK)) {
      log("检测到 pause，恢复并退出");
      break;
    }
    /* 管理器里停用/卸载只会写标记文件，不会通知已经在跑的守护。 */
    if (!access(SF_MODDIR "/disable", F_OK) ||
        !access(SF_MODDIR "/remove", F_OK)) {
      log("检测到模块停用/卸载标记，恢复并退出");
      break;
    }
    heartbeat();
    int cfg_changed = reload_if_changed(seen);
    seen = cfg_stamp();
    /* 熄屏轻巡检：SCREEN_OFF 下只读一个亮屏信号，不 popen dumpsys */
    if (cfg.screen_policy != SCREEN_KEEP) {
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
            /* 电流突增只当"可能亮屏"的提示，必须再用 dumpsys 确认 */
            if (screen_awake()) woke = 1;
            else g_deep = 0;
          } else if (r == 1 && !g_deep) {
            g_deep = 1;
            set_timer_slack(5000000000UL);
            log("进入深度空闲（%d s 轻巡检，电流 %ldmA）", cfg.idle_interval, ma);
          } else if (g_deep && (!valid || ma >= cfg.idle_current_ma)) {
            g_deep = 0;
            set_timer_slack(3000000000UL);
            log("电流回升，回到熄屏巡检");
          }
        }
        if (!woke) {
          if (g_soff_keep) {
            /* keep 应用：不交还，只把被厂商顶高的上限压回去。 */
            caps_patrol(0);
            gpu_patrol();
            /* cap 策略：温控每 4 拍（≈60s）才采一次，只在温度越过阈值时重算封顶 */
            if (cfg.strategy != 1 && cfg.screen_policy == SCREEN_CAP && ++g_soff_th >= 4) {
              g_soff_th = 0;
              if (adapt_screen_off_tick(&cfg.ad, cfg.soff_interval * 4)) {
                /* 温控只改了 adapt 快照；cap 还是进熄屏时算的旧值。 */
                assign_pct(g_pol, g_npol);
                apply_normal(1);
              }
            }
          }
          publish_status();
          sleep(g_deep ? cfg.idle_interval : cfg.soff_interval);
          continue;
        }
        leave_screen_off();
        missing_since = 0;
      }
      /* ACTIVE：先看 sysfs 信号，熄屏后就不必再跑一次 dumpsys window。 */
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
      set_timer_slack(50000UL);
      g_deep = 0;
      g_soff_keep = 0;
      g_soff_pkg[0] = 0;
      g_idle_ema = 0;
      g_idle_hold = 0;
      if (!strcmp(g_state, "full")) apply_normal(1);
    }
    /* 灯亮才算一拍：熄屏轻巡检直接 continue，tick 不涨 */
    g_tick_n++;
    /* 实时偏移：每轮读一次 PSI / 最热温度传感器 / 充电态。 */
    {
      long anow = mono_sec();
      int adt = ad_last ? (int)(anow - ad_last) : cfg.interval;
      if (adt < 1) adt = 1;
      if (adt > 120) adt = 120;          /* 被暂停/长眠隔久了：按上限算，别把整段当成一轮 */
      ad_last = anow;
      /* PSI 基线只在常态更新。以前还要求 delta==0：温度只要停在迟滞带 */
      int base_ok = !strcmp(g_state, "normal");
      int adv = base_ok && !g_screen_off;
      /* 智能模式整条冻结 PSI：它已经按逐簇负载给了目标，PSI 再借额度 */
      struct adapt_cfg ac = cfg.ad;
      if (cfg.strategy == 1) { ac.psi = 0; ac.th = 0; }
      if (adapt_tick_ex(&ac, adt, base_ok, adv)) {
        struct adapt a;
        adapt_snapshot(&a);
        adapt_changed = 1;
        log("  实时偏移 %+d 个百分点（PSI %d 档 / 温控 %d 档 / %s）",
            effective_delta(&a), a.psi_steps,
            cfg.strategy == 1 ? 0 : a.th_steps,
            a.chg_on ? "插电放宽" : "未插电");
      }
    }
    /* 先探前台：智能模式要用"这一拍是否换人"做首拍预热 */
    char fg[PKG_LEN] = {0};
    load_game_pkgs();
    load_app_modes();
    int fg_rc = foreground_pkg_throttled(fg);
    /* 游戏态下 top-app PID 集合可能没变（小窗和游戏同进程组） */
    if (fg_rc == 0 && !strcmp(g_state, "game") && !is_game(fg))
      fg_rc = foreground_pkg_throttled_ex(fg, 1);
    int game = fg_rc == 0 && g_npkg && is_game(fg);
    uint64_t cat_stamp0 = g_cat_stamp;
    load_app_categories();
    int cat_prev = g_cat_idx;
    category_apply(fg);
    int cat_changed = (g_cat_idx != cat_prev) || (g_cat_stamp != cat_stamp0);
    /* 前台包名无条件记下来（命中名单与否都要）。 */
    int fg_changed = fg[0] && strcmp(fg, g_fg) != 0;
    if (fg[0]) snprintf(g_fg, sizeof g_fg, "%s", fg);
    else g_fg[0] = 0;

    /* 配置和偏移都看过之后，统一在这里重算生效封顶。 */
    /* 智能模式：每拍推进一次状态机。档位变了就走和"配置变了"同一条 */
    int smart_changed = 0;
    if (cfg.strategy == 1) smart_changed = smart_tick(fg_changed && !game);
    if (smart_changed) cpu_min_refresh();
    if (cfg_changed || adapt_changed || smart_changed) {
      assign_pct(g_pol, g_npol);
      /* 配置变更时压制收口一次：天花板/下限回到新封顶，压制随后 */
      if (cfg_changed) {
        sup_exit();
        /* 进阶最低频改了：先按账本还原旧下限，再让巡逻按新目标写一次 */
        cpu_min_restore_user();
        cpu_min_refresh();
      }
      /* 全局分档刚被重算，生效中的应用专属档位要盖回去 */
      if (g_app_pct > 0 || g_app_big > 0 || g_app_little > 0)
        set_caps_tiers(g_app_pct, g_app_big, g_app_little,
                       g_app[0] ? g_app : "应用专属");
      if (reclaim_caps_allowed()) apply_normal(1);
    }

    /* 切应用/冷启动时系统会重新写 input_boost_ms 和抬高 GPU 底频 */
    if (fg_changed && suppress_on() && !g_sup_chg_pause && !game) {
      suppress_boost_apply();
      suppress_uclamp_apply();
      suppress_extra_apply();
      boost_off_apply();
    }
    /* 切应用时厂商可能顺手重写 cgroup 参数；后台收口随前台变化补一次 */
    if (fg_changed && cfg.bg_light && !game) bg_light_apply();

    /* 满血窗口：开机后前 N 秒、或（可选）熄屏期间。 */
    int boost = 0;
    const char *bwhy = "-";
    if (cfg.boot_full_s > 0 && boot_boost_active()) { boost = 1; bwhy = "boot"; }
    else if (cfg.screen_policy == SCREEN_KEEP && cfg.screen_uncap &&
             !screen_awake())                        { boost = 1; bwhy = "screen"; }

    if (game) {
      missing_since = 0;
      adapt_cancel();   /* 交权态不攒 PSI/温控档位 */
      category_clear();
      if (strcmp(g_game, fg)) {
        snprintf(g_game, sizeof g_game, "%s", fg);
        unlock_game();
      }
      publish_status();
      sleep(cfg.interval * 2);
      continue;
    }

    if (boost) {
      missing_since = 0;
      adapt_cancel();
      category_clear();
      if (strcmp(g_state, "game") || strcmp(g_reason, bwhy)) {
        snprintf(g_reason, sizeof g_reason, "%s", bwhy);
        if (!strcmp(bwhy, "boot")) big_offline_restore("开机满血");
        hand_back_full(strcmp(bwhy, "boot") ? "熄屏解除限频，交还系统"
                                            : "开机满血窗口，暂不封顶");
        snprintf(g_state, sizeof g_state, "game");
      }
      publish_status();
      sleep(cfg.interval);
      continue;
    }

    /* 分应用档位。优先级 游戏名单 > 满血窗口 > 应用专属 > 全局百分比 */
    int mpct = 0, mbig = 0, mlit = 0, mgpu = 0;
    int has_mode = (!game && !boost && fg[0])
                     ? mode_lookup(fg, &mpct, &mbig, &mlit, &mgpu) : 0;
    int is_full = has_mode && mpct == MODE_FULL;
    /* keep 只影响熄屏：亮屏时按没有专属档位处理，继续走全局分档/学习。 */
    if (has_mode && (mpct == MODE_KEEP || mpct == MODE_SOFF)) has_mode = 0;
    int want_pct = (has_mode && !is_full && mpct > 0) ? mpct : 0;
    int want_big = (has_mode && !is_full) ? mbig : 0;
    int want_little = (has_mode && !is_full) ? mlit : 0;
    int want_gpu = (has_mode && !is_full) ? mgpu : 0;

    if (is_full) {
      missing_since = 0;
      adapt_cancel();
      category_clear();
      snprintf(g_fg, sizeof g_fg, "%s", fg);
      /* full 两者都交还；把生效值标成 full/0，离开时才会按全局重新算。 */
      g_app_pct = MODE_FULL;
      g_app_big = 0;
      g_app_little = 0;
      g_app_gpu = 0;
      if (strcmp(g_app, fg) || strcmp(g_state, "game") || strcmp(g_reason, "app")) {
        snprintf(g_app, sizeof g_app, "%s", fg);
        snprintf(g_reason, sizeof g_reason, "app");
        big_offline_restore("满血应用");
        hand_back_full("应用设为满血，交还系统");
        snprintf(g_state, sizeof g_state, "game");
      }
      publish_status();
      sleep(cfg.interval);
      continue;
    }

    /* 封顶档位要不要换：只在数值变了时重算。 */
    int caps_changed = (want_pct != g_app_pct) ||
                       (want_big != g_app_big) ||
                       (want_little != g_app_little);
    int gpu_changed = (want_gpu != g_app_gpu);
    if (caps_changed) {
      set_caps_tiers(want_pct, want_big, want_little,
                     (want_pct || want_big || want_little) ? fg : "全局分档");
      g_app_pct = want_pct;
      g_app_big = want_big;
      g_app_little = want_little;
    }
    g_app_gpu = want_gpu;
    snprintf(g_app, sizeof g_app, "%s", has_mode ? fg : "");
    /* GPU 只换了应用档位、CPU 没换时也要立刻写，巡检只往回压、不会主动升 */
    if (gpu_changed) {
      if (want_gpu > 0) log("  封顶重算（%s 专属 GPU %d%%）", fg, want_gpu);
      else log("  GPU 回到全局 %d%%", cfg.gpu_pct);
      apply_gpu_cap(1);
    }
    if (has_mode) {
      missing_since = 0;
      int had_cat = g_cat_idx >= 0;
      category_clear();
      if (had_cat) cat_changed = 1;
      apply_normal(caps_changed || adapt_changed || cat_changed || smart_changed);
      gpu_patrol();     /* 这一支提前 continue，不巡检的话 Boost 会把 40% 顶回去 */
      publish_status();
      sleep(cfg.strategy == 1 ? kSmartTickSec : cfg.interval);
      continue;
    }

    /* 熄屏 / 开机窗口结束：直接压回，不吃"退出宽限"。 */
    if (!strcmp(g_state, "game") && strcmp(g_reason, "game")) {
      /* 这一支同时兜"满血窗口到期"和"离开满血应用"两种 */
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
      
      /* force 带上 caps_changed：封顶档位这一轮刚换过（离开专属应用回到全局） */
      apply_normal(caps_changed || adapt_changed || cat_changed);
      if (cat_changed) apply_gpu_cap(1);
      gpu_patrol();
      /* 工作点参数周期性补应用：系统会动态切 walt↔scx，切走的窗口里 */
      suppress_gov_apply();
    }
    publish_status();
    /* 动态封顶：亮屏常态把长睡切成 1s 快拍（快拍里只做压制与压回） */
    {
      struct adapt a;
      adapt_snapshot(&a);
      if (suppress_on() && !a.chg_on) {
        if (g_sup_chg_pause) {
          g_sup_chg_pause = 0;
          log("动态封顶：拔电恢复");
          /* 充电期间交还过 boost/uclamp；拔电立刻补回，不能等下一次状态变化 */
          suppress_boost_apply();
          suppress_smooth_apply();
          suppress_uclamp_apply();
          suppress_extra_apply();
          g_sup_reapply_at = mono_sec();
        }
        for (int k = 0; k < cfg.interval && !g_stop; k++) {
          suppress_tick();
          sleep(1);
        }
      } else {
        if (suppress_on() && !g_sup_chg_pause) {
          sup_exit();
          g_sup_chg_pause = 1;
          log("动态封顶：充电中暂停（插电不封顶）");
        } else if (cfg.strategy == 1) {
          g_sup_chg_pause = 0;
          sleep(kSmartTickSec);
        } else {
          sleep(cfg.interval);
        }
      }
    }
  }

  cleanup();
  return 0;
}
#endif /* !SF_TEST */