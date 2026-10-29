# Soc智能限频

Android 的 CPU / GPU 频率封顶模块：平时按百分比限频，进游戏自动交还系统；附带 WebUI 与深度睡眠。

作者：梦笙Mess1on　　版本：V1.6　　适用：KernelSU / Magisk / APatch · arm64-v8a

## 核心特性

| 特性 | 说明 |
|---|---|
| 按簇自动分档 | 读每个 `policy*` 的频率表自动分档，不写死 CPU 编号；封顶值向下吸附到频点，绝不超过设定百分比 |
| 游戏与分应用放开 | `games.txt` 认包名，进游戏解除封顶并把节点权限交还系统；`app_modes.conf` 支持 `full` / `keep` / 单独数值 |
| 智能学习建议 | 按"封顶真正生效时"的采样，给每簇放宽或收紧建议；按应用学习可单独开启（默认关） |
| GPU 多后端 | 高通 KGSL、MTK Mali devfreq，以及天玑 9500 这类无 GPU devfreq 机型的 `gpufreqv2 limit_table` |
| WebUI + 深度睡眠 | 页面调参 / 名单 / 建议；熄屏 20 秒冻结第三方后台、7:30 一次性 Doze，可配豁免与唤醒榜 |

## 安装

需要 root（KernelSU / Magisk / APatch）。CPU / GPU 封顶不挑 Android 版本；深睡的 7:30 收口依赖 Android 12+ 的 DeviceIdleController shell 接口。

| 管理器 | 安装 |
|---|---|
| KernelSU / SukiSU Ultra | 模块页「从本地安装」选 zip；SukiSU ≥ v4.2.1-r2 热安装，装完不用重启 |
| Magisk | 模块页「从本地安装」选 zip，重启生效 |
| APatch | 模块页安装到 `/data/adb/ap/modules`，重启生效 |

升级不会覆盖 `/data/adb/soc_freqcap` 里的配置、名单和学习样本。WebUI 需要管理器注入 root 桥：KernelSU / APatch 已验证；Magisk 的 WebUI 很可能不可用，命令行仍可用。

## 最小运行示例

```sh
D=/data/adb/modules/soc_freqcap      # APatch: /data/adb/ap/modules/soc_freqcap
sh $D/socfreqd status                 # 守护在不在、当前档位
sh $D/socfreqd probe                  # 会封到多少（只读，不改任何东西）
```

WebUI：管理器模块页 → 打开 WebUI。首页徽标显示「常态限频中」或「游戏中 · 已解锁」。

## 基本配置

配置在 `/data/adb/soc_freqcap/socfreq.conf`，每项带注释；常用项：

| 键 | 默认 | 说明 |
|---|---|---|
| `CPU_PCT_BIG` / `CPU_PCT_LITTLE` | 65 / 65 | 超大核 / 小核封顶百分比 |
| `CPU_PCT_MID` / `CPU_PCT_MID2` | 66 / 64 | 中间两档（4 簇以上机型用） |
| `GPU_PCT` | 45 | GPU 封顶百分比 |
| `SCREEN_POLICY` | uncap（新装）/ keep（升级） | 熄屏交还系统，或保持封顶 |
| `LEARN` / `LEARN_APP` | 1 / 0 | 智能学习 / 按应用学习 |
| `INTERVAL` | 5 | 巡检间隔（秒） |

数值会被夹回合法区间，调速器名只认白名单。完整键表、分应用档位、实时偏移（PSI / 温控 / 电量阶梯）见 [配置文档](docs/configuration.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [设计与行为](docs/design.md) | 按簇分档、封顶吸附、与系统调度相处 |
| [安装、升级与卸载](docs/install.md) | 兼容性、KernelSU 热安装、卸载 |
| [配置](docs/configuration.md) | 全部配置键、分应用档位、实时偏移、SoC 频率表 |
| [智能学习](docs/learning.md) | 采样口径、建议阈值、按应用学习 |
| [深度睡眠](docs/deep-sleep.md) | 冻结 / Doze 时序、豁免、唤醒榜、待机耗电排查 |
| [芯片适配包](docs/adaptation.md) | 自动探测扑空时怎么导出、修改、验证 |
| [WebUI](docs/webui.md) | 页面、主题、发行商词典、图标边界 |
| [命令行与音量键](docs/cli.md) | `socfreqd` 子命令、`action.sh` 按键菜单 |
| [排查与恢复](docs/troubleshooting.md) | 卸载残留、管理器输出、已知边界 |
| [开发与自检](docs/development.md) | 文件清单、测试命令、从源码编译 |

## License

本项目采用 [PolyForm Internal Use License 1.0.0](LICENSE)：允许在个人或组织内部使用、修改和分发，但不得用它提供与本软件竞争的产品；再分发时需附带许可证文本。完整条款以 `LICENSE` 为准。
