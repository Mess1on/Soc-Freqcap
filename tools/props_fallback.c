/* 兜底实现：没有 NDK/bionic 时（musl 静态编译）用 getprop 命令行取属性。
 *
 * 只在本地尽量减少依赖的构建里用，不进模块 zip。
 * 不走 /bin/sh（Android 上不保证存在），直接 fork + execv getprop；
 * ro.* 缓存到进程退出，sys.* 只缓存 3 秒（boot_completed 会变）。
 * 写入上限按 bionic 的 PROP_VALUE_MAX=92 截断，调用方缓冲区都比它大。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_PROP 92
#define CACHE_N 8

static struct {
  char k[64];
  char v[MAX_PROP];
  long at;
} g_cache[CACHE_N];
static int g_cache_n;
static unsigned g_cache_rr;

static long mono_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec;
}

static int run_getprop(const char *name, char *out, size_t n) {
  int fd[2];
  if (pipe(fd)) return -1;
  pid_t p = fork();
  if (p < 0) { close(fd[0]); close(fd[1]); return -1; }
  if (p == 0) {
    dup2(fd[1], STDOUT_FILENO);
    close(fd[0]);
    close(fd[1]);
    execl("/system/bin/getprop", "getprop", name, (char *)NULL);
    execl("/system/xbin/getprop", "getprop", name, (char *)NULL);
    _exit(127);
  }
  close(fd[1]);
  char buf[MAX_PROP * 2];
  ssize_t got = read(fd[0], buf, sizeof buf - 1);
  close(fd[0]);
  int st = 0;
  waitpid(p, &st, 0);
  if (got <= 0) return -1;
  buf[got] = 0;
  char *nl = strchr(buf, '\n');
  if (nl) *nl = 0;
  snprintf(out, n, "%s", buf);
  return 0;
}

int __system_property_get(const char *name, char *value) {
  if (!name || !value) return 0;
  value[0] = 0;
  if (name[0] == '\0' || strlen(name) >= sizeof g_cache[0].k) return 0;

  int ro = (name[0] == 'r' && name[1] == 'o' && name[2] == '.');
  long ttl = ro ? 3600 : 3;
  long now = mono_s();
  for (int i = 0; i < g_cache_n; i++)
    if (!strcmp(g_cache[i].k, name) && now - g_cache[i].at < ttl) {
      snprintf(value, MAX_PROP, "%s", g_cache[i].v);
      return (int)strlen(value);
    }

  char v[MAX_PROP];
  if (run_getprop(name, v, sizeof v)) return 0;

  int slot;
  if (g_cache_n < CACHE_N) slot = g_cache_n++;
  else { slot = (int)(g_cache_rr++ % CACHE_N); }
  snprintf(g_cache[slot].k, sizeof g_cache[slot].k, "%s", name);
  snprintf(g_cache[slot].v, sizeof g_cache[slot].v, "%s", v);
  g_cache[slot].at = now;

  snprintf(value, MAX_PROP, "%s", v);
  return (int)strlen(value);
}
