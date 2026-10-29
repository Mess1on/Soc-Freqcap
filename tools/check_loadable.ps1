# 加载期自检：断言这个 aarch64 ELF 能不能被 Bionic linker 接受。
#
# 为什么单独成一个脚本、且不信任任何"构建成功"的报告：
# 拒载发生在 exec 之前，守护一个字节都没执行，没有 logcat、没有 stderr、
# 没有 rc。用户看到的只是 WebUI 上的"无法连接守护"，
# 排查只能靠把 socfreqd 捞出来跑 readelf。这类问题必须在构建阶段 fail 掉，
# 不能指望用户截图来报。
#
# 判据（对应 Bionic 的两条硬要求）：
#   静态：PT_TLS 的 p_align >= 64，且所有 PT_LOAD 的 p_align >= 0x4000
#   动态：不存在 PT_TLS（无 .tdata），且必须有 PT_INTERP（确实动态链接）
#
# 两种链接方式的判据互斥，混用就会漏判：静态产物没有 INTERP，
# 动态产物没有 TLS。所以这里先判 PT_INTERP 在不在，再决定用哪套判据。
#
# 用法：powershell -File check_loadable.ps1 <elf> [static|dynamic|auto]
#   auto（默认）按产物实际的链接方式选判据。
#
# 路径全部按 $PSScriptRoot / 传入路径解析，不写死任何绝对路径：
# 这个脚本要随模块分发，换台机器、换个目录都能直接跑。

param(
  [Parameter(Mandatory=$true)][string]$elf,
  [ValidateSet('static','dynamic','auto')][string]$mode='auto'
)

$ErrorActionPreference='Stop'

# NDK 不在 PATH 里，找不到就报错而不是静默跳过检查。
$re = Get-Command llvm-readelf -ErrorAction SilentlyContinue
if(-not $re){
  foreach($c in @(
    "$env:ANDROID_NDK_HOME\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe",
    "$env:LOCALAPPDATA\Android\Sdk\ndk\25.2.9519653\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe")){
    if($c -and (Test-Path $c)){ $re=$c; break }
  }
}
if(-not $re){ "  FAIL 找不到 llvm-readelf（装 NDK 后把它的 bin 加进 PATH）"; exit 1 }

if(-not (Test-Path $elf)){ "  FAIL 找不到 $elf"; exit 1 }

$bytes=[System.IO.File]::ReadAllBytes($elf)

# ELF magic 和位序先确认：后面所有偏移量都建立在"这确实是 ELF64 LE"上。
if($bytes.Length -lt 64 -or $bytes[0] -ne 0x7f -or $bytes[1] -ne 0x45 -or
   $bytes[2] -ne 0x4c -or $bytes[3] -ne 0x46){
  "  FAIL 不是 ELF 文件"; exit 1
}
if($bytes[4] -ne 2 -or $bytes[5] -ne 1){ "  FAIL 不是 ELF64 little-endian"; exit 1 }
if([BitConverter]::ToUInt16($bytes,0x12) -ne 183){ "  FAIL e_machine 不是 AArch64"; exit 1 }
$eType=[BitConverter]::ToUInt16($bytes,0x10)
# e_type：静态非 PIE 是 ET_EXEC(2)，动态默认产出 ET_DYN(3)。
# ET_DYN 本身不算错，但静态模式下出现它就不对——某些 loader 会按共享库
# 处理而走错分支，表现为 "cannot execute binary file"。
# 所以这条留到下面按链接方式判定，这里只挡非 2/3 的怪值。
if($eType -ne 2 -and $eType -ne 3){ "  FAIL e_type = $eType，不是 ET_EXEC(2) 或 ET_DYN(3)"; exit 1 }

# 直接从文件头数 PT_INTERP / PT_TLS，不依赖 readelf 的文本输出格式。
# 文本输出会随版本改列数，字节偏移不会。
$phoff=[BitConverter]::ToUInt64($bytes,0x20)
$phentsize=[BitConverter]::ToUInt16($bytes,0x36)
$phnum=[BitConverter]::ToUInt16($bytes,0x38)
if($phentsize -lt 56){ "  FAIL e_phentsize=$phentsize 不合常理"; exit 1 }

$PT_INTERP=3; $PT_LOAD=1; $PT_TLS=7
$hasInterp=$false; $hasTls=$false
$loadAligns=@(); $tlsAlign=$null
for($i=0;$i -lt $phnum;$i++){
  $o=[int]($phoff + [uint64]$i * $phentsize)
  if($o + 56 -gt $bytes.Length){ "  FAIL 程序头越界"; exit 1 }
  $type=[BitConverter]::ToUInt32($bytes,$o)
  # p_align 是 8 字节，位于 p_filesz(32) p_memsz(40) p_flags(44) p_align(48) 之后
  $align=[BitConverter]::ToUInt64($bytes,$o+48)
  switch($type){
    $PT_INTERP { $hasInterp=$true }
    $PT_TLS    { $hasTls=$true; $tlsAlign=$align }
    $PT_LOAD   { $loadAligns += $align }
  }
}

# auto：按产物实际的链接方式选判据
if($mode -eq 'auto'){
  $mode = if($hasInterp){ 'dynamic' } else { 'static' }
}

$bad=0
"  · 链接方式 $($mode.ToString())（PT_INTERP=$hasInterp, PT_TLS=$hasTls, LOAD=$($loadAligns.Count) 段）"

if($mode -eq 'static'){
  # 静态非 PIE：没有 PT_INTERP
  if($hasInterp){
    "  FAIL 声明为静态，但产物有 PT_INTERP（动态链接了）"; $bad++
  } else {
    "  ok   无 PT_INTERP（确实静态链接）"
  }
  # 静态非 PIE 必须是 ET_EXEC：ET_DYN 会让某些 loader 按共享库处理而走错分支
  if($eType -eq 2){
    "  ok   e_type = ET_EXEC（静态非 PIE）"
  } else {
    "  FAIL 静态模式下 e_type = ET_DYN(3)，应为 ET_EXEC(2)"; $bad++
  }
  # 静态 libc 带 .tdata 就会产生 TLS 段，Bionic 要求 p_align >= 64
  if(-not $hasTls){
    "  FAIL 没有 PT_TLS —— 静态 libc 通常带 .tdata，读不出 TLS 段说明产物异常"; $bad++
  } elseif($tlsAlign -lt 64){
    "  FAIL PT_TLS p_align = 0x$($tlsAlign.ToString('x')) < 64，Bionic 会拒载"; $bad++
  } else {
    "  ok   PT_TLS p_align = 0x$($tlsAlign.ToString('x')) (>= 64)"
  }
} else {
  # 动态版：运行时解析 .so，.tdata 交给 libc.so，所以不该有 PT_TLS
  if($hasTls){
    "  FAIL 动态链接的产物带 PT_TLS（align=0x$($tlsAlign.ToString('x'))），不应出现"; $bad++
  } else {
    "  ok   无 PT_TLS（动态链接，由 libc.so 提供 TLS）"
  }
  if(-not $hasInterp){
    "  FAIL 声明为动态，但没有 PT_INTERP"; $bad++
  } else {
    "  ok   有 PT_INTERP（确实动态链接）"
  }
}

# 16KB 页设备（Android 15+）要求所有 PT_LOAD 的 p_align >= 16384。
# 两种链接方式都要查：动态链接的 .so 同样要满足。
# 必须按 16384 判，按 4KB 判的话回到 -z max-page-size 之前的产物照样"通过"。
if($loadAligns.Count -eq 0){
  "  FAIL 没有 PT_LOAD 段"; $bad++
} else {
  # 初值必须显式是 [uint64]：写 0xffffffffffffffff 会被 PowerShell 解析成
  # Int64 的 -1，于是每个 align 都"小于 16384"，全段假红。
  $min=[uint64]::MaxValue
  foreach($a in $loadAligns){ if($a -lt $min){ $min=$a } }
  if($min -lt 16384){
    "  FAIL 最小 PT_LOAD p_align = 0x$($min.ToString('x')) < 0x4000，16KB 页设备会拒载"; $bad++
  } else {
    "  ok   所有 PT_LOAD p_align >= 0x4000（$($loadAligns.Count) 段，兼容 16KB 页）"
  }
}

# 16KB 页还要求 LOAD 段的 offset % align == vaddr % align，
# 否则加载器把文件偏移映射到虚拟地址时对不齐。
$lbad=0
for($i=0;$i -lt $phnum;$i++){
  $o=[int]($phoff + [uint64]$i * $phentsize)
  if([BitConverter]::ToUInt32($bytes,$o) -ne $PT_LOAD){ continue }
  $offset=[BitConverter]::ToUInt64($bytes,$o+8)
  $vaddr=[BitConverter]::ToUInt64($bytes,$o+16)
  $al=[BitConverter]::ToUInt64($bytes,$o+48)
  if($al -eq 0){ continue }
  if(($offset % $al) -ne ($vaddr % $al)){ $lbad++ }
}
if($lbad){
  "  FAIL $lbad 个 LOAD 段 offset%align != vaddr%align"; $bad++
} else {
  "  ok   LOAD 段 offset%align == vaddr%align"
}

if($bad){ "  加载期自检：$bad 项不合格"; exit 1 } else { "  加载期自检：全部通过" }
exit 0
