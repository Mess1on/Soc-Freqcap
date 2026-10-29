# 排查与恢复

（返回 [README](../README.md)）

## 出问题能自己恢复

每次写 sysfs 之前，先把"原来的值是多少、原来什么权限"记进 journal。
正常退出时按 journal 还原；被强杀（kill -9）来不及还原也没关系，
下次启动会先按 journal 还原再取新的基准值。

所以强杀守护不会让手机一直卡在低频上。

### 卸载后频率没回原厂 / 出现“限制性能的组件”弹窗

模块只写 CPU/GPU 的 `scaling_max_freq` 等节点，**重启后内核会把这些节点重置**。
如果重启后频率仍然偏低，先确认模块真的不在了：

```sh
ls /data/adb/modules /data/adb/modules_update 2>/dev/null | grep soc_freqcap
pidof socfreqd
ps -A | grep deep_sleep
```

再看当前上限和硬件最高：

```sh
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo "$p max=$(cat $p/scaling_max_freq) hw=$(cat $p/cpuinfo_max_freq)"
done
```

- 两个目录都没有 `soc_freqcap`、也没有进程 → 本模块已经清干净；此时上限仍低，是**别的性能工具 /
  厂商省电 / 游戏助手**在写节点或禁用了性能组件，不是本模块的残留。
- **`/data/adb/modules_update/soc_freqcap` 还在**：这是一次没应用完的更新，重启后会被装回去。
  在管理器里再卸载一次，或删掉这个待更新目录后再重启。这就是"卸载了又自己回来"的常见原因。
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
- 智能学习只在封顶真的生效时累计：被厂商热限频压着的那一段不算你的帽，
  那一档的建议会直接写成"被厂商压着，先不给建议"。这台机只有两档时就只有两行。
- **第三方耗电统计把游戏时段记成"待机"**：这类应用靠自己的常驻采样进程算账。进程在游戏时
  被 ColorOS `o-kill`（`dumpsys activity exit-info` 里是 `reason=13` 的 `o-kill(4013)`）后，
  采样会断几十分钟；恢复时它把断档按"待机"结算，断档期间掉的电量就全成了"待机功耗"。
  系统侧 `dumpsys batterystats --history` 同期仍是 `+screen`，本模块的深睡日志也没有
  "已进入"（亮屏不压 Doze）。把统计应用在最近任务里锁定、允许后台活动，能减少被系统回收；
  这段账不是模块能替它补的。
