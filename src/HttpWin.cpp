#include "HttpWin.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {

std::string ErrMsg(const char* what, const std::string& detail = {})
{
    std::ostringstream oss;
    oss << what;
    if (!detail.empty())
        oss << " (" << detail << ")";
    return oss.str();
}

bool EnsureScheme(std::string& url)
{
    if (url.empty())
        return false;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
        url.insert(0, "https://");
#else
        url.insert(0, "http://");
#endif
    return true;
}

// 解析出 cpp-httplib::Client 所需的 "http(s)://host[:port]" 与 path[?query][#frag 丢弃]
bool SplitHttpUrl(const std::string& url, bool& https, std::string& baseForClient, std::string& pathAndQuery)
{
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return false;
    std::string scheme = url.substr(0, schemeEnd);
    for (char& c : scheme)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (scheme == "https")
        https = true;
    else if (scheme == "http")
        https = false;
    else
        return false;

    const size_t rest = schemeEnd + 3;
    if (rest >= url.size())
        return false;

    const size_t pathStart = url.find_first_of("/?#", rest);
    std::string authority;
    if (pathStart == std::string::npos) {
        authority = url.substr(rest);
        pathAndQuery = "/";
    } else {
        authority = url.substr(rest, pathStart - rest);
        const char ch = url[pathStart];
        if (ch == '/')
            pathAndQuery = url.substr(pathStart);
        else if (ch == '?')
            pathAndQuery = std::string("/") + url.substr(pathStart);
        else
            pathAndQuery = "/";
    }
    if (authority.empty())
        return false;

    baseForClient = (https ? std::string("https://") : std::string("http://")) + authority;
    return true;
}

void MergeUserHeaders(httplib::Headers& out, const std::vector<HttpHeader>& headers, bool& hasContentType)
{
    hasContentType = false;
    for (const auto& h : headers) {
        if (h.first.empty())
            continue;
        std::string lower = h.first;
        for (char& c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower == "host")
            continue;
        if (lower == "content-type")
            hasContentType = true;
        out.insert({h.first, h.second});
    }
}

std::string HeadersToRawString(const httplib::Headers& hdrs)
{
    std::ostringstream oss;
    for (const auto& kv : hdrs)
        oss << kv.first << ": " << kv.second << "\r\n";
    return oss.str();
}

struct ClientCancelGuard {
    HttpCancelToken* tok;
    httplib::Client* cli;
    ClientCancelGuard(HttpCancelToken* t, httplib::Client* c)
        : tok(t)
        , cli(c)
    {
        if (tok && cli) {
            std::lock_guard<std::mutex> lock(tok->mtx);
            tok->hRequest = cli;
        }
    }
    ~ClientCancelGuard()
    {
        if (tok) {
            std::lock_guard<std::mutex> lock(tok->mtx);
            tok->hRequest = nullptr;
        }
    }
};

} // namespace

bool HttpWinWarmup()
{
    return true;
}

void HttpWinShutdown() {}

void HttpCancelToken_reset(HttpCancelToken* token)
{
    if (!token)
        return;
    std::lock_guard<std::mutex> lock(token->mtx);
    token->cancelled.store(false);
    token->hRequest = nullptr;
}

void HttpRequestCancel(HttpCancelToken* token)
{
    if (!token)
        return;
    token->cancelled.store(true);
    // 避免在 UI 线程里直接 stop() 造成短暂卡顿：改为后台执行取消。
    std::thread([token]() {
        std::lock_guard<std::mutex> lock(token->mtx);
        if (!token->hRequest)
            return;
        auto* client = static_cast<httplib::Client*>(token->hRequest);
        token->hRequest = nullptr;
        client->stop();
    }).detach();
}

HttpResult HttpRequestSync(const std::string& method,
                           const std::string& urlUtf8,
                           const std::vector<HttpHeader>& headers,
                           const std::string& bodyUtf8,
                           HttpCancelToken* cancel,
                           int timeoutSeconds)
{
    HttpResult out;
    const auto t0 = std::chrono::steady_clock::now();
    if (timeoutSeconds <= 0)
        timeoutSeconds = 15;

    std::string meth = method;
    for (char& c : meth)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (meth.empty())
        meth = "GET";

    std::string url = urlUtf8;
    if (!EnsureScheme(url)) {
        out.errorMessage = ErrMsg("URL 为空");
        return out;
    }

    bool https = false;
    std::string base;
    std::string path;
    if (!SplitHttpUrl(url, https, base, path)) {
        out.errorMessage = ErrMsg("URL 解析失败");
        return out;
    }
    out.requestUrlUtf8 = url;
#if !defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    if (https) {
        out.errorMessage = ErrMsg("当前构建未启用 HTTPS（缺少 OpenSSL）");
        return out;
    }
#endif

    httplib::Client cli(base);
    if (!cli.is_valid()) {
        out.errorMessage = ErrMsg("无效的 HTTP(S) 地址");
        return out;
    }

    // 无 OpenSSL 的 HTTP-only 构建下，若自动跟随到 https:// 会失败甚至超时。
    // 这里关闭跟随，让调用方直接看到 30x 与 Location 头，提示切换 HTTPS 构建。
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    cli.set_follow_location(true);
#else
    cli.set_follow_location(false);
#endif
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    cli.enable_server_certificate_verification(true);
#endif
    cli.set_connection_timeout(timeoutSeconds, 0);
    cli.set_read_timeout(timeoutSeconds, 0);
    cli.set_write_timeout(timeoutSeconds, 0);
    cli.set_default_headers({{"User-Agent", "apitool/1.0 (cpp-httplib)"}});

    httplib::Headers hdrs;
    bool hasContentType = false;
    MergeUserHeaders(hdrs, headers, hasContentType);

    const bool sendBody = !bodyUtf8.empty()
        && (meth == "POST" || meth == "PUT" || meth == "PATCH" || meth == "DELETE" || meth == "OPTIONS");

    std::string contentTypeVal = "application/json; charset=utf-8";
    if (hasContentType) {
        for (const auto& h : headers) {
            std::string lower = h.first;
            for (char& c : lower)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (lower == "content-type") {
                contentTypeVal = h.second;
                break;
            }
        }
    }
    ClientCancelGuard guard(cancel, &cli);

    httplib::Result res{};

    if (cancel && cancel->cancelled.load()) {
        out.errorMessage = "请求已取消";
        return out;
    }

    if (meth == "GET")
        res = cli.Get(path, hdrs);
    else if (meth == "HEAD")
        res = cli.Head(path, hdrs);
    else if (meth == "POST") {
        if (sendBody)
            res = cli.Post(path, hdrs, bodyUtf8, contentTypeVal);
        else
            res = cli.Post(path, hdrs);
    } else if (meth == "PUT") {
        if (sendBody)
            res = cli.Put(path, hdrs, bodyUtf8, contentTypeVal);
        else {
            httplib::Request req;
            req.method = "PUT";
            req.path = path;
            req.headers = hdrs;
            res = cli.send(req);
        }
    } else if (meth == "PATCH") {
        if (sendBody)
            res = cli.Patch(path, hdrs, bodyUtf8, contentTypeVal);
        else {
            httplib::Request req;
            req.method = "PATCH";
            req.path = path;
            req.headers = hdrs;
            res = cli.send(req);
        }
    } else if (meth == "DELETE") {
        if (sendBody)
            res = cli.Delete(path, hdrs, bodyUtf8, contentTypeVal);
        else
            res = cli.Delete(path, hdrs);
    } else if (meth == "OPTIONS") {
        if (sendBody) {
            httplib::Request req;
            req.method = "OPTIONS";
            req.path = path;
            req.headers = hdrs;
            if (!hasContentType)
                req.headers.insert({"Content-Type", contentTypeVal});
            req.body = bodyUtf8;
            res = cli.send(req);
        } else
            res = cli.Options(path, hdrs);
    } else {
        httplib::Request req;
        req.method = meth;
        req.path = path;
        req.headers = hdrs;
        if (sendBody) {
            if (!hasContentType)
                req.headers.insert({"Content-Type", contentTypeVal});
            req.body = bodyUtf8;
        }
        res = cli.send(req);
    }

    const auto t1 = std::chrono::steady_clock::now();
    out.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (cancel && cancel->cancelled.load()) {
        out.errorMessage = "请求已取消";
        return out;
    }

    if (!res) {
        out.errorMessage = ErrMsg("请求失败", httplib::to_string(res.error()));
        return out;
    }

    out.statusCode = static_cast<uint32_t>(res->status);
    out.statusText = res->reason;
    out.responseBodyUtf8 = res->body;
    out.responseHeadersUtf8 = HeadersToRawString(res->headers);
    out.ok = true;
    return out;
}
