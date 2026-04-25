#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct HttpResult {
    bool ok = false;
    uint32_t statusCode = 0;
    std::string statusText;
    std::string responseHeadersUtf8;
    std::string responseBodyUtf8;
    std::string requestUrlUtf8;
    std::string errorMessage;
    int64_t elapsedMs = 0;
};

using HttpHeader = std::pair<std::string, std::string>;

// 用于从其它线程取消进行中的请求（对 cpp-httplib 的 Client 调用 stop()）。
struct HttpCancelToken {
    std::mutex mtx;
    void* hRequest = nullptr; // httplib::Client*，仅内部使用
    std::atomic<bool> cancelled{false};
};

void HttpCancelToken_reset(HttpCancelToken* token);
void HttpRequestCancel(HttpCancelToken* token);

// 进程退出前调用（保留接口；OpenSSL/httplib 无 WinHTTP 会话需关闭）
void HttpWinShutdown();

// 启动后尽早调用（保留接口；可在此做后续扩展预热）
bool HttpWinWarmup();

// 同步请求；若构建启用 OpenSSL 则支持 HTTPS，否则仅支持 HTTP。cancel 可为 nullptr。
HttpResult HttpRequestSync(const std::string& method,
                           const std::string& urlUtf8,
                           const std::vector<HttpHeader>& headers,
                           const std::string& bodyUtf8,
                           HttpCancelToken* cancel = nullptr,
                           int timeoutSeconds = 15);
