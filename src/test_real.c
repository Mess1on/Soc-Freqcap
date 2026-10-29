/*
 * test_real.c — 直接跑守护真实代码的单元自检。
 *
 * 为什么不再自带一份实现：
 * 之前 test_logic.c 把 grab_pkg 抄了一遍，于是 4 条"解析 focus"全绿，
 * 而线上那版有个反向判断，把刚解析出来的包名立刻擦掉 —— 游戏态根本不触发。
 * 测试里抄一份就永远抓不到这类错。本文件被 socfreqd.c 以 SF_TEST 包含，
 * 调的就是守护自己的函数，一个字符都没改。
 *
 * 构建（需要 NDK 与设备）：
 *   aarch64-linux-android21-clang -D SF_TEST -static -O1 -o test_real src/socfreqd.c
 *   adb push test_real /data/local/tmp && adb shell su -c /data/local/tmp/test_real
 *
 * 注意：sf_test_main 由 socfreqd.c 在 main 位置调用，所以本文件不写 main。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int sf_fails, sf_passes;

/* 只关心 CPU 段的旧用例走这个窄口，双值用例直接调 mode_lookup。 */
static int mode_lookup_cpu(const char *pkg, int *out) {
  return mode_lookup(pkg, out, NULL);
}

/* detail 传 NULL 表示没有额外上下文，失败时只报断言名 */
static void sf_check(int cond, const char *name, const char *detail) {
  if (cond) {
    printf("  ok   %s\n", name);
    sf_passes++;
  } else {
    printf("  FAIL %s%s%s\n", name, detail ? " -> " : "",
           detail ? detail : "");
    sf_fails++;
  }
}

/* 把一段 dumpsys 文本变成可读流，喂给 fg_from_stream */
static FILE *sf_stream(const char *text) {
  FILE *f = tmpfile();
  if (!f) return NULL;
  fputs(text, f);
  rewind(f);
  return f;
}

/* 断言 fg_from_stream 从 dumpsys 文本里取出 want 这个包名 */
static void sf_expect_pkg(const char *name, const char *dumpsys, const char *want) {
  char out[256] = {0}, buf[600];
  FILE *f = sf_stream(dumpsys);
  int rc = f ? fg_from_stream(f, out) : -999;
  if (f) fclose(f);
  snprintf(buf, sizeof buf, "got=[%s] rc=%d want=[%s]", out, rc, want);
  sf_check(rc == 0 && !strcmp(out, want), name, buf);
}

static void sf_expect_fail(const char *name, const char *dumpsys) {
  char out[256] = {0}, buf[600];
  FILE *f = sf_stream(dumpsys);
  int rc = f ? fg_from_stream(f, out) : -999;
  if (f) fclose(f);
  snprintf(buf, sizeof buf, "rc=%d out=[%s]", rc, out);
  sf_check(rc != 0, name, buf);
}

static void sf_w16(FILE *f, unsigned v) {
  fputc(v & 255, f); fputc((v >> 8) & 255, f);
}
static void sf_w32(FILE *f, unsigned long v) {
  for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 255), f);
}

/* 造一个中央目录 >4MB 的 zip。大 APK（抖音这类）会走到"尾部 4MB 窗口放不下
   中央目录，按目录大小重读"那条路；旧写法在 realloc 成功后又 free 旧指针，
   是双重释放，读名字时直接把守护打成段错误。这里不需要合法 APK 内容，
   只要目录结构能把那条路走一遍。 */
static int sf_make_bigcd_zip(const char *path) {
  enum { N = 50000 };
  char nm[96];
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  long off = 0;
  for (int i = 0; i < N; i++) {
    snprintf(nm, sizeof nm, "res/%055d.png", i);
    int nl = (int)strlen(nm);
    sf_w32(f, 0x04034b50UL); sf_w16(f, 20); sf_w16(f, 0); sf_w16(f, 0);
    sf_w16(f, 0); sf_w16(f, 0); sf_w32(f, 0); sf_w32(f, 0); sf_w32(f, 0);
    sf_w16(f, (unsigned)nl); sf_w16(f, 0);
    fwrite(nm, 1, (size_t)nl, f);
    off += 30 + nl;
  }
  long cd0 = off, cdsz = 0;
  for (int i = 0; i < N; i++) {
    snprintf(nm, sizeof nm, "res/%055d.png", i);
    int nl = (int)strlen(nm);
    long lo = (long)i * (30 + nl);
    sf_w32(f, 0x02014b50UL); sf_w16(f, 20); sf_w16(f, 20);
    sf_w16(f, 0); sf_w16(f, 0); sf_w16(f, 0); sf_w16(f, 0);
    sf_w32(f, 0); sf_w32(f, 0); sf_w32(f, 0);
    sf_w16(f, (unsigned)nl); sf_w16(f, 0); sf_w16(f, 0);
    sf_w16(f, 0); sf_w16(f, 0); sf_w32(f, 0); sf_w32(f, (unsigned long)lo);
    fwrite(nm, 1, (size_t)nl, f);
    cdsz += 46 + nl;
  }
  sf_w32(f, 0x06054b50UL); sf_w16(f, 0); sf_w16(f, 0);
  sf_w16(f, N); sf_w16(f, N); sf_w32(f, (unsigned long)cdsz);
  sf_w32(f, (unsigned long)cd0); sf_w16(f, 0);
  int ok = !ferror(f);
  fclose(f);
  return ok;
}

/* 样本文件是"一行一组空格分隔的数"，这里按列取数，不看它从哪来 */
static long sf_col_sum(const char *path, int col) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[1024];
  long sum = 0;
  while (fgets(line, sizeof line, f)) {
    char *save = NULL;
    int k = 0;
    for (char *t = strtok_r(line, " \t\n", &save); t;
         t = strtok_r(NULL, " \t\n", &save)) {
      if (++k == col) { sum += strtol(t, NULL, 10); break; }
    }
  }
  fclose(f);
  return sum;
}

static int sf_line_cols(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[2048];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;          /* v2 文件头 */
    /* 先把行尾空白去掉，再数空格。之前拿"字段数"去索引字符数组，
       某个位置恰好是空格就会多数/少数一列，误报 30 列。 */
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                   line[len - 1] == ' ' || line[len - 1] == '\t'))
      line[--len] = 0;
    if (!len) continue;
    n = 1;
    for (char *p = line; *p; p++) if (*p == ' ' || *p == '\t') n++;
    break;
  }
  fclose(f);
  return n;
}

/* 断言失败时把首行一起打出来：只报"30 列"看不出是少了哪个字段。 */
static void sf_first_data_line(const char *path, char *out, int cap) {
  out[0] = 0;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    snprintf(out, cap, "%s", line);
    break;
  }
  fclose(f);
}

static int sf_first_col_is(const char *path, const char *key) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[1024];
  int hit = 0;
  while (fgets(line, sizeof line, f)) {
    char *save = NULL;
    char *t = strtok_r(line, " \t\n", &save);
    if (t && !strcmp(t, key)) { hit = 1; break; }
  }
  fclose(f);
  return hit;
}

int sf_test_main(void) {
  printf("socfreqd 真实代码自检\n\n");

  printf("== 前台包名解析: mCurrentFocus ==\n");

  sf_expect_pkg("单屏常规",
                "  mCurrentFocus=Window{a1b2c3 u0 com.tencent.tmgp.sgame/"
                "com.tencent.tmgp.sgame.MainActivity}",
                "com.tencent.tmgp.sgame");

  sf_expect_pkg("多用户 u10",
                "  mCurrentFocus=Window{a1b2c3 u10 com.android.chrome/"
                "com.google.android.apps.chrome.Main}",
                "com.android.chrome");

  sf_expect_pkg("活动名与包名不同",
                "  mCurrentFocus=Window{dead u0 com.foo.bar.app/com.foo.bar.app.Main}",
                "com.foo.bar.app");

  /* 盯返回值的正负号：
   * 若代码写成 if (!grab_pkg(...)) dst[0]=0，成功反而被清空，游戏态永不触发 */
  sf_expect_pkg("解析成功必须保留结果",
                "  mCurrentFocus=Window{cafe u0 com.probe.must.survive/"
                "com.probe.must.survive.A}",
                "com.probe.must.survive");

  sf_expect_pkg("两位用户码 u99",
                "  mCurrentFocus=Window{a1b2c3 u99 com.multiuser.app/com.multiuser.app.Main}",
                "com.multiuser.app");

  sf_expect_pkg("下划线与数字包名",
                "  mCurrentFocus=Window{a1b2c3 u0 com.garena.game_2024/com.garena.game_2024.A}",
                "com.garena.game_2024");

  printf("\n== 优先级与兜底 ==\n");

  sf_expect_pkg("focus 优先于 focusedApp",
                "  mFocusedApp=AppWindowToken{a1b2c3 u0 com.ignored.app/com.ignored.app.A}\n"
                "  mCurrentFocus=Window{a1b2c3 u0 com.wins.focus/com.wins.focus.Main}",
                "com.wins.focus");

  sf_expect_pkg("focus=null 退回 focusedApp",
                "  mCurrentFocus=null\n"
                "  mFocusedApp=AppWindowToken{a1b2c3 u0 com.fallback.app/com.fallback.app.Main}",
                "com.fallback.app");

  sf_expect_pkg("双缺退回全屏窗口",
                "  mCurrentFocus=null\n"
                "  mTopFullscreenOpaqueWindowState=Window{9f2b1a u0 com.fullscreen.game/com.fullscreen.game.Main}",
                "com.fullscreen.game");

  sf_expect_pkg("双屏取第一个全屏窗口",
                "  mTopFullscreenOpaqueWindowState=Window{aaa u0 com.first.display/com.first.display.A}\n"
                "  mTopFullscreenOpaqueWindowState=Window{bbb u0 com.second.display/com.second.display.A}",
                "com.first.display");

  sf_expect_pkg("focus 无效时仍可退回全屏窗口",
                "  mCurrentFocus=Window{a1b2c3 u0 StatusBarNoSlash}\n"
                "  mTopFullscreenOpaqueWindowState=Window{9f2b1a u0 com.below.game/com.below.game.Main}",
                "com.below.game");

  sf_expect_pkg("浮窗遮挡时取下层窗口",
                "  mCurrentFocus=Window{a1b2c3 u0 com.omarea.vtools/com.omarea.vtools.FloatWin}\n"
                "  mTopFullscreenOpaqueWindowState=Window{9f2b1a u0 com.real.game/com.real.game.Main}",
                "com.real.game");

  sf_expect_pkg("MT 浮窗遮挡时取下层窗口",
                "  mCurrentFocus=Window{a1b2c3 u0 bin.mt.plus/bin.mt.plus.FloatWin}\n"
                "  mTopFullscreenOpaqueWindowState=Window{9f2b1a u0 com.mt.game/com.mt.game.Main}",
                "com.mt.game");

  printf("\n== 解析失败必须报错而不是留垃圾 ==\n");

  sf_expect_fail("空输入", "");
  sf_expect_fail("无任何前台信息", "  mCurrentFocus=null\n  mFocusedApp=null\n");
  sf_expect_fail("窗口名无斜杠", "  mCurrentFocus=Window{a1b2c3 u0 StatusBar}\n");
  sf_expect_fail("缺少用户号", "  mCurrentFocus=Window{com.foo.bar/com.foo.bar.Main}\n");
  sf_expect_fail("用户号非数字", "  mCurrentFocus=Window{a1b2c3 uX com.foo.bar/com.foo.bar.Main}\n");

  /* 超长行不得溢出 out[] */
  {
    static char big[4096];
    static char text[4300];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    snprintf(text, sizeof text, "  mCurrentFocus=Window{a1b2c3 u0 %s}\n", big);
    char out[256] = {0};
    FILE *f = sf_stream(text);
    int rc = f ? fg_from_stream(f, out) : -999;
    if (f) fclose(f);
    sf_check(rc != 0 || strlen(out) < sizeof out, "超长包名不溢出", out);
  }

  printf("\n== grab_pkg 直接调用 ==\n");
  {
    char out[256] = {0};
    sf_check(grab_pkg("  mCurrentFocus=Window{a1b2c3 u0 com.a.b/com.a.b.C}", out) == 0 &&
                 !strcmp(out, "com.a.b"),
             "成功返回 0", out);
    sf_check(grab_pkg("  mCurrentFocus=null", out) != 0, "失败返回 -1", NULL);
    sf_check(grab_pkg("", out) != 0, "空行返回 -1", NULL);
    sf_check(grab_pkg("u0 com.x.y/", out) != 0, "空包名返回 -1", NULL);
  }

  printf("\n== 频点吸附 snap_from_list ==\n");
  {
    const char *tbl = "300000 400000 600000 800000 1000000 1200000 1400000 1600000";
    struct { long want, got; } cases[] = {
        {1600000, 1600000}, /* 正好命中 */   {1500000, 1400000}, /* 落在两档之间取低 */
        {1300000, 1200000},                  {300000, 300000},
        /* 低于最低档时返回原值，不返回 0：scaling_max_freq 写 0 会被内核拒，
           整档直接不封顶；按原值写下去内核自己会夹到它能给的最低档。
           （这条以前写成 0，是因为自检版只编译不执行，用例从没真跑过。） */
        {299999, 299999},                     {2500000, 1600000}, /* 高于最高 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
      long got = snap_from_list(tbl, cases[i].want);
      char buf[128], name[64];
      snprintf(name, sizeof name, "吸附 %ld", cases[i].want);
      snprintf(buf, sizeof buf, "got=%ld want=%ld", got, cases[i].got);
      sf_check(got == cases[i].got, name, buf);
    }
    /* 吸附值永远不得超过意图值，这是封顶语义的底线 */
    long w;
    int over = 0;
    for (w = 100000; w <= 2000000; w += 37000)
      if (snap_from_list(tbl, w) > w) over = 1;
    sf_check(!over, "吸附结果永不超过意图值", NULL);

    sf_check(snap_from_list("", 1000000) == 1000000, "无频点表时返回原值", NULL);
    sf_check(snap_from_list("   \n\t ", 1000000) == 1000000, "空白频点表返回原值", NULL);
    sf_check(snap_from_list("abc def", 1000000) == 1000000, "非数字频点被忽略", NULL);
  }

  printf("\n== 分档 assign_pct（真实 sysfs 形态的临时目录） ==\n");
  {
    /*
     * 直接跑守护的 assign_pct。它会调 snap_freq 去读
     * <dir>/scaling_available_frequencies，所以给每个 policy 造一份真节点，
     * 而不是把分档逻辑抄一份 —— 抄的那份验证不了线上行为。
     */
    char root[256];
    snprintf(root, sizeof root, "/data/local/tmp/sf_tier_test");
    extern int mkdir(const char *, unsigned);
    char pdir[256];

    struct { const char *name; long hw; long tbl[9]; } fake[] = {
        {"policy0", 3000000, {300000, 600000, 900000, 1200000, 1500000, 1800000, 2100000, 2400000, 3000000}},
        {"policy4", 2000000, {200000, 500000, 800000, 1100000, 1400000, 1700000, 2000000, 0, 0}},
        {"policy6", 1400000, {140000, 400000, 700000, 1000000, 1400000, 0, 0, 0, 0}},
        {"policy7", 1000000, {100000, 300000, 500000, 700000, 1000000, 0, 0, 0, 0}},
    };
    const int NF = 4;

    /* 建目录并写频点表；失败就跳过，绝不假装通过 */
    int built = 1;
    for (int i = 0; i < NF && built; i++) {
      snprintf(pdir, sizeof pdir, "%s/%s", root, fake[i].name);
      if (mkdir(root, 0755)) { /* 可能已存在 */ }
      if (mkdir(pdir, 0755)) built = 0;
      char fp[320], line[512];
      int off = 0;
      for (int j = 0; j < 9 && fake[i].tbl[j]; j++)
        off += snprintf(line + off, sizeof line - off, "%s%ld", j ? " " : "", fake[i].tbl[j]);
      snprintf(fp, sizeof fp, "%s/scaling_available_frequencies", pdir);
      FILE *f = fopen(fp, "w");
      if (!f) { built = 0; break; }
      fprintf(f, "%s\n", line);
      fclose(f);
    }

    if (!built) {
      printf("  skip 分档自检（无法创建 %s，需要 root）\n", root);
    } else {
      /* 统一把四档设成可区分的值：80/60/40/20 */
      cfg.pct[0] = 80; cfg.pct[1] = 60; cfg.pct[2] = 40; cfg.pct[3] = 20;
      struct policy tp[NF];
      for (int i = 0; i < NF; i++) {
        memset(&tp[i], 0, sizeof tp[i]);
        snprintf(tp[i].dir, sizeof tp[i].dir, "%s", pdir);
        snprintf(tp[i].dir, sizeof tp[i].dir, "%s/%s", root, fake[i].name);
        tp[i].hw_max = fake[i].hw;
        snprintf(tp[i].name, sizeof tp[i].name, "%s", fake[i].name);
      }
      assign_pct(tp, NF);

      /* 最高档拿 80%，最低档拿 20%，且每个簇的 cap 都不超过 hw*pct/100 */
      int ordered = 1;
      for (int i = 0; i < NF; i++)
        if (tp[i].pct < 20 || tp[i].pct > 80) ordered = 0;
      sf_check(ordered, "四档都落到设置区间内", NULL);
      sf_check(tp[0].pct == 80, "最高档 policy0 拿 80%", NULL);
      sf_check(tp[NF - 1].pct == 20, "最低档拿 20%", NULL);

      int within = 1;
      for (int i = 0; i < NF; i++)
        if (tp[i].cap > tp[i].hw_max * tp[i].pct / 100) within = 0;
      sf_check(within, "封顶值不超过意图百分比", NULL);

      int nonzero = 1;
      for (int i = 0; i < NF; i++)
        if (tp[i].cap <= 0) nonzero = 0;
      sf_check(nonzero, "封顶值都吸附到真实频点", NULL);

      /* tier 必须指向正确的那一档：WebUI 靠它把 policy 对回滑杆，
         错了就会显示"这一簇归 MID 管"但用户拖的是 BIG。 */
      int tier_ok = 1;
      for (int i = 0; i < NF; i++)
        if (tp[i].tier < 0 || tp[i].tier > 3) tier_ok = 0;
      sf_check(tier_ok, "tier 落在 0..3 内", NULL);
      /* 从高到低排的机型：最高频簇是 BIG(0)，最低频簇是 LITTLE(3) */
      {
        long lo = 0, hi = 0;
        for (int i = 0; i < NF; i++) {
          if (tp[i].hw_max > hi) hi = tp[i].hw_max;
          if (tp[i].hw_max < lo || lo == 0) lo = tp[i].hw_max;
        }
        int big_is0 = 1, little_is3 = 1;
        for (int i = 0; i < NF; i++) {
          if (tp[i].hw_max == hi && tp[i].tier != 0) big_is0 = 0;
          if (tp[i].hw_max == lo && tp[i].tier != 3) little_is3 = 0;
        }
        sf_check(big_is0, "最高频簇 tier=BIG(0)", NULL);
        sf_check(little_is3, "最低频簇 tier=LITTLE(3)", NULL);
      }

      /* 清理 */
      for (int i = 0; i < NF; i++) {
        char fp[320];
        snprintf(fp, sizeof fp, "%s/%s/scaling_available_frequencies", root, fake[i].name);
        remove(fp);
        snprintf(pdir, sizeof pdir, "%s/%s", root, fake[i].name);
        rmdir(pdir);
      }
      rmdir(root);
    }
  }

  printf("\n== 包名白名单 is_game ==\n");
  {
    extern void load_game_pkgs(void);
    g_npkg = 0;
    const char *list[] = {"com.tencent.tmgp.sgame", "com.netease.mc", "com.miHoYo.Nap"};
    for (int i = 0; i < 3 && g_npkg < MAX_PKG; i++) {
      snprintf(g_pkgs[g_npkg], PKG_LEN, "%s", list[i]);
      g_npkg++;
    }
    sf_check(is_game("com.tencent.tmgp.sgame"), "命中名单内包名", NULL);
    sf_check(is_game("com.miHoYo.Nap"), "大小写敏感匹配", NULL);
    sf_check(!is_game("com.other.game"), "未命中返回假", NULL);
    sf_check(!is_game("com.tencent.tmgp.sgam"), "不匹配前缀", NULL);
    sf_check(!is_game(""), "空包名不命中", NULL);
  }

  printf("\n== 分应用档位 app_modes ==\n");
  {
    /* 喂真文件、走真解析。路径必须是 /data/local/tmp 下的临时件：
     * MODE_FILE 是用户生效中的那份，测试往里写一次，
     * 用户给相机设的档位就没了。 */
    const char *fp = "/data/local/tmp/sf_app_modes.test";
    int n, pct;

    FILE *w = fopen(fp, "w");
    if (!w) {
      sf_check(0, "能写临时档位文件", "fopen 失败，下面整组用例跳过");
    } else {
      fputs("# 注释行不是档位\n"
            "\n"
            "com.android.camera=full\n"
            "com.autonavi.minimap=80\n"
            "com.miHoYo.Nap=FULL\n"
            "com.bad.range=0\n"
            "com.bad.range2=101\n"
            "com.bad.range3=-5\n"
            "com.bad.value=abc\n"
            "nodots=50\n"
            "com.bad char=50\n"
            "com.dup.one=40\n"
            "com.dup.one=90\n"
            "com.trailing=70   \n"
            "com.learn.one=75  #learn\n"
            "com.learn.two=full  #learn\n"
            "com.learn.none=#learn\n"
            "com.crlf.one=60\r\n"
            "com.dual.both=70:40\n"
            "com.dual.gpufollow=60:\n"
            "com.dual.cpufollow=:40\n"
            "com.keep.one=keep\n"
            "com.keep.upper=KEEP\n"
            "com.dual.bad1=0:40\n"
            "com.dual.bad2=70:101\n"
            "com.dual.bad3=:\n"
            "com.dual.bad4=70:40:50\n"
            "com.dual.bad5=keep:40\n", w);
      fclose(w);
      g_mode_mtime = -1;
      n = load_app_modes_from(fp);
      char nb[16];
      snprintf(nb, sizeof nb, "实到 %d 条", n);
      /* 合法条目恰好 13 条：camera / minimap / Nap / dup.one(合并成一条) /
       * trailing / learn.one / learn.two / crlf.one / dual.both /
       * dual.gpufollow / dual.cpufollow / keep.one / keep.upper */
      sf_check(n == 13, "只收合法条目（注释/空行/越界/坏值/无点/带空格都跳过）", nb);
      /* 学习写回来的那条带来源标记（值后面跟 " #learn"）。截不掉就是整行被
       * 当成坏值丢弃——表现是页面上明明列着一条档位，核心上却没戴帽，
       * 这是最难查的一种静默失效。 */
      sf_check(mode_lookup_cpu("com.learn.one", &pct) && pct == 75,
               "值后面的 #learn 注释被截掉，数字照样读出来", NULL);
      sf_check(mode_lookup_cpu("com.learn.two", &pct) && pct == MODE_FULL,
               "full 带来源标记也读得出满血", NULL);
      sf_check(!mode_lookup_cpu("com.learn.none", &pct), "只有标记没有值：按坏行丢掉", NULL);
      sf_check(mode_lookup_cpu("com.android.camera", &pct) && pct == MODE_FULL,
               "full → MODE_FULL", NULL);
      sf_check(mode_lookup_cpu("com.miHoYo.Nap", &pct) && pct == MODE_FULL,
               "FULL 也认（api.sh 会归一成 small，但手改文件的人不一定）", NULL);
      sf_check(mode_lookup_cpu("com.autonavi.minimap", &pct) && pct == 80,
               "数字档原样读到", NULL);
      sf_check(mode_lookup_cpu("com.trailing", &pct) && pct == 70,
               "值后面带空格要能读出来", NULL);
      /* CRLF 这一条是最容易被"顺手写的解析"漏掉的：
       * 值读成 "60\r"，atoi 照样给 60，看似没事；
       * 但 trim 漏了的话包名/值都带 \r，strcmp 永不相等，
       * 用户给相机设了档位却完全不生效。 */
      sf_check(mode_lookup_cpu("com.crlf.one", &pct) && pct == 60,
               "CRLF 行尾不影响命中（装机 tr 之外的路径也要能吃）", NULL);
      sf_check(!mode_lookup_cpu("com.bad.range", &pct), "0% 被拒（会写死节点上限）", NULL);
      sf_check(!mode_lookup_cpu("com.bad.range2", &pct), "101% 被拒", NULL);
      sf_check(!mode_lookup_cpu("com.bad.range3", &pct), "负数被拒", NULL);
      sf_check(!mode_lookup_cpu("com.bad.value", &pct), "非数字值被拒", NULL);
      sf_check(!mode_lookup_cpu("nodots", &pct), "无点的包名被拒", NULL);
      sf_check(!mode_lookup_cpu("com.bad char", &pct), "含空格的行被拒", NULL);
      /* 重复条目取后一条：守护若"先到先得"，页面上改一次值、
       * 实际生效的还是旧的那条，而文件里明明只有两行同名的。 */
      sf_check(mode_lookup_cpu("com.dup.one", &pct) && pct == 90,
               "同一包重复出现时后一条覆盖前一条", NULL);

      /* CPU:GPU 双值：拆两段各判各的；空段 = 那一边跟随全局（0）。 */
      {
        int gpuv = -1;
        sf_check(mode_lookup("com.dual.both", &pct, &gpuv) && pct == 70 && gpuv == 40,
                 "70:40 拆成 CPU 70 / GPU 40", NULL);
        sf_check(mode_lookup("com.dual.gpufollow", &pct, &gpuv)
                 && pct == 60 && gpuv == 0,
                 "60: 的 GPU 段留空 = 跟随全局", NULL);
        sf_check(mode_lookup("com.dual.cpufollow", &pct, &gpuv)
                 && pct == 0 && gpuv == 40,
                 ":40 的 CPU 段留空 = 跟随全局，GPU 单独封", NULL);
        sf_check(!mode_lookup("com.dual.bad1", &pct, &gpuv), "0:40 被拒", NULL);
        sf_check(!mode_lookup("com.dual.bad2", &pct, &gpuv), "70:101 被拒", NULL);
        sf_check(!mode_lookup("com.dual.bad3", &pct, &gpuv), "两段都空被拒", NULL);
        sf_check(!mode_lookup("com.dual.bad4", &pct, &gpuv), "多一个冒号被拒", NULL);
        sf_check(!mode_lookup("com.dual.bad5", &pct, &gpuv), "keep:40 被拒（keep 不带 GPU 段）", NULL);
        sf_check(mode_lookup("com.keep.one", &pct, &gpuv) && pct == MODE_KEEP,
                 "keep → MODE_KEEP（熄屏不交还）", NULL);
        sf_check(mode_lookup("com.keep.upper", &pct, &gpuv) && pct == MODE_KEEP,
                 "KEEP 大小写都认", NULL);
      }

      /* mtime 缓存：内容没变再读一次不能重新解析（每轮巡检都重读整个文件
       * 是白烧 I/O），内容变了必须重载（用户改档位要热生效，不重启守护）。
       * 后半段靠 utimensat 把 mtime 明确往前推——不推的话同秒内写完再读，
       * mtime 没变，这条断言就变成"看运气"。utimensat 失败要直接报红，
       * 不能悄悄跳过：跳过的用例和没写的用例是同一回事。 */
      n = load_app_modes_from(fp);
      sf_check(n == 13 && g_nmode == 13, "mtime 未变时走缓存、表内容不变", NULL);
      {
        struct stat st0;
        stat(fp, &st0);
        FILE *a = fopen(fp, "a");
        fputs("com.append.two=55\n", a);
        fclose(a);
        struct timespec ts[2] = {{ st0.st_mtime + 2, 0 }, { st0.st_mtime + 2, 0 }};
        int urc = utimensat(AT_FDCWD, fp, ts, 0);
        char ub[48];
        snprintf(ub, sizeof ub, "utimensat rc=%d", urc);
        sf_check(urc == 0, "能把 mtime 往前推（热重载断言的前提）", ub);
        n = load_app_modes_from(fp);
        sf_check(n == 14, "文件变了就重载（热生效，不用重启守护）", NULL);
        sf_check(mode_lookup_cpu("com.append.two", &pct) && pct == 55, "新条目命中", NULL);
      }
      remove(fp);
    }

    /* 没有这个文件是常态（用户还没设过任何档位），必须当"零条"而不是错。
     * 这里不能再用 MODE_FILE 判——本机若真有用户数据，结果就取决于用户设没设过。
     * 所以指到一个确定不存在的临时路径。 */
    {
      g_mode_mtime = -1;
      int saved = g_nmode;
      g_nmode = 3;
      n = load_app_modes_from("/data/local/tmp/sf_app_modes.not.exist");
      sf_check(n == 0 && g_nmode == 0, "档位文件不存在 → 0 条，不是失败", NULL);
      (void)saved;
    }
  }

  printf("\n== 亮屏信号取值（socfreqd.c）==\n");
  {
    sf_check(screen_value_on(0, 1) == 1, "fb/blank 0 = 亮", NULL);
    sf_check(screen_value_on(1, 1) == 0, "fb/blank 1 = 灭", NULL);
    sf_check(screen_value_on(0, 0) == 0, "backlight 0 = 灭", NULL);
    sf_check(screen_value_on(1127, 0) == 1, "backlight >0 = 亮", NULL);
    sf_check(screen_value_on(-1, 0) == -1 && screen_value_on(-1, 1) == -1,
             "读不到信号返回 -1（调用处按亮屏/继续轻巡检处理）", NULL);
  }

  printf("\n== 电量阶梯（adapt.c）==\n");
  {
    struct adapt_cfg c;
    memset(&c, 0, sizeof c);
    c.batt = 1; c.batt_low = 20; c.batt_low_cut = 5;
    c.batt_crit = 10; c.batt_crit_cut = 10;
    c.up_max = 15; c.down_max = 30;
    sf_check(batt_cut_pct(25, 0, &c) == 0, "25% 不收紧", NULL);
    sf_check(batt_cut_pct(15, 0, &c) == 5, "低于 20% 第一刀 -5", NULL);
    sf_check(batt_cut_pct(5, 0, &c) == 15, "低于 10% 两刀叠加 -15", NULL);
    sf_check(batt_cut_pct(5, 1, &c) == 0, "插电时不收紧", NULL);
    c.batt = 0;
    sf_check(batt_cut_pct(5, 0, &c) == 0, "开关关掉就完全不收紧", NULL);
    c.batt = 1;
    sf_check(batt_cut_pct(-1, 0, &c) == 0, "容量读不到不收紧", NULL);

    struct adapt a;
    memset(&a, 0, sizeof a);
    adapt_apply_cap(&a, &c, 0, 50000, 0, 5, 100, 5, 1);
    sf_check(a.batt_cut == 15 && a.delta == -15,
             "容量 5%：delta 里包含 -15", NULL);
    c.down_max = 10;
    memset(&a, 0, sizeof a);
    adapt_apply_cap(&a, &c, 0, 50000, 0, 5, 100, 5, 1);
    sf_check(a.delta == -10, "总闸 ADAPT_DOWN_MAX 仍优先于电量阶梯", NULL);
  }

  printf("\n== 温控 zone 过滤（adapt.c）==\n");
  {
    sf_check(thermal_type_ok("cpu-0-0") && thermal_type_ok("GPU") &&
             thermal_type_ok("soc_thermal"),
             "cpu / gpu / soc 三类 zone 都收（大小写都认）", NULL);
    sf_check(!thermal_type_ok("vbat") && !thermal_type_ok("battery") &&
             !thermal_type_ok("bcl"),
             "vbat / battery / bcl 不是温度 zone，不能收", NULL);
    sf_check(!thermal_type_ok("cpu-hw-trip-0") && !thermal_type_ok("CPU-HW-TRIP-1") &&
             !thermal_type_ok("soc-trip"),
             "cpu-hw-trip-* 是阈值不是温度，不能收（temp 恒 95000 会把整机误判成 95℃）", NULL);
    /* 集成一层：真造两个 zone，一个正常 42℃、一个 trip 95℃，走真实扫描+取最大值。
       只测谓词的话，扫描里漏调谓词也能全绿。 */
    system("rm -rf /data/local/tmp/thm_trip_test && "
           "mkdir -p /data/local/tmp/thm_trip_test/thermal_zone0 "
           "/data/local/tmp/thm_trip_test/thermal_zone1");
    FILE *f = fopen("/data/local/tmp/thm_trip_test/thermal_zone0/type", "w");
    if(f){ fputs("cpu-0-0", f); fclose(f); }
    f = fopen("/data/local/tmp/thm_trip_test/thermal_zone0/temp", "w");
    if(f){ fputs("42000", f); fclose(f); }
    f = fopen("/data/local/tmp/thm_trip_test/thermal_zone1/type", "w");
    if(f){ fputs("cpu-hw-trip-0", f); fclose(f); }
    f = fopen("/data/local/tmp/thm_trip_test/thermal_zone1/temp", "w");
    if(f){ fputs("95000", f); fclose(f); }
    setenv("SOCFREQ_THERMAL_DIR", "/data/local/tmp/thm_trip_test", 1);
    setenv("SOCFREQ_PSI_PATH", "/data/local/tmp/thm_trip_test/none", 1);
    setenv("SOCFREQ_BATT_PATH", "/data/local/tmp/thm_trip_test/none", 1);
    setenv("SOCFREQ_CAP_PATH", "/data/local/tmp/thm_trip_test/none", 1);
    {
      struct adapt_cfg c;
      struct adapt a;
      memset(&c, 0, sizeof c);
      c.th = 1; c.th_high = 70; c.th_low = 60; c.th_step = 5; c.th_max = 25; c.th_back = 60;
      adapt_init();
      adapt_tick(&c, 5, 0);
      adapt_snapshot(&a);
      sf_check(a.temp_m == 42000,
               "真实扫描：trip 95℃ 被过滤，最热温度仍是正常 zone 的 42℃", NULL);
      sf_check(a.th_steps == 0, "因此温控不压档，学习不会被 dlt 卡住", NULL);
    }
    system("rm -rf /data/local/tmp/thm_trip_test");
  }

  printf("\n== 电流归一化 / 深度空闲判据 ==\n");
  {
    char b1[96];
    sf_check(batt_current_ma(-540, 1) == 540, "mA 机型：负号取绝对值", NULL);
    sf_check(batt_current_ma(-540000, 0) == 540, "µA 机型：/1000 后得 540mA", NULL);
    sf_check(batt_current_ma(1120, 0) == 1, "µA 小电流向下取整不溢出", NULL);

    const char *cp = "/data/local/tmp/sf_current.test";
    long cv = 0;
    FILE *cf = fopen(cp, "w");
    if (cf) {
      fputs("-540\n", cf); fclose(cf);
      sf_check(read_signed(cp, &cv) && cv == -540,
               "有符号读数：-540 是有效电流，不是读失败", NULL);
      cf = fopen(cp, "w"); if (cf) { fputs("bad\n", cf); fclose(cf); }
      sf_check(!read_signed(cp, &cv), "非数字电流返回读失败", NULL);
      cf = fopen(cp, "w"); if (cf) { fputs("540\n", cf); fclose(cf); }
      sf_check(read_signed(cp, &cv) && cv == 540, "正电流照样读得出", NULL);
      remove(cp);
    }

    long ema = 0; int hold = 0;
    int r = idle_step(&ema, &hold, 600, 1, 250, 2, 150);
    sf_check(r == 0 && ema == 600 && hold == 0, "第一拍只立 EMA 基线", NULL);
    r = idle_step(&ema, &hold, 100, 1, 250, 2, 150);
    sf_check(r == 0 && hold == 1, "低电流第 1 拍只计数", NULL);
    r = idle_step(&ema, &hold, 100, 1, 250, 2, 150);
    snprintf(b1, sizeof b1, "hold=%d ema=%ld", hold, ema);
    sf_check(r == 1 && hold >= 2, "连续 2 拍低电流进入深度空闲", b1);

    ema = 100; hold = 1;
    r = idle_step(&ema, &hold, 300, 1, 250, 2, 150);
    sf_check(r == 2 && hold == 0, "相对 EMA 突增 150mA 给出唤醒提示（不直接判亮屏）", NULL);

    ema = 100; hold = 1;
    long before = ema;
    r = idle_step(&ema, &hold, 0, 0, 250, 2, 150);
    sf_check(r == 0 && hold == 0 && ema == before,
             "电流读不到：清计数、不改基线、永不进深睡", NULL);
  }

  printf("\n== 学习天数按 24 小时切块（不按零点）==\n");
  {
    /* 旧口径是 time(NULL)/86400，国内早上 8 点翻篇：晚上装、早上看就成了"2 天"。
       新口径从首次采样起每满 24 小时才加一天，这里是那套算法的纯函数。 */
    char b1[96];
    long t0 = 1000000;
    long d9 = learn_day_index(t0, t0 + 9 * 3600);
    snprintf(b1, sizeof b1, "9h -> 第 %ld 天", d9 + 1);
    sf_check(d9 == 0, "装完 9 小时仍算第 1 天（过零点不加天）", b1);
    sf_check(learn_day_index(t0, t0 + 23 * 3600) == 0, "23 小时仍是第 1 天", NULL);
    sf_check(learn_day_index(t0, t0 + 24 * 3600) == 1, "满 24 小时才算第 2 天", NULL);
    sf_check(learn_day_index(t0, t0 + 28 * 86400) == 0, "28 天环回（daybits 只有 28 位）", NULL);
    sf_check(learn_day_index(0, t0 + 99 * 86400) == 0, "没有起点时按第 1 天", NULL);
    sf_check(learn_day_index(t0 + 5000, t0) == 0, "时钟倒退不产生负索引", NULL);
  }

  printf("\n== 旧格式样本在升级时清空（learn.c）==\n");
  {
    /* v1 的行没有"这道帽是多少"那一列，和新判据的帽有效性对不上。
       升级时必须两份一起清掉并留标记，页面才知道该说"重新积累"而不是"没数据"。
       这里不碰 sysfs，只验迁移本身。 */
    const char *op = "/data/local/tmp/sf_v1.test";
    const char *oa = "/data/local/tmp/sf_v1_app.test";
    const char *mark = "/data/local/tmp/sf_v1.test.v2reset";
    remove(op); remove(oa); remove(mark);
    FILE *f = fopen(op, "w");
    if (f) { fprintf(f, "policy0 0 3600000 40000 0 0 0 0 0 0 0 10000 60000 0 0 0 0 3\n"); fclose(f); }
    f = fopen(oa, "w");
    if (f) { fprintf(f, "com.a.b 0 3600000 40000 0 0 0 0 0 0 0 10000 60000 0 0 0 0 3\n"); fclose(f); }
    learn_init(op, oa);
    sf_check(access(op, F_OK) != 0 && access(oa, F_OK) != 0,
             "v1 的两份样本都被清掉", NULL);
    sf_check(access(mark, F_OK) == 0, "留下 v2reset 标记（页面据此提示重新积累）", NULL);
    learn_set_enabled(0);
    sf_check(access(mark, F_OK) != 0, "关掉学习时标记也撤掉，不留残渣", NULL);
    remove(op); remove(oa); remove(mark);

    f = fopen(op, "w");
    if (f) { fprintf(f, "#fmt=2\npolicy0 0 3600000 40000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 3 75\n"); fclose(f); }
    learn_init(op, oa);
    sf_check(access(op, F_OK) == 0 && access(mark, F_OK) != 0,
             "已经是 v2 的样本不会被清", NULL);
    learn_set_enabled(0);
    remove(op); remove(oa); remove(mark);
  }

  printf("\n== 学习两层互斥（learn.c）==\n");
  {
    /* 验的不是"分布准不准"（那要几天才看得出），验的是三件当场能定的事：
       戴全局帽的那一拍只进全局行、戴应用帽的那一拍只进按应用那层、
       两份文件都落得下来且字段数一致。采样读的是真 sysfs，所以只能在真机上跑。 */
    const char *gp = "/data/local/tmp/sf_learn.test";
    const char *ap = "/data/local/tmp/sf_learn_app.test";
    static char dbuf[4][80];
    const char *dirs[4];
    int tiers[4];
    long hw[4], cap[4], pct[4];
    int np = 0;
    for (int i = 0; i < 8 && np < 4; i++) {
      snprintf(dbuf[np], sizeof dbuf[np], "/sys/devices/system/cpu/cpufreq/policy%d", i);
      char p[200];
      snprintf(p, sizeof p, "%s/stats/time_in_state", dbuf[np]);
      if (access(p, F_OK)) continue;
      dirs[np] = dbuf[np]; tiers[np] = np; hw[np] = 2000000; cap[np] = 1500000; pct[np] = 75; np++;
    }
    remove(gp); remove(ap);
    if (!np) {
      sf_check(0, "这台机上有可读的 policy 目录", "一个都没找到，整组跳过");
    } else {
      char b1[512];
      learn_init(gp, ap);
      learn_set_enabled(1);
      learn_tick(1, "", 0, dirs, tiers, hw, cap, pct, np);     /* 第一拍只刷基线 */
      sleep(2);
      learn_tick(1, "", 0, dirs, tiers, hw, cap, pct, np);     /* 戴的是全局那道帽 */
      learn_flush();
      long g1 = sf_col_sum(gp, 3), a1 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "全局采样=%ld 按应用=%ld 目录数=%d", g1, a1, np);
      sf_check(g1 >= 1000, "戴全局帽的这一拍进了全局行", b1);
      sf_check(a1 <= 0, "同一拍按应用那层一个字都不写", b1);
      sleep(2);
      /* eligible 故意还传 1：专属帽不进全局行是 learn.c 自己的不变量，不靠调用处自觉 */
      learn_tick(1, "com.test.app", 1, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long g2 = sf_col_sum(gp, 3), a2 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "全局 %ld→%ld 按应用=%ld", g1, g2, a2);
      sf_check(g2 == g1, "戴应用帽时全局行不再涨（误传 eligible 也拦得住）", b1);
      sf_check(a2 >= 1000, "这一拍进了按应用那层", b1);
      sf_check(sf_first_col_is(ap, "com.test.app"), "按应用行的第一列是包名", NULL);
      char aline[300];
      sf_first_data_line(ap, aline, sizeof aline);
      snprintf(b1, sizeof b1, "按应用=%d 全局=%d app首行=[%s]",
               sf_line_cols(ap), sf_line_cols(gp), aline);
      sf_check(sf_line_cols(ap) == 31 && sf_line_cols(gp) == 31,
               "两份样本每行都是 31 个字段（24 桶 + 帽，同构）", b1);
      learn_set_enabled(0);
      sf_check(access(gp, F_OK) != 0 && access(ap, F_OK) != 0,
               "关掉学习时两份样本一起删", NULL);
      remove(gp); remove(ap);
    }
  }

  printf("\n== 学习：全局帽下的前台包归属（learn.c）==\n");
  {
    /* 第二段开的是"没设专属档位的包也学"。验三件当场能定的事：
       全局帽下的同一拍两层都涨、同一个包按上一轮的前台归因、
       帽换过之后应用行从零重攒（否则 65% 的顶帽证据会一路推着 70→75→80 涨）。 */
    const char *gp = "/data/local/tmp/sf_learn2.test";
    const char *ap = "/data/local/tmp/sf_learn2_app.test";
    static char dbuf[4][80];
    const char *dirs[4];
    int tiers[4];
    long hw[4], cap[4], pct[4];
    int np = 0;
    for (int i = 0; i < 8 && np < 4; i++) {
      snprintf(dbuf[np], sizeof dbuf[np], "/sys/devices/system/cpu/cpufreq/policy%d", i);
      char p[200];
      snprintf(p, sizeof p, "%s/stats/time_in_state", dbuf[np]);
      if (access(p, F_OK)) continue;
      dirs[np] = dbuf[np]; tiers[np] = np; hw[np] = 2000000; cap[np] = 1500000; pct[np] = 75; np++;
    }
    remove(gp); remove(ap);
    if (!np) {
      sf_check(0, "这台机上有可读的 policy 目录", "一个都没找到，整组跳过");
    } else {
      char b1[128];
      learn_init(gp, ap);
      learn_set_enabled(1);
      learn_tick(1, "", 0, dirs, tiers, hw, cap, pct, np);      /* 只刷基线 */
      sleep(2);
      learn_tick(1, "com.test.global", 0, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long g1 = sf_col_sum(gp, 3), a1 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "全局=%ld 按应用=%ld 目录数=%d", g1, a1, np);
      sf_check(g1 >= 1000 && a1 >= 1000, "全局帽下的同一拍：全局行和前台包行都涨", b1);
      sf_check(sf_first_col_is(ap, "com.test.global"), "归属的是上一轮的前台包", NULL);

      sleep(2);
      learn_tick(1, "com.test.global", 0, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long a2 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "上一拍=%ld 这一拍=%ld", a1, a2);
      sf_check(a2 > a1, "同一道帽下继续累计", b1);

      for (int i = 0; i < np; i++) { cap[i] += 75000; pct[i] += 5; } /* 全部档位抬 5 个点 */
      sleep(2);
      learn_tick(1, "com.test.global", 0, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long a3 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "换帽前=%ld 换帽后=%ld", a2, a3);
      sf_check(a3 < a2, "帽换了，应用行从零重攒（不混两道帽的分布）", b1);

      /* 戴应用专属帽的一拍（eligible=0）：全局行必须原样不动。
         旧代码在这里会把全局行的帽改成应用百分比并清零，用户每切一次
         带档位的应用（抖音 40%）就丢一次全局 CPU 样本。 */
      sleep(2);
      learn_tick(1, "com.test.global", 0, dirs, tiers, hw, cap, pct, np);
      sleep(2);
      learn_tick(1, "com.test.global", 0, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long g4 = sf_col_sum(gp, 3), a4 = sf_col_sum(ap, 3);
      for (int i = 0; i < np; i++) { cap[i] += 75000; pct[i] += 5; }
      sleep(2);
      learn_tick(0, "com.test.global", 1, dirs, tiers, hw, cap, pct, np);
      learn_flush();
      long g5 = sf_col_sum(gp, 3), a5 = sf_col_sum(ap, 3);
      snprintf(b1, sizeof b1, "全局 %ld→%ld 应用 %ld→%ld", g4, g5, a4, a5);
      sf_check(g4 > 0 && g5 == g4, "戴应用帽的一拍不清全局行（哪怕百分比变了）", b1);
      sf_check(a5 < a4, "同一拍应用行按新帽从零重攒", b1);

      learn_set_enabled(0);
      remove(gp); remove(ap);
    }
  }

  printf("\n== 档位切换要能强制生效（升档陷阱）==\n");
  {
    /* set_caps 的语义：app_pct>0 四档统一，0 回到 cfg 的分档。
     * 关键是"从低档应用切到高档应用时 cap 必须真的变大"——
     * 巡检的判断是 cur<=cap 就跳过，cap 没重算就永远停在旧值。 */
    struct policy tp[2];
    memset(tp, 0, sizeof tp);
    snprintf(tp[0].dir, sizeof tp[0].dir, "/data/local/tmp/sf_nodir");
    tp[0].hw_max = 3000000; tp[0].pol_no = 0; tp[0].tier = 0;
    tp[1].hw_max = 2000000; tp[1].pol_no = 1; tp[1].tier = 3;
    struct policy save[MAX_POLICY];
    int save_n = g_npol;
    memcpy(save, g_pol, sizeof g_pol);
    memcpy(g_pol, tp, sizeof tp);
    g_npol = 2;
    int save_big = cfg.pct[0], save_lit = cfg.pct[3];
    cfg.pct[0] = 65; cfg.pct[3] = 65;

    set_caps(0, "test");
    long g1 = g_pol[0].cap;
    set_caps(70, "test");
    long g2 = g_pol[0].cap;
    set_caps(0, "test");
    long g3 = g_pol[0].cap;
    sf_check(g2 > g1, "专属档位 70% 的封顶高于全局 65%", NULL);
    sf_check(g3 == g1, "退出专属档位能精确回到全局封顶", NULL);
    sf_check(g_pol[0].pct == 65 && g_pol[1].pct == 65, "重算后 pct 与 cap 同步", NULL);
    set_caps(80, "test");
    sf_check(g_pol[0].pct == 80 && g_pol[1].pct == 80,
             "数字档是四簇统一（不是只动超大核）", NULL);
    /* snap 只会往下吸附，任何情况下都不得超过意图值 */
    int within = 1;
    for (int p = 1; p <= 100; p++) {
      set_caps(p, "test");
      for (int i = 0; i < g_npol; i++)
        if (g_pol[i].cap > g_pol[i].hw_max * p / 100) within = 0;
    }
    sf_check(within, "任意档位下封顶都不越过设定百分比", NULL);

    memcpy(g_pol, save, sizeof g_pol);
    g_npol = save_n;
    cfg.pct[0] = save_big; cfg.pct[3] = save_lit;
  }

  printf("\n== 交权态不抢回节点（游戏被实时偏移打断）==\n");
  {
    /* 主循环本身没法在自检里跑（会真写节点），所以把判据抽成纯函数：
       cfg/adapt 重算时只有它放行才调 apply_normal。游戏态放行一次，
       g_state 会被改成 normal，而游戏分支同一包名不会重新 unlock_game，
       结果就是整个游戏期间被封顶、写权限也收不回来。 */
    char save[16];
    snprintf(save, sizeof save, "%s", g_state);
    snprintf(g_state, sizeof g_state, "game");
    sf_check(!reclaim_caps_allowed(), "游戏/满血/熄屏期间不抢回节点", NULL);
    snprintf(g_state, sizeof g_state, "normal");
    sf_check(reclaim_caps_allowed(), "常态下允许写回封顶", NULL);
    snprintf(g_state, sizeof g_state, "%s", save);
  }

  printf("\n== GPU 生效值（实时偏移 / 频点吸附）==\n");
  {
    struct gpu save = g_gpu;
    memset(&g_gpu, 0, sizeof g_gpu);
    g_gpu.ok = 1;
    g_gpu.max_hz = 3000000;
    g_gpu.min_hz = 500000;
    const long t[] = {3000000, 2600000, 2200000, 1800000, 1500000,
                      1200000, 1000000, 800000, 600000, 500000};
    g_gpu.ntbl = (int)(sizeof t / sizeof *t);
    for (int i = 0; i < g_gpu.ntbl; i++) g_gpu.tbl[i] = t[i];

    char b1[64];
    long got = gpu_snap(45);
    snprintf(b1, sizeof b1, "45%% -> %ldHz", got);
    sf_check(got == 1200000, "45% 吸附到不高于目标的最高频点", b1);
    sf_check(gpu_snap(100) == 3000000 && gpu_snap(1) == 500000,
             "两端夹在硬件最高/最低之间", NULL);

    got = gpu_target_for(45, -25);
    snprintf(b1, sizeof b1, "45%%-25 -> %ldHz，期望 %ldHz", got, gpu_snap(20));
    sf_check(got == gpu_snap(20), "温控/PSI 负偏移直接压低 GPU 目标", b1);
    sf_check(gpu_target_for(45, 60) == 3000000, "正偏移最多到硬件最高", NULL);
    sf_check(gpu_target_for(45, -100) == 500000, "负偏移不会穿到最低档以下", NULL);
    sf_check(gpu_pwr_for(1200000) == 5, "pwrlevel 索引按同一张降序表算", NULL);

    /* MTK 天玑：OPP 表是按 freq: 排的 kHz，index 0 最高。 */
    long mt[8] = {0};
    int mn = mtk_parse_opp_text(
        "[00] freq: 1716000, volt:  85000, vsram:  75000\n"
        "[01] freq: 1690000, volt:  85000, vsram:  75000\n"
        "[37] freq:  754000, volt:  68500, vsram:  75000\n", mt, 8);
    sf_check(mn == 3 && mt[0] == 1716000000L && mt[2] == 754000000L,
             "MTK OPP 表只取 freq:，kHz 归一到 Hz", NULL);
    g_gpu.max_hz = 1716000000L;
    g_gpu.min_hz = 26000000L;
    g_gpu.ntbl = 3;
    g_gpu.tbl[0] = 1716000000L;
    g_gpu.tbl[1] = 1690000000L;
    g_gpu.tbl[2] = 754000000L;
    sf_check(gpu_pwr_for(754000000L) == 2 && gpu_pwr_for(1000000000L) == 2
             && gpu_pwr_for(1716000000L) == 0,
             "MTK 吸附后的频率能映射到 OPP index", NULL);

    g_gpu = save;
  }

  printf("\n== SoC 芯片代号 last_word ==\n");
  {
    /*
     * 分词逻辑原本内联在 read_soc_name 里，属性值在 PC 上注入不了，
     * 切词这件事没法验。抽出 last_word 之后可以直接喂字符串。
     *
     * 注意 last_word 记的是"最后一个词的开头"而不是"最后一个非分隔字符"：
     * 后者会把 "SM8750" 切成 "0"、"sun" 切成 "n"，
     * 横幅就显示成「检测到你的 SoC：0」。
     */
    struct { const char *in, *want; } cases[] = {
        {"SM8750", "sm8750"},
        {"sun", "sun"},
        {"Qualcomm Technologies, Inc SM8750", "sm8750"},
        {"mt6893", "mt6893"},
        {"MediaTek Dimensity 9200", "9200"},
        {"SM8550 ", "sm8550"},            /* 尾部空格 */
        {"  SM8550", "sm8550"},            /* 头部空格 */
        {"SM 8550", "8550"},               /* 中间空格：取后半 */
        {"Tensor G3", "g3"},
        {"A,B,C", "c"},                    /* 纯逗号分隔 */
        {"\tSM8750\t", "sm8750"},          /* 制表符 */
        {"SM8550 , ", "sm8550"},           /* 尾部逗号+空格 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
      char got[128], name[96];
      memset(got, 0xAA, sizeof got);
      last_word(cases[i].in, got, sizeof got);
      snprintf(name, sizeof name, "切词 \"%s\"", cases[i].in);
      sf_check(!strcmp(got, cases[i].want), name, got);
    }
    /* 全是分隔符 → 空，read_soc_name 会跳过这个属性继续试下一个 */
    char e[16];
    last_word("   ", e, sizeof e);
    sf_check(e[0] == 0, "全是分隔符时返回空", e);
    last_word("", e, sizeof e);
    sf_check(e[0] == 0, "空串返回空", e);
    /* 小缓冲区不能溢出 */
    char small[4];
    memset(small, 0xAA, sizeof small);
    last_word("SM8750", small, sizeof small);
    sf_check(small[3] == 0, "小缓冲区截断且以 0 结尾", small);
    sf_check(strlen(small) == 3, "截断长度正确", small);
  }

  printf("\n== SoC 芯片代号 read_soc_name（PC 上取不到属性，只验边界）==\n");
  {
    char soc[128];
    memset(soc, 0xAA, sizeof soc);
    read_soc_name(soc, sizeof soc);
    sf_check(soc[0] != 0, "无属性时也能给出非空值", soc);
    sf_check(strchr(soc, ' ') == NULL || !strcmp(soc, "未知"),
             "结果是单个词（不带空格）", soc);
    char small[8];
    memset(small, 0xAA, sizeof small);
    read_soc_name(small, sizeof small);
    sf_check(small[sizeof small - 1] == 0 || strlen(small) < sizeof small,
             "缓冲区小时不越界", small);
    sf_check(strlen(soc) < sizeof soc, "结果未溢出", NULL);
  }

  printf("\n== 图标：图片头解析（字节序回归）==\n");
  {
    /* icon.c 里这三个函数是 static，通过 SF_TEST 专用的壳调进来。 */
    extern int sf_test_png_dim(const uint8_t *, size_t, int *, int *);
    extern int sf_test_webp_dim(const uint8_t *, size_t, int *, int *);
    extern int sf_test_score_pixels(const char *, const uint8_t *, size_t);

    /* PNG：签名 + 块长 + "IHDR" + 宽(4,大端) + 高(4,大端) */
    #define PNGH(w1,w2,w3,w4,h1,h2,h3,h4) { \
      0x89,'P','N','G','\r','\n',0x1a,'\n',  0,0,0,13,  'I','H','D','R', \
      w1,w2,w3,w4,  h1,h2,h3,h4 }
    const uint8_t png192[24] = PNGH(0,0,0,192, 0,0,0,192);
    const uint8_t png144[24] = PNGH(0,0,0,144, 0,0,0,144);
    const uint8_t png48 [24] = PNGH(0,0,0,48,  0,0,0,48);
    const uint8_t png64 [24] = PNGH(0,0,0,64,  0,0,0,64);
    const uint8_t pngNS [24] = PNGH(0,0,0,12,  0,0,0,105);   /* 12x105 非正方 */
    const uint8_t png600[24] = PNGH(0,0,2,88,  0,0,2,88);    /* 600x600 太大 */
    const uint8_t png32 [24] = PNGH(0,0,0,32,  0,0,0,32);    /* 32 太小 */
    const uint8_t pngBad[24] = { 'P','N','G',0,0,0,0,0, 0,0,0,13,'I','H','D','R',
                                 0,0,0,192,0,0,0,192 };
    int w = -1, h = -1;

    /* 这一条就是回归本体：rd32（小端）会把 00 00 00 C0 读成 3221225472，
       被 w>512 判掉，于是所有 PNG 全军覆没、页面只剩色块。 */
    sf_check(sf_test_png_dim(png192, sizeof png192, &w, &h) && w == 192 && h == 192,
             "PNG IHDR 按大端读 192x192", NULL);
    w = h = -1;
    sf_check(sf_test_png_dim(png64, sizeof png64, &w, &h) && w == 64 && h == 64,
             "PNG 64x64", NULL);
    sf_check(!sf_test_png_dim(pngBad, sizeof pngBad, &w, &h), "签名不对要拒绝", NULL);
    sf_check(!sf_test_png_dim(png192, 23, &w, &h), "头部不足 24 字节要拒绝", NULL);

    /* WebP VP8X：画布宽高在 24..29，24 位小端，存的是"实际值减 1" */
    uint8_t vx[30] = { 'R','I','F','F', 0,0,0,0, 'W','E','B','P',
                       'V','P','8','X', 10,0,0,0, 0,0,0,0,
                       0xFF,0x01,0x00, 0xFF,0x01,0x00 };
    w = h = -1;
    sf_check(sf_test_webp_dim(vx, sizeof vx, &w, &h) && w == 512 && h == 512,
             "WebP VP8X 512x512", NULL);

    /* VP8L：字节 20 是 0x2f，之后 14 位宽-1、14 位高-1 */
    uint8_t vl[32] = { 'R','I','F','F', 0,0,0,0, 'W','E','B','P',
                       'V','P','8','L', 5,0,0,0, 0x2f,
                       0x63,0xC0,0x18, 0x00, 0,0,0,0 };
    w = h = -1;
    sf_check(sf_test_webp_dim(vl, sizeof vl, &w, &h) && w == 100 && h == 100,
             "WebP VP8L 100x100", NULL);

    /* VP8（有损）：0x9d 0x01 0x2a 之后各 16 位里取低 14 位 */
    uint8_t vy[30] = { 'R','I','F','F', 0,0,0,0, 'W','E','B','P',
                       'V','P','8',' ', 0x18,0,0,0, 0,0,0,
                       0x9d,0x01,0x2a, 0x80,0x00, 0x80,0x00 };
    w = h = -1;
    sf_check(sf_test_webp_dim(vy, sizeof vy, &w, &h) && w == 128 && h == 128,
             "WebP VP8 128x128", NULL);

    /* score_pixels 的取舍：只看图本身，名字必须是 res/ 下的 */
    sf_check(sf_test_score_pixels("res/aK.png", png192, sizeof png192) >= 1,
             "res/ 下的正方形 PNG 要收", NULL);
    sf_check(sf_test_score_pixels("assets/x.png", png192, sizeof png192) == -1,
             "assets/ 一律不收（多是动画帧）", NULL);
    sf_check(sf_test_score_pixels("res/x.png", pngNS, sizeof pngNS) == -1,
             "非正方不收", NULL);
    sf_check(sf_test_score_pixels("res/big.png", png600, sizeof png600) == -1,
             "超过 512 不收", NULL);
    sf_check(sf_test_score_pixels("res/s.png", png32, sizeof png32) == -1,
             "小于 48 不收", NULL);
    int s192 = sf_test_score_pixels("res/a.png", png192, sizeof png192);
    int s64  = sf_test_score_pixels("res/b.png", png64,  sizeof png64);
    int s144 = sf_test_score_pixels("res/c.png", png144, sizeof png144);
    int s48  = sf_test_score_pixels("res/d.png", png48,  sizeof png48);
    /* 目标值跟着 ICON_SLOT_PX(124) 走，不是 192：
       第一轮和第二轮必须同一个目标，否则改一处就漂移。 */
    sf_check(s144 > s192, "越接近槽位 124 分越高（144 应赢过 192）", NULL);
    /* 64 和 192 离槽位一样远（d=60/68 都落在 q=4），分数应当相同。
       整数分值必然有平台，这条把平台本身记下来。
       注意**不要**写成 s64 < s192：两个 5 比 5 是 false，
       用例必红，而打分公式其实是对的——很容易让人回头去改本来没问题的
       那一段。分值是 1..9 的整数档，不是 w 的单调函数。 */
    sf_check(s64 == s192, "64 与 192 离槽位同样远，同分（分值是整数档位，不是单调函数）", NULL);
    /* 但"另一侧偏得更远就要更低"仍然要成立，否则上面那条
       会被理解成"两边都随便给"。48 离槽位 76，比 64 的 60 更远。 */
    sf_check(s48 < s64, "48 比 64 更偏离槽位，分数更低", NULL);
    #undef PNGH
  }

  printf("\n== 图标：score_name（空指针回归）==\n");
  {
    extern int sf_test_score_name(const char *);
    /* 这一组是回归本体：score_name 里 strrchr 的结果没判空，
       碰到没有点号的条目名就 strcmp(NULL + 1, "png")，
       读地址 0x1 直接 SIGSEGV。而扫描循环对每个中央目录条目
       都调它、没有前置过滤，所以一条 "LICENSES" 就能让整个命令崩、
       页面全色块——不是"少一张图"。

       这三个名字不是编的，是三个真包里数出来的：
         coolapk.apk  CD 5913 条，无点号 7 条（LICENSES、assets/…）
         base.apk     CD 3227 条，无点号 5 条
         Kin.apk      CD  105 条，无点号 2 条
       LICENSES 这类位置很靠前，扫到就崩。 */
    sf_check(sf_test_score_name("LICENSES") == -1, "LICENSES（无点号）不崩且不收", NULL);
    sf_check(sf_test_score_name("META-INF/services/vq1") == -1,
             "META-INF/services/vq1（无点号）不崩且不收", NULL);
    sf_check(sf_test_score_name("assets/libwbsafeedit") == -1,
             "assets/libwbsafeedit（无点号）不崩且不收", NULL);
    sf_check(sf_test_score_name("okhttp3/internal/publicsuffix/NOTICE") == -1,
             "NOTICE（无点号）不崩且不收", NULL);
    /* 目录条目：中央目录里目录也占一条，以 '/' 结尾且没有点号 */
    sf_check(sf_test_score_name("res/mipmap-xxhdpi/") == -1,
             "目录条目跳过（省得白算）", NULL);
    /* 这条不是回归，是确认整条排除逻辑还活着 */
    sf_check(sf_test_score_name("res/mipmap-xxhdpi-v4/ic_launcher_round.png") == -1,
             "round 变体不进第一轮", NULL);
    /* 正常的必须还能收，否则上面几条会一起"通过" */
    sf_check(sf_test_score_name("res/mipmap-xxhdpi-v4/ic_launcher.png") > 10,
             "正常图标仍要收（防止上面的拒绝把功能一起否掉）", NULL);
    /* 钉死**具体值**，不钉方向。
       密度权重这三轮翻过两次（35+i → 39-i → 中间档最高），
       每次都只是"方向对不对"，改错了方向照样绿。
       xxhdpi 距槽位 124 最近 → 偏差 20 折算 1 分，基础分 39-1=38；
       i>=3 再 +1 = 39；文件名是 ic_launcher 再加 12 = 51。
       钉成具体值之后，任何一次权重/分档改动都会立刻红——
       而这正是要的效果：改这些是有意的行为，得有人确认。 */
    sf_check(sf_test_score_name("res/mipmap-xxhdpi-v4/ic_launcher.png") == 51,
             "xxhdpi + launcher 文件名 = 51（权重与分档都钉死，改前必须确认）", NULL);

    /* ── 真机回归：取到的是背景层，不是桌面图标 ─────────────────────
       自适应图标的包里 mipmap-xxhdpi/ 同时有 ic_launcher.png、
       ic_launcher_foreground.png、ic_launcher_background.png 三张。
       只按密度给分的话，三张同为最高分，再叠加"同分取字节小的"，
       于是**稳定选中背景层**（它通常是三者里最小的一张纯色图）。
       真机表现：列表里出现一个纯色方块，用户说"图标取错了"。
       现在按文件名分主次：launcher 加 12，前景/背景层减 20。 */
    {
      int s_launcher = sf_test_score_name("res/mipmap-xxhdpi/ic_launcher.png");
      int s_bg   = sf_test_score_name("res/mipmap-xxhdpi/ic_launcher_background.png");
      int s_fg   = sf_test_score_name("res/mipmap-xxhdpi/ic_launcher_foreground.png");
      int s_misc = sf_test_score_name("res/mipmap-xxhdpi/banner_promo.png");
      sf_check(s_launcher > s_bg && s_launcher > s_fg,
               "ic_launcher 必须赢过同目录的前景/背景层（这就是那个纯色方块的成因）", NULL);
      /* 整包只有分层素材时，背景层仍要能收：分数必须 > 0，
         否则退化到第二轮按尺寸瞎猜，反而更容易取到游戏内素材。 */
      sf_check(s_bg > 10 && s_fg > 10,
               "分层素材仍要收（只作备选，不能否掉——否掉就掉进第二轮瞎猜）", NULL);
      sf_check(s_launcher > s_misc,
               "mipmap 里的普通素材（横幅/宣传图）不能让 launcher 落选", NULL);
      /* 目录的版本限定符（-v4 / -v26）不该改变分数：
         真包里 res/mipmap-xxhdpi-v4/ 和 res/mipmap-xxhdpi/ 都常见，
         同一个图标因为目录后缀不同而分出高下是纯粹的噪声。 */
      sf_check(sf_test_score_name("res/mipmap-xxhdpi-v4/app_icon.png") ==
               sf_test_score_name("res/mipmap-xxhdpi/app_icon.png"),
               "目录的 -v4 后缀不改变分数（限定符不是判据）", NULL);
    }

    /* ── 真机抓来的条目表：王者荣耀 ─────────────────────────────
       它把桌面图标命名成 app_icon.png（不是 ic_launcher），
       同一个 mipmap-xxhdpi-v4/ 里还躺着一张 noticelogo.png。
       没有"按文件名分档"时两张同为最高分，
       再叠加"同分取字节小的"（noticelogo 23873 < app_icon 46217），
       于是稳定选中 noticelogo —— 用户看到的就是一枚徽标而不是英雄头像。
       这组用例用真名真字节数钉住这个场景。 */
    {
      int s_app   = sf_test_score_name("res/mipmap-xxhdpi-v4/app_icon.png");
      int s_notice = sf_test_score_name("res/mipmap-xxhdpi-v4/noticelogo.png");
      int s_loading = sf_test_score_name("res/mipmap-xxxhdpi-v4/broadcast_loading.png");
      int s_round  = sf_test_score_name("res/mipmap-xxxhdpi-v4/app_icon_round.png");
      sf_check(s_round == -1, "app_icon_round 仍按整词 round 排除", NULL);
      sf_check(s_app > s_notice,
               "app_icon 必须赢过同目录的 noticelogo（真机那个徽标就是这么来的）", NULL);
      sf_check(s_app > s_loading,
               "app_icon 必须赢过 mipmap 里的小素材（broadcast_loading 只有 537B）", NULL);
      /* "noticelogo" 里含 logo 三个字母，但不是独立单词，
         整词匹配不该给它加分——这条防的是把 has_word 退回 strstr。 */
      sf_check(s_notice < 40,
               "noticelogo 不得因 \"logo\" 子串加分（整词匹配才算）", NULL);
    }
  }

  /* ── 实时偏移的步进机 ─────────────────────────────────────────
     adapt_apply 是纯函数（喂数、不碰文件），所以这一组能在真机上直接跑。
     传感器读数本身另由 tools/sandbox_probe.sh 与手工负载验。 */
  {
    struct adapt_cfg c;
    memset(&c, 0, sizeof c);
    c.psi = 1; c.psi_gain = 10; c.psi_step = 5; c.psi_hold = 30; c.psi_back = 15;
    c.th = 1;  c.th_high = 75;  c.th_low = 68;  c.th_step = 5; c.th_max = 25; c.th_back = 20;
    c.chg = 1; c.chg_pct = 100;
    c.up_max = 15; c.down_max = 30;
    struct adapt a;

    /* 1) 第一拍只立基线，绝不因为"看起来很高"就借额度 */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 900, 50000, 0, 100, 5);
    sf_check(a.psi_steps == 0 && a.base == 900, "第一拍只立基线，不借档", NULL);

    /* 2) 静置噪声：基线很低时，绝对增量不够就不许借
          （这条防的是"只看倍数"—— 基线 5、当前 12 也是 2.4 倍） */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 5, 50000, 0, 100, 5);
    adapt_apply(&a, &c, 12, 50000, 0, 105, 5);
    sf_check(a.psi_steps == 0, "基线很低时 12ms/s 不算卡（绝对增量也要过）", NULL);

    /* 3) 真卡：明显高出基线才借，且一次只借一档 */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 60, 50000, 0, 100, 5);
    adapt_apply(&a, &c, 400, 50000, 0, 105, 5);
    sf_check(a.psi_steps == 1, "stall 高出基线才借一档", NULL);
    adapt_apply(&a, &c, 400, 50000, 0, 110, 5);
    adapt_apply(&a, &c, 400, 50000, 0, 115, 5);
    sf_check(a.psi_steps == 2, "一直卡才逐档加到上限", NULL);
    adapt_apply(&a, &c, 400, 50000, 0, 120, 5);
    sf_check(a.psi_steps <= 2 && a.delta <= c.psi_gain,
             "借出总量不超过 PSI_GAIN", NULL);

    /* 4) 不卡了要收回，而且一次只收一档、按时间间隔收。
          注意 hold 会被持续的压力续期：上面 t=120 那一拍仍在卡，
          lend_until 已经推到 150，所以 130/149 两拍都还不该抽档。 */
    adapt_apply(&a, &c, 20, 50000, 0, 130, 5);
    sf_check(a.psi_steps == 2, "刚不卡的头一档不立刻抽走（还在 hold 里）", NULL);
    adapt_apply(&a, &c, 20, 50000, 0, 149, 5);
    sf_check(a.psi_steps == 2, "hold 没到点，一档都不抽", NULL);
    adapt_apply(&a, &c, 20, 50000, 0, 155, 5);
    sf_check(a.psi_steps == 1, "过了 hold 才收第一档", NULL);
    adapt_apply(&a, &c, 20, 50000, 0, 162, 5);
    sf_check(a.psi_steps == 1, "两档之间要隔够 PSI_BACK，不许一口气抽干", NULL);
    adapt_apply(&a, &c, 20, 50000, 0, 171, 5);
    sf_check(a.psi_steps == 0, "再等一轮收干净", NULL);

    /* 5) 温控：到线逐档压，迟滞带里不动，回带才还 */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 20, 74900 /* 74.9℃ */, 0, 200, 5);
    sf_check(a.th_steps == 0, "差一度到触发线就不压（阈值不许漂）", NULL);
    adapt_apply(&a, &c, 20, 75100, 0, 205, 5);
    sf_check(a.th_steps == 1 && a.delta == -5, "超线压一档 = -5pp", NULL);
    adapt_apply(&a, &c, 20, 71000, 0, 210, 5);
    sf_check(a.th_steps == 1, "迟滞带里（68~75℃）保持不动，不来回抖", NULL);
    adapt_apply(&a, &c, 20, 67900, 0, 215, 5);
    sf_check(a.th_steps == 1, "刚回到恢复线，先等 TH_BACK", NULL);
    adapt_apply(&a, &c, 20, 67900, 0, 236, 5);
    sf_check(a.th_steps == 0, "凉够了才逐档还回去", NULL);
    for (int i = 0; i < 40; i++) adapt_apply(&a, &c, 20, 95000, 0, 300 + i * 5, 5);
    sf_check(a.th_steps * c.th_step <= c.th_max,
             "压档总量不超过 TH_MAX（不会一路压到 1%）", NULL);

    /* 6) PSI 借 + 温控压 同时发生：合计是两者相加，谁也不盖谁 */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 60, 60000, 0, 400, 5);
    adapt_apply(&a, &c, 400, 80000, 0, 405, 5);
    sf_check(a.psi_steps == 1 && a.th_steps == 1 && a.delta == 0,
             "借一档 + 压一档 = 0（两条独立记账）", NULL);

    /* 7) 充电态：插电才算，拔电立刻掉 */
    memset(&a, 0, sizeof a);
    adapt_apply(&a, &c, 20, 50000, 1, 50000, 5);
    sf_check(a.chg_on == 1 && a.chg_pct == 100, "插电时地板抬到 CHG_PCT", NULL);
    adapt_apply(&a, &c, 20, 50000, 0, 505, 5);
    sf_check(a.chg_on == 0 && a.chg_pct == 0, "拔电立刻回到设定值", NULL);
    c.chg = 0;
    adapt_apply(&a, &c, 20, 50000, 1, 510, 5);
    sf_check(a.chg_on == 0, "关掉充电态后插电也不抬（开关真的管用）", NULL);
    c.chg = 1;

    /* 8) 交权态不更新 PSI 基线：游戏/满血/熄屏那段负载高又没有帽，
          让它进基线会把"卡"的门槛抬高，回到日常后护栏就钝了。 */
    memset(&a, 0, sizeof a);
    a.base = 50; a.now = 600;
    adapt_apply_gated(&a, &c, 500, 50000, 0, 600, 5, 0);
    sf_check(a.base == 50, "allow_base=0：游戏期的高 stall 不进基线", NULL);
    adapt_apply_gated(&a, &c, 500, 50000, 0, 605, 5, 1);
    sf_check(a.base > 50, "回到常态后基线继续平滑", NULL);
    memset(&a, 0, sizeof a);
    adapt_apply_gated(&a, &c, 500, 50000, 0, 610, 5, 0);
    sf_check(a.base == 0 && a.psi_steps == 0,
             "开机就在交权态：没有基线就不判，也不借档", NULL);

    /* 9) 合计偏移的总闸：单项都合法、叠起来也不能过狠 */
    memset(&a, 0, sizeof a);
    a.psi_steps = 100;
    adapt_apply_gated(&a, &c, 0, 50000, 0, 700, 5, 1);
    sf_check(a.delta == c.up_max,
             "合计放宽不超过 ADAPT_UP_MAX", NULL);
    memset(&a, 0, sizeof a);
    a.th_steps = 100;
    adapt_apply_gated(&a, &c, 0, 70000, 0, 705, 5, 1);
    sf_check(a.delta == -c.down_max,
             "合计收紧不超过 ADAPT_DOWN_MAX", NULL);
  }

  /* 逐核频率历史：占用百分比、右进左移、8 格滚动、范围。 */
  {
    sf_check(core_load_pct(1000, 400, 900, 350) == 50,
             "占用按 /proc/stat 的 total/idle 差值算", NULL);
    sf_check(core_load_pct(1000, 400, 1000, 400) == 0 &&
             core_load_pct(1000, 400, 1100, 400) == 0,
             "没有增量或计数器回退时不报假占用", NULL);
    struct core_hist h;
    memset(&h, 0, sizeof h);
    long lo = 0, hi = 0;
    core_hist_push(&h, 100, 10);
    sf_check(h.n == CORE_HIST_N && h.freq[0] == 100 &&
             h.freq[CORE_HIST_N - 1] == 100 && h.load[0] == 10,
             "首拍铺满 8 格，页面不会只有一根柱", NULL);
    core_hist_push(&h, 200, 20);
    sf_check(h.freq[CORE_HIST_N - 1] == 200 &&
             h.freq[CORE_HIST_N - 2] == 100 && h.load[CORE_HIST_N - 1] == 20,
             "新样本放最右、旧样本左移", NULL);
    core_hist_range(&h, &lo, &hi);
    sf_check(lo == 100 && hi == 200, "范围取历史样本的 min/max", NULL);
    for (int k = 3; k <= 9; k++) core_hist_push(&h, 100 * k, 10 * k);
    sf_check(h.freq[CORE_HIST_N - 1] == 900 && h.load[CORE_HIST_N - 1] == 90,
             "连续刷新后最右始终是最新样本", NULL);
    core_hist_range(&h, &lo, &hi);
    sf_check(lo == 200 && hi == 900, "8 格滚动后最旧样本被挤掉", NULL);
  }

  /* 大中央目录：名字/图标读取必须能走完"重读中央目录"这条路而不是崩溃。
     不要求读出内容——测试 zip 里没有 manifest，读不到名字才是正常结果。 */
  {
    char zp[128];
    snprintf(zp, sizeof zp, "/data/local/tmp/sf_bigcd_%d.zip", (int)getpid());
    if (sf_make_bigcd_zip(zp)) {
      char lab[128] = "x";
      int r = appinfo_label(zp, lab, sizeof lab);
      sf_check(r == 0 && lab[0] == 0,
               "大中央目录 APK：读不到名字时正常返回，不段错误", NULL);
      int se = dup(2), dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) dup2(dn, 2);
      int ir = icon_from_apk(zp, NULL);
      if (dn >= 0) { dup2(se, 2); close(dn); }
      close(se);
      sf_check(ir == 0, "大中央目录 APK：图标取不到时正常返回，不段错误", NULL);
      unlink(zp);
    } else {
      sf_check(0, "能造出大中央目录测试包", "fopen 失败");
    }
  }

  printf("\n%s\n", sf_fails ? "有失败项" : "全部通过");
  printf("通过 %d，失败 %d\n", sf_passes, sf_fails);
  return sf_fails ? 1 : 0;
}

#ifdef SF_TEST
int main(void) { return sf_test_main(); }
#endif