# 生成不依赖外部 DLL 的 Release 版 squirrel.exe（静态 CRT + 静态 OpenSSL 等）。
# 前提：已安装 Visual Studio 2022（含 MSVC）、Git，并已安装/引导 vcpkg 且设置环境变量 VCPKG_ROOT。
# 用法：在仓库根目录执行  .\scripts\build-standalone.ps1
# 可选：-BuildDir build-static  指定构建目录

param(
    [string]$BuildDir = "build-static"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root

if (-not $env:VCPKG_ROOT) {
    Write-Error "请设置环境变量 VCPKG_ROOT 指向 vcpkg 根目录，例如: `$env:VCPKG_ROOT = 'C:\vcpkg'"
}

$Toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    Write-Error "未找到 vcpkg 工具链文件: $Toolchain"
}

Write-Host "配置 CMake（triplet: x64-windows-static）..." -ForegroundColor Cyan
cmake -S . -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static"

Write-Host "编译 Release..." -ForegroundColor Cyan
cmake --build $BuildDir --config Release

$Exe = Join-Path $Root (Join-Path $BuildDir "Release\squirrel.exe")
if (Test-Path $Exe) {
    Write-Host "完成: $Exe" -ForegroundColor Green
    Write-Host "若仅需 HTTP，也可使用 -DAPITOOL_ENABLE_HTTPS=OFF 进一步减小体积（无需 OpenSSL）。"
} else {
    Write-Error "未找到输出: $Exe"
}
