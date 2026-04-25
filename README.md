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

1. **找不到 OpenSSL**
   - 不是致命问题，项目会自动降级为 HTTP-only。
   - 如需 HTTPS，再安装 OpenSSL 并重新配置 CMake。

2. **推送 GitHub 报 TLS 错误**
   - 常见于代理/网络波动，建议检查代理连通性后重试。

