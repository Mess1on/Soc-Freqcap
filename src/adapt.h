/*
 * adapt.h — 实时偏移（PSI 借额度 / 温控降档 / 充电态放宽）
 *
 * 偏移只作用在"这一轮生效的封顶百分比"上，从不写配置文件：
 * 滑杆上显示的就是用户设的值，背后没有第二个东西在改它。
 */
#ifndef SOC_ADAPT_H
#define SOC_ADAPT_H

/* PSI 基线的时间常数（秒）：拿这台机自己最近 10 分钟的 stall 率当参照，
 * 而不是拿一个写死的绝对阈值 —— 各家的 PSI 读数差着量级。 */
#define PSI_TAU 600

struct adapt_cfg {
  int psi;        /* 1 = 开 PSI 护栏 */
  int psi_gain;   /* 最多借出多少个百分点 */
  int psi_step;   /* 每档多少个百分点 */
  int psi_hold;   /* 判到卡之后，额度至少借住多少秒 */
  int psi_back;   /* 不卡了，每隔多少秒收回一档 */
  int th;         /* 1 = 开温控闭环 */
  int th_high;    /* 超过这个温度（℃）开始逐档往下压 */
  int th_low;     /* 回到这个温度以下逐档还回去 */
  int th_step;    /* 每档多少个百分点 */
  int th_max;     /* 最多压多少个百分点（相对用户设的帽） */
  int th_back;    /* 每多少秒还一档 */
  int chg;        /* 1 = 插电时放宽 */
  int chg_pct;    /* 插电时常态封顶至少到多少（1~100） */
  int up_max;     /* 合计最多放宽多少个百分点 */
  int down_max;   /* 合计最多收紧多少个百分点 */
  /* 电量阶梯：低电量时再收紧，和 PSI/温控共用总闸。 */
  int batt;       /* BATT_STEP：1 = 开 */
  int batt_low;   /* 低于这个容量（%）加第一刀 */
  int batt_low_cut;
  int batt_crit;  /* 低于这个容量（%）再叠第二刀 */
  int batt_crit_cut;
};

struct adapt {
  int now;          /* 本轮的单调秒 */
  int stall;        /* 本轮 stall 率，ms/s */
  int base;         /* 自身基线，ms/s */
  int avg10x100;    /* PSI some avg10 ×100 */
  int have_psi;     /* 这台机读得到 /proc/pressure/cpu */
  int temp_m;       /* 最热 cpu/gpu 传感器，毫摄氏度（内核原样）；0 = 没有 */
  int charging;     /* 电池在充/已充满 */
  int psi_steps;
  int th_steps;
  int chg_on;       /* 本轮因插电抬到了 CHG_PCT */
  int chg_pct;      /* 抬到多少（0 = 没抬） */
  int delta;        /* 合计偏移（百分点，可为负；含电量阶梯） */
  int capacity;     /* 电池容量百分比；-1 = 读不到 */
  int batt_cut;     /* 电量阶梯贡献的收紧量（正数，已含在 delta 里） */
  int lend_until;   /* PSI 额度借到这一秒 */
  int back_until;   /* 下一次允许收回 PSI 档位的时刻 */
  int cool_until;   /* 温控下一档还回来的时间点（秒） */
};

void adapt_init(void);
/* 采一轮传感器并推进步进机。返回 1 = 生效封顶和上一轮不一样，调用处要重算。
   allow_base=0 时 PSI 基线只读不写（游戏/满血/熄屏这类没有帽的时段）。 */
int  adapt_tick(const struct adapt_cfg *c, int dt, int allow_base);
void adapt_reset(void);
/* 纯步进：喂数就能验，不碰文件。自检版直接调它。
   temp_m 是毫摄氏度（内核 thermal_zone 的原样读数）。 */
void adapt_apply(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                 int charging, int now, int dt);
/* 带基线闸的版本：allow_base=0 只读基线，用来验交权态不污染基线。 */
void adapt_apply_gated(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                       int charging, int now, int dt, int allow_base);
/* 完整版本：额外喂容量百分比（-1 = 读不到），电量阶梯只在这里生效。 */
void adapt_apply_cap(struct adapt *a, const struct adapt_cfg *c, int stall, int temp_m,
                     int charging, int capacity, int now, int dt, int allow_base);
/* 纯函数：容量 + 充电态 → 电量阶梯收紧量（0 表示不收紧）。 */
int  batt_cut_pct(int capacity, int charging, const struct adapt_cfg *c);
int  adapt_batt_cut(void);
int  adapt_delta(void);
int  adapt_active(void);
void adapt_snapshot(struct adapt *out);
/* 纯函数：温度 zone 的 type 是否属于 cpu/gpu/soc（单测直接喂字符串）。 */
int  thermal_type_ok(const char *type);

#endif
