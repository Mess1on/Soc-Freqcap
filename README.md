# Soc智能限频

Android 的 CPU / GPU 频率封顶模块：平时按百分比限频，进游戏自动交还系统；附带 WebUI 与深度睡眠。

作者：梦笙Mess1on & Ktwo　　版本：V2.5 Beta1　　适用：KernelSU / Magisk / APatch · arm64-v8a

## 核心特性

| 特性 | 说明 |
|---|---|
| 按簇自动分档 | 读每个 `policy*` 的频率表自动分档，不写死 CPU 编号；封顶值向下吸附到频点，绝不超过设定百分比 |
| 芯片预设 | 全新安装默认「均衡」档；CPU/GPU 封顶页可一键套「省电 / 均衡」，覆盖 14 套骁龙/天玑落档参数；点预设会同时打开动态封顶并切到对应风格 |
| 动态封顶 | 仅限频模式；页面一个开关；省电风格持续低载后把静置天花板收到 960MHz（按真实频点向上取，不低过硬件最低），下限放回硬件最低、hispeed 置 0；负载统计排除 CPU0；普通/智能模式的百分比不会超过厂商原始上限 |
| 超大核离线 | 常态直接关掉最高频簇；游戏、满血应用、开机满血窗口先拉回在线（最多 2.5 秒）再交还，熄屏保持离线；开机完成前不执行，退出/停用会按账本还原 |
| 应用分类 | 视频 / 社交 / 阅读自动分类，可手动调整；默认按本机芯片的三场景参数落档，每类可按 BIG / MID2 / MID / LITTLE 分别设 CPU 上限，GPU 0 = 跟随当前封顶百分比；分类按场景覆盖当前封顶，滑条只改草稿，点保存并确认后才写入 |
| 智能调度 | 首页可在「限频 / 智能」间切换；智能按六态动态给目标，PSI/皮肤温/ADPF 只作辅助输入；页面可逐状态调 BIG/其他 CPU 上限、下限和 GPU，阈值/窗口/驻留可调；`CPU_PCT_*` 只当天花板 |
| 游戏与分应用放开 | `games.txt` 认包名，进游戏解除封顶并把节点权限交还系统（只撤模块自己的帽，厂商已压低的游戏/热控上限保留）；`app_modes.conf` 支持 `full` / `keep` / 单独数值 |
| GPU 多后端 | 高通 KGSL、MTK Mali devfreq、天玑 `gpufreqv2 limit_table`，以及老 MTK 的 `gpufreq_opp_dump` + GED `custom_upbound_gpu_freq` |
| WebUI + 深度睡眠 | 页面调参 / 名单 / 建议；熄屏后 Doze 收口（force-idle + 白名单），可配豁免与唤醒榜 |
| 参数导入 / 导出 | 维护页一键导出 `socfreq.conf`，粘贴导入；逐项校验、原子替换，失败不留半份配置 |

## 安装

需要 root（KernelSU / Magisk / APatch）。CPU / GPU 封顶不挑 Android 版本；深睡的 60 秒收口依赖 Android 12+ 的 DeviceIdleController shell 接口。

| 管理器 | 安装 |
|---|---|
| KernelSU / SukiSU Ultra | 模块页「从本地安装」选 zip；SukiSU ≥ v4.2.1-r2 可热安装。KernelSU 稳定版 3.3.1 尚未包含热安装，需重启或等后续正式版 |
| Magisk | 模块页「从本地安装」选 zip，重启生效 |
| APatch | 模块页安装到 `/data/adb/ap/modules`，重启生效 |

升级不会覆盖 `/data/adb/soc_freqcap` 里的配置和名单。WebUI 需要管理器注入 root 桥：KernelSU / APatch 已验证；Magisk 的 WebUI 很可能不可用，命令行仍可用。

> **装不上、提示 `Installation Blocked / A metamodule with custom installer is active`？**
> 这不是本模块的问题：你装过声明 `metamodule=1` 的模块（常见是 Hybrid Mount）又把它停用了，
> 管理器会拒绝安装任何新模块。重新启用或卸载它、重启后再装即可。
> 详细步骤见 [docs/troubleshooting.md](docs/troubleshooting.md)（「装不上：Installation Blocked」一节），
> 模块内也带了修复脚本 `tools/fix_metamodule.sh`。

## 更新

管理器（Magisk / KernelSU / APatch）通过 `module.prop` 里的 `updateJson`
读 [update.json](update.json)，线上 `versionCode` 更大时在模块卡片标出新版。

模块**只负责报版本号，不下载、不安装**——新版统一放网盘，地址写在
`update.json` 的 `zipUrl`，并同步置顶在 [CHANGELOG.md](CHANGELOG.md)。

当前下载地址：[https://pan.quark.cn/s/9efff9bed2cf](https://pan.quark.cn/s/9efff9bed2cf)。
点管理器里的「更新」会下载失败，这是预期行为：GitHub 只放版本号，安装包在夸克网盘，
复制上面的地址下载 zip 后再手动安装。

历史版本安装包归档在 [releases/](releases/)。发版流程见
[docs/cli.md](docs/cli.md) 的「版本检查」一节。

## 最小运行示例

```sh
D=/data/adb/modules/soc_freqcap      # APatch: /data/adb/ap/modules/soc_freqcap
$D/socfreqd status                 # 守护在不在、当前档位
$D/socfreqd probe                  # 会封到多少（只读，不改任何东西）
```

WebUI：管理器模块页 → 打开 WebUI。首页徽标显示「常态限频中」或「游戏中 · 已解锁」。

## 基本配置

配置在 `/data/adb/soc_freqcap/socfreq.conf`，每项带注释；常用项：

| 键 | 默认 | 说明 |
|---|---|---|
| `CPU_PCT_BIG` / `CPU_PCT_LITTLE` | 65 / 65 | 超大核 / 小核封顶百分比 |
| `CPU_MIN_PCT_*` | 0 | 进阶最低频百分比，0 = 跟随系统；页面在 CPU 封顶里折叠 |
| `CPU_FLOOR_M` | 0 | M 簇封顶下限（Hz，0=不设）；动态封顶/智能/温控/电量偏移都不会把它压到该值以下。Find N5 设 1785600 |
| `CPU_PCT_MID` / `CPU_PCT_MID2` | 66 / 64 | 中间两档（4 簇以上机型用） |
| `GPU_PCT` | 45 | GPU 封顶百分比 |
| `STRATEGY` | cap | `cap` 固定百分比封顶；`smart` 六态智能调度，百分比只当天花板 |
| `APP_SWITCH_TIER` | 2 | 智能模式切前台首拍预热档：0=关、1=轻载、2=中载、3=重载 |
| `SCREEN_POLICY` | cap | 熄屏保持封顶（默认开，页面一个开关）；关 = `uncap` 交还系统；高级用户可手工写 `keep` |
| `CAP_BACKEND` | auto | CPU 封顶第二道锁：自动叠加 MTK PPM；`sysfs` 可一键退回只写 sysfs。高通 msm 为实验性强制项。隐藏项，无页面开关 |
| `INTERVAL` | 5 | 巡检间隔（秒） |

数值会被夹回合法区间，调速器名只认白名单。完整键表、分应用档位、实时偏移（PSI / 温控 / 电量阶梯）见 [配置文档](docs/configuration.md)。

## 智能调度

首页「调度策略」可在「限频 / 智能」间切换，对应 `socfreq.conf` 的 `STRATEGY=cap|smart`。

| 负载状态 | 目标 |
|---|---|
| 空闲 | 天花板 ×45% |
| 轻载 | 天花板 ×65% |
| 中载 | 天花板 ×85% |
| 重载 | 天花板 ×100% |

每个簇按自己的 `CPU_PCT_*` 算，m=3/4 机型的中间档也一样；重载就是天花板。
CPU 用全核平均 80/60%、GPU 90/80/65%，2 秒一拍；升档 4s、降档 10s。
智能模式由 FSM 独占：动态封顶、PSI 借额度和实时温控都不生效；动态封顶开关仍可切换并保留选择，切回限频后恢复；电量阶梯仍保留。
插电时充电地板仍会把目标抬高，省电对比请拔电。游戏、满血、熄屏交还三条路径两种策略完全一样。

## 文档

| 文档 | 内容 |
|---|---|
| [设计与行为](docs/design.md) | 按簇分档、封顶吸附、与系统调度相处 |
| [安装、升级与卸载](docs/install.md) | 兼容性、KernelSU 热安装、卸载 |
| [配置](docs/configuration.md) | 全部配置键、分应用档位、实时偏移、SoC 频率表 |
| [深度睡眠](docs/deep-sleep.md) | Doze 时序、豁免、唤醒榜、待机耗电排查 |
| [芯片适配包](docs/adaptation.md) | 自动探测扑空时怎么导出、修改、验证 |
| [WebUI](docs/webui.md) | 页面、主题、发行商词典、图标边界 |
| [命令行与音量键](docs/cli.md) | `socfreqd` 子命令、`action.sh` 按键菜单、版本检查与发版流程 |
| [排查与恢复](docs/troubleshooting.md) | 卸载残留、管理器输出、已知边界 |
| [开发与自检](docs/development.md) | 文件清单、测试命令、从源码编译 |

## License

源码公开：可以查看、复制源码，并为了自己的设备修改、编译和使用（含个人备份）。**不允许再分发**（打包、转载、上架、随成品或链接提供下载），**不允许任何商业用途**（销售、收费、广告变现等）。修改版同样适用；商业或再分发授权需事先取得作者书面同意。完整条款以 [LICENSE](LICENSE) 为准。
