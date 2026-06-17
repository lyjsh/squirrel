#include "HttpWin.h"
#include "CookieJar.h"
#include "JsonHighlight.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "imgui_stdlib.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace {

std::string TrimCopy(const std::string& s);
enum class RowOp {
    None,
    Add,
    Del
};

std::string ToUpper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string ToLowerCopy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// 模糊过滤：空格分隔的多个关键词均需在不区分大小写的「方法 + 完整 URL」中命中（子串）
bool HistoryMatchesFilter(const char* filter, const std::string& method, const std::string& fullUrl)
{
    if (!filter || !filter[0])
        return true;

    std::string hayRaw = method;
    hayRaw.push_back(' ');
    hayRaw += fullUrl;

    std::string f(filter);
    while (!f.empty() && std::isspace(static_cast<unsigned char>(f.front())))
        f.erase(0, 1);
    while (!f.empty() && std::isspace(static_cast<unsigned char>(f.back())))
        f.pop_back();
    if (f.empty())
        return true;

    // re: 前缀启用正则匹配（忽略大小写），例如：re:^GET\\s+https?://api\\.
    if (f.rfind("re:", 0) == 0) {
        const std::string pattern = f.substr(3);
        if (pattern.empty())
            return true;
        try {
            const std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
            return std::regex_search(hayRaw, re);
        } catch (const std::regex_error&) {
            return false;
        }
    }

    std::string hay = ToLowerCopy(hayRaw);
    f = ToLowerCopy(std::move(f));
    size_t pos = 0;
    while (pos < f.size()) {
        while (pos < f.size() && f[pos] == ' ')
            ++pos;
        if (pos >= f.size())
            break;
        size_t sp = f.find(' ', pos);
        if (sp == std::string::npos)
            sp = f.size();
        const std::string tok = f.substr(pos, sp - pos);
        if (!tok.empty() && hay.find(tok) == std::string::npos)
            return false;
        pos = sp + 1;
    }
    return true;
}

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 0xF]);
        }
    }
    return o;
}

std::string MergeUrlQuery(const std::string& baseUrl,
                          const std::vector<std::tuple<bool, std::string, std::string, std::string>>& params)
{
    std::string q;
    for (const auto& t : params) {
        if (!std::get<0>(t))
            continue;
        const std::string& k = std::get<1>(t);
        const std::string& v = std::get<2>(t);
        if (k.empty())
            continue;
        if (!q.empty())
            q.push_back('&');
        q += UrlEncode(k);
        q.push_back('=');
        q += UrlEncode(v);
    }
    if (q.empty())
        return baseUrl;
    if (baseUrl.find('?') != std::string::npos)
        return baseUrl + '&' + q;
    return baseUrl + '?' + q;
}

std::string BuildFormUrlEncoded(const std::vector<std::tuple<bool, std::string, std::string>>& rows)
{
    std::string out;
    for (const auto& row : rows) {
        if (!std::get<0>(row))
            continue;
        const std::string& k = std::get<1>(row);
        const std::string& v = std::get<2>(row);
        if (k.empty())
            continue;
        if (!out.empty())
            out.push_back('&');
        out += UrlEncode(k);
        out.push_back('=');
        out += UrlEncode(v);
    }
    return out;
}

std::string PercentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char ch) -> int {
                if (ch >= '0' && ch <= '9')
                    return ch - '0';
                if (ch >= 'a' && ch <= 'f')
                    return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F')
                    return ch - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+')
            out.push_back(' ');
        else
            out.push_back(s[i]);
    }
    return out;
}

void SplitUrlAndQuery(const std::string& fullUrl, std::string& baseUrl,
                      std::vector<std::tuple<bool, std::string, std::string, std::string>>& queryRows)
{
    queryRows.clear();
    const size_t schemePos = fullUrl.find("://");
    const size_t searchFrom = (schemePos == std::string::npos) ? 0 : schemePos + 3;
    const size_t qPos = fullUrl.find('?', searchFrom);
    if (qPos == std::string::npos) {
        baseUrl = fullUrl;
        queryRows.emplace_back(true, std::string(), std::string(), std::string());
        return;
    }

    baseUrl = fullUrl.substr(0, qPos);
    std::string query = fullUrl.substr(qPos + 1);
    const size_t hashPos = query.find('#');
    if (hashPos != std::string::npos)
        query = query.substr(0, hashPos);

    if (query.empty()) {
        queryRows.emplace_back(true, std::string(), std::string(), std::string());
        return;
    }

    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        std::string pair = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        pos = (amp == std::string::npos) ? query.size() : amp + 1;
        if (pair.empty())
            continue;
        const size_t eq = pair.find('=');
        const std::string k = PercentDecode(eq == std::string::npos ? pair : pair.substr(0, eq));
        const std::string v = PercentDecode(eq == std::string::npos ? std::string() : pair.substr(eq + 1));
        queryRows.emplace_back(true, k, v, std::string());
    }
    if (queryRows.empty())
        queryRows.emplace_back(true, std::string(), std::string(), std::string());
}

std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string HeaderValueByName(const std::string& rawHeaders, const std::string& keyLower)
{
    size_t pos = 0;
    while (pos < rawHeaders.size()) {
        const size_t end = rawHeaders.find('\n', pos);
        std::string line = (end == std::string::npos)
            ? rawHeaders.substr(pos)
            : rawHeaders.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = ToLowerAscii(TrimCopy(line.substr(0, colon)));
            if (k == keyLower)
                return TrimCopy(line.substr(colon + 1));
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}

std::string FilenameFromContentDisposition(const std::string& contentDisposition)
{
    if (contentDisposition.empty())
        return {};

    const std::string lower = ToLowerAscii(contentDisposition);
    size_t p = lower.find("filename*=");
    if (p != std::string::npos) {
        std::string v = contentDisposition.substr(p + 10);
        const size_t semi = v.find(';');
        if (semi != std::string::npos)
            v = v.substr(0, semi);
        v = TrimCopy(v);
        if (!v.empty() && v.front() == '"' && v.back() == '"' && v.size() >= 2)
            v = v.substr(1, v.size() - 2);
        const size_t quote2 = v.find("''");
        if (quote2 != std::string::npos)
            v = v.substr(quote2 + 2);
        return PercentDecode(v);
    }

    p = lower.find("filename=");
    if (p != std::string::npos) {
        std::string v = contentDisposition.substr(p + 9);
        const size_t semi = v.find(';');
        if (semi != std::string::npos)
            v = v.substr(0, semi);
        v = TrimCopy(v);
        if (!v.empty() && v.front() == '"' && v.back() == '"' && v.size() >= 2)
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

std::string FilenameFromUrl(const std::string& url)
{
    const size_t scheme = url.find("://");
    size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
    start = url.find('/', start);
    if (start == std::string::npos)
        return {};
    size_t end = url.find_first_of("?#", start);
    if (end == std::string::npos)
        end = url.size();
    std::string path = url.substr(start, end - start);
    if (path.empty() || path.back() == '/')
        return {};
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return PercentDecode(name);
}

std::string SanitizeFilename(std::string name)
{
    if (name.empty())
        return {};
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\'
            || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty())
        name = "download.bin";
    return name;
}

bool LooksLikeFileResponse(const HttpResult& r)
{
    const std::string ct = ToLowerAscii(HeaderValueByName(r.responseHeadersUtf8, "content-type"));
    const std::string cd = ToLowerAscii(HeaderValueByName(r.responseHeadersUtf8, "content-disposition"));
    if (cd.find("attachment") != std::string::npos || cd.find("filename=") != std::string::npos)
        return true;

    if (ct.empty())
        return false;
    if (ct.find("application/octet-stream") != std::string::npos
        || ct.find("application/pdf") != std::string::npos
        || ct.find("application/zip") != std::string::npos
        || ct.find("application/vnd") != std::string::npos
        || ct.find("image/") == 0
        || ct.find("audio/") == 0
        || ct.find("video/") == 0) {
        return true;
    }
    return false;
}

std::filesystem::path BuildDownloadDir()
{
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && *userProfile) {
        return std::filesystem::path(userProfile) / "Downloads" / "squirrel-downloads";
    }
    return std::filesystem::current_path() / "downloads";
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

std::filesystem::path HistoryLogPath()
{
    return BuildAppDataDir() / "request_history.log";
}

int64_t NowEpochMs()
{
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

bool SaveResponseToFile(const HttpResult& r, std::filesystem::path& outPath, std::string& err)
{
    err.clear();
    outPath.clear();
    try {
        std::filesystem::path dir = BuildDownloadDir();
        std::filesystem::create_directories(dir);

        std::string filename = FilenameFromContentDisposition(
            HeaderValueByName(r.responseHeadersUtf8, "content-disposition"));
        if (filename.empty())
            filename = FilenameFromUrl(r.requestUrlUtf8);
        if (filename.empty())
            filename = "download.bin";
        filename = SanitizeFilename(filename);

        std::filesystem::path path = dir / filename;
        if (std::filesystem::exists(path)) {
            const std::filesystem::path stem = path.stem();
            const std::filesystem::path ext = path.extension();
            for (int i = 1; i <= 9999; ++i) {
                std::filesystem::path candidate =
                    dir / (stem.string() + "_" + std::to_string(i) + ext.string());
                if (!std::filesystem::exists(candidate)) {
                    path = candidate;
                    break;
                }
            }
        }

        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) {
            err = "无法创建文件";
            return false;
        }
        ofs.write(r.responseBodyUtf8.data(),
                  static_cast<std::streamsize>(r.responseBodyUtf8.size()));
        if (!ofs.good()) {
            err = "写入文件失败";
            return false;
        }
        outPath = path;
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

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

bool PrettyFormatJson(const std::string& input, std::string& output, std::string& err)
{
    output.clear();
    err.clear();
    int indent = 0;
    bool inString = false;
    bool escape = false;
    std::vector<char> stack;
    constexpr int kJsonIndentSpaces = 4;
    auto putIndent = [&]() { output.append(static_cast<size_t>(indent) * kJsonIndentSpaces, ' '); };

    for (char c : input) {
        if (inString) {
            output.push_back(c);
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                inString = false;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)))
            continue;

        switch (c) {
        case '"':
            inString = true;
            output.push_back(c);
            break;
        case '{':
        case '[':
            stack.push_back(c);
            output.push_back(c);
            output.push_back('\n');
            ++indent;
            putIndent();
            break;
        case '}':
        case ']': {
            if (stack.empty()
                || (c == '}' && stack.back() != '{')
                || (c == ']' && stack.back() != '[')) {
                err = "JSON 括号不匹配";
                return false;
            }
            stack.pop_back();
            output.push_back('\n');
            if (indent > 0)
                --indent;
            putIndent();
            output.push_back(c);
            break;
        }
        case ',':
            output.push_back(',');
            output.push_back('\n');
            putIndent();
            break;
        case ':':
            output += ": ";
            break;
        default:
            output.push_back(c);
            break;
        }
    }

    if (inString) {
        err = "JSON 字符串未闭合";
        return false;
    }
    if (!stack.empty()) {
        err = "JSON 括号未闭合";
        return false;
    }
    if (output.empty())
        output = input;
    return true;
}

bool PrettyFormatXml(const std::string& input, std::string& output, std::string& err)
{
    output.clear();
    err.clear();
    int indent = 0;
    size_t i = 0;
    auto putIndent = [&]() { output.append(static_cast<size_t>(indent) * 2, ' '); };

    while (i < input.size()) {
        if (input[i] == '<') {
            std::string tag;
            if (input.compare(i, 4, "<!--") == 0) {
                const size_t end = input.find("-->", i + 4);
                if (end == std::string::npos) {
                    err = "XML 注释未闭合";
                    return false;
                }
                tag = input.substr(i, end - i + 3);
                i = end + 3;
            } else if (input.compare(i, 9, "<![CDATA[") == 0) {
                const size_t end = input.find("]]>", i + 9);
                if (end == std::string::npos) {
                    err = "XML CDATA 未闭合";
                    return false;
                }
                tag = input.substr(i, end - i + 3);
                i = end + 3;
            } else if (input.compare(i, 2, "<?") == 0) {
                const size_t end = input.find("?>", i + 2);
                if (end == std::string::npos) {
                    err = "XML 声明未闭合";
                    return false;
                }
                tag = input.substr(i, end - i + 2);
                i = end + 2;
            } else {
                const size_t end = input.find('>', i + 1);
                if (end == std::string::npos) {
                    err = "XML 标签未闭合";
                    return false;
                }
                tag = input.substr(i, end - i + 1);
                i = end + 1;
            }

            const bool isClosing = tag.size() > 1 && tag[1] == '/';
            const bool isDeclLike = tag.size() > 1 && (tag[1] == '?' || tag[1] == '!');
            const bool selfClosing = tag.size() > 2 && tag[tag.size() - 2] == '/';

            if (isClosing && indent > 0)
                --indent;
            putIndent();
            output += tag;
            output.push_back('\n');
            if (!isClosing && !isDeclLike && !selfClosing)
                ++indent;
        } else {
            size_t next = input.find('<', i);
            if (next == std::string::npos)
                next = input.size();
            const std::string text = TrimCopy(input.substr(i, next - i));
            if (!text.empty()) {
                putIndent();
                output += text;
                output.push_back('\n');
            }
            i = next;
        }
    }

    if (!output.empty() && output.back() == '\n')
        output.pop_back();
    if (output.empty())
        output = input;
    return true;
}

bool MethodAllowsBody(const std::string& m)
{
    return m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE" || m == "OPTIONS";
}

bool MethodAllowsMultipart(const std::string& m)
{
    return m == "POST" || m == "PUT";
}

void ApplyMethodBodyDefaults(const std::string& method, int& bodyMode, int& rawContentTypeMode, int& pendingReqTab)
{
    if (!MethodAllowsBody(ToUpper(method))) {
        bodyMode = 0;
        return;
    }
    bodyMode = 1;
    rawContentTypeMode = 0;
    pendingReqTab = 4;
}

#if defined(_WIN32)
bool PickOpenFilePath(std::string& outPath)
{
    wchar_t fileBuf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return false;
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return false;
    outPath.resize(static_cast<size_t>(bytes - 1));
    WideCharToMultiByte(CP_UTF8, 0, fileBuf, -1, outPath.data(), bytes, nullptr, nullptr);
    return true;
}
#else
bool PickOpenFilePath(std::string&)
{
    return false;
}
#endif

std::string GuessMimeTypeFromPath(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "application/octet-stream";
    std::string ext = ToLowerCopy(path.substr(dot + 1));
    if (ext == "json")
        return "application/json";
    if (ext == "xml")
        return "application/xml";
    if (ext == "txt" || ext == "text")
        return "text/plain";
    if (ext == "html" || ext == "htm")
        return "text/html";
    if (ext == "png")
        return "image/png";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "gif")
        return "image/gif";
    if (ext == "webp")
        return "image/webp";
    if (ext == "pdf")
        return "application/pdf";
    if (ext == "zip")
        return "application/zip";
    if (ext == "csv")
        return "text/csv";
    if (ext == "mp4")
        return "video/mp4";
    if (ext == "mp3")
        return "audio/mpeg";
    return "application/octet-stream";
}

bool ReadFileBinary(const std::filesystem::path& path, std::string& out, std::string& err)
{
    err.clear();
    out.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        err = "文件不存在: " + path.string();
        return false;
    }
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) {
        err = "无法读取文件大小";
        return false;
    }
    constexpr std::uintmax_t kMaxBytes = 64ull * 1024ull * 1024ull;
    if (sz > kMaxBytes) {
        err = "文件超过 64MB 上限";
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        err = "无法打开文件";
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(out.data(), static_cast<std::streamsize>(sz));
        if (!ifs.good()) {
            err = "读取文件失败";
            out.clear();
            return false;
        }
    }
    return true;
}

// kind: 0=text, 1=file — (enabled, kind, name, value/path, contentType)
using MultipartRow = std::tuple<bool, int, std::string, std::string, std::string>;

void ResetRequestToBlank(std::string& method,
                         std::string& url,
                         std::vector<std::tuple<bool, std::string, std::string, std::string>>& queryRows,
                         std::vector<std::pair<std::string, std::string>>& headerRows,
                         int& bodyMode,
                         std::string& reqBody,
                         std::string& reqBodyBeforeFormat,
                         int& rawContentTypeMode,
                         std::vector<std::tuple<bool, std::string, std::string>>& formRows,
                         std::vector<MultipartRow>& multipartRows,
                         int& pendingReqTab)
{
    method = "POST";
    url.clear();
    queryRows.clear();
    queryRows.emplace_back(true, std::string(), std::string(), std::string());
    headerRows.clear();
    headerRows.emplace_back();
    bodyMode = 1;
    reqBody.clear();
    reqBodyBeforeFormat.clear();
    rawContentTypeMode = 0;
    formRows.clear();
    formRows.emplace_back(true, std::string(), std::string());
    multipartRows.clear();
    multipartRows.emplace_back(true, 0, std::string(), std::string(), std::string());
    pendingReqTab = 4;
}

bool BuildMultipartParts(const std::vector<MultipartRow>& rows,
                         std::vector<HttpMultipartPart>& out,
                         std::string& err)
{
    out.clear();
    err.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!std::get<0>(rows[i]))
            continue;
        const int kind = std::get<1>(rows[i]);
        const std::string& name = std::get<2>(rows[i]);
        const std::string& value = std::get<3>(rows[i]);
        const std::string& ctypeIn = std::get<4>(rows[i]);
        if (name.empty()) {
            err = "第 " + std::to_string(i + 1) + " 行：字段名不能为空";
            return false;
        }
        HttpMultipartPart part;
        part.name = name;
        if (kind == 1) {
            if (value.empty()) {
                err = "第 " + std::to_string(i + 1) + " 行：请选择文件";
                return false;
            }
            std::string bytes;
            if (!ReadFileBinary(value, bytes, err))
                return false;
            part.content = std::move(bytes);
            part.filename = std::filesystem::path(value).filename().string();
            part.content_type = ctypeIn.empty() ? GuessMimeTypeFromPath(value) : ctypeIn;
        } else {
            part.content = value;
            part.content_type = ctypeIn.empty() ? "text/plain" : ctypeIn;
        }
        out.push_back(std::move(part));
    }
    if (out.empty()) {
        err = "请至少添加一个有效的 form-data 字段";
        return false;
    }
    return true;
}

bool LooksLikeHttpUrlInput(const std::string& rawUrl)
{
    const std::string u = TrimCopy(rawUrl);
    if (u.empty())
        return false;
    if (u.front() == '{' || u.front() == '[')
        return false;
    if (u.find(' ') != std::string::npos || u.find('\t') != std::string::npos)
        return false;

    const size_t schemePos = u.find("://");
    if (schemePos != std::string::npos) {
        const std::string scheme = ToLowerCopy(u.substr(0, schemePos));
        if (scheme != "http" && scheme != "https")
            return false;
        return schemePos + 3 < u.size();
    }

    // 无 scheme 也允许（后续会自动补），但至少要像 host（例如 localhost / 域名 / IPv4）
    if (u == "localhost")
        return true;
    if (u.find('.') != std::string::npos)
        return true;
    return std::isdigit(static_cast<unsigned char>(u.front())) != 0;
}

// Google Material Design 3 浅色主题（Primary #1A73E8）
namespace Material {
constexpr ImVec4 kPrimary(0.102f, 0.451f, 0.910f, 1.f);
constexpr ImVec4 kPrimaryHover(0.098f, 0.404f, 0.824f, 1.f);
constexpr ImVec4 kPrimaryActive(0.082f, 0.349f, 0.706f, 1.f);
constexpr ImVec4 kOnPrimary(1.f, 1.f, 1.f, 1.f);
constexpr ImVec4 kPrimaryContainer(0.827f, 0.890f, 0.992f, 1.f);
constexpr ImVec4 kSurface(1.f, 1.f, 1.f, 1.f);
constexpr ImVec4 kSurfaceDim(0.973f, 0.976f, 0.980f, 1.f);
constexpr ImVec4 kSurfaceVariant(0.945f, 0.953f, 0.957f, 1.f);
constexpr ImVec4 kOutline(0.855f, 0.863f, 0.878f, 1.f);
constexpr ImVec4 kOnSurface(0.125f, 0.129f, 0.141f, 1.f);
constexpr ImVec4 kOnSurfaceVariant(0.373f, 0.388f, 0.408f, 1.f);
constexpr ImVec4 kSuccess(0.094f, 0.502f, 0.220f, 1.f);
constexpr ImVec4 kWarning(0.890f, 0.455f, 0.039f, 1.f);
constexpr ImVec4 kError(0.851f, 0.188f, 0.145f, 1.f);
} // namespace Material

void ApplyMaterialTheme()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 8.f;
    st.WindowRounding = 12.f;
    st.ChildRounding = 12.f;
    st.PopupRounding = 12.f;
    st.ScrollbarRounding = 8.f;
    st.GrabRounding = 8.f;
    st.TabRounding = 8.f;
    st.WindowPadding = ImVec2(16.f, 14.f);
    st.FramePadding = ImVec2(14.f, 10.f);
    st.ItemSpacing = ImVec2(12.f, 10.f);
    st.ItemInnerSpacing = ImVec2(8.f, 6.f);
    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 0.f;
    st.FrameBorderSize = 1.f;
    st.PopupBorderSize = 1.f;
    st.TabBarBorderSize = 0.f;

    using namespace Material;
    ImVec4* c = st.Colors;

    c[ImGuiCol_Text] = kOnSurface;
    c[ImGuiCol_TextDisabled] = kOnSurfaceVariant;
    c[ImGuiCol_WindowBg] = kSurfaceDim;
    c[ImGuiCol_ChildBg] = kSurface;
    c[ImGuiCol_PopupBg] = kSurface;
    c[ImGuiCol_Border] = kOutline;
    c[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.06f);
    c[ImGuiCol_FrameBg] = kSurface;
    c[ImGuiCol_FrameBgHovered] = kPrimaryContainer;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.78f, 0.86f, 0.98f, 1.f);
    c[ImGuiCol_TitleBg] = kSurface;
    c[ImGuiCol_TitleBgActive] = kSurface;
    c[ImGuiCol_TitleBgCollapsed] = kSurfaceDim;
    c[ImGuiCol_CheckMark] = kPrimary;
    c[ImGuiCol_SliderGrab] = kPrimary;
    c[ImGuiCol_SliderGrabActive] = kPrimaryActive;
    c[ImGuiCol_Button] = kSurfaceVariant;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.91f, 0.92f, 0.94f, 1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.86f, 0.88f, 0.91f, 1.f);
    c[ImGuiCol_Header] = kPrimaryContainer;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.76f, 0.86f, 0.98f, 1.f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.68f, 0.80f, 0.96f, 1.f);
    c[ImGuiCol_Separator] = kOutline;
    c[ImGuiCol_SeparatorHovered] = kPrimary;
    c[ImGuiCol_SeparatorActive] = kPrimaryActive;
    c[ImGuiCol_Tab] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_TabHovered] = kPrimaryContainer;
    c[ImGuiCol_TabSelected] = kSurface;
    c[ImGuiCol_TabSelectedOverline] = kPrimary;
    c[ImGuiCol_TabDimmed] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_TabDimmedSelected] = kSurfaceVariant;
    c[ImGuiCol_TabDimmedSelectedOverline] = kPrimary;
    c[ImGuiCol_TabActive] = kSurface;
    c[ImGuiCol_TabUnfocused] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_TabUnfocusedActive] = kSurfaceVariant;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.70f, 0.73f, 0.76f, 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0.60f, 0.65f, 0.72f);
    c[ImGuiCol_ScrollbarGrabActive] = kPrimary;
    c[ImGuiCol_TableHeaderBg] = kSurfaceVariant;
    c[ImGuiCol_TableBorderStrong] = kOutline;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.91f, 0.92f, 0.94f, 1.f);
    c[ImGuiCol_TableRowBg] = kSurface;
    c[ImGuiCol_TableRowBgAlt] = kSurfaceDim;
}

void PushFilledButtonStyle()
{
    using namespace Material;
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPrimaryActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kOnPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.f, 8.f));
}

void PopFilledButtonStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void PushTonalButtonStyle()
{
    using namespace Material;
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimaryContainer);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.86f, 0.98f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.68f, 0.80f, 0.96f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, kPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
}

void PopTonalButtonStyle()
{
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void PushCompactTonalButtonStyle()
{
    using namespace Material;
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimaryContainer);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.86f, 0.98f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.68f, 0.80f, 0.96f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, kPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 4.f));
}

void PopCompactTonalButtonStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void PushOutlinedButtonStyle()
{
    using namespace Material;
    ImGui::PushStyleColor(ImGuiCol_Button, kSurface);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryContainer);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.86f, 0.98f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, kPrimary);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
}

void PopOutlinedButtonStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void PushCopyableFieldStyle(const ImVec4* textColor = nullptr, bool highlighted = false)
{
    using namespace Material;
    if (highlighted)
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kPrimaryContainer);
    else
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 3.f));
    if (textColor)
        ImGui::PushStyleColor(ImGuiCol_Text, *textColor);
}

void PopCopyableFieldStyle(bool hasTextColor)
{
    if (hasTextColor)
        ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void CopyableLine(const char* id, std::string& text, const ImVec4* textColor = nullptr, float width = -1.f,
                  bool highlighted = false)
{
    PushCopyableFieldStyle(textColor, highlighted);
    if (width < 0.f)
        ImGui::SetNextItemWidth(width);
    else if (width > 0.f)
        ImGui::SetNextItemWidth(width);
    else
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f + 6.f);
    ImGui::InputText(id, &text, ImGuiInputTextFlags_ReadOnly);
    PopCopyableFieldStyle(textColor != nullptr);
}

void CopyableBlock(const char* id, std::string& text, const ImVec4* textColor = nullptr)
{
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int lines = std::max(1, JsonHighlight::CountLines(text));
    PushCopyableFieldStyle(textColor);
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextMultiline(id, &text, ImVec2(-1.f, lineH * static_cast<float>(lines)),
                              ImGuiInputTextFlags_ReadOnly);
    PopCopyableFieldStyle(textColor != nullptr);
}

std::string BuildResponseSummaryText(const std::string& statusLine, const std::string& timeLine, size_t lastBodyBytes,
                                     int respBodyLineCount, const std::string& downloadLine)
{
    std::string out = statusLine;
    if (!timeLine.empty()) {
        if (!out.empty())
            out += "  ";
        out += timeLine;
    }
    if (lastBodyBytes > 0) {
        out += " | ";
        out += std::to_string(lastBodyBytes);
        out += " B";
    }
    if (respBodyLineCount > 0) {
        out += " | ";
        out += std::to_string(respBodyLineCount);
        out += " 行";
    }
    if (!downloadLine.empty()) {
        out += " | ";
        out += downloadLine;
    }
    return out;
}

bool DrawRowOpButton(const char* labelWithId)
{
    using namespace Material;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.f, 2.f));
    ImGui::PushStyleColor(ImGuiCol_Button, kSurfaceVariant);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryContainer);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.68f, 0.80f, 0.96f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, kPrimary);
    const bool clicked = ImGui::SmallButton(labelWithId);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

void DrawInlineStatusChip(const char* id, const char* text, bool ok, double shownAt, double now)
{
    using namespace Material;
    if (!text || !text[0])
        return;

    constexpr double kVisibleSec = 2.8;
    constexpr double kFadeSec = 0.6;
    const double elapsed = now - shownAt;
    if (elapsed >= kVisibleSec)
        return;

    float alpha = 1.f;
    if (elapsed > kVisibleSec - kFadeSec)
        alpha = static_cast<float>((kVisibleSec - elapsed) / kFadeSec);

    const ImVec4 bg = ok ? ImVec4(0.878f, 0.949f, 0.894f, alpha)
                         : ImVec4(0.992f, 0.878f, 0.878f, alpha);
    const ImVec4 fg = ok ? ImVec4(kSuccess.x, kSuccess.y, kSuccess.z, alpha)
                         : ImVec4(kError.x, kError.y, kError.z, alpha);

    ImGui::SameLine(0.f, 8.f);
    ImGui::PushID(id);
    const ImVec2 pad(8.f, 3.f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float chipH = textSize.y + pad.y * 2.f;
    const ImVec2 size(textSize.x + pad.x * 2.f, chipH);

    ImGui::InvisibleButton("##chip", size);
    const ImVec2 p0 = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(bg), chipH * 0.5f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), ImGui::GetColorU32(fg), text);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
    ImGui::PopID();
}

// 转圈加载（仅作进行中提示，与服务器响应时间无关）
void DrawInlineSpinner(float radius, float thickness, ImU32 col)
{
    const float s = radius * 2.f + 8.f;
    ImGui::InvisibleButton("##spin", ImVec2(s, s));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 center(p0.x + s * 0.5f, p0.y + s * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = static_cast<float>(ImGui::GetTime()) * 4.f;
    const float pi = 3.14159265f;
    dl->PathClear();
    dl->PathArcTo(center, radius, t, t + pi * 1.5f, 24);
    dl->PathStroke(col, false, thickness);
}

ImVec4 StatusLineColor(int httpCode)
{
    using namespace Material;
    if (httpCode <= 0)
        return kOnSurfaceVariant;
    if (httpCode >= 200 && httpCode < 300)
        return kSuccess;
    if (httpCode >= 300 && httpCode < 400)
        return kWarning;
    if (httpCode >= 400)
        return kError;
    return kOnSurfaceVariant;
}

struct AsyncSlot {
    std::mutex mtx;
    std::atomic<bool> finished{false};
    HttpResult result;
};

std::string ToLowerStr(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string HeadersFingerprint(const std::vector<std::pair<std::string, std::string>>& rows)
{
    std::vector<std::pair<std::string, std::string>> v;
    v.reserve(rows.size());
    for (const auto& p : rows) {
        if (p.first.empty())
            continue;
        v.push_back({ToLowerStr(p.first), p.second});
    }
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second < b.second); });
    std::string o;
    for (const auto& p : v) {
        o += p.first;
        o += '=';
        o += p.second;
        o += '\n';
    }
    return o;
}

std::string RequestFingerprint(const std::string& methodUpper,
                               const std::string& fullUrl,
                               const std::vector<std::pair<std::string, std::string>>& headerRows,
                               const std::string& body)
{
    return methodUpper + "\n" + fullUrl + "\n" + HeadersFingerprint(headerRows) + "\n" + body;
}

struct RequestHistoryEntry {
    std::string fingerprint;
    std::string method;
    std::string url;
    int64_t createdAtMs = 0;
    std::vector<std::tuple<bool, std::string, std::string, std::string>> queryRows;
    std::vector<std::pair<std::string, std::string>> headerRows;
    int bodyMode = 0;
    std::string reqBody;
    int rawContentTypeMode = 0;
    std::vector<std::tuple<bool, std::string, std::string>> formRows;
    std::vector<MultipartRow> multipartRows;
};

void PushHistoryUnique(std::vector<RequestHistoryEntry>& hist, RequestHistoryEntry e)
{
    const std::string& fp = e.fingerprint;
    hist.erase(std::remove_if(hist.begin(), hist.end(),
                              [&](const RequestHistoryEntry& x) { return x.fingerprint == fp; }),
               hist.end());
    hist.insert(hist.begin(), std::move(e));
    constexpr size_t kMax = 200;
    if (hist.size() > kMax)
        hist.resize(kMax);
}

constexpr char kHistFieldSep = '\x1f';
constexpr char kHistRowSep = '\x1e';

std::string SerializeQueryRows(const std::vector<std::tuple<bool, std::string, std::string, std::string>>& rows)
{
    std::string o;
    for (const auto& t : rows) {
        if (!o.empty())
            o.push_back(kHistRowSep);
        o.push_back(std::get<0>(t) ? '1' : '0');
        o.push_back(kHistFieldSep);
        o += std::get<1>(t);
        o.push_back(kHistFieldSep);
        o += std::get<2>(t);
        o.push_back(kHistFieldSep);
        o += std::get<3>(t);
    }
    return o;
}

void DeserializeQueryRows(const std::string& s,
                          std::vector<std::tuple<bool, std::string, std::string, std::string>>& rows)
{
    rows.clear();
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t rowEnd = s.find(kHistRowSep, pos);
        const std::string row = (rowEnd == std::string::npos) ? s.substr(pos) : s.substr(pos, rowEnd - pos);
        pos = (rowEnd == std::string::npos) ? s.size() : rowEnd + 1;
        if (row.empty())
            continue;

        std::vector<std::string> fields;
        size_t fp = 0;
        while (fp < row.size()) {
            const size_t fe = row.find(kHistFieldSep, fp);
            fields.push_back((fe == std::string::npos) ? row.substr(fp) : row.substr(fp, fe - fp));
            if (fe == std::string::npos)
                break;
            fp = fe + 1;
        }
        if (fields.size() < 4)
            continue;
        rows.emplace_back(fields[0] != "0", fields[1], fields[2], fields[3]);
    }
    if (rows.empty())
        rows.emplace_back(true, std::string(), std::string(), std::string());
}

std::string SerializeHeaderRows(const std::vector<std::pair<std::string, std::string>>& rows)
{
    std::string o;
    for (const auto& p : rows) {
        if (!o.empty())
            o.push_back(kHistRowSep);
        o += p.first;
        o.push_back(kHistFieldSep);
        o += p.second;
    }
    return o;
}

void DeserializeHeaderRows(const std::string& s, std::vector<std::pair<std::string, std::string>>& rows)
{
    rows.clear();
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t rowEnd = s.find(kHistRowSep, pos);
        const std::string row = (rowEnd == std::string::npos) ? s.substr(pos) : s.substr(pos, rowEnd - pos);
        pos = (rowEnd == std::string::npos) ? s.size() : rowEnd + 1;
        if (row.empty())
            continue;
        const size_t sep = row.find(kHistFieldSep);
        if (sep == std::string::npos)
            rows.emplace_back(row, std::string());
        else
            rows.emplace_back(row.substr(0, sep), row.substr(sep + 1));
    }
    if (rows.empty())
        rows.emplace_back(std::string(), std::string());
}

std::string SerializeFormRows(const std::vector<std::tuple<bool, std::string, std::string>>& rows)
{
    std::string o;
    for (const auto& t : rows) {
        if (!o.empty())
            o.push_back(kHistRowSep);
        o.push_back(std::get<0>(t) ? '1' : '0');
        o.push_back(kHistFieldSep);
        o += std::get<1>(t);
        o.push_back(kHistFieldSep);
        o += std::get<2>(t);
    }
    return o;
}

void DeserializeFormRows(const std::string& s, std::vector<std::tuple<bool, std::string, std::string>>& rows)
{
    rows.clear();
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t rowEnd = s.find(kHistRowSep, pos);
        const std::string row = (rowEnd == std::string::npos) ? s.substr(pos) : s.substr(pos, rowEnd - pos);
        pos = (rowEnd == std::string::npos) ? s.size() : rowEnd + 1;
        if (row.empty())
            continue;

        std::vector<std::string> fields;
        size_t fp = 0;
        while (fp < row.size()) {
            const size_t fe = row.find(kHistFieldSep, fp);
            fields.push_back((fe == std::string::npos) ? row.substr(fp) : row.substr(fp, fe - fp));
            if (fe == std::string::npos)
                break;
            fp = fe + 1;
        }
        if (fields.size() < 3)
            continue;
        rows.emplace_back(fields[0] != "0", fields[1], fields[2]);
    }
    if (rows.empty())
        rows.emplace_back(true, std::string(), std::string());
}

std::string SerializeMultipartRows(const std::vector<MultipartRow>& rows)
{
    std::string o;
    for (const auto& t : rows) {
        if (!o.empty())
            o.push_back(kHistRowSep);
        o.push_back(std::get<0>(t) ? '1' : '0');
        o.push_back(kHistFieldSep);
        o += std::to_string(std::get<1>(t));
        o.push_back(kHistFieldSep);
        o += std::get<2>(t);
        o.push_back(kHistFieldSep);
        o += std::get<3>(t);
        o.push_back(kHistFieldSep);
        o += std::get<4>(t);
    }
    return o;
}

void DeserializeMultipartRows(const std::string& s, std::vector<MultipartRow>& rows)
{
    rows.clear();
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t rowEnd = s.find(kHistRowSep, pos);
        const std::string row = (rowEnd == std::string::npos) ? s.substr(pos) : s.substr(pos, rowEnd - pos);
        pos = (rowEnd == std::string::npos) ? s.size() : rowEnd + 1;
        if (row.empty())
            continue;
        std::vector<std::string> fields;
        size_t fp = 0;
        while (fp < row.size()) {
            const size_t fe = row.find(kHistFieldSep, fp);
            fields.push_back((fe == std::string::npos) ? row.substr(fp) : row.substr(fp, fe - fp));
            if (fe == std::string::npos)
                break;
            fp = fe + 1;
        }
        if (fields.size() < 5)
            continue;
        rows.emplace_back(fields[0] != "0", std::stoi(fields[1]), fields[2], fields[3], fields[4]);
    }
    if (rows.empty())
        rows.emplace_back(true, 0, std::string(), std::string(), std::string());
}

std::string MultipartFingerprint(const std::vector<MultipartRow>& rows)
{
    std::string o;
    for (const auto& t : rows) {
        if (!std::get<0>(t))
            continue;
        o += std::to_string(std::get<1>(t));
        o += ':';
        o += std::get<2>(t);
        o += '=';
        o += std::get<3>(t);
        o += ';';
    }
    return o;
}

void AppendPayloadField(std::string& payload, const char* key, const std::string& value)
{
    if (!payload.empty())
        payload.push_back('&');
    payload += key;
    payload.push_back('=');
    payload += UrlEncode(value);
}

std::string SerializeHistoryPayload(const RequestHistoryEntry& e)
{
    std::string payload;
    AppendPayloadField(payload, "bodyMode", std::to_string(e.bodyMode));
    AppendPayloadField(payload, "rawMode", std::to_string(e.rawContentTypeMode));
    AppendPayloadField(payload, "body", e.reqBody);
    AppendPayloadField(payload, "query", SerializeQueryRows(e.queryRows));
    AppendPayloadField(payload, "headers", SerializeHeaderRows(e.headerRows));
    AppendPayloadField(payload, "form", SerializeFormRows(e.formRows));
    AppendPayloadField(payload, "multipart", SerializeMultipartRows(e.multipartRows));
    return payload;
}

void ApplyHistoryPayload(const std::string& payload, RequestHistoryEntry& e)
{
    size_t pos = 0;
    while (pos < payload.size()) {
        const size_t amp = payload.find('&', pos);
        const std::string pair = (amp == std::string::npos) ? payload.substr(pos) : payload.substr(pos, amp - pos);
        pos = (amp == std::string::npos) ? payload.size() : amp + 1;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = pair.substr(0, eq);
        const std::string value = PercentDecode(pair.substr(eq + 1));
        if (key == "bodyMode")
            e.bodyMode = std::stoi(value);
        else if (key == "rawMode")
            e.rawContentTypeMode = std::stoi(value);
        else if (key == "body")
            e.reqBody = value;
        else if (key == "query")
            DeserializeQueryRows(value, e.queryRows);
        else if (key == "headers")
            DeserializeHeaderRows(value, e.headerRows);
        else if (key == "form")
            DeserializeFormRows(value, e.formRows);
        else if (key == "multipart")
            DeserializeMultipartRows(value, e.multipartRows);
    }
}

void WriteHistoryLogEntry(std::ostream& ofs, const RequestHistoryEntry& e)
{
    const int64_t ts = (e.createdAtMs > 0) ? e.createdAtMs : NowEpochMs();
    ofs << "v2\t" << ts << '\t' << UrlEncode(ToUpper(e.method)) << '\t' << UrlEncode(e.url) << '\t'
        << UrlEncode(SerializeHistoryPayload(e)) << '\n';
}

void AppendHistoryLogEntry(const RequestHistoryEntry& e)
{
    try {
        const std::filesystem::path p = HistoryLogPath();
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::app | std::ios::binary);
        if (!ofs)
            return;
        WriteHistoryLogEntry(ofs, e);
    } catch (...) {
    }
}

std::vector<std::string> SplitHistoryLogLine(const std::string& line)
{
    std::vector<std::string> fields;
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(pos));
            break;
        }
        fields.push_back(line.substr(pos, tab - pos));
        pos = tab + 1;
    }
    return fields;
}

void LoadHistoryFromLog(std::vector<RequestHistoryEntry>& history)
{
    try {
        const std::filesystem::path p = HistoryLogPath();
        if (!std::filesystem::exists(p))
            return;
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs)
            return;

        std::string line;
        while (std::getline(ifs, line)) {
            if (line.empty())
                continue;

            const std::vector<std::string> fields = SplitHistoryLogLine(line);
            RequestHistoryEntry he;

            if (fields.size() >= 5 && fields[0] == "v2") {
                try {
                    he.createdAtMs = std::stoll(fields[1]);
                } catch (...) {
                    he.createdAtMs = 0;
                }
                he.method = ToUpper(TrimCopy(PercentDecode(fields[2])));
                he.url = TrimCopy(PercentDecode(fields[3]));
                ApplyHistoryPayload(PercentDecode(fields[4]), he);
            } else if (fields.size() >= 3) {
                try {
                    he.createdAtMs = std::stoll(fields[0]);
                } catch (...) {
                    he.createdAtMs = 0;
                }
                he.method = ToUpper(TrimCopy(PercentDecode(fields[1])));
                const std::string fullUrl = TrimCopy(PercentDecode(fields[2]));
                SplitUrlAndQuery(fullUrl, he.url, he.queryRows);
            } else {
                continue;
            }

            if (he.method.empty() || !LooksLikeHttpUrlInput(MergeUrlQuery(he.url, he.queryRows)))
                continue;

            const std::string fullUrl = MergeUrlQuery(he.url, he.queryRows);
            const std::string bodyOut = (he.bodyMode == 1) ? he.reqBody
                : (he.bodyMode == 2 ? BuildFormUrlEncoded(he.formRows)
                                    : (he.bodyMode == 3 ? MultipartFingerprint(he.multipartRows) : std::string()));
            he.fingerprint = RequestFingerprint(he.method, fullUrl, he.headerRows, bodyOut);
            PushHistoryUnique(history, std::move(he));
        }
    } catch (...) {
        // 历史加载失败不影响主流程
    }
}

void RewriteHistoryLog(const std::vector<RequestHistoryEntry>& history)
{
    try {
        const std::filesystem::path p = HistoryLogPath();
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return;

        for (auto it = history.rbegin(); it != history.rend(); ++it)
            WriteHistoryLogEntry(ofs, *it);
    } catch (...) {
        // 日志重写失败不影响主流程
    }
}

} // namespace

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

int main()
{
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    GLFWwindow* window = glfwCreateWindow(1280, 820, "squirrel", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Segoe UI（Google 风格无衬线）+ 微软雅黑合并，保证中英文显示
    ImFont* fontEditorUtf8 = nullptr;
    ImFont* fontTitle = nullptr;
    ImFont* fontCode = nullptr;
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
        ImFont* ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &cfg,
                                                  io.Fonts->GetGlyphRangesDefault());
        if (!ui)
            ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 19.0f, &cfg, ranges);
        if (ui) {
            ImFontConfig cfgMerge;
            cfgMerge.MergeMode = true;
            cfgMerge.OversampleH = 2;
            cfgMerge.OversampleV = 2;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 19.0f, &cfgMerge, ranges);
            io.FontDefault = ui;
        }
        fontEditorUtf8 = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];

        ImFontConfig cfgTitle;
        cfgTitle.OversampleH = 2;
        cfgTitle.OversampleV = 2;
        fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 26.0f, &cfgTitle,
                                                 io.Fonts->GetGlyphRangesDefault());
        if (!fontTitle)
            fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 26.0f, &cfgTitle,
                                                     io.Fonts->GetGlyphRangesDefault());
        if (fontTitle) {
            ImFontConfig cfgTitleMerge;
            cfgTitleMerge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyhbd.ttc", 26.0f, &cfgTitleMerge, ranges);
        }

        ImFontConfig cfgCode;
        cfgCode.OversampleH = 2;
        cfgCode.OversampleV = 2;
        constexpr float kCodeLatinPx = 15.0f;
        constexpr float kCodeCjkPx = 17.0f;
        fontCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kCodeCjkPx, &cfgCode, ranges);
        if (!fontCode)
            fontCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", kCodeCjkPx, &cfgCode, ranges);
        if (fontCode) {
            ImFontConfig cfgCodeMerge;
            cfgCodeMerge.MergeMode = true;
            cfgCodeMerge.OversampleH = 2;
            cfgCodeMerge.OversampleV = 2;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", kCodeLatinPx, &cfgCodeMerge,
                                         io.Fonts->GetGlyphRangesDefault());
        } else {
            fontCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", kCodeLatinPx, &cfgCode,
                                                    io.Fonts->GetGlyphRangesDefault());
            if (!fontCode)
                fontCode = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\cour.ttf", kCodeLatinPx, &cfgCode,
                                                        io.Fonts->GetGlyphRangesDefault());
            if (fontCode) {
                ImFontConfig cfgCodeMerge;
                cfgCodeMerge.MergeMode = true;
                cfgCodeMerge.OversampleH = 2;
                cfgCodeMerge.OversampleV = 2;
                io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kCodeCjkPx, &cfgCodeMerge, ranges);
            }
        }
        if (!fontCode)
            fontCode = fontEditorUtf8;
    }

    ImGui::StyleColorsLight();
    ApplyMaterialTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();
    // 首帧前上传字体纹理、预热 WinHTTP 会话，减少首次交互卡顿
    ImGui_ImplOpenGL2_CreateFontsTexture();
    (void)HttpWinWarmup();

    std::string method = "POST";
    std::string url;
    std::vector<std::tuple<bool, std::string, std::string, std::string>> queryRows;
    queryRows.emplace_back(true, std::string(), std::string(), std::string());
    std::vector<std::pair<std::string, std::string>> headerRows;
    headerRows.emplace_back(std::string(), std::string());
    int bodyMode = 1;
    std::string reqBody;
    std::string reqBodyBeforeFormat;
    int rawContentTypeMode = 0;
    std::vector<std::tuple<bool, std::string, std::string>> formRows;
    formRows.emplace_back(true, std::string(), std::string());
    std::vector<MultipartRow> multipartRows;
    multipartRows.emplace_back(true, 0, std::string(), std::string(), std::string());
    int requestTimeoutSec = 15;
    std::string bodyFormatStatus;
    bool bodyFormatOk = true;
    double bodyFormatStatusAt = 0.0;
    std::string downloadLine;
    int historyCleanupRange = 0;
    std::string historyCleanupStatus;
    bool settingsPanelOpen = false;
    static bool settingsPanelWasOpen = false;
    static ImVec2 settingsPanelStoredSize(520.f, 220.f);
    float reqPaneRatio = 0.50f;

    std::string respBodyText;
    std::string respBodyBeforeFormat;
    std::string respFormatStatus;
    bool respFormatOk = true;
    double respFormatStatusAt = 0.0;
    std::string respHdrText;
    bool cookieShowAllDomains = true;
    std::string cookieActionStatus;
    std::string cookieDraftDomain;
    std::string cookieDraftName;
    std::string cookieDraftValue;
    std::string cookieDraftPath = "/";
    std::string statusLine = "就绪";
    std::string timeLine;
    int lastHttpCode = 0;
    size_t lastBodyBytes = 0;

    AsyncSlot async;
    std::atomic<bool> inFlight{false};
    HttpCancelToken httpCancel;
    double reqStartTime = 0.0;

    std::vector<RequestHistoryEntry> requestHistory;
    LoadHistoryFromLog(requestHistory);
    RewriteHistoryLog(requestHistory);
    int selectedHistoryIdx = -1;
    bool draftRequestMode = true;

    CookieJar cookieJar;
    cookieJar.LoadFromDisk();

    int pendingReqTab = 4; // 首次启动默认打开 Body（JSON）

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (async.finished.exchange(false)) {
            HttpResult r;
            {
                std::lock_guard<std::mutex> lock(async.mtx);
                r = std::move(async.result);
            }
            inFlight = false;
            if (r.ok) {
                lastHttpCode = static_cast<int>(r.statusCode);
                lastBodyBytes = r.responseBodyUtf8.size();
                statusLine = std::to_string(r.statusCode)
                    + (r.statusText.empty() ? std::string(" OK") : (" " + r.statusText));
                timeLine = std::to_string(r.elapsedMs) + " ms";
                respHdrText = r.responseHeadersUtf8;
                downloadLine.clear();
                if (LooksLikeFileResponse(r)) {
                    std::filesystem::path savedPath;
                    std::string saveErr;
                    if (SaveResponseToFile(r, savedPath, saveErr)) {
                        downloadLine = "已下载文件: " + savedPath.string();
                        respBodyText = "[文件流响应]\n已保存到:\n" + savedPath.string()
                            + "\n\n大小: " + std::to_string(r.responseBodyUtf8.size()) + " B";
                    } else {
                        respBodyText = "[文件流响应]\n保存失败: " + saveErr;
                    }
                } else {
                    respBodyText = r.responseBodyUtf8;
                }
                if (cookieJar.enabled) {
                    cookieJar.IngestFromResponse(r.responseHeadersUtf8, r.requestUrlUtf8);
                }
                respBodyBeforeFormat.clear();
                respFormatStatus.clear();
            } else {
                lastHttpCode = 0;
                lastBodyBytes = 0;
                if (r.errorMessage == "请求已取消") {
                    statusLine = "已取消";
                } else {
                    statusLine = "错误";
                }
                timeLine.clear();
                respHdrText.clear();
                respBodyText = r.errorMessage;
                downloadLine.clear();
                respBodyBeforeFormat.clear();
                respFormatStatus.clear();
            }
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        const float menuBarH = 56.f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Material::kSurface);
        ImGui::BeginChild("top_app_bar", ImVec2(0, menuBarH), false, ImGuiWindowFlags_NoScrollbar);
        {
            const float lineH = ImGui::GetFrameHeight();
            const float availY = ImGui::GetContentRegionAvail().y;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (availY - lineH) * 0.5f));
            if (fontTitle)
                ImGui::PushFont(fontTitle);
            ImGui::PushStyleColor(ImGuiCol_Text, Material::kPrimary);
            ImGui::TextUnformatted("Squirrel");
            ImGui::PopStyleColor();
            if (fontTitle)
                ImGui::PopFont();
            ImGui::SameLine(ImGui::GetWindowWidth() - 108.f);
            PushTonalButtonStyle();
            if (ImGui::Button("设置", ImVec2(88.f, 0.f)))
                settingsPanelOpen = true;
            PopTonalButtonStyle();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        const float sidebarW = 280.f;
        const float mainH = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("main_row", ImVec2(0, mainH), false);

        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Material::kSurface);
        ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
        ImGui::PushStyleColor(ImGuiCol_Text, Material::kOnSurface);
        ImGui::TextUnformatted("请求历史");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        static char historyFilterBuf[256] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##histfilter", "搜索历史…（正则: re:pattern）", historyFilterBuf,
                                 sizeof(historyFilterBuf));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::BeginChild("hist_list", ImVec2(0, 0), false);
        bool anyShown = false;

        {
            const float draftRowH = ImGui::GetTextLineHeightWithSpacing() + 4.f;
            const float draftRowW = std::max(1.f, ImGui::GetContentRegionAvail().x);
            if (ImGui::Selectable("##draftrow", draftRequestMode, 0, ImVec2(draftRowW, draftRowH))) {
                ResetRequestToBlank(method, url, queryRows, headerRows, bodyMode, reqBody, reqBodyBeforeFormat,
                                    rawContentTypeMode, formRows, multipartRows, pendingReqTab);
                draftRequestMode = true;
                selectedHistoryIdx = -1;
            }

            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f;
            const float textX = rowMin.x + 6.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 plusCol =
                ImGui::GetColorU32(draftRequestMode ? Material::kPrimary : Material::kOnSurfaceVariant);
            const ImU32 labelCol =
                ImGui::GetColorU32(draftRequestMode ? Material::kOnSurface : Material::kOnSurfaceVariant);
            dl->AddText(ImVec2(textX, textY), plusCol, "+");
            const float plusW = ImGui::CalcTextSize("+").x;
            dl->AddText(ImVec2(textX + plusW + 6.f, textY), labelCol, "新建请求");

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("空白请求（POST + JSON Body）");
        }
        ImGui::Separator();
        ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(requestHistory.size()); ++i) {
            const RequestHistoryEntry& e = requestHistory[static_cast<size_t>(i)];
            const std::string full = MergeUrlQuery(e.url, e.queryRows);
            if (!HistoryMatchesFilter(historyFilterBuf, e.method, full))
                continue;
            anyShown = true;

            ImGui::PushID(i);
            const bool sel = (selectedHistoryIdx == i);
            const float rowH = ImGui::GetTextLineHeightWithSpacing() + 4.f;
            const float rowW = std::max(1.f, ImGui::GetContentRegionAvail().x);

            const std::string m = ToUpper(e.method);
            ImVec4 mcol = Material::kOnSurfaceVariant;
            if (m == "GET")
                mcol = Material::kSuccess;
            else if (m == "POST")
                mcol = Material::kWarning;
            else if (m == "PUT")
                mcol = Material::kPrimary;
            else if (m == "PATCH")
                mcol = ImVec4(0.576f, 0.204f, 0.902f, 1.f);
            else if (m == "DELETE")
                mcol = Material::kError;

            if (ImGui::Selectable("##histrow", sel, 0, ImVec2(rowW, rowH))) {
                selectedHistoryIdx = i;
                draftRequestMode = false;
                method = e.method;
                url = e.url;
                queryRows = e.queryRows;
                if (queryRows.empty())
                    queryRows.emplace_back(true, std::string(), std::string(), std::string());
                headerRows = e.headerRows;
                if (headerRows.empty())
                    headerRows.emplace_back();
                bodyMode = e.bodyMode;
                reqBody = e.reqBody;
                reqBodyBeforeFormat.clear();
                rawContentTypeMode = e.rawContentTypeMode;
                formRows = e.formRows;
                if (formRows.empty())
                    formRows.emplace_back(true, std::string(), std::string());
                multipartRows = e.multipartRows;
                if (multipartRows.empty())
                    multipartRows.emplace_back(true, 0, std::string(), std::string(), std::string());
                if (e.bodyMode >= 1)
                    pendingReqTab = 4;
            }

            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f;
            const float textX = rowMin.x + 6.f;

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(textX, textY), ImGui::GetColorU32(mcol), m.c_str());
            const float methodW = ImGui::CalcTextSize(m.c_str()).x;
            const float urlX = textX + methodW + 10.f;
            const float urlMaxX = rowMax.x - 6.f;
            ImGui::PushClipRect(ImVec2(urlX, rowMin.y), ImVec2(urlMaxX, rowMax.y), true);
            dl->AddText(ImVec2(urlX, textY), ImGui::GetColorU32(ImGuiCol_Text), full.c_str());
            ImGui::PopClipRect();

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n单击加载  |  双击复制 URL", full.c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    ImGui::SetClipboardText(full.c_str());
            }
            ImGui::PopID();
        }
        if (requestHistory.empty()) {
            static std::string histEmptyHint = "发送请求后将显示在此\n相同请求只保留一条";
            CopyableBlock("##hist_empty", histEmptyHint, &Material::kOnSurfaceVariant);
        } else if (!anyShown) {
            static std::string histNoMatchHint = "无匹配记录";
            CopyableLine("##hist_nomatch", histNoMatchHint, &Material::kOnSurfaceVariant);
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.f, 12.f);

        const ImGuiWindowFlags workspaceChildFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Material::kSurface);
        ImGui::BeginChild("workspace", ImVec2(0, 0), true, workspaceChildFlags);

        if (ImGui::BeginTabBar("reqwin_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("HTTP 请求")) {

        ImGui::BeginChild("url_row", ImVec2(0, 52), false);
        ImGui::AlignTextToFramePadding();
        ImGui::SetNextItemWidth(112);
        const char* methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
        int mi = 0;
        for (int i = 0; i < 7; ++i) {
            if (method == methods[i]) {
                mi = i;
                break;
            }
        }
        if (ImGui::Combo("##m", &mi, methods, IM_ARRAYSIZE(methods))) {
            const std::string prevMethod = method;
            method = methods[mi];
            if (method != prevMethod)
                ApplyMethodBodyDefaults(method, bodyMode, rawContentTypeMode, pendingReqTab);
        }

        ImGui::SameLine();
        const float rightPanelW = inFlight.load() ? 350.f : 230.f;
        ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - rightPanelW));
        ImGui::InputTextWithHint("##url", "输入请求 URL", &url);

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("超时(s)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.f);
        ImGui::BeginDisabled(inFlight.load());
        ImGui::InputInt("##timeoutsec", &requestTimeoutSec, 0, 0);
        ImGui::EndDisabled();
        requestTimeoutSec = std::clamp(requestTimeoutSec, 1, 600);
        ImGui::SameLine();
        if (inFlight.load()) {
            DrawInlineSpinner(10.f, 3.f, ImGui::GetColorU32(Material::kPrimary));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const double waited = ImGui::GetTime() - reqStartTime;
            static std::string inflightText;
            char inflightBuf[32];
            snprintf(inflightBuf, sizeof(inflightBuf), "请求中 %.1fs", static_cast<float>(waited));
            inflightText = inflightBuf;
            CopyableLine("##inflight", inflightText, &Material::kOnSurfaceVariant);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("此处为本地已等待时间（界面实时刷新）。完成后响应区「耗时」为整次请求在客户端测得的往返时间。");
            ImGui::SameLine();
            PushOutlinedButtonStyle();
            if (ImGui::Button("取消", ImVec2(72, 0)))
            {
                statusLine = "取消中…";
                HttpRequestCancel(&httpCancel);
            }
            PopOutlinedButtonStyle();
        } else {
            PushFilledButtonStyle();
            if (ImGui::Button("发送", ImVec2(96, 0)) && !inFlight.load()) {
                const bool validUrl = LooksLikeHttpUrlInput(url);
                if (!validUrl) {
                    statusLine = "错误";
                    timeLine.clear();
                    respHdrText.clear();
                    respBodyText = "URL 无效：请输入 http(s) 地址或域名（例如 https://example.com）";
                    lastHttpCode = 0;
                    lastBodyBytes = 0;
                    downloadLine.clear();
                    respBodyBeforeFormat.clear();
                    respFormatStatus.clear();
                } else {
                    const std::string m = ToUpper(method);
                    std::string fullUrl = MergeUrlQuery(url, queryRows);
                    std::vector<HttpHeader> hdrs;
                    for (const auto& p : headerRows) {
                        if (p.first.empty())
                            continue;
                        hdrs.emplace_back(p.first, p.second);
                    }
                    std::string bodyOut;
                    std::string bodyContentType;
                    std::vector<HttpMultipartPart> multipartParts;
                    std::string prepareErr;

                    if (bodyMode == 1 && MethodAllowsBody(m))
                        bodyOut = reqBody;
                    else if (bodyMode == 2 && MethodAllowsBody(m)) {
                        bodyOut = BuildFormUrlEncoded(formRows);
                        bodyContentType = "application/x-www-form-urlencoded";
                    } else if (bodyMode == 3 && MethodAllowsBody(m)) {
                        if (!MethodAllowsMultipart(m)) {
                            prepareErr = "form-data 文件上传仅支持 POST / PUT 方法";
                        } else if (!BuildMultipartParts(multipartRows, multipartParts, prepareErr)) {
                        } else {
                            bodyOut = MultipartFingerprint(multipartRows);
                        }
                    }

                    if (!prepareErr.empty()) {
                        statusLine = "错误";
                        timeLine.clear();
                        respHdrText.clear();
                        respBodyText = prepareErr;
                        lastHttpCode = 0;
                        lastBodyBytes = 0;
                        downloadLine.clear();
                    } else {
                    HttpCancelToken_reset(&httpCancel);
                    reqStartTime = ImGui::GetTime();
                    inFlight = true;
                    statusLine = "请求中…";
                    timeLine.clear();
                    lastHttpCode = 0;
                    lastBodyBytes = 0;

                    if (bodyMode == 1 && MethodAllowsBody(m)) {
                        if (rawContentTypeMode == 0)
                            bodyContentType = "application/json; charset=utf-8";
                        else if (rawContentTypeMode == 1)
                            bodyContentType = "text/plain; charset=utf-8";
                        else
                            bodyContentType = "application/xml; charset=utf-8";
                    }

                    if (!bodyOut.empty() && !bodyContentType.empty()) {
                        bool hasContentType = false;
                        for (const auto& h : hdrs) {
                            if (ToLowerStr(h.first) == "content-type") {
                                hasContentType = true;
                                break;
                            }
                        }
                        if (!hasContentType)
                            hdrs.emplace_back("Content-Type", bodyContentType);
                    }

                    if (cookieJar.enabled) {
                        cookieJar.ApplyCookieHeader(fullUrl, hdrs);
                    }

                    {
                        const int64_t createdAtMs = NowEpochMs();
                        RequestHistoryEntry he;
                        he.fingerprint = RequestFingerprint(m, fullUrl, headerRows, bodyOut);
                        he.method = method;
                        he.url = url;
                        he.createdAtMs = createdAtMs;
                        he.queryRows = queryRows;
                        he.headerRows = headerRows;
                        he.bodyMode = bodyMode;
                        he.reqBody = reqBody;
                        he.rawContentTypeMode = rawContentTypeMode;
                        he.formRows = formRows;
                        he.multipartRows = multipartRows;
                        AppendHistoryLogEntry(he);
                        PushHistoryUnique(requestHistory, std::move(he));
                        draftRequestMode = false;
                        selectedHistoryIdx = 0;
                    }

                    AsyncSlot* slot = &async;
                    HttpCancelToken* pcancel = &httpCancel;
                    const int timeoutSec = requestTimeoutSec;
                    std::thread([slot, m, fullUrl, hdrs, bodyOut, multipartParts, pcancel, timeoutSec]() {
                        const std::vector<HttpMultipartPart>* ptr =
                            multipartParts.empty() ? nullptr : &multipartParts;
                        HttpResult r = HttpRequestSync(m, fullUrl, hdrs, bodyOut, pcancel, timeoutSec, ptr);
                        std::lock_guard<std::mutex> lock(slot->mtx);
                        slot->result = std::move(r);
                        slot->finished = true;
                    }).detach();
                    }
                }
            }
            PopFilledButtonStyle();
        }
        ImGui::EndChild();

        ImGui::Spacing();

        const float splitterH = 8.f;
        const float totalH = ImGui::GetContentRegionAvail().y;
        const float minPaneH = 140.f;
        const float maxReqH = std::max(minPaneH, totalH - minPaneH - splitterH);
        const float reqBlockH = std::clamp(totalH * reqPaneRatio, minPaneH, maxReqH);
        const float respBlockH = std::max(minPaneH, totalH - reqBlockH - splitterH);

        const ImGuiWindowFlags reqTabChildFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Material::kSurfaceDim);
        ImGui::BeginChild("req_tabs_region", ImVec2(0, reqBlockH), true, reqTabChildFlags);
        if (ImGui::BeginTabBar("rtabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Params", nullptr, pendingReqTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (pendingReqTab == 0)
                    pendingReqTab = -1;
                RowOp qOp = RowOp::None;
                int qOpAt = -1;
                if (ImGui::BeginTable("pt", 4,
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH
                                          | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 48.f);
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < queryRows.size(); ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        bool on = std::get<0>(queryRows[i]);
                        ImGui::Checkbox(("##qon" + std::to_string(i)).c_str(), &on);
                        std::get<0>(queryRows[i]) = on;
                        ImGui::TableSetColumnIndex(1);
                        const float btnW = 22.f;
                        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                        ImGui::PushItemWidth(std::max(40.f, ImGui::GetContentRegionAvail().x - (btnW * 2.f + spacing * 2.f)));
                        ImGui::InputText(("##qk" + std::to_string(i)).c_str(), &std::get<1>(queryRows[i]));
                        ImGui::PopItemWidth();
                        ImGui::SameLine();
                        if (DrawRowOpButton(("+##qadd" + std::to_string(i)).c_str())) {
                            qOp = RowOp::Add;
                            qOpAt = static_cast<int>(i);
                        }
                        ImGui::SameLine();
                        if (DrawRowOpButton(("-##qdel" + std::to_string(i)).c_str())) {
                            qOp = RowOp::Del;
                            qOpAt = static_cast<int>(i);
                        }
                        ImGui::TableSetColumnIndex(2);
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##qv" + std::to_string(i)).c_str(), &std::get<2>(queryRows[i]));
                        ImGui::PopItemWidth();
                        ImGui::TableSetColumnIndex(3);
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##qd" + std::to_string(i)).c_str(), &std::get<3>(queryRows[i]));
                        ImGui::PopItemWidth();
                    }
                    ImGui::EndTable();
                }
                if (qOp == RowOp::Add && qOpAt >= 0) {
                    queryRows.insert(queryRows.begin() + (qOpAt + 1),
                                     std::make_tuple(true, std::string(), std::string(), std::string()));
                }
                if (qOp == RowOp::Del && qOpAt >= 0) {
                    if (queryRows.size() > 1)
                        queryRows.erase(queryRows.begin() + qOpAt);
                    else
                        queryRows[0] = std::make_tuple(true, std::string(), std::string(), std::string());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Auth")) {
                static std::string authHint = "鉴权方式（占位，后续可接 Bearer / Basic 等）";
                CopyableBlock("##auth_hint", authHint, &Material::kOnSurfaceVariant);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Headers##req_headers")) {
                RowOp hOp = RowOp::None;
                int hOpAt = -1;
                if (ImGui::BeginTable("ht", 2,
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Key");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < headerRows.size(); ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const float btnW = 22.f;
                        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                        ImGui::PushItemWidth(std::max(40.f, ImGui::GetContentRegionAvail().x - (btnW * 2.f + spacing * 2.f)));
                        ImGui::InputText(("##hk" + std::to_string(i)).c_str(), &headerRows[i].first);
                        ImGui::PopItemWidth();
                        ImGui::SameLine();
                        if (DrawRowOpButton(("+##hadd" + std::to_string(i)).c_str())) {
                            hOp = RowOp::Add;
                            hOpAt = static_cast<int>(i);
                        }
                        ImGui::SameLine();
                        if (DrawRowOpButton(("-##hdel" + std::to_string(i)).c_str())) {
                            hOp = RowOp::Del;
                            hOpAt = static_cast<int>(i);
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##hv" + std::to_string(i)).c_str(), &headerRows[i].second);
                        ImGui::PopItemWidth();
                    }
                    ImGui::EndTable();
                }
                if (hOp == RowOp::Add && hOpAt >= 0)
                    headerRows.insert(headerRows.begin() + (hOpAt + 1), std::make_pair(std::string(), std::string()));
                if (hOp == RowOp::Del && hOpAt >= 0) {
                    if (headerRows.size() > 1)
                        headerRows.erase(headerRows.begin() + hOpAt);
                    else
                        headerRows[0] = std::make_pair(std::string(), std::string());
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Cookies")) {
                ImGui::Checkbox("自动管理 Cookie", &cookieJar.enabled);
                ImGui::SameLine();
                ImGui::Checkbox("显示全部域名", &cookieShowAllDomains);

                bool httpsDummy = false;
                std::string currentHost;
                std::string currentPathDummy;
                CookieJar::ExtractUrlParts(MergeUrlQuery(url, queryRows), httpsDummy, currentHost, currentPathDummy);

                ImGui::Spacing();
                PushTonalButtonStyle();
                if (ImGui::Button("清理过期")) {
                    cookieJar.RemoveExpired();
                    cookieActionStatus = "已清理过期 Cookie";
                }
                PopTonalButtonStyle();
                ImGui::SameLine();
                PushOutlinedButtonStyle();
                if (ImGui::Button("清空当前域名")) {
                    if (currentHost.empty()) {
                        cookieActionStatus = "请先填写有效 URL";
                    } else {
                        cookieJar.ClearDomain(currentHost);
                        cookieActionStatus = "已清空 " + currentHost + " 的 Cookie";
                    }
                }
                PopOutlinedButtonStyle();
                ImGui::SameLine();
                PushOutlinedButtonStyle();
                if (ImGui::Button("清空全部")) {
                    cookieJar.ClearAll();
                    cookieActionStatus = "Cookie 已全部清空";
                }
                PopOutlinedButtonStyle();
                if (!cookieActionStatus.empty()) {
                    ImGui::SameLine();
                    static std::string cookieStatusCopy;
                    cookieStatusCopy = cookieActionStatus;
                    CopyableLine("##cookie_status_top", cookieStatusCopy, &Material::kOnSurfaceVariant);
                }

                constexpr float kManualPanelMinH = 104.f;
                const float remainingH = ImGui::GetContentRegionAvail().y;
                const float tableH = std::max(64.f, remainingH - kManualPanelMinH);

                int cookieDeleteAt = -1;
                const std::string previewUrl = MergeUrlQuery(url, queryRows);
                const std::vector<size_t> visibleCookies =
                    cookieJar.VisibleIndices(previewUrl, cookieShowAllDomains);

                ImGui::BeginChild("cookie_table_region", ImVec2(0, tableH), true);
                if (ImGui::BeginTable("cookie_tbl", 7,
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH
                                          | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
                                          | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Domain", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed, 80.f);
                    ImGui::TableSetupColumn("Expires", ImGuiTableColumnFlags_WidthFixed, 136.f);
                    ImGui::TableSetupColumn("Secure", ImGuiTableColumnFlags_WidthFixed, 56.f);
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                    if (visibleCookies.empty()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (cookieJar.cookies().empty()) {
                            static std::string noCookieHint = "暂无 Cookie";
                            CopyableLine("##no_cookie", noCookieHint, &Material::kOnSurfaceVariant, -1.f);
                        } else {
                            static std::string noMatchCookieHint =
                                "当前 URL 无匹配 Cookie，可勾选「显示全部域名」";
                            CopyableLine("##no_match_cookie", noMatchCookieHint, &Material::kOnSurfaceVariant, -1.f);
                        }
                    }
                    for (size_t vi = 0; vi < visibleCookies.size(); ++vi) {
                        const size_t idx = visibleCookies[vi];
                        const HttpCookie& c = cookieJar.cookies()[idx];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        std::string cookieDom = c.domain;
                        CopyableLine(("##cdom" + std::to_string(idx)).c_str(), cookieDom, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(1);
                        std::string cookieName = c.name;
                        CopyableLine(("##cname" + std::to_string(idx)).c_str(), cookieName, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(2);
                        std::string cookieVal = c.value;
                        CopyableLine(("##cval" + std::to_string(idx)).c_str(), cookieVal, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(3);
                        std::string cookiePath = c.path;
                        CopyableLine(("##cpath" + std::to_string(idx)).c_str(), cookiePath, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(4);
                        std::string cookieExp = CookieJar::FormatExpiry(c);
                        CopyableLine(("##cexp" + std::to_string(idx)).c_str(), cookieExp, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(5);
                        std::string cookieSec = c.secure ? "Yes" : "No";
                        CopyableLine(("##csec" + std::to_string(idx)).c_str(), cookieSec, nullptr, -1.f);
                        ImGui::TableSetColumnIndex(6);
                        if (ImGui::SmallButton(("X##cd" + std::to_string(idx)).c_str()))
                            cookieDeleteAt = static_cast<int>(idx);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();

                if (cookieDeleteAt >= 0) {
                    cookieJar.RemoveAt(static_cast<size_t>(cookieDeleteAt));
                    cookieActionStatus = "已删除 Cookie";
                }

                ImGui::BeginChild("cookie_manual_region", ImVec2(0, 0), true);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("手动添加");

                const float fieldSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
                const float addBtnW = 88.f;
                const float rowAvail = ImGui::GetContentRegionAvail().x;
                const float domainW = std::max(100.f, rowAvail * 0.22f);
                const float nameW = std::max(80.f, rowAvail * 0.16f);
                const float pathW = 80.f;
                const float valueW =
                    std::max(80.f, rowAvail - domainW - nameW - pathW - addBtnW - fieldSpacing * 4.f);

                ImGui::SetNextItemWidth(domainW);
                ImGui::InputTextWithHint("##ckdom", "Domain（留空用当前域名）", &cookieDraftDomain);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(nameW);
                ImGui::InputTextWithHint("##ckname", "Name *", &cookieDraftName);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(valueW);
                ImGui::InputTextWithHint("##ckval", "Value", &cookieDraftValue);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(pathW);
                ImGui::InputTextWithHint("##ckpath", "Path", &cookieDraftPath);
                ImGui::SameLine();
                PushTonalButtonStyle();
                const bool addClicked = ImGui::Button("添加", ImVec2(addBtnW, 0));
                PopTonalButtonStyle();

                auto tryAddCookie = [&]() {
                    std::string domain = TrimCopy(cookieDraftDomain);
                    if (domain.empty())
                        domain = currentHost;
                    if (domain.empty()) {
                        cookieActionStatus = "请填写 Domain，或在上方 URL 中填写域名";
                        return;
                    }
                    const std::string name = TrimCopy(cookieDraftName);
                    if (name.empty()) {
                        cookieActionStatus = "请填写 Name";
                        return;
                    }
                    HttpCookie c;
                    c.domain = domain;
                    c.name = name;
                    c.value = cookieDraftValue;
                    c.path = TrimCopy(cookieDraftPath).empty() ? "/" : TrimCopy(cookieDraftPath);
                    cookieJar.Upsert(std::move(c));
                    cookieDraftName.clear();
                    cookieDraftValue.clear();
                    cookieActionStatus = "Cookie 已添加（共 " + std::to_string(cookieJar.cookies().size()) + " 条）";
                };

                if (addClicked)
                    tryAddCookie();

                if (!cookieActionStatus.empty()) {
                    static std::string cookieStatusCopy2;
                    cookieStatusCopy2 = cookieActionStatus;
                    CopyableLine("##cookie_status_bottom", cookieStatusCopy2,
                                 cookieActionStatus.rfind("已添加", 0) == 0 ? &Material::kSuccess
                                                                            : &Material::kError);
                } else {
                    static std::string cookieHint =
                        "响应 Set-Cookie 会自动写入；Headers 中手动设置 Cookie 头时不会自动覆盖。";
                    CopyableBlock("##cookie_hint", cookieHint, &Material::kOnSurfaceVariant);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Body##req_body", nullptr,
                                    pendingReqTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (pendingReqTab == 4)
                    pendingReqTab = -1;
                const bool allow = MethodAllowsBody(ToUpper(method));
                ImGui::BeginDisabled(!allow);
                const char* bitems[] = {"无", "raw", "x-www-form-urlencoded", "form-data（文件上传）"};
                ImGui::SetNextItemWidth(260.f);
                ImGui::Combo("Body 类型", &bodyMode, bitems, IM_ARRAYSIZE(bitems));
                if (bodyMode == 1) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Content-Type");
                    ImGui::SameLine();
                    const char* ctypeItems[] = {"JSON", "Text", "XML"};
                    ImGui::SetNextItemWidth(170.f);
                    ImGui::Combo("##rawctype", &rawContentTypeMode, ctypeItems, IM_ARRAYSIZE(ctypeItems));
                }
                ImGui::BeginDisabled(bodyMode == 0);
                if (bodyMode == 1) {
                    if (rawContentTypeMode == 0 || rawContentTypeMode == 2) {
                        const bool asJson = (rawContentTypeMode == 0);
                        const bool showingFormatted = !reqBodyBeforeFormat.empty();
                        const char* fmtBtnLabel = showingFormatted ? "恢复原始"
                                                                 : (asJson ? "格式化 JSON" : "格式化 XML");
                        PushCompactTonalButtonStyle();
                        if (ImGui::Button(fmtBtnLabel)) {
                            if (showingFormatted) {
                                reqBody = reqBodyBeforeFormat;
                                reqBodyBeforeFormat.clear();
                                bodyFormatOk = true;
                                bodyFormatStatus = "已恢复原始内容";
                                bodyFormatStatusAt = ImGui::GetTime();
                            } else {
                                std::string formatted;
                                std::string err;
                                const bool ok = asJson
                                    ? PrettyFormatJson(reqBody, formatted, err)
                                    : PrettyFormatXml(reqBody, formatted, err);
                                bodyFormatOk = ok;
                                if (ok) {
                                    reqBodyBeforeFormat = reqBody;
                                    reqBody = formatted;
                                    bodyFormatStatus = asJson ? "JSON 已格式化" : "XML 已格式化";
                                } else {
                                    bodyFormatStatus = err.empty() ? "格式化失败" : err;
                                }
                                bodyFormatStatusAt = ImGui::GetTime();
                            }
                        }
                        PopCompactTonalButtonStyle();
                        if (!bodyFormatStatus.empty()) {
                            const double now = ImGui::GetTime();
                            if (now - bodyFormatStatusAt >= 2.8)
                                bodyFormatStatus.clear();
                            else
                                DrawInlineStatusChip("body_fmt", bodyFormatStatus.c_str(), bodyFormatOk,
                                                     bodyFormatStatusAt, now);
                        }
                    } else {
                        bodyFormatStatus.clear();
                    }

                    if (rawContentTypeMode == 0) {
                        if (ImGui::IsWindowAppearing())
                            ImGui::SetKeyboardFocusHere();
                        JsonHighlight::DrawEditableJsonView("##body_json", reqBody, ImVec2(-1, -40.f), fontCode);
                    } else {
                        JsonHighlight::DrawEditablePlainView("##body_plain", reqBody, ImVec2(-1, -40.f), fontCode);
                    }
                } else if (bodyMode == 2) {
                    RowOp fOp = RowOp::None;
                    int fOpAt = -1;
                    if (ImGui::BeginTable("form_body_tbl", 3,
                                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 48.f);
                        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();
                        for (size_t i = 0; i < formRows.size(); ++i) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            bool on = std::get<0>(formRows[i]);
                            ImGui::Checkbox(("##fon" + std::to_string(i)).c_str(), &on);
                            std::get<0>(formRows[i]) = on;
                            ImGui::TableSetColumnIndex(1);
                            const float btnW = 22.f;
                            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                            ImGui::PushItemWidth(std::max(40.f, ImGui::GetContentRegionAvail().x - (btnW * 2.f + spacing * 2.f)));
                            ImGui::InputText(("##fk" + std::to_string(i)).c_str(), &std::get<1>(formRows[i]));
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (DrawRowOpButton(("+##fadd" + std::to_string(i)).c_str())) {
                                fOp = RowOp::Add;
                                fOpAt = static_cast<int>(i);
                            }
                            ImGui::SameLine();
                            if (DrawRowOpButton(("-##fdel" + std::to_string(i)).c_str())) {
                                fOp = RowOp::Del;
                                fOpAt = static_cast<int>(i);
                            }
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushItemWidth(-1);
                            ImGui::InputText(("##fv" + std::to_string(i)).c_str(), &std::get<2>(formRows[i]));
                            ImGui::PopItemWidth();
                        }
                        ImGui::EndTable();
                    }
                    if (fOp == RowOp::Add && fOpAt >= 0)
                        formRows.insert(formRows.begin() + (fOpAt + 1),
                                        std::make_tuple(true, std::string(), std::string()));
                    if (fOp == RowOp::Del && fOpAt >= 0) {
                        if (formRows.size() > 1)
                            formRows.erase(formRows.begin() + fOpAt);
                        else
                            formRows[0] = std::make_tuple(true, std::string(), std::string());
                    }
                    static std::string formUrlHint =
                        "发送时将自动编码并补充 Content-Type: application/x-www-form-urlencoded";
                    CopyableBlock("##form_url_hint", formUrlHint, &Material::kOnSurfaceVariant);
                } else if (bodyMode == 3) {
                    static std::string mpHint = "multipart/form-data：可混用文本字段与文件字段";
                    CopyableBlock("##mp_hint", mpHint, &Material::kOnSurfaceVariant);
                    if (!MethodAllowsMultipart(ToUpper(method))) {
                        static std::string mpWarn = "form-data 文件上传仅支持 POST / PUT";
                        CopyableLine("##mp_warn", mpWarn, &Material::kWarning, -1.f);
                    }
                    RowOp mpOp = RowOp::None;
                    int mpOpAt = -1;
                    int mpPickFileAt = -1;
                    if (ImGui::BeginTable("mp_body_tbl", 6,
                                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg
                                              | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                          ImVec2(0, -28.f))) {
                        ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 48.f);
                        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 88.f);
                        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Value / 文件", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Content-Type", ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 56.f);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();
                        for (size_t i = 0; i < multipartRows.size(); ++i) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            bool on = std::get<0>(multipartRows[i]);
                            ImGui::Checkbox(("##mpon" + std::to_string(i)).c_str(), &on);
                            std::get<0>(multipartRows[i]) = on;
                            ImGui::TableSetColumnIndex(1);
                            int kind = std::get<1>(multipartRows[i]);
                            const char* kindItems[] = {"文本", "文件"};
                            ImGui::SetNextItemWidth(-1);
                            ImGui::Combo(("##mpkind" + std::to_string(i)).c_str(), &kind, kindItems,
                                         IM_ARRAYSIZE(kindItems));
                            std::get<1>(multipartRows[i]) = kind;
                            ImGui::TableSetColumnIndex(2);
                            const float btnW = 22.f;
                            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                            ImGui::PushItemWidth(std::max(40.f, ImGui::GetContentRegionAvail().x
                                                               - (btnW * 2.f + spacing * 2.f)));
                            ImGui::InputText(("##mpk" + std::to_string(i)).c_str(), &std::get<2>(multipartRows[i]));
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (DrawRowOpButton(("+##mpadd" + std::to_string(i)).c_str())) {
                                mpOp = RowOp::Add;
                                mpOpAt = static_cast<int>(i);
                            }
                            ImGui::SameLine();
                            if (DrawRowOpButton(("-##mpdel" + std::to_string(i)).c_str())) {
                                mpOp = RowOp::Del;
                                mpOpAt = static_cast<int>(i);
                            }
                            ImGui::TableSetColumnIndex(3);
                            ImGui::PushItemWidth(-1);
                            if (kind == 1) {
                                ImGui::InputTextWithHint(("##mpv" + std::to_string(i)).c_str(), "文件路径",
                                                         &std::get<3>(multipartRows[i]));
                            } else {
                                ImGui::InputText(("##mpv" + std::to_string(i)).c_str(), &std::get<3>(multipartRows[i]));
                            }
                            ImGui::PopItemWidth();
                            ImGui::TableSetColumnIndex(4);
                            ImGui::PushItemWidth(-1);
                            ImGui::InputTextWithHint(("##mpct" + std::to_string(i)).c_str(),
                                                     kind == 1 ? "留空自动推断" : "可选", &std::get<4>(multipartRows[i]));
                            ImGui::PopItemWidth();
                            ImGui::TableSetColumnIndex(5);
                            if (kind == 1) {
                                if (ImGui::SmallButton(("选文件##mpf" + std::to_string(i)).c_str()))
                                    mpPickFileAt = static_cast<int>(i);
                            }
                        }
                        ImGui::EndTable();
                    }
                    if (mpOp == RowOp::Add && mpOpAt >= 0) {
                        multipartRows.insert(multipartRows.begin() + (mpOpAt + 1),
                                             MultipartRow{true, 0, std::string(), std::string(), std::string()});
                    }
                    if (mpOp == RowOp::Del && mpOpAt >= 0) {
                        if (multipartRows.size() > 1)
                            multipartRows.erase(multipartRows.begin() + mpOpAt);
                        else
                            multipartRows[0] = MultipartRow{true, 0, std::string(), std::string(), std::string()};
                    }
                    if (mpPickFileAt >= 0 && mpPickFileAt < static_cast<int>(multipartRows.size())) {
                        std::string picked;
                        if (PickOpenFilePath(picked))
                            std::get<3>(multipartRows[static_cast<size_t>(mpPickFileAt)]) = picked;
                    }
                    static std::string mpSendHint = "发送为 multipart/form-data；单文件最大 64MB。";
                    CopyableBlock("##mp_send_hint", mpSendHint, &Material::kOnSurfaceVariant);
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!allow) {
                    static std::string bodyDisabledHint = "当前方法通常不使用 Body。";
                    CopyableLine("##body_disabled_hint", bodyDisabledHint, &Material::kOnSurfaceVariant, -1.f);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scripts")) {
                static std::string scriptsHint = "Pre-request / Tests（占位）";
                CopyableBlock("##scripts_hint", scriptsHint, &Material::kOnSurfaceVariant);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);
        ImGui::InvisibleButton("req_resp_splitter", ImVec2(-1.f, splitterH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const float dy = ImGui::GetIO().MouseDelta.y;
            if (totalH > 1.f) {
                reqPaneRatio = std::clamp(reqPaneRatio + dy / totalH, 0.20f, 0.80f);
            }
        }
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            const ImU32 col = ImGui::GetColorU32(ImGui::IsItemActive()
                                                     ? Material::kPrimary
                                                     : Material::kOutline);
            const float y = (a.y + b.y) * 0.5f;
            dl->AddLine(ImVec2(a.x + 8.f, y), ImVec2(b.x - 8.f, y), col, 2.0f);
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.f);

        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Material::kSurfaceDim);
        ImGui::BeginChild("resp_region", ImVec2(0, respBlockH), true);
        ImGui::AlignTextToFramePadding();
        {
            static std::string respSummaryText;
            static const char* cachedRespBodyData = nullptr;
            static size_t cachedRespBodySize = 0;
            static int cachedRespBodyLineCount = 0;
            if (cachedRespBodyData != respBodyText.data() || cachedRespBodySize != respBodyText.size()) {
                cachedRespBodyData = respBodyText.data();
                cachedRespBodySize = respBodyText.size();
                cachedRespBodyLineCount = respBodyText.empty() ? 0 : JsonHighlight::CountLines(respBodyText);
            }
            respSummaryText =
                BuildResponseSummaryText(statusLine, timeLine, lastBodyBytes, cachedRespBodyLineCount, downloadLine);
            const ImVec4 statusColor = StatusLineColor(lastHttpCode);
            CopyableLine("##resp_summary", respSummaryText, &statusColor, -1.f);
        }

        // 与上方请求区 rtabs 中的「Body / Headers」标签同名会导致 Tab 选中状态与内容错位，需独立 ID
        ImGui::PushID("response_tabs");
        if (ImGui::BeginTabBar("resp_tabs_bar", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Body##resp_body")) {
                const bool respAsJson = JsonHighlight::LooksLikeJson(respBodyText);
                const bool respAsXml = !respAsJson && JsonHighlight::LooksLikeXml(respBodyText);
                const bool showRespFormatBar = respAsJson || respAsXml;
                if (showRespFormatBar) {
                    const bool showingFormatted = !respBodyBeforeFormat.empty();
                    const char* fmtBtnLabel = showingFormatted ? "恢复原始"
                                                               : (respAsJson ? "格式化 JSON" : "格式化 XML");
                    PushCompactTonalButtonStyle();
                    if (ImGui::Button(fmtBtnLabel)) {
                        if (showingFormatted) {
                            respBodyText = respBodyBeforeFormat;
                            respBodyBeforeFormat.clear();
                            respFormatOk = true;
                            respFormatStatus = "已恢复原始内容";
                            respFormatStatusAt = ImGui::GetTime();
                        } else {
                            std::string formatted;
                            std::string err;
                            const bool ok = respAsJson
                                ? PrettyFormatJson(respBodyText, formatted, err)
                                : PrettyFormatXml(respBodyText, formatted, err);
                            respFormatOk = ok;
                            if (ok) {
                                respBodyBeforeFormat = respBodyText;
                                respBodyText = formatted;
                                respFormatStatus = respAsJson ? "JSON 已格式化" : "XML 已格式化";
                            } else {
                                respFormatStatus = err.empty() ? "格式化失败" : err;
                            }
                            respFormatStatusAt = ImGui::GetTime();
                        }
                    }
                    PopCompactTonalButtonStyle();
                    if (!respFormatStatus.empty()) {
                        const double now = ImGui::GetTime();
                        if (now - respFormatStatusAt >= 2.8)
                            respFormatStatus.clear();
                        else
                            DrawInlineStatusChip("resp_fmt", respFormatStatus.c_str(), respFormatOk,
                                                 respFormatStatusAt, now);
                    }
                } else {
                    respFormatStatus.clear();
                }

                const float respViewBottom = showRespFormatBar ? -40.f : -6.f;
                if (showRespFormatBar)
                    ImGui::SameLine();
                PushCompactTonalButtonStyle();
                if (ImGui::Button("复制 Body"))
                    ImGui::SetClipboardText(respBodyText.c_str());
                PopCompactTonalButtonStyle();
                if (respAsJson) {
                    JsonHighlight::DrawSelectableJsonView("##resp_json_view", respBodyText,
                                                          ImVec2(-1, respViewBottom), fontCode);
                } else {
                    JsonHighlight::DrawSelectablePlainView("##resp_plain_view", respBodyText,
                                                           ImVec2(-1, respViewBottom), fontCode);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Headers##resp_headers")) {
                PushCompactTonalButtonStyle();
                if (ImGui::Button("复制 Headers"))
                    ImGui::SetClipboardText(respHdrText.c_str());
                PopCompactTonalButtonStyle();
                JsonHighlight::DrawSelectablePlainView("##resp_hdr_view", respHdrText, ImVec2(-1, -6), fontCode);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        ImGui::EndChild();

        ImGui::End();

        const bool settingsPanelJustOpened = settingsPanelOpen && !settingsPanelWasOpen;
        if (settingsPanelJustOpened) {
            const ImGuiViewport* svp = ImGui::GetMainViewport();
            const ImVec2 workPos = svp->WorkPos;
            const ImVec2 workSize = svp->WorkSize;
            const ImVec2 winSize = settingsPanelStoredSize;
            ImGui::SetNextWindowPos(
                ImVec2(workPos.x + (workSize.x - winSize.x) * 0.5f, workPos.y + (workSize.y - winSize.y) * 0.5f),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);
        }

        if (settingsPanelOpen) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f);
            if (ImGui::Begin("设置", &settingsPanelOpen, ImGuiWindowFlags_NoCollapse)) {
                settingsPanelStoredSize = ImGui::GetWindowSize();
                static std::string settingsHistTitle = "历史记录管理";
                CopyableLine("##settings_hist_title", settingsHistTitle, nullptr, -1.f);
                ImGui::Separator();
                const char* cleanupRanges[] = {"1个月前", "3个月前", "6个月前", "1年前"};
                ImGui::SetNextItemWidth(180.f);
                ImGui::Combo("清理范围", &historyCleanupRange, cleanupRanges, IM_ARRAYSIZE(cleanupRanges));
                ImGui::SameLine();
                PushTonalButtonStyle();
                if (ImGui::Button("清理该时间之前历史")) {
                    int days = 30;
                    if (historyCleanupRange == 1)
                        days = 90;
                    else if (historyCleanupRange == 2)
                        days = 180;
                    else if (historyCleanupRange == 3)
                        days = 365;
                    const int64_t cutoffMs = NowEpochMs() - static_cast<int64_t>(days) * 24 * 60 * 60 * 1000;

                    const size_t before = requestHistory.size();
                    requestHistory.erase(
                        std::remove_if(requestHistory.begin(), requestHistory.end(),
                                       [&](const RequestHistoryEntry& e) {
                                           return e.createdAtMs > 0 && e.createdAtMs < cutoffMs;
                                       }),
                        requestHistory.end());
                    const size_t removed = before - requestHistory.size();
                    RewriteHistoryLog(requestHistory);
                    if (selectedHistoryIdx >= static_cast<int>(requestHistory.size())) {
                        selectedHistoryIdx = -1;
                        draftRequestMode = true;
                    }
                    historyCleanupStatus = "已清理 " + std::to_string(removed) + " 条历史记录";
                }
                PopTonalButtonStyle();
                ImGui::SameLine();
                PushOutlinedButtonStyle();
                if (ImGui::Button("清空全部历史")) {
                    requestHistory.clear();
                    selectedHistoryIdx = -1;
                    draftRequestMode = true;
                    RewriteHistoryLog(requestHistory);
                    historyCleanupStatus = "历史记录已全部清空";
                }
                PopOutlinedButtonStyle();

                if (!historyCleanupStatus.empty()) {
                    ImGui::Spacing();
                    static std::string historyCleanupCopy;
                    historyCleanupCopy = historyCleanupStatus;
                    CopyableLine("##hist_cleanup_status", historyCleanupCopy, &Material::kOnSurfaceVariant, -1.f);
                }

                ImGui::Spacing();
                ImGui::Separator();
                static std::string cookieMgmtTitle = "Cookie 管理";
                CopyableLine("##cookie_mgmt_title", cookieMgmtTitle, nullptr, -1.f);
                static std::string cookieCountText;
                cookieCountText = "共 " + std::to_string(cookieJar.cookies().size()) + " 条 Cookie";
                CopyableLine("##cookie_count", cookieCountText, &Material::kOnSurfaceVariant, -1.f);
                PushOutlinedButtonStyle();
                if (ImGui::Button("清空全部 Cookie")) {
                    cookieJar.ClearAll();
                    cookieActionStatus = "Cookie 已全部清空";
                }
                PopOutlinedButtonStyle();
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
        settingsPanelWasOpen = settingsPanelOpen;

        ImGui::Render();
        const int display_w = static_cast<int>(io.DisplaySize.x);
        const int display_h = static_cast<int>(io.DisplaySize.y);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.973f, 0.976f, 0.980f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    HttpWinShutdown();
    glfwTerminate();
    return 0;
}
