# 夜间暴力测试：ASan 下的 fuzz 目标（5 个输入面 + runtime/render）+ 真包猴子。
#
#   powershell -ExecutionPolicy Bypass -File script\nightly-fuzz.ps1 [-Hours 9] [-BuildDir build/asan-msvc]
#
# 支持两种构建：
#   * llvm-mingw/clang 构建（--preset mingw-asan-fuzz）：目标是 libFuzzer，覆盖率引导
#   * MSVC ASan 构建：目标退化成暴力变异循环，用 OA_FUZZ_SECONDS 切片
# 脚本自动识别（探测 libFuzzer 的 -max_total_time 选项）。
#
# 产物都在 <BuildDir>/nightly/：
#   summary.txt        进度/结果（一行一件事）
#   <target>.log       每个目标的完整输出
#   artifacts/         crash 样本（libFuzzer 的 crash-* 或暴力模式的最后一个输入）
#   corpus/<target>/   累积语料（下一轮接着喂，越跑越深）
param(
    [double]$Hours = 9,
    [string]$BuildDir = "build/mingw-asan",
    [int]$SliceMinutes = 20,
    [int]$MonkeyMinutes = 30,
    [int]$StressMinutes = 10,
    [string]$Engine = "auto"
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$out = Join-Path $BuildDir "nightly"
$artifacts = Join-Path $out "artifacts"
$corpusRoot = Join-Path $out "corpus"
New-Item -ItemType Directory -Force -Path $out, $artifacts, $corpusRoot | Out-Null
$summary = Join-Path $out "summary.txt"

function Say([string]$msg) {
    $line = "{0}  {1}" -f (Get-Date -Format 'MM-dd HH:mm:ss'), $msg
    Write-Host $line
    Add-Content -Path $summary -Value $line
}

# MSVC 的 ASan 是动态运行时：DLL 在 VS 的 bin 里，先垫进 PATH，否则一启动就报缺 DLL。
$asanDll = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($asanDll) { $env:PATH = $asanDll.DirectoryName + ";" + $env:PATH }

# ASan 的泄漏检查在 mingw/MSVC 上都容易误报，夜里只关心真崩溃。
$env:ASAN_OPTIONS = "detect_leaks=0:abort_on_error=1:handle_abort=1:detect_container_overflow=0"
$env:UBSAN_OPTIONS = "print_stacktrace=1"

$fuzzDir = Join-Path $BuildDir "tests/fuzz"
if (-not (Test-Path $fuzzDir)) {
    Say "FATAL 找不到 $fuzzDir：先构建 fuzz 目标"
    exit 1
}
function Find-Exe([string]$name) {
    $hit = Get-ChildItem $fuzzDir -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

$deadline = (Get-Date).AddHours($Hours)
$targets = @('psb', 'iet', 'image', 'save', 'runtime', 'render')

# 引擎探测：libFuzzer 认 -max_total_time，暴力模式不认。
if ($Engine -eq 'auto') {
    $probe = Find-Exe "oa_fuzz_psb"
    if ($probe) {
        $help = & $probe "-help=1" 2>&1 | Out-String
        if ($help -match 'max_total_time') { $Engine = 'libfuzzer' } else { $Engine = 'brute' }
    } else {
        $Engine = 'brute'
    }
}
Say "START build=$BuildDir engine=$Engine hours=$Hours slice=${SliceMinutes}m deadline=$($deadline.ToString('HH:mm'))"

$rounds = 0
$crashes = 0
$script:pkgCursor = 0   # 猴子轮转游标：跨轮累加，避免每轮都挑同几个包
# 真包猴子与 fuzz 目标**并行**跑（各占各的核）：每个包跑满 MonkeyMinutes 分钟。
# 包是扫出来的（build/*/root.pfs），新加游戏不用改脚本。
$monkeyJob = $null
$monkeyExe = Find-Exe "oa_monkey"
$packages = @(Get-ChildItem "build/*/root.pfs" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.FullName })
if ($packages.Count -eq 0) {
    Say "SKIP monkey（build/*/root.pfs 一个都没有）"
} elseif (-not $monkeyExe) {
    Say "SKIP monkey（没构建出来）"
} else {
    $monkeyJob = Start-Job -ScriptBlock {
        param($exe, $pkgs, $minutes, $outDir, $deadlineTicks, $summaryPath)
        $i = 0
        while ((Get-Date).Ticks -lt $deadlineTicks) {
            $pkg = $pkgs[$i % $pkgs.Count]
            ++$i
            $seed = Get-Random -Maximum 2147483647
            $name = Split-Path -Leaf (Split-Path -Parent $pkg)
            $trace = Join-Path $outDir ("monkey-{0}-{1}.trace" -f $name, $seed)
            $log = Join-Path $outDir ("monkey-{0}.log" -f $name)
            $stamp = Get-Date -Format 'MM-dd HH:mm:ss'
            Add-Content -Path $summaryPath -Value ("{0}  RUN  monkey {1} minutes={2} seed={3}" -f $stamp, $name, $minutes, $seed)
            & $exe $pkg --minutes $minutes --frames 2000000 --seed $seed --render --trace $trace *>> $log
            $stamp = Get-Date -Format 'MM-dd HH:mm:ss'
            if ($LASTEXITCODE -ne 0) {
                Add-Content -Path $summaryPath -Value ("{0}  CRASH monkey {1} seed={2} trace={3}" -f $stamp, $name, $seed, $trace)
            } else {
                Add-Content -Path $summaryPath -Value ("{0}  OK   monkey {1}" -f $stamp, $name)
                Remove-Item $trace -ErrorAction SilentlyContinue
            }
        }
    } -ArgumentList $monkeyExe, $packages, $MonkeyMinutes, $out, $deadline.Ticks, $summary
}

# 并发压力与 fuzz/猴子**并行**：每个包跑满 StressMinutes，runtime 自己的渲染器
# （tick 里 render_frame，和 app/安卓同一条主线程路径）+ 静音（设备照开，回调
# 线程照跑）。与猴子错开包轮转。
$stressJob = $null
$stressExe = Find-Exe "oa_stress"
if ($packages.Count -eq 0) {
    Say "SKIP stress（build/*/root.pfs 一个都没有）"
} elseif (-not $stressExe) {
    Say "SKIP stress（没构建出来）"
} else {
    $stressJob = Start-Job -ScriptBlock {
        param($exe, $pkgs, $minutes, $outDir, $deadlineTicks, $summaryPath, $offset)
        $i = $offset
        while ((Get-Date).Ticks -lt $deadlineTicks) {
            $pkg = $pkgs[$i % $pkgs.Count]
            ++$i
            $seed = Get-Random -Maximum 2147483647
            $name = Split-Path -Leaf (Split-Path -Parent $pkg)
            $log = Join-Path $outDir ("stress-{0}.log" -f $name)
            $stamp = Get-Date -Format 'MM-dd HH:mm:ss'
            Add-Content -Path $summaryPath -Value ("{0}  RUN  stress {1} minutes={2} seed={3}" -f $stamp, $name, $minutes, $seed)
            & $exe $pkg --minutes $minutes --threads 2 --seed $seed --render --silent *>> $log
            $stamp = Get-Date -Format 'MM-dd HH:mm:ss'
            if ($LASTEXITCODE -ne 0) {
                Add-Content -Path $summaryPath -Value ("{0}  CRASH stress {1} seed={2} log={3}" -f $stamp, $name, $seed, $log)
            } else {
                Add-Content -Path $summaryPath -Value ("{0}  OK   stress {1}" -f $stamp, $name)
            }
        }
    } -ArgumentList $stressExe, $packages, $StressMinutes, $out, $deadline.Ticks, $summary, 3
}

while ((Get-Date) -lt $deadline) {
    ++$rounds
    foreach ($t in $targets) {
        if ((Get-Date) -ge $deadline) { break }
        $exe = Find-Exe "oa_fuzz_$t"
        if (-not $exe) { Say "SKIP $t（没构建出来）"; continue }

        $corpus = Join-Path $corpusRoot $t
        New-Item -ItemType Directory -Force -Path $corpus | Out-Null
        $seed = Get-Random -Maximum 2147483647
        $log = Join-Path $out ("{0}.log" -f $t)
        Say "RUN  $t round=$rounds seed=$seed"

        if ($Engine -eq 'libfuzzer') {
            $artPrefix = Join-Path $artifacts ("{0}-" -f $t)
            & $exe $corpus "-max_total_time=$($SliceMinutes * 60)" "-seed=$seed" `
                "-artifact_prefix=$artPrefix" "-rss_limit_mb=4096" "-timeout=60" `
                "-print_final_stats=1" *>&1 | Tee-Object -FilePath $log -Append | Select-Object -Last 6
        } else {
            $env:OA_FUZZ_SECONDS = "$($SliceMinutes * 60)"
            $env:OA_FUZZ_ITERS = "100000000"
            $env:OA_FUZZ_SEED = "$seed"
            $env:OA_FUZZ_DUMP = Join-Path $artifacts ("{0}-last-{1}.bin" -f $t, $seed)
            & $exe $corpus *>&1 | Tee-Object -FilePath $log -Append | Select-Object -Last 6
        }
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            ++$crashes
            Say "CRASH $t exit=$code"
            Get-ChildItem $artifacts -Filter "$t-*" -ErrorAction SilentlyContinue |
                ForEach-Object { Say "      $($_.Name)" }
        } else {
            Say "OK   $t 无崩溃"
        }
    }


}

if ($monkeyJob) {
    Wait-Job $monkeyJob | Out-Null
    Receive-Job $monkeyJob | Out-Null
    Remove-Job $monkeyJob
}
if ($stressJob) {
    Wait-Job $stressJob | Out-Null
    Receive-Job $stressJob | Out-Null
    Remove-Job $stressJob
}

Say "DONE rounds=$rounds crashes=$crashes"
exit 0
