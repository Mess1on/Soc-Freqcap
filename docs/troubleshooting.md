# 排查与恢复

（返回 [README](../README.md)）

## 出问题能自己恢复

每次写 sysfs 之前，先把"原来的值是多少、原来什么权限"记进 journal。
正常退出时按 journal 还原；被强杀（kill -9）来不及还原也没关系，
下次启动会先按 journal 还原再取新的基准值。

所以强杀守护不会让手机一直卡在低频上。

### 装不上：`Installation Blocked` / `A metamodule with custom installer is active`

红框里写着 `Current state: Disabled`，`Action required: Re-enable or uninstall it, then reboot`。
**这跟本模块无关**，是管理器（KernelSU / Magisk 等）自己的闸门：

你在设备上装了一个声明了 `metamodule=1` 的模块（最常见是 **Hybrid Mount**），
又把它**停用**了。管理器于是拒绝安装任何新模块，直到它被重新启用或卸载。
拦截发生在解包之前，所以换哪个 zip 都一样，模块自己的安装脚本根本没机会执行。

查一下是谁：

```sh
su -c 'grep -l "^metamodule=" /data/adb/modules/*/module.prop | while read f; do d=${f%/module.prop}; echo "${d##*/}  disabled=$([ -f "$d/disable" ] && echo yes || echo no)"; done'
```

**修复（二选一）：**

```sh
# 1) 重新启用那个 metamodule，然后重启，再去装本模块
su -c 'rm -f /data/adb/modules/hybrid_mount/disable' && reboot

# 2) 或者干脆卸掉它（管理器里点卸载 / 或删目录），重启后再装
```

模块里带了同一套逻辑的脚本，装好之后也能用：

```sh
su -c 'sh /data/adb/modules/soc_freqcap/tools/fix_metamodule.sh'          # 只检查
su -c 'sh /data/adb/modules/soc_freqcap/tools/fix_metamodule.sh --fix'    # 重新启用，然后重启
```

它只删一个 `disable` 标记文件，不改系统分区、不碰任何配置。

### 打开 WebUI 卡死 / 手机自己重启 / 重启后没 root

这三件事经常被连在一起报，但成因完全不同，先别混着猜。

**先取证（只读，什么都不改）：**

```sh
su -c 'sh /data/adb/modules/soc_freqcap/tools/collect_crash.sh'
```

它会把型号、`ro.boot.bootreason`、内核里有没有 `panic/watchdog/hung task`、
pstore（上次崩溃的内核日志，有就是铁证）、全部模块列表（标出被停用的和
`metamodule`）、本模块日志和频率节点现状写成一个 txt。把这个文件发出来，
比描述十句都准。

**几个判据：**

- `ro.boot.bootreason` 是 `reboot,` / `shutdown,` = 正常重启（可能是长按电源或手动重启）；
  带 `watchdog` / `panic` / `hard_reset` 才是系统自己挂掉。
- 重启后"没 root"：先确认是不是**掉到了没装 KernelSU 的槽位/内核**，
  以及 `/data/adb/modules` 里有没有被停用（`disable`）或标记卸载（`remove`）的模块。
  装了 `metamodule=1` 的模块（常见 Hybrid Mount）被停用时，管理器还会**拒绝安装任何模块**，
  见上一节。
- 打开 WebUI 卡死：页面每个命令都有 12 秒超时，正常不会无限转圈。
  如果整机都卡住（不只是页面），那更像内核/系统服务层面的问题，按上面取证。

**怀疑是本模块时，先把它停掉再观察，不用卸载：**

```sh
su -c 'sh /data/adb/modules/soc_freqcap/tools/safemode.sh on' && reboot   # 进安全模式
su -c 'sh /data/adb/modules/soc_freqcap/tools/safemode.sh off' && reboot  # 退出
```

安全模式下守护完全不启动：不改频率、不冻结、不碰 Doze，模块还在，随时能退出来。

**本版起不再使用 SIGSTOP 兜底**：拿不到 cgroup 冻结就不冻。SIGSTOP 会把进程硬按在原地，
一旦按到系统正在同步调用的进程，system_server 卡在 binder 上，watchdog 60 秒后整机重启
—— 这是"突然强制重启"最像的一条路径。

### 卸载后频率没回原厂 / 出现“限制性能的组件”弹窗

模块只写 CPU/GPU 的 `scaling_max_freq` 等节点，**重启后内核会把这些节点重置**。
如果重启后频率仍然偏低，先确认模块真的不在了：

```sh
ls -d /data/adb/modules/soc_freqcap /data/adb/ap/modules/soc_freqcap \
      /data/adb/modules_update/soc_freqcap /data/adb/ap/modules_update/soc_freqcap 2>/dev/null
pidof socfreqd
ps -A | grep -E 'deep_sleep|service.sh'
```

再看当前上限和硬件最高：

```sh
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "$p max=$(cat $p/scaling_max_freq) hw=$(cat $p/cpuinfo_max_freq)"
done
```

- 两个根（KernelSU/Magisk 的 `/data/adb/modules` 与 APatch 的 `/data/adb/ap/modules`）
  以及各自的 `modules_update` 都没有 `soc_freqcap`、也没有进程 → 本模块已经清干净；
  此时上限仍低，是**别的性能工具 / 厂商省电 / 游戏助手**在写节点或禁用了性能组件，
  不是本模块的残留。
- **看的是 `scaling_max_freq`，不是 `scaling_cur_freq`**：后者是调度器当前跑多快，
  空闲时低于上限是正常的。只有 `scaling_max_freq` 低于 `cpuinfo_max_freq` 才叫"被压着"。
- **另一个管理器根下还有一份**（从 KernelSU 换到 APatch 后只卸了一边很常见）：
  那份的 `service.sh` 会照常开机启动并继续限频。要在**对应管理器**里再卸载一次。
- **`modules_update/soc_freqcap` 还在**（KernelSU 和 APatch 各有一处）：这是一次
  没应用完的更新，重启后会被装回去。在管理器里再卸载一次，或删掉这个待更新目录后再重启。
  这就是"卸载了又自己回来"的常见原因。
- 新版卸载脚本会先停看门狗（`service.sh` 子 shell）、守护和深睡，再还原三本账，
  还原完再杀一遍防复活；两处 `modules_update` 都会清。若仍有节点写不进去，
  账本会备份到 `/data/local/tmp/soc_freqcap_restore`，不会随数据目录一起删掉。
- **Scene 的弹窗**（“检测到系统中可能限制性能的组件”）是 Scene 看到有人在改 CPU/性能节点；
  Scene 自己也有 CPU 频率 / 开机情景模式。检查 Scene 的「开机应用 / 情景模式 / 性能模式」
  是否设了上限，并看它弹窗里要禁用的那个“组件”到底是系统组件还是 `soc_freqcap`。
  本模块没有任何对话框，也不会在卸载后保留进程。
- 临时把上限放回硬件最高（重启也会重置，先应急用）：
  ```sh
  for p in /sys/devices/system/cpu/cpufreq/policy*; do
    cat $p/cpuinfo_max_freq > $p/scaling_max_freq 2>/dev/null
  done
  dumpsys deviceidle unforce
  settings delete global device_idle_constants
  ```
- **截图里“检测到系统中可能限制性能的组件，是否将其禁用(重启手机后还原)?”不是本模块的 UI**：
  本模块没有任何对话框。点「取消」后看它属于哪个 App（常见是 Scene、爱玩机、游戏助手一类的性能工具），
  把那个 App 的名字/版本发过来才能继续定位。
- 本模块的卸载流程：停守护 → 按 journal 还原 → 没有 journal 时把 CPU 上限放回硬件最高 →
  停深睡并解冻、撤临时白名单、退出强制空闲 → 删数据目录。卸载脚本在 `uninstall.sh`，可直接读。

### 荣耀 / 华为 / 三星：装完约一分钟、或打开 WebUI 时硬重启，之后 root 消失

V2.1 的出厂配置把 `CPU_GOVERNOR` 写成了 `conservative`。部分荣耀 / 华为 / 三星内核
不支持切换通用调速器，切过去会直接 kernel panic：装完约一分钟（开机满血窗口
60 秒结束、守护第一次写调速器）硬重启；反复几次后管理器会把模块判成坏模块，
看起来就像"root 没了"。

新版已经改了三处：

1. 出厂默认改成**不干预**（`CPU_GOVERNOR=` 留空），不再写调速器；
2. 升级安装时会把存量配置里由旧默认写入的 `CPU_GOVERNOR=conservative` 清成空；
3. 守护识别到荣耀 / 华为 / 三星机型时**一律不改写调速器**，即使配置里还留着这一行。

如果现在还在重启循环里，按顺序试：

```sh
# 1) 先让守护别再启动（能进系统/能 adb 时）
su -c 'touch /data/adb/soc_freqcap/safemode' && reboot

# 2) 装上新版后删掉安全模式标记，再重启
su -c 'rm -f /data/adb/soc_freqcap/safemode' && reboot
```

进不了系统时用管理器的安全模式（开机时禁用所有模块），进系统后
`/data/adb/soc_freqcap/safemode` 一样有效。也可以手工清掉那一行：

```sh
su -c 'c=/data/adb/soc_freqcap/socfreq.conf; sed "s/^CPU_GOVERNOR=.*/CPU_GOVERNOR=/" $c > $c.new && mv -f $c.new $c'
```

## 关于管理器里看到的输出

三个坑都在安装/执行脚本里，都表现成"看起来是管理器的问题"：

**「执行」按钮点下去一片空白。** `ui_print` 是管理器提供的函数，装机时有，
但点「执行」的运行环境里不一定有。脚本里每一行都调它 →
每一行都变成 `ui_print: command not found` 打到 stderr → 界面整页空白。
两个脚本开头都做了兜底（`command -v ui_print` 不通过就用 `echo` 顶上）。

**输出前面多一个 `v` 或 `x`。** 部分管理器会自己给 `ui_print` 的输出加一套
行首标记，脚本里再加一遍就成了 `v 未检测到冲突`。
所以脚本自己**不加**这类前缀。

**刷机日志最上方出现方框/竖线。** 标题夹在横杠中间（`━━ 标题 ━━`）时，
部分管理器会把横杠两端的文字当行首标记解析。现在标题独占一行，
所有分隔线宽度统一 20 字。

**关于页一直弹「about 输出里没有 ---PROP--- 标记」。** 这是 WebUI 与
`api.sh` 版本混装，或个别 ROM 的 `sh` 把 `printf -- '---PROP---'` 的 `--`
当成了格式串（只吐出两个减号）。V1.4 已把标记改成 `printf '%s\n'`，
前端在标记丢失时也会从 `module.prop` 的 `id=/version=` 行恢复；
更新时请刷完整 zip（或同时热推 `webroot/index.html` 与 `webroot/api.sh`），
不要只换其中一个。

## 已知的边界

- 和其他改频率/调度器的模块（Scene、Scene5 等）不要同时用，
  两个都想当"节流阀"会互相打架。安装时会检测并提示，但不阻止。
- 部分新 SoC 的 GPU 没有暴露频率表节点，这时只限 CPU，日志里会告警。
- 读写范围只有两处：`/data/adb/soc_freqcap` 这一份数据目录，和自己封顶的那几个
  cpufreq / devfreq 节点；不需要网络权限，也不碰别人的文件。
- 图标：manifest 里那条声明路径读不出来的包（自适应图标只指向矢量 drawable、
  或资源名被缩短到认不出），那一行退回首字母色块 —— 这是设计上的降级，不是失败。
  那一档的建议会直接写成"被厂商压着，先不给建议"。这台机只有两档时就只有两行。
- **第三方耗电统计把游戏时段记成"待机"**：这类应用靠自己的常驻采样进程算账。进程在游戏时
  被 ColorOS `o-kill`（`dumpsys activity exit-info` 里是 `reason=13` 的 `o-kill(4013)`）后，
  采样会断几十分钟；恢复时它把断档按"待机"结算，断档期间掉的电量就全成了"待机功耗"。
  系统侧 `dumpsys batterystats --history` 同期仍是 `+screen`，本模块的深睡日志也没有
  "已进入"（亮屏不压 Doze）。把统计应用在最近任务里锁定、允许后台活动，能减少被系统回收；
  这段账不是模块能替它补的。
