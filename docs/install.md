# 安装、升级与卸载

（返回 [README](../README.md)）

## 兼容性

| 项目 | 说明 |
|---|---|
| 安装 | Magisk / KernelSU / APatch 都能装 |
| WebUI 桥 | 见下表，**只有标"已验证"的那条在实际机器上跑通过** |
| 模块根 | 首次调用自动探测 `/data/adb/modules` 与 `/data/adb/ap/modules`（APatch 装在后者） |
| CPU | 任何暴露 `/sys/devices/system/cpu/cpufreq/policy*` 的内核，按 policy 数自动分档 |
| GPU | 高通 KGSL、MTK Mali（devfreq 节点），以及 MTK gpufreqv2（天玑 9500 这类没有 GPU devfreq 的机型：读 `stack_working_opp_table`、写 `limit_table` 的 ceiling）。厂商 PowerHAL 自己的上限仍可能比模块帽更低。认不出来就只限 CPU，不报错也不乱写 |
| 架构 | aarch64（arm64-v8a） |

### KernelSU 热安装：装完不用重启

`customize.sh` 里按 KernelSU 的约定申请热安装（`MODULE_HOT_INSTALL_REQUEST`），
并指定 `scripts/hotinstall.sh` 作为安装后脚本。支持这个约定的管理器会在安装完成后：

1. 用新版本替换 `/data/adb/modules/soc_freqcap`（模块文件、**WebUI**、`action.sh`、
   `module.prop` 立即生效，管理器不再提示重启）；
2. 执行 `scripts/hotinstall.sh`：先让旧的深睡守护退出（它会解冻、unforce 并撤掉临时白名单），
   再用新二进制 `restart` 主守护（等旧实例恢复完频率再接手），深睡开关开着就重新拉起。

模块处于「暂停」状态时保持停住，不替用户恢复；配置、名单、学习样本都在
`/data/adb/soc_freqcap`，不受热安装影响。

不支持这个约定的管理器会忽略这两个变量，自动回到"重启后生效"，不会更糟。
本模块不写 `system/`、也不依赖 `service.sh` 才启动，所以适合热安装；
热安装时 `service.sh` 不会执行，守护由 `hotinstall.sh` 负责拉起。

版本边界：**SukiSU Ultra 从 v4.2.1-r2 起支持**（hot install 在 2026-10-15 合入）；
4.1.3 及更早的 ksud 没有这段实现，安装界面会提示"当前管理器不支持热安装"，
并照常走重启后生效。KernelSU 官方管理器按官方文档实现该约定。安装脚本会用
`KSU_HAS_HOT_INSTALL` 判断并如实提示，不会把"仍需重启"说成"已热安装"。

## 卸载

管理器里卸载会在模块目录下执行 `uninstall.sh`：先让守护自己还原，再按 journal
兜底还原一遍（数值、节点权限、调速器名三本账都读），最后把数据目录
`/data/adb/soc_freqcap` 整个删掉。两道都做完，sysfs 上不留东西，
手机上也不留 root 私有的孤儿目录。

即使守护被强杀、来不及自己还原，journal 里存着原始值，卸载时照样能还原回来。
从老版本直升的机器上账本可能还在模块目录里（`<模块>/run/`），
两处都会读——只兜一处等于留一半。
