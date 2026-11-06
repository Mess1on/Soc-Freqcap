# Soc智能限频

Android 的 CPU / GPU 频率封顶模块：平时按百分比限频，进游戏自动交还系统；附带 WebUI 与深度睡眠。

作者：梦笙Mess1on & Ktwo　　版本：V2.1　　适用：KernelSU / Magisk / APatch · arm64-v8a

## 核心特性

| 特性 | 说明 |
|---|---|
| 按簇自动分档 | 读每个 `policy*` 的频率表自动分档，不写死 CPU 编号；封顶值向下吸附到频点，绝不超过设定百分比 |
| 智能调度 | 首页可在「限频 / 智能」间切换；智能按空闲、轻载、中载、重载四档动态给目标，`CPU_PCT_*` 只当天花板 |
| 游戏与分应用放开 | `games.txt` 认包名，进游戏解除封顶并把节点权限交还系统；`app_modes.conf` 支持 `full` / `keep` / 单独数值 |
| GPU 多后端 | 高通 KGSL、MTK Mali devfreq，以及天玑 9500 这类无 GPU devfreq 机型的 `gpufreqv2 limit_table` |
| WebUI + 深度睡眠 | 页面调参 / 名单 / 建议；熄屏后 Doze 收口（force-idle + 白名单），可配豁免与唤醒榜 |

## 安装

需要 root（KernelSU / Magisk / APatch）。CPU / GPU 封顶不挑 Android 版本；深睡的 60 秒收口依赖 Android 12+ 的 DeviceIdleController shell 接口。

| 管理器 | 安装 |
|---|---|
| KernelSU / SukiSU Ultra | 模块页「从本地安装」选 zip；SukiSU ≥ v4.2.1-r2 热安装，装完不用重启 |
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
| `CPU_PCT_MID` / `CPU_PCT_MID2` | 66 / 64 | 中间两档（4 簇以上机型用） |
| `GPU_PCT` | 45 | GPU 封顶百分比 |
| `STRATEGY` | cap | `cap` 固定百分比封顶；`smart` 四档智能调度，百分比只当天花板 |
| `SCREEN_POLICY` | uncap（新装）/ keep（升级） | 熄屏交还系统，或保持封顶 |
| `INTERVAL` | 5 | 巡检间隔（秒） |

数值会被夹回合法区间，调速器名只认白名单。完整键表、分应用档位、实时偏移（PSI / 温控 / 电量阶梯）见 [配置文档](docs/configuration.md)。

## 智能调度

首页「调度策略」可在「限频 / 智能」间切换，对应 `socfreq.conf` 的 `STRATEGY=cap|smart`。

| 负载状态 | 大核目标 | 其余档目标 |
|---|---|---|
| 空闲 | 30% | 40% |
| 轻载 | 45% | 50% |
| 中载 | 65% | 60% |
| 重载 | 85% | 75% |

实际目标 = `min(状态目标, CPU_PCT_*)`。默认 `CPU_PCT_BIG/LITTLE=65` 时，重载仍按 65% 封顶，智能模式主要在低负载时继续往下省；想让重载跑到 85/75，需要把对应的 `CPU_PCT_*` 提高到 85/75 以上。游戏、满血、熄屏交还三条路径两种策略完全一样。

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
