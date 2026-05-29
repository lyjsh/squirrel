#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct HttpCookie {
    std::string domain;
    std::string path = "/";
    std::string name;
    std::string value;
    bool secure = false;
    bool httpOnly = false;
    int64_t expiresMs = 0; // 0 = session cookie
};

class CookieJar {
public:
    bool enabled = true;

    void LoadFromDisk();
    void SaveToDisk() const;

    void IngestFromResponse(const std::string& responseHeaders, const std::string& requestUrl);
    void ApplyCookieHeader(const std::string& requestUrl,
                           std::vector<std::pair<std::string, std::string>>& headers) const;

    void Upsert(HttpCookie cookie);
    void RemoveAt(size_t index);
    void RemoveExpired();
    void ClearAll();
    void ClearDomain(const std::string& domain);

    const std::vector<HttpCookie>& cookies() const { return cookies_; }

    std::vector<size_t> VisibleIndices(const std::string& requestUrl, bool showAllDomains) const;

    static bool ExtractUrlParts(const std::string& url, bool& https, std::string& host, std::string& path);
    static std::string FormatExpiry(const HttpCookie& cookie);

private:
    void UpsertNoSave(HttpCookie cookie);

    std::vector<HttpCookie> cookies_;

    static std::filesystem::path StoragePath();
    static HttpCookie ParseSetCookie(const std::string& setCookieValue, const std::string& defaultHost,
                                     const std::string& defaultPath, bool requestSecure);
    static bool DomainMatches(const std::string& cookieDomain, const std::string& requestHost);
    static bool PathMatches(const std::string& cookiePath, const std::string& requestPath);
    static bool IsExpired(const HttpCookie& cookie, int64_t nowMs);
    static std::string CookieKey(const HttpCookie& cookie);
};
