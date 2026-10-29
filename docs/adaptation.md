# 芯片适配包

（返回 [README](../README.md)）

## 芯片适配包（自动探测扑空的机器）

有些机器的节点名和目录结构跟常规不一样：一个 `policy*` 目录都读不到，
或者找不到可写的 GPU 上限节点。这种机器装上就表现为
「看不到核心表」「GPU 那一档是灰的」——不是配置错了，是这台机的 sysfs 长得不在预期的那几条路径上。
只能人工适配，而适配得先知道他手上那台机到底有什么。**用户侧只有一个动作：导出**。
改好的节点表由作者写进下一个模块版本，他到时候更新模块就生效，不用自己动任何文件。

**导出**（关于页「导出适配包」，或 `sh webroot/api.sh pkg`）把现场打成一个 txt，落在

```
/storage/emulated/0/Download/Soc智能限频/socpkg-<品牌>-<型号>-<芯片>-<时间>.txt
```

选公共 Download 是因为 QQ / 微信 的发送框能直接翻到它，不用装 root 文件管理器。
写不进去回执是 `E/PKG_DIR`（目录建不出来）或 `E/PKG_WRITE`（文件是空的）。

包里有机种属性、这台机上**当前能用的节点表**（就是下面那几行 `#ovr`）、
守护自己的 `probe` 和 `export` 原样、逐个 policy 的频率表与调速器、GPU 节点普查
（把 KGSL / devfreq / mali 几条常见形态挨个点一遍，在的连值一起列出来）、
守护要用的那几个接口（`dumpsys` / `ps` / `packages.list`）读不读得到、
当前配置全文、当前建议结论、最近 40 行日志，最后一段是**这份包缺什么**的自动结论
（policy 目录数、几个 policy 没有驻留表、有没有可写的 GPU 上限节点）——作者先看这段就知道要补什么。

`#ovr` 只有四种键，和数据目录里那份 `override.conf` 的行一一对应：

```
#ovr cpu-dir <policy 目录> <这一档的硬件最高频>
#ovr gpu-max <可写的上限节点>
#ovr gpu-min <对应的下限节点>
#ovr gpu-freqs <空格分隔的频点表>
```

守护侧的生效顺序是**兜底，不是覆盖**：自动探测拿到东西时一个字都不看这份文件；
`detect_policies_raw()` 一条都没返回才按 `cpu-dir` 逐条造行（目录此刻已不存在的直接跳过），
GPU 的频率表和上限节点也是各自探不到才回落到 `gpu-freqs` / `gpu-max`。
覆盖只在守护启动时读一次，所以适配表要重启后才生效。

**这张表怎么发到用户机上**：作者把改好的表存成包里的 `adapt/<机型>.conf`，装机脚本按 `#match` 认机器，
命中才写进数据目录当 `override.conf`，并在第一行留下 `#from adapt/<文件名>` 标记。用户只要更新模块。

```
adapt/PJZ110.conf:
  #match brand=OnePlus        # 三种键都要命中才算命中
  #match model=PJZ110
  #match soc=SM8750           # 比对 ro.soc.model，取不到时退回 ro.board.platform，按包含匹配
  cpu-dir /sys/devices/system/cpu/cpufreq/policy0 3532800
  gpu-max /sys/class/devfreq/1a00000.qcom,kgsl-3d0/max_freq
```

三条规矩都有台架盯着（`src/test_install_merge.sh` 场景 8）：

- **没有 `#match` 的文件一律不装** —— 没条件就等于对所有机器适用，太危险。
- **只带进那四种键开头的行**：作者把 `cpu-dir` 打成 `CPU-DIR` 时不会被静默装上。
- **撤表只撤带 `#from adapt/` 标记的那份**：某台机器的适配下架后，用户升级就回到自动探测；
  而用户（或作者远程指导时）用命令行自己导进去的表没有这个标记，装机不会替他删。

包里的 `adapt/_TEMPLATE.conf` 是格式说明，`#match model=__NO_SUCH_MODEL__` 命中不了任何机器，
节点行也全是注释状态 —— 它存在的意义就是让那个目录随包发布。

从收到的包里取现成的一行不用手抄：

```sh
grep '^#ovr ' 收到的包.txt | sed 's/^#ovr //'   # 直接就是 adapt 文件正文那四种键
```

导出端的 GPU 候选现在按**名字白名单**（mali/kgsl/gpufreq/gpu/3d0）挑，并排掉
ufshci / dvfsrc / busmon / bwmon / memlat。以前通配 `/sys/class/devfreq/*/max_freq`
会把 UFS 控制器（如荣耀 MT6993 的 `16810000.ufshci`）写成 `gpu-max`，导入后限的是闪存，
页面还显示"GPU 已接上"。没有 GPU devfreq 的 MTK 新平台现在由守护直接走 gpufreqv2：
读 `stack_working_opp_table` 当频点表、写 `limit_table` 的 ceiling；包里的「GPU 节点普查」
仍会带上 `limit_table`、`stack_working_opp_table`、GED 参数等现场，方便核对不同内核。
不要拿 ufshci 的频点当 GPU 频点表。

**为什么页面上没有"导入适配包"这个按钮**：那份表是给某一台机子手搓的，
让用户自己点一下就能装进机器，等于给他一个把手机钉死在别人节点表上的入口——
导错一次，他的机器此后就一直按着那台机子的拓扑跑，而他自己看不出所以然。
所以导入和撤回只留在命令行（作者在自己机器上验一份刚改好的表要用）：

```sh
sh webroot/api.sh pkg_import [文件]   # 不传路径取 Download 那个目录里最新的一份
sh webroot/api.sh ovr_clear           # 删掉 override.conf 并重启，回到全自动探测
```

导入端先验首行是 `#SOCPKG 1`，再把 `#ovr` 行逐条校验：键不认、路径不是绝对路径、
频率不是纯数字，整份拒绝（`E/PKG_OVR`），不会留下半截没人知道缺了什么的覆盖；
包里一条 `#ovr` 都没有是 `E/PKG_NOOVR`（那份包什么也没适配）。
回执会打印**用的是哪个文件**、芯片名，再逐条对照"这个节点在这台机上到底存不存在"——
「导入成功」但路径全是别人机器上的，等于什么都没适配，这一段就是为了让这种情况当场露出来。

关于页那张卡片是**只读**的：它显示这台机当前的探测结果，
以及在用哪一份适配表（没在用时明说"全靠自动探测"），用户看得到自己的机被适配成了什么样，但改不了。

探测正常的机器整节都用不到，放着不用就行。

发之前自己看一眼：包里有配置全文和最近 40 行日志，日志里可能出现你加进名单的应用包名。

**改完一份表，先在真机上验一遍兜底路径**：`tools/sandbox_probe.sh`（要 root）
在 `/data/local/tmp/fakeroot` 造一台"假机器"——有 `cpu0/cpufreq`、`cpu4/cpufreq` 两个逐核目录，
但没有 `/sys/devices/system/cpu/cpufreq/policy*`，所以自动探测必然扑空，正是适配包要救的那种机器。
它把沙箱里的守护真跑 8 秒，验四件事：没表时探测不到、有表时两个簇和 GPU 都按表列出来
（名字取的是能分清簇的那一段，不是一排重名的 `cpufreq`）、封顶真写进了节点（50% 的 3532800 落成 1766400）、
退出时按 journal 恢复。全程只在沙箱里读写，碰不到真 sysfs，跑完自己清掉：

```sh
adb push tools/sandbox_probe.sh /data/local/tmp/
adb shell su -c 'sh /data/local/tmp/sandbox_probe.sh'   # 末行报 N/N 通过
```
