# 构建 socfreqd，并在加载期不合格时直接 exit 1。
#
# 用法（在模块根目录下）：
#   powershell -File tools/build_daemon.ps1            # 静态（默认）
#   powershell -File tools/build_daemon.ps1 -dynamic   # 动态
#
# 路径一律按 $PSScriptRoot 推导，不含任何绝对路径——
# 这个脚本随模块分发，换台机器、换个目录都能直接跑。

param([switch]$dynamic)

$ErrorActionPreference='Stop'

# tools/ 的上一级就是模块根
$root   = Split-Path $PSScriptRoot -Parent
$out    = Join-Path $root 'socfreqd'
$fix    = Join-Path $PSScriptRoot 'fix_tls_align.js'
$check  = Join-Path $PSScriptRoot 'check_loadable.ps1'
$srcMain = Join-Path $root 'src\socfreqd.c'
$srcIcon = Join-Path $root 'src\icon.c'
$srcInfo = Join-Path $root 'src\appinfo.c'
$srcLearn = Join-Path $root 'src\learn.c'
$srcAdapt = Join-Path $root 'src\adapt.c'

foreach($f in @($srcMain,$srcIcon,$srcInfo,$srcLearn,$srcAdapt,$fix,$check)){
  if(-not (Test-Path $f)){ "FAIL 缺少 $f"; exit 1 }
}

# 找 NDK 的 clang。优先 PATH，再退到 ANDROID_NDK_HOME / ANDROID_NDK_ROOT，
# 最后扫 Android SDK 的标准位置（注意 ndk/ 下面是版本号子目录，要往下找一层）。
function Find-Tool([string]$name){
  $c = Get-Command "$name.cmd" -ErrorAction SilentlyContinue
  if($c){ return $c.Source }
  $c = Get-Command $name -ErrorAction SilentlyContinue
  if($c){ return $c.Source }

  # 候选根：每个既可能是 NDK 本体，也可能只含版本号子目录
  $roots = @()
  foreach($e in @($env:ANDROID_NDK_HOME, $env:ANDROID_NDK_ROOT)){
    if($e){ $roots += $e }
  }
  if($env:ANDROID_HOME){ $roots += (Join-Path $env:ANDROID_HOME 'ndk') }
  if($env:LOCALAPPDATA){ $roots += (Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk') }
  if($env:USERPROFILE){ $roots += (Join-Path $env:USERPROFILE 'AppData\Local\Android\Sdk\ndk') }

  foreach($r in ($roots | Select-Object -Unique)){
    if(-not (Test-Path $r)){ continue }
    # r 本身是 NDK，或 r\<版本> 是 NDK
    $cands = @($r)
    $cands += (Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
               Where-Object { Test-Path (Join-Path $_.FullName 'toolchains') } |
               ForEach-Object { $_.FullName })
    foreach($c in $cands){
      $bin = Join-Path $c 'toolchains\llvm\prebuilt\windows-x86_64\bin'
      if(-not (Test-Path $bin)){ continue }
      foreach($n in @("$name.cmd","$name.exe")){
        $p = Join-Path $bin $n
        if(Test-Path $p){ return $p }
      }
    }
  }
  return $null
}

$cc = Find-Tool 'aarch64-linux-android21-clang'
if(-not $cc){
  "FAIL 找不到 aarch64-linux-android21-clang"
  "  装 NDK 后设置 ANDROID_NDK_HOME，或把它加进 PATH"
  exit 1
}

"=== 编译 ==="
"clang: $cc"
$cflags = @('-O2','-s','-Wall','-Wextra','-Wl,-z,max-page-size=16384','-lz',
            '-o',$out,$srcMain,$srcIcon,$srcInfo,$srcLearn,$srcAdapt)
if($dynamic){
  "模式: 动态链接（不编 -static，不跑 TLS 补丁）"
} else {
  "模式: 静态链接"
  $cflags = @('-static') + $cflags
}
& $cc @cflags 2>&1 | Out-String
if($LASTEXITCODE -ne 0){ "编译失败"; exit 1 }

"=== 补 TLS 对齐 ==="
if($dynamic){
  "动态链接不需要：.tdata 交给 libc.so，产物里没有 PT_TLS 段"
} else {
  node $fix $out 2>&1 | Out-String
  if($LASTEXITCODE -ne 0){ "TLS 对齐修复失败"; exit 1 }
}

"=== 加载期自检 ==="
# 独立脚本判定，只看 ELF 字节：检查和编译放同一个文件的话，
# 这里自己出错（比如初值类型不对）就会跟着一起假绿。
#
# 用当前进程自己跑，不写死 pwsh —— 系统自带的 Windows PowerShell 5.1
# 也能跑本脚本，写死 pwsh 的话在只装了 5.1 的机器上这里直接找不到命令。
$linkMode = if($dynamic){ 'dynamic' } else { 'static' }
& $check $out $linkMode 2>&1 | Out-String
if($LASTEXITCODE -ne 0){ "构建失败：加载期自检未通过"; exit 1 }

"=== 自检版只编译（不执行）==="
# test_real.c 是设备上跑的 aarch64 用例，PC 上执行不了；
# 但它**能不能编译**这件事不用上设备就能查：
# 改了守护的函数签名、而自检版还按老写法调用的话，
# 要等到上真机跑自检才发现——那时已经刷完机了。
#
# 两个坑：
#  - SF_TEST 不编 main()，于是"static 函数没人调"的警告成串，
#    用 -Wno-unused-function 关掉（不是真问题，关掉才看得见真问题）；
#  - 这个脚本头上一行是 $ErrorActionPreference='Stop'，而 PowerShell 把
#    原生命令的任何 stderr 输出都当成错误记录抛出来（NativeCommandError），
#    于是警告也能把构建打断——成功与否只能看退出码，不能看有没有红字。
#    所以这一句临时降到 Continue，判 $LASTEXITCODE，判完恢复。
$ErrorActionPreference = 'Continue'
# SF_TEST 下主循环整段不编，只在循环里读的几个全局会变成"没用到"
# （g_app 就是典型：唯一的读点在施加密封那一块）。这不是真问题，
# 和上面 -Wno-unused-function 同一个道理，一起关掉。
$tstOut = & $cc -fsyntax-only -Wall -Wno-unused-function -Wno-unused-variable -DSF_TEST `
              -I (Join-Path $root 'src') $srcMain $srcIcon $srcInfo $srcLearn $srcAdapt 2>&1
$testRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if($testRc -ne 0 -or ($tstOut | Out-String).Trim()){
  "构建失败：自检版（-DSF_TEST）编译不过 —— src/test_real.c 和守护已经脱节"
  $tstOut | Out-String
  exit 1
}
"  ok   自检版编译通过"

"  大小 $([math]::Round((Get-Item $out).Length/1KB,1)) KB"
"构建完成，全部检查通过"
