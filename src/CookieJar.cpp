#include "CookieJar.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

std::string TrimCopy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string StripPort(std::string host)
{
    if (host.empty())
        return host;
    if (host.front() == '[') {
        const size_t end = host.find(']');
        if (end != std::string::npos)
            return host.substr(0, end + 1);
        return host;
    }
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon)
        return host.substr(0, colon);
    return host;
}

int64_t NowEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::vector<std::string> HeaderValuesByName(const std::string& rawHeaders, const std::string& keyLower)
{
    std::vector<std::string> values;
    size_t pos = 0;
    while (pos < rawHeaders.size()) {
        const size_t end = rawHeaders.find('\n', pos);
        std::string line = (end == std::string::npos) ? rawHeaders.substr(pos) : rawHeaders.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string k = ToLowerAscii(TrimCopy(line.substr(0, colon)));
            if (k == keyLower)
                values.push_back(TrimCopy(line.substr(colon + 1)));
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return values;
}

std::string DefaultCookiePath(const std::string& requestPath)
{
    if (requestPath.empty() || requestPath[0] != '/')
        return "/";
    const size_t slash = requestPath.find_last_of('/');
    if (slash == 0)
        return "/";
    return requestPath.substr(0, slash);
}

int64_t ParseHttpDateMs(const std::string& raw)
{
    const std::string s = TrimCopy(raw);
    if (s.empty())
        return 0;

    std::tm tm = {};
    const char* formats[] = {
        "%a, %d %b %Y %H:%M:%S GMT",
        "%A, %d-%b-%Y %H:%M:%S GMT",
        "%a, %d-%b-%Y %H:%M:%S GMT",
    };
    for (const char* fmt : formats) {
        std::istringstream iss(s);
        iss >> std::get_time(&tm, fmt);
        if (!iss.fail()) {
#if defined(_WIN32)
            const time_t t = _mkgmtime(&tm);
#else
            const time_t t = timegm(&tm);
#endif
            if (t <= 0)
                return 0;
            return static_cast<int64_t>(t) * 1000;
        }
    }
    return 0;
}

std::filesystem::path BuildAppDataDir()
{
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData)
        return std::filesystem::path(localAppData) / "squirrel";
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile)
        return std::filesystem::path(userProfile) / "AppData" / "Local" / "squirrel";
    return std::filesystem::current_path() / "squirrel-data";
}

constexpr char kFieldSep = '\x1f';
constexpr char kRowSep = '\x1e';

} // namespace

std::filesystem::path CookieJar::StoragePath()
{
    return BuildAppDataDir() / "cookies.dat";
}

bool CookieJar::ExtractUrlParts(const std::string& url, bool& https, std::string& host, std::string& path)
{
    https = false;
    host.clear();
    path = "/";

    std::string u = TrimCopy(url);
    if (u.empty())
        return false;

    size_t rest = 0;
    const size_t schemePos = u.find("://");
    if (schemePos != std::string::npos) {
        const std::string scheme = ToLowerAscii(u.substr(0, schemePos));
        if (scheme == "https")
            https = true;
        else if (scheme != "http")
            return false;
        rest = schemePos + 3;
    }

    if (rest >= u.size())
        return false;

    size_t pathStart = u.find_first_of("/?#", rest);
    if (pathStart == std::string::npos) {
        host = u.substr(rest);
        return !host.empty();
    }

    host = u.substr(rest, pathStart - rest);
    if (host.empty())
        return false;

    if (u[pathStart] == '/')
        path = u.substr(pathStart);
    return true;
}

bool CookieJar::DomainMatches(const std::string& cookieDomain, const std::string& requestHost)
{
    std::string cd = ToLowerAscii(StripPort(cookieDomain));
    std::string rh = ToLowerAscii(StripPort(requestHost));
    if (cd.empty() || rh.empty())
        return false;
    if (!cd.empty() && cd.front() == '.')
        cd.erase(0, 1);
    if (rh == cd)
        return true;
    if (rh.size() > cd.size() && rh.compare(rh.size() - cd.size() - 1, cd.size() + 1, "." + cd) == 0)
        return true;
    return false;
}

bool CookieJar::PathMatches(const std::string& cookiePath, const std::string& requestPath)
{
    std::string cp = cookiePath.empty() ? "/" : cookiePath;
    std::string rp = requestPath.empty() ? "/" : requestPath;
    if (cp == "/")
        return true;
    if (rp.size() < cp.size())
        return false;
    if (rp.compare(0, cp.size(), cp) != 0)
        return false;
    if (cp.back() == '/')
        return true;
    return rp.size() == cp.size() || rp[cp.size()] == '/';
}

bool CookieJar::IsExpired(const HttpCookie& cookie, int64_t nowMs)
{
    if (cookie.expiresMs <= 0)
        return false;
    return cookie.expiresMs <= nowMs;
}

std::string CookieJar::CookieKey(const HttpCookie& cookie)
{
    return ToLowerAscii(cookie.domain) + "\n" + cookie.path + "\n" + cookie.name;
}

HttpCookie CookieJar::ParseSetCookie(const std::string& setCookieValue, const std::string& defaultHost,
                                     const std::string& defaultPath, bool requestSecure)
{
    HttpCookie out;
    out.domain = ToLowerAscii(StripPort(defaultHost));
    out.path = defaultPath.empty() ? "/" : defaultPath;

    std::string value = TrimCopy(setCookieValue);
    if (value.empty())
        return out;

    const size_t semi = value.find(';');
    const std::string pair = semi == std::string::npos ? value : value.substr(0, semi);
    const size_t eq = pair.find('=');
    if (eq == std::string::npos)
        return out;

    out.name = TrimCopy(pair.substr(0, eq));
    out.value = TrimCopy(pair.substr(eq + 1));
    if (out.name.empty())
        return out;

    size_t pos = (semi == std::string::npos) ? value.size() : semi + 1;
    while (pos < value.size()) {
        const size_t next = value.find(';', pos);
        std::string attr = TrimCopy(next == std::string::npos ? value.substr(pos) : value.substr(pos, next - pos));
        pos = (next == std::string::npos) ? value.size() : next + 1;
        if (attr.empty())
            continue;

        const size_t attrEq = attr.find('=');
        const std::string attrName = ToLowerAscii(attrEq == std::string::npos ? attr : attr.substr(0, attrEq));
        const std::string attrVal = attrEq == std::string::npos ? std::string() : TrimCopy(attr.substr(attrEq + 1));

        if (attrName == "domain") {
            std::string d = ToLowerAscii(StripPort(attrVal));
            if (!d.empty() && d.front() == '.')
                d.erase(0, 1);
            if (!d.empty())
                out.domain = d;
        } else if (attrName == "path") {
            if (!attrVal.empty() && attrVal.front() == '/')
                out.path = attrVal;
        } else if (attrName == "expires") {
            const int64_t ms = ParseHttpDateMs(attrVal);
            if (ms > 0)
                out.expiresMs = ms;
        } else if (attrName == "max-age") {
            try {
                const long long sec = std::stoll(attrVal);
                if (sec <= 0)
                    out.expiresMs = NowEpochMs() - 1;
                else
                    out.expiresMs = NowEpochMs() + sec * 1000;
            } catch (...) {
            }
        } else if (attrName == "secure") {
            out.secure = true;
        } else if (attrName == "httponly") {
            out.httpOnly = true;
        }
    }

    if (requestSecure)
        (void)requestSecure;
    return out;
}

void CookieJar::LoadFromDisk()
{
    cookies_.clear();
    const std::filesystem::path path = StoragePath();
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t rowEnd = content.find(kRowSep, pos);
        const std::string row = (rowEnd == std::string::npos) ? content.substr(pos) : content.substr(pos, rowEnd - pos);
        pos = (rowEnd == std::string::npos) ? content.size() : rowEnd + 1;
        if (row.empty())
            continue;

        std::vector<std::string> fields;
        size_t fp = 0;
        while (fp < row.size()) {
            const size_t fe = row.find(kFieldSep, fp);
            fields.push_back((fe == std::string::npos) ? row.substr(fp) : row.substr(fp, fe - fp));
            if (fe == std::string::npos)
                break;
            fp = fe + 1;
        }
        if (fields.size() < 7)
            continue;

        HttpCookie c;
        c.domain = fields[0];
        c.path = fields[1].empty() ? "/" : fields[1];
        c.name = fields[2];
        c.value = fields[3];
        c.secure = fields[4] == "1";
        c.httpOnly = fields[5] == "1";
        try {
            c.expiresMs = std::stoll(fields[6]);
        } catch (...) {
            c.expiresMs = 0;
        }
        if (!c.name.empty() && !c.domain.empty())
            cookies_.push_back(std::move(c));
    }
    RemoveExpired();
}

void CookieJar::SaveToDisk() const
{
    try {
        const std::filesystem::path path = StoragePath();
        std::filesystem::create_directories(path.parent_path());
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return;

        for (size_t i = 0; i < cookies_.size(); ++i) {
            const HttpCookie& c = cookies_[i];
            if (i > 0)
                ofs.put(kRowSep);
            ofs << c.domain << kFieldSep << c.path << kFieldSep << c.name << kFieldSep << c.value << kFieldSep
                << (c.secure ? '1' : '0') << kFieldSep << (c.httpOnly ? '1' : '0') << kFieldSep << c.expiresMs;
        }
    } catch (...) {
    }
}

void CookieJar::UpsertNoSave(HttpCookie cookie)
{
    if (cookie.name.empty() || cookie.domain.empty())
        return;
    if (cookie.path.empty())
        cookie.path = "/";
    cookie.domain = ToLowerAscii(StripPort(cookie.domain));

    const std::string key = CookieKey(cookie);
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                    [&](const HttpCookie& c) { return CookieKey(c) == key; }),
                     cookies_.end());
    if (IsExpired(cookie, NowEpochMs()))
        return;
    cookies_.push_back(std::move(cookie));
}

void CookieJar::Upsert(HttpCookie cookie)
{
    UpsertNoSave(std::move(cookie));
    SaveToDisk();
}

void CookieJar::IngestFromResponse(const std::string& responseHeaders, const std::string& requestUrl)
{
    if (!enabled)
        return;

    bool https = false;
    std::string host;
    std::string path;
    if (!ExtractUrlParts(requestUrl, https, host, path))
        return;

    const std::string defaultPath = DefaultCookiePath(path);
    const std::vector<std::string> setCookies = HeaderValuesByName(responseHeaders, "set-cookie");
    if (setCookies.empty())
        return;

    for (const std::string& sc : setCookies) {
        HttpCookie cookie = ParseSetCookie(sc, host, defaultPath, https);
        if (cookie.name.empty())
            continue;
        if (cookie.secure && !https)
            continue;
        UpsertNoSave(std::move(cookie));
    }
    SaveToDisk();
}

void CookieJar::ApplyCookieHeader(const std::string& requestUrl,
                                  std::vector<std::pair<std::string, std::string>>& headers) const
{
    if (!enabled)
        return;

    bool https = false;
    std::string host;
    std::string path;
    if (!ExtractUrlParts(requestUrl, https, host, path))
        return;

    const int64_t nowMs = NowEpochMs();
    std::vector<const HttpCookie*> matched;
    for (const HttpCookie& c : cookies_) {
        if (IsExpired(c, nowMs))
            continue;
        if (c.secure && !https)
            continue;
        if (!DomainMatches(c.domain, host))
            continue;
        if (!PathMatches(c.path, path))
            continue;
        matched.push_back(&c);
    }
    if (matched.empty())
        return;

    std::sort(matched.begin(), matched.end(), [](const HttpCookie* a, const HttpCookie* b) {
        if (a->path.size() != b->path.size())
            return a->path.size() > b->path.size();
        return a->name < b->name;
    });

    std::string cookieHeader;
    for (const HttpCookie* c : matched) {
        if (!cookieHeader.empty())
            cookieHeader += "; ";
        cookieHeader += c->name;
        cookieHeader += '=';
        cookieHeader += c->value;
    }

    for (const auto& h : headers) {
        if (ToLowerAscii(h.first) == "cookie")
            return;
    }
    headers.emplace_back("Cookie", cookieHeader);
}

void CookieJar::RemoveAt(size_t index)
{
    if (index >= cookies_.size())
        return;
    cookies_.erase(cookies_.begin() + static_cast<std::ptrdiff_t>(index));
    SaveToDisk();
}

void CookieJar::RemoveExpired()
{
    const int64_t nowMs = NowEpochMs();
    const size_t before = cookies_.size();
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                    [&](const HttpCookie& c) { return IsExpired(c, nowMs); }),
                     cookies_.end());
    if (cookies_.size() != before)
        SaveToDisk();
}

void CookieJar::ClearAll()
{
    cookies_.clear();
    SaveToDisk();
}

void CookieJar::ClearDomain(const std::string& domain)
{
    const std::string d = ToLowerAscii(StripPort(domain));
    if (d.empty())
        return;
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                    [&](const HttpCookie& c) { return DomainMatches(c.domain, d); }),
                     cookies_.end());
    SaveToDisk();
}

std::vector<size_t> CookieJar::VisibleIndices(const std::string& requestUrl, bool showAllDomains) const
{
    std::vector<size_t> out;
    bool https = false;
    std::string host;
    std::string path;
    ExtractUrlParts(requestUrl, https, host, path);

    const int64_t nowMs = NowEpochMs();
    for (size_t i = 0; i < cookies_.size(); ++i) {
        const HttpCookie& c = cookies_[i];
        if (IsExpired(c, nowMs))
            continue;
        if (!showAllDomains && !host.empty() && !DomainMatches(c.domain, host))
            continue;
        out.push_back(i);
    }
    return out;
}

std::string CookieJar::FormatExpiry(const HttpCookie& cookie)
{
    if (cookie.expiresMs <= 0)
        return "Session";
    const int64_t nowMs = NowEpochMs();
    if (cookie.expiresMs <= nowMs)
        return "Expired";

    const time_t sec = static_cast<time_t>(cookie.expiresMs / 1000);
    std::tm tm = {};
#if defined(_WIN32)
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm) == 0)
        return "Session";
    return buf;
}
