# 开发与自检

（返回 [README](../README.md)）

## 文件清单

模块目录（`/data/adb/modules/soc_freqcap`，升级时整目录被删掉重解包）：

```
module.prop            模块信息
socfreqd               守护程序（aarch64 静态编译）
socfreq.conf           配置默认值（装机时被搬走，不留在原地）
games.txt              游戏包名名单（同上）
app_modes.conf         分应用档位的空模板（同上）
vendors.txt            发行商词典（同上）
service.sh             开机启动 + 看门狗 + 写开机满血窗口
scripts/deep_sleep.sh  深度睡眠守护（独立 shell 进程，由 service.sh 拉起）
scripts/builtin_modes.txt  内置满血清单（相机类；装机时只给已安装的包追加）
scripts/builtin_white.txt  内置深睡豁免清单（通讯/音乐/健康/系统关键应用；只给已安装的包写 white=）
scripts/hotinstall.sh      KernelSU 热安装后脚本（停旧守护 → 起新版 → 重启深睡）
post-fs-data.sh        空实现（早期不动 sysfs）
customize.sh           安装脚本（环境自检 + 冲突提示 + 搬家）
uninstall.sh           卸载还原
action.sh              管理器里的操作按钮
adapt/                 机型适配表（作者随版本发布的 override.conf 来源，见「芯片适配包」）
webroot/index.html     WebUI 界面
webroot/api.sh         WebUI 后端（带输入校验）
src/                   源码与自检脚本
```

数据目录（`/data/adb/soc_freqcap`，0700，升级不动它）：

```
socfreq.conf           当前配置（用户在页面上改的就是这一份）
games.txt              当前名单
app_modes.conf         分应用档位（pkg=full / keep / pkg=NN / pkg=NN:NN）
vendors.txt            发行商词典（只给 WebUI 挑候选用）
games.base.txt         上次装机时"包里那份名单"的留档，只用于名单合并
socfreqd.lock          单实例锁（里面是 pid）
socfreqd.log           日志
state                  守护状态快照
run/heartbeat          守护每轮写一次的心跳 mtime；看门狗 >90s 未更新才拉起
pause                  存在 = 看门狗不再拉起守护
labels.tsv             应用名缓存（pkg<TAB>apk路径<TAB>名字）：按 APK 路径判新装/升级
                       （路径里那个随机后缀每次更新都会变），所以不需要时间型过期
learn.tsv              智能学习的样本（一个采样对象一行，共 31 列：目录 档 采样ms
                       顶帽ms 被压ms 24 个频率桶 天位图 配置帽%；档 0-3 是 CPU，4 是 GPU）
                       删掉它 = 清零重学
learn_app.tsv          按应用学习的样本，字段和上面那份同构（31 列），只是第一列
                       从目录换成包名；专属帽的时段只记这里，全局帽的时段两层都记
learn.tsv.since        学习计时起点（首次采样的 epoch 秒）。天数按它每满 24 小时加一，
                       不按零点切；删样本时一起删，重置为下一次采样重新起算
learn_meta.tsv         每档最近一次改帽的方向和时间（7 天反向冷却用）
learn.tsv.v2reset      旧格式被清空的标记；新样本落盘后自动删除
dsleep.conf            深度睡眠配置：enable=0/1 + 每行 white=包名（默认关）
ds_state               深睡状态与两张唤醒榜（dsactive / dsw / dswc / wcsince）
ds_wakes.conf          唤醒榜持久化（boot= / since= / A=当次 / S=累计）
ds_snap.conf           深睡中才存在；里面是本次临时加白的名单（旧版还有 Doze 常量原值）。
                       正常退出会删掉，启动时发现它还在 = 上次被杀，按它还原
ds_stats.conf          上次深睡退出时的当次榜快照
dsleep.log             深度睡眠日志（进出、被系统弹出、加白、拔电重置）
override.conf          机型适配表（自动探测扑空时才按它兜底）。正常来源是装机脚本
                       从包里的 adapt/ 按 #match 命中后装进来的，第一行带 #from adapt/ 标记
icon/                  WebUI 图标缓存（<包名>.<APK 的 mtime>.<取图算法版>.b64
                       外加同名 .via 来源标记；改选图规则时把 api.sh 里的 ICON_ALGO +1，
                       老缓存里选错的那些会自然失效，不用叫用户手动清）
run/                   journal 与运行时文件
  nodes.saved            数值节点原值 + 权限
  perms.saved            只记权限（游戏期放宽到 666 的那批）
  govs.saved             调速器名字
  status                 首页读的状态
  boot_boost_until       开机满血窗口的截止秒（/proc/uptime 口径）
  dsleep.pid             深度睡眠守护的 pid（看护和停止都按它 + /proc cmdline 认人）
  freeze.saved           冻结账本（后端/组路径 + pid|starttime|包名）；解冻的唯一依据
  freeze.queue           待冻结队列（pid|starttime|包名），冻结窗口结束就清空
```

**名单怎么跟着版本升级。** 上面那句"升级不动数据目录"会带来一个副作用：
包里精选名单新增的游戏，升级时进不了用户机器——数据目录里那份是装机那天的旧版，
新条目一个都盖不进去。所以装机脚本做的是三方合并，基准就是 `games.base.txt`：

| 情况 | 结果 |
|---|---|
| 包里有、基准里没有 | 补进用户名单（这就是升级通道） |
| 基准里有、包里没有 | 从用户名单删掉（上游下架的条目） |
| 用户自己加的 | 基准和包里都没有它 → 两个差集都不含，不动 |
| 用户自己删的 | 包和基准都还有 → 不属于"新增"，不会被塞回去 |

从没有基准的老版本直升时只做"补新增"、不做删除：那种情况下分不清
"用户自己加的"和"上游删掉的"，宁可多留几条，也不能把人自己加进去的包删掉。

## 自检

源码目录里带了几份自检脚本：

```sh
sh src/test_api.sh        # 配置写入的校验与注入防护 + 图标取图链路（PC 上可跑）
sh src/test_watchdog.sh   # 看门狗存活判据（PC 上可跑）
sh src/test_deep_sleep.sh # 深睡状态机：进入/媒体加白/亮屏还原/崩溃快照 + 巡检开销（桩 dumpsys）
sh src/test_hotinstall.sh # 热安装脚本：停旧/起新、暂停保持、深睡按开关拉起
sh src/test_install_merge.sh  # 装机迁移段台架：配置搬家/补键 + 名单三方合并
sh src/test_action_keys.sh    # action.sh 音量键状态机（假 getevent 喂剧本）
node src/test_contract.js # 跨文件契约（导出键、协议、接线），要七个参数，见下
node src/test_advice_ui.js webroot/index.html # 智能建议卡片：喂真 api.sh 的输出，验渲染与"应用"发几条写
node src/test_ovr_ui.js webroot/index.html   # 芯片适配卡片：同样喂真 api.sh 的 ovr_show/pkg/pkg_import/ovr_clear
node src/test_func_ui.js webroot/index.html webroot/api.sh  # 功能页档位：档位值要落在后端校验区间内
node src/test_ds_ui.js webroot/index.html   # 睡眠页：state 解析、状态条、双榜、豁免名单与开关调用
node src/test_about_ui.js webroot/index.html # 关于页：标记被 shell 吃掉时从 module.prop 行恢复
node src/test_theme_ui.js webroot/index.html # 主题：保存优先 / 系统偏好兜底 / localStorage 禁用
node src/test_gpu_preview.js webroot/index.html # GPU 预览：设定值吸附 + 最低频点回落
node src/test_icons_ui.js webroot/index.html  # 应用名字/图标：labels 串行、图标小批失败重试、不误记"没图"
node src/test_pkgrow_ui.js webroot/index.html # 行构造：缓存命中的名字/图标要在插入文档前就画上
node src/test_apps_search.js webroot/index.html # 应用搜索：中文别名（相机 → camera）与不误伤
node src/test_mutation.js # 变异测试：证明用例真能抓到解析 bug
node src/test_bridge.js webroot/index.html   # 三种管理器的桥接
node tools/test_appinfo.js # 应用名/图标读法对着 SDK 的 aapt2 比答案（见下）
# 下面这两份要在手机上跑（PC 上跑不了 aarch64 二进制）：
sh   tools/smoke_ondevice.sh   # 装机后的现场核对
sh   tools/sandbox_probe.sh    # 芯片适配兜底路径（造一台探测不到的假机器，见上）
```

`test_install_merge.sh` 是把 `customize.sh` 里那段**抠出来**在临时目录里跑的，
不是照抄一份逻辑到测试里：装机脚本只在刷机的时侯跑一次，
PC 上其余自检碰不到它，抄一份就会和原脚本各自漂移。

`tools/test_appinfo.js` 解决的是另一类问题：`src/appinfo.c` 那份 AXML /
`resources.arsc` 的读法，在这台 Windows 上**根本执行不了**
（NDK 只产 arm64 的安卓二进制，本机没有 host gcc、也没装 WSL），
所以"偏移抄对没有"没法靠跑一遍确认。做法是先有一份能跑的参照实现
（`tools/apklabel.js`，同一套算法的 JS 版），拿本机 SDK 的
`aapt2 dump badging` 当权威答案比：两边的 `application: label=` 和 `icon=`
必须一致，C 那份才是照着已经验过的偏移抄的。
本地没有可测的 APK 或没有 aapt2 时这组会整组跳过并明说"没验"，不会假绿。
代价也要写在这儿：**参照实现和 C 是两份**，改了格式判定要两边一起改——
所以两边的注释里都标了同一个出处（实测的包：gnirehtet / 酷安 / Kin / WakeUp）。

`test_contract.js` 要**七个**参数，顺序固定：

```sh
node src/test_contract.js src/socfreqd.c webroot/index.html \
     socfreq.conf webroot/api.sh README.md games.txt src/icon.c \
     [比对基准脚本]
```

少传这七个里的任何一个都会在开头就报"参数不对"并打印用法（`exit 2`），
不会跑到一半才崩在某个 `readFileSync(undefined)` 上。

末尾还有一个**可选**参数：另一个模块的 `api.sh`，用来做相似度对比。
它不随本包发布，所以没传就跳过那两条，其余检查照跑：

```sh
node src/test_contract.js ... src/icon.c src/ref_api_ktwo.sh
```

> 这一条当初是必填的，结果外部用户照着 README 跑，第一步就卡在
> "参数不对"——要一个他手上根本没有的文件。别人的模块怎么装、
> 叫什么名字，只有作者知道。这种参数不该挡在自检的门口。

最后一个参数是**任意一份同类限频模块的接口脚本**，只用来跑一条断言：
本模块的 `api.sh` 和它不能雷同到让人觉得是同一个东西。
判据是剔除 `fi`/`done`/`}` 这类 shell 语法糖之后，双方还剩几行逐字相同
（上限 3 行），以及最长公共连续片段（上限 20 字符）。

**这一份基准文件需要你自己准备，不会随包分发**——
把别人的代码再分发一次和抄它是一回事。随便找个同类限频模块的
`api.sh` 传进去即可（第八个参数），找不到就不传，
那两条相似度断言会打印 `skip` 后跳过，其余检查照常跑完。

各项的具体条数以脚本最后一行打印的 `全部通过 (N)` 为准——
这里不写死数字：每加一批断言它就会烂一次，写在文档里的数必然比代码旧。

> 这条当初是必填的。当时的想法是"少一个输入还假装全绿，比报错更糟"，
> 但它挡住的不是自己的自检，而是别人的：外部用户手上根本没有
> 别人的 `api.sh`，照着 README 敲第一步就 `exit 2`，后面一条都跑不到。
> 缺输入该跳过还是该报错，取决于那个输入是不是只有作者才有——
> 前七个是本仓库自己的文件，必须齐全；第八个不是。

接口约定：成功输出数据到 stdout，失败输出 `E/<原因码>` 到 stderr 并返回非零。
原因码（`BAD_PKG` / `RANGE` / `VERIFY` / `NO_CONF` / `WRITE` …）是给程序看的，
不混进 stdout，因为 WebUI 拿 stdout 当数据解析，错误信息混进去整段就废了。

### 名单必须是 LF 结尾

`games.txt` 存成 CRLF 的话，游戏一个都列不出来，页面显示「本机已装 0 个」，
也不报错。原因：`grep -x` 要求整行相等，而 CRLF 的行尾多一个 `\r`，
和来自 `pm` 的包名永远对不上。安装时会自动去掉一次 `\r`，
但**平时用 Windows 编辑器改名单记得存成 LF**（VS Code 右下角点一下编码换）。

顺带说明为什么这类问题能在测试全绿的情况下溜到手机上：
开发机上跑的是 MSYS，它读文件时会翻译行尾、把 `\r` 吃掉，
于是「CRLF 匹配不上」在 PC 上根本不会发生。同类的还有 `mktemp`——
它在 PC 上有 `/tmp` 可写，在设备的 root shell 里没有。

```sh
# 装完在手机上查一遍（PC 上的测试替不掉这一步）
adb push tools/smoke_ondevice.sh /data/local/tmp/
su -c 'sh /data/local/tmp/smoke_ondevice.sh'
```

**前面那个 `sh` 不能省。** `adb push` 推过去的文件是 0644，没有执行位；
直接 `su -c '/data/local/tmp/smoke_ondevice.sh'` 会被内核拒绝，报
`Permission denied`。而这行报错出现在自检脚本的第一条命令之前，
看起来像是脚本本身坏了——实际上一条检查都还没跑到。
交给 `sh` 解释执行不需要执行位。（想直接跑也行，
`adb push` 之后补一句 `adb shell chmod 755 /data/local/tmp/smoke_ondevice.sh`。）

目录名取自 `module.prop` 里的 `id`（**不是**模块显示名 `Soc智能限频`）：
APatch 装在 `/data/adb/ap/modules/soc_freqcap`，就作为参数传进去。

```sh
su -c 'sh /data/local/tmp/smoke_ondevice.sh /data/adb/ap/modules/soc_freqcap'
```

> 目录写错时脚本会立刻退出并打出上面的用法。早退是有意的：
> 硬跑下去，`api.sh` 不存在 → `sh: cannot open` → `2>/dev/null` 吞掉 →
> 最后只剩一句"名单匹配不上"，把人往完全错的方向带。

**脚本内部调 `socfreqd` 时不能写成 `sh "$D/socfreqd"`。** shell 对"脚本操作数"
是读进来当命令解析、不是 `execve`，把 ELF 当脚本解析只会得到
`cannot execute binary file`，stdout 一行都没有——于是上面第 3 到 6 项
全都变成恒空判断，**代码全对也报 FAIL**。
`api.sh` 是真脚本，那几处 `sh "$API"` 保留才对。

只查八件事：游戏列表有没有内容、名单条数对不对、守护认不认得 SoC、
逐核频率表读没读到（`ncores` / `coremax_0` / `corepol_*`）、
新键接上了没有 + 守护读的是不是搬家后的配置、守护活着没有、
**这台机上的真 APK 读不读得出应用名和声明的图标**（前六项 PC 上都能验，
只有这一项必须上真机——酷安那种资源名被缩短过的包，
在临时目录里造不出第二个来），
以及**智能学习的样本文件和 advice 判据接上了没有**（列数对不对、有没有落盘）。
任何一项不合格就非零退出，并把接口的原样输出打出来。

算法与前台包名解析的自检要跑 aarch64 二进制，在设备上执行：

```sh
# 构建自检版（与守护同一批源文件，SF_TEST 下不编 main）
aarch64-linux-android21-clang -DSF_TEST -static -s -O1 \
     -Wall -Wno-unused-function -Wl,-z,max-page-size=16384 -o test_real \
     src/socfreqd.c src/icon.c src/appinfo.c src/learn.c src/adapt.c -lz
# 必须过一遍 TLS 对齐，否则推上手机直接 Abort：
#   executable's TLS segment is underaligned: alignment is 8, needs 64
node tools/fix_tls_align.js test_real
adb push test_real /data/local/tmp/
adb shell su -c /data/local/tmp/test_real
```

**这一份要在真机上跑，不能只 `-fsyntax-only`。** 构建脚本里那步只编译不执行，
所以曾经有一条断言（`snap_from_list` 低于最低频点时该返回什么）和实现各说各话了很久：
实现返回原值（写 0 会被内核拒、整档不封顶），用例却写着 0 —— 只有真跑一次才会红出来。

`icon.c`、`appinfo.c` 和 `-lz` 都不能省：图标和名字都是从 APK 的 zip 条目里读的，
需要 `inflate`；少了 `src/icon.c` 会报 `undefined symbol: icon_from_apk`，
少了 `src/appinfo.c` 会报 `undefined symbol: appinfo_label`，
少了 `-lz` 会报 `undefined symbol: inflateInit2_`。

这里测试直接 include 守护源码，跑的就是线上那份代码——
刻意不把逻辑抄一份到测试里：两份实现会各自漂移，
抄的那份全绿而线上那份早已失效的情况很难被发现。

## 从源码编译

用随包分发的构建脚本，它比手敲编译命令多做了加载期的检查：

```sh
powershell -File tools/build_daemon.ps1
powershell -File tools/build_daemon.ps1 -dynamic   # 动态链接，省 700KB
```

**Windows PowerShell 5.1 和 PowerShell 7 都能跑**，不必特意装新的——
5.1 是系统自带的，上面这条命令直接可用。

（5.1 读 `.ps1` 时若文件没有 BOM 会按系统 ANSI 编码解码，
中文机器上是 GBK，中文注释和字符串全成乱码、报 ParserError，
而且退出码是 1，看着像"检查没过"。所以 `tools/` 下的脚本一律存成
UTF-8 with BOM，`pack.ps1` 会在打包前断言这一点。）

脚本会自己找 NDK（PATH / `ANDROID_NDK_HOME` / 标准 SDK 路径都试），
路径全部按脚本自身位置推导，放在任何目录下都能跑。

等价的手工命令（**四个参数都不能省**）：

```sh
aarch64-linux-android21-clang -O2 -static -s -Wall -Wextra \
     -Wl,-z,max-page-size=16384 -o socfreqd \
     src/socfreqd.c src/icon.c src/appinfo.c -lz
node tools/fix_tls_align.js socfreqd
llvm-readelf -lW socfreqd | awk '$1=="TLS"{print "TLS = "$NF}'
llvm-readelf -lW socfreqd | awk '$1=="LOAD"{print "LOAD = "$NF}'
```

| 缺了 | 结果 |
|---|---|
| `src/icon.c` | `undefined symbol: icon_from_apk` |
| `src/appinfo.c` | `undefined symbol: appinfo_label`（名单只剩包名，图标也回不到"按声明取"那一条） |
| `-lz` | `undefined symbol: inflateInit2_`（图标取自 APK 的 zip 条目，要 inflate） |
| `-Wl,-z,max-page-size=16384` | `PT_LOAD` 对齐停在 4KB，Android 15+ 的 16KB 页设备拒载 |
| `fix_tls_align.js` | `PT_TLS` 对齐是 8，Bionic 直接拒载 |

最后两条 `awk` 是**加载期自检**：TLS 段的 align 必须 ≥ `0x40`，
每个 LOAD 段的 align 必须 ≥ `0x4000`。这两项不对的话守护在 `exec` 之前就被
linker 拒绝，用户看到的只是「无法连接守护」，没有 logcat、没有 stderr、
没有 rc，只能把 `socfreqd` 捞出来跑 readelf 才能定位——
所以必须在构建阶段挡住，不能指望用户来报。

动态链接的产物用另一套判据：**不应有 `PT_TLS`**（`.tdata` 交给 libc.so）
且**必须有 `PT_INTERP`**。`tools/check_loadable.ps1` 会按产物实际的链接方式
自动选判据，两种都不合格都 `exit 1`。

`-s` 是去掉调试信息，包体积从 3.3 MB 降到 745 KB。
剩下的体积主要是静态链接的 libc，换来的是不依赖任何系统 .so。

### 两条只有真机才会暴露的加载期地雷

这两条都在 `exec` 之前被 Bionic 的 linker 拒绝，
守护一个字都没跑起来，页面上只显示「无法连接守护」，完全看不出真正原因。

**一、`PT_TLS` 的 `p_align` 是 8，而 Android 要求 >= 64。**

报错原文：

```
executable's TLS segment is underaligned:
alignment is 8, needs to be at least 64 for ARM64 Bionic
```

来源是 **NDK r25c 静态 libc 自带的 `.tdata`，不是本模块的代码**——
实测连 `int main(void){return 0;}` 编出来也一样是 `0x8`。
这个段是 `errno` 之类的东西，全静态链接的 C 程序都有。

r25c 的链接器没有 TLS 对齐的入口（`-z tlsalign=64` 被忽略，
`--tls-align=64` 直接报 unknown argument），所以只能在产物上补：

```sh
node tools/fix_tls_align.js socfreqd
```

只改 `p_align` 这 8 个字节，文件大小不变。
脚本改之前会核对 `p_offset % p_align == p_vaddr % p_align`
（loader 也会核对这一条，改完不自洽照样拒载），不自洽就报错退出。

**二、`PT_LOAD` 的 `p_align` 是 `0x1000`（4 KB），16 KB 页设备会拒载。**

Android 15+ 开始有 16 KB 页的机型。这个用链接器参数解决，
`-Wl,-z,max-page-size=16384` 实测在 r25c 上有效
（`p_align` 变成 `0x4000`，各段的 offset 和 vaddr 会一起重排，保持自洽）。

**两条都要在产物上核对，别信构建脚本自己说的"通过"：**

```sh
# 静态产物：TLS 对齐 >= 64，所有 LOAD 对齐 >= 0x4000
llvm-readelf -lW socfreqd | awk '$1=="TLS"{print "TLS align = "$NF}'
llvm-readelf -lW socfreqd | awk '$1=="LOAD"{print "LOAD align = "$NF}'
# 期望 TLS 那行 >= 0x40，所有 LOAD 那些 >= 0x4000
```

这两条不对的话，守护在 `exec` 之前就被 linker 拒绝，
用户看到的只是「无法连接守护」，没有 logcat、没有 stderr、没有 rc，
只能把 `socfreqd` 捞出来跑 readelf 才能定位。**这类问题要在构建阶段挡住，
不能指望用户来报。**

（`tools/build_daemon.ps1` 会自动跑上面那套 `awk` 断言，不合格时 `exit 1`；
`tools/check_loadable.ps1` 可以单独对任意 ELF 跑同一套判据。）

### 为什么保留静态链接

动态链接（去掉 `-static`）确实没有 `PT_TLS`，体积还能从 745 KB 降到 37 KB
（`tools/build_daemon.ps1 -dynamic`，实测）。但本模块的 C 代码只用 libc
和系统调用，没有 pthread / dl / NSS，不存在必须运行时解析的符号。
为省 700 KB 换来"linker64 与 libc.so 版本对不上"这种失败模式，
对一个开机脚本不值得。只有想在 recovery 或极早期启动里跑才需要考虑。
