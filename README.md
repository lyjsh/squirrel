# squirrel

一个基于 ImGui + GLFW + cpp-httplib 的轻量级桌面 HTTP 调试工具（Windows）。

## 功能

- 支持常见 HTTP 方法：`GET` / `POST` / `PUT` / `PATCH` / `DELETE` / `HEAD` / `OPTIONS`
- URL Query、Headers、Body 可视化编辑
- Body 支持：
  - `raw`（JSON / Text / XML）
  - `x-www-form-urlencoded`
- JSON / XML 一键格式化
- 请求取消、超时自定义（发送按钮旁可设置秒数）
- 左侧历史记录（支持模糊搜索与 `re:` 正则匹配）
- 文件流响应自动下载保存
- 请求历史落盘，重启后自动恢复

## 依赖说明

- `cpp-httplib` 使用本地头文件：`third_party/cpp-httplib/httplib.h`
- OpenSSL 为可选依赖：
  - 找到 OpenSSL：支持 `HTTPS`
  - 未找到 OpenSSL：自动降级为 `HTTP-only`（可正常编译运行）

## 构建

### 使用 CMake（通用）

```bash
cmake -S . -B build
cmake --build build --config Release
```

可执行文件目标名：`squirrel`

### 显式关闭 HTTPS（可选）

```bash
cmake -S . -B build -DAPITOOL_ENABLE_HTTPS=OFF
cmake --build build --config Release
```

### 启用 HTTPS（可选）

确保系统可找到 OpenSSL 开发库（头文件 + lib）。若使用 vcpkg，可在配置时追加：

```bash
-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
```

### 独立 exe（不依赖外部 DLL）

工程已默认 **静态 CRT**（`/MT`）并 **静态** 链入 GLFW、ImGui。若启用 HTTPS 且使用 vcpkg 默认的 `x64-windows` triplet，OpenSSL 会以 **动态库** 形式链接，运行时需要 `libssl-3-x64.dll`、`libcrypto-3-x64.dll` 与 exe 同目录。

要得到 **单个 exe、无需拷贝 DLL**，请使用 vcpkg 的 **静态** triplet，例如：

**PowerShell（推荐，仓库已提供脚本）：**

```powershell
$env:VCPKG_ROOT = 'C:\你的\vcpkg'   # 若尚未设置
.\scripts\build-standalone.ps1
```

输出一般为 `build-static\Release\squirrel.exe`。

**或手动配置 CMake：**

```bash
cmake -S . -B build-static -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build-static --config Release
```

若不需要 HTTPS，可关闭 OpenSSL 以进一步减小体积（同样为单 exe）：

```bash
cmake -S . -B build -DAPITOOL_ENABLE_HTTPS=OFF
cmake --build build --config Release
```

## 运行与数据目录

- 下载文件目录（自动创建）：
  - `C:\Users\<用户名>\Downloads\squirrel-downloads`
- 请求历史日志：
  - `%LOCALAPPDATA%\squirrel\request_history.log`

## 安装包（Inno Setup）

仓库提供安装脚本：`innosetup/apitool.iss`

- 先编译 Release（生成 `squirrel.exe`）
- 再使用 Inno Setup 编译脚本，输出安装包到 `dist/`

## 常见问题

1. **MinGW 运行提示缺少 libwinpthread-1.dll**  
   请在 CLion 中 **Reload CMake Project** 后 **Rebuild**；工程在 MinGW 下会对 `squirrel` 追加 **`-static`**，生成不依赖该 DLL 的 exe。若链接阶段报错（常见于仅有 OpenSSL DLL 而无静态库），可关闭 HTTPS：`-DAPITOOL_ENABLE_HTTPS=OFF`，或改用 **Visual Studio + vcpkg**。临时运行也可把 MinGW 的 `bin\libwinpthread-1.dll` 放在 exe 同目录。

2. **找不到 OpenSSL**
   - 不是致命问题，项目会自动降级为 HTTP-only。
   - 如需 HTTPS，再安装 OpenSSL 并重新配置 CMake。

3. **推送 GitHub 报 TLS 错误**
   - 常见于代理/网络波动，建议检查代理连通性后重试。

