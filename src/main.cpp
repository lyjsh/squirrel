#include "HttpWin.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"
#include "imgui_stdlib.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
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

namespace {

std::string TrimCopy(const std::string& s);

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

// ImGui 会把标签里首次出现的 "##" 之后隐藏为 ID；URL 中若含 "##" 需打断，否则整段只剩极少可见字符。
std::string EscapeImGuiLabel(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '#' && s[i + 1] == '#') {
            o += "#\xE2\x80\x8B#"; // U+200B，避免被解析为 ##
            i += 2;
        } else {
            o.push_back(s[i]);
            ++i;
        }
    }
    return o;
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

void AppendHistoryLogLine(const std::string& method, const std::string& url)
{
    try {
        const std::filesystem::path p = HistoryLogPath();
        std::filesystem::create_directories(p.parent_path());
        std::ofstream ofs(p, std::ios::app | std::ios::binary);
        if (!ofs)
            return;
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        ofs << ms << '\t' << UrlEncode(method) << '\t' << UrlEncode(url) << '\n';
    } catch (...) {
        // 日志写入失败不影响主流程
    }
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
    auto putIndent = [&]() { output.append(static_cast<size_t>(indent) * 2, ' '); };

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

// Postman 浅色主题（参考官方浅色界面）
void ApplyPostmanLightStyle()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 4.f;
    st.WindowRounding = 0.f;
    st.ChildRounding = 4.f;
    st.PopupRounding = 4.f;
    st.ScrollbarRounding = 4.f;
    st.GrabRounding = 4.f;
    st.TabRounding = 4.f;
    st.WindowPadding = ImVec2(12, 10);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.WindowBorderSize = 1.f;
    st.ChildBorderSize = 1.f;
    st.FrameBorderSize = 1.f;

    ImVec4* c = st.Colors;
    const ImVec4 white(1.f, 1.f, 1.f, 1.f);
    const ImVec4 bgPage(0.96f, 0.96f, 0.97f, 1.f);
    const ImVec4 border(0.88f, 0.88f, 0.90f, 1.f);
    const ImVec4 textMain(0.13f, 0.13f, 0.15f, 1.f);
    const ImVec4 textMuted(0.45f, 0.45f, 0.48f, 1.f);

    c[ImGuiCol_Text] = textMain;
    c[ImGuiCol_TextDisabled] = textMuted;
    c[ImGuiCol_WindowBg] = bgPage;
    c[ImGuiCol_ChildBg] = white;
    c[ImGuiCol_PopupBg] = white;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0.04f);
    c[ImGuiCol_FrameBg] = white;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.97f, 0.97f, 0.98f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.94f, 0.95f, 0.97f, 1.f);
    c[ImGuiCol_TitleBg] = white;
    c[ImGuiCol_TitleBgActive] = white;
    c[ImGuiCol_CheckMark] = ImVec4(0.15f, 0.55f, 0.95f, 1.f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.55f, 0.95f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.93f, 0.93f, 0.94f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.88f, 0.88f, 0.90f, 1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.82f, 0.82f, 0.85f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.94f, 0.95f, 0.97f, 1.f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.92f, 0.96f, 1.f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.86f, 0.90f, 0.96f, 1.f);
    c[ImGuiCol_Separator] = border;
    // Tab：选中项用橙色高亮（与 Send 按钮一致），未选中为浅灰；悬停统一略提亮橙色，避免选中 Tab 悬停时变灰
    const ImVec4 tabOrange(1.0f, 0.424f, 0.216f, 1.f);
    const ImVec4 tabOrangeHover(1.0f, 0.52f, 0.32f, 1.f);
    const ImVec4 tabOrangeUnfocused(0.98f, 0.50f, 0.30f, 1.f);
    c[ImGuiCol_Tab] = ImVec4(0.93f, 0.93f, 0.94f, 1.f);
    c[ImGuiCol_TabHovered] = tabOrangeHover;
    c[ImGuiCol_TabSelected] = tabOrange;
    c[ImGuiCol_TabSelectedOverline] = ImVec4(1.0f, 0.65f, 0.40f, 1.f);
    c[ImGuiCol_TabDimmed] = ImVec4(0.94f, 0.94f, 0.95f, 1.f);
    c[ImGuiCol_TabDimmedSelected] = tabOrangeUnfocused;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(1.0f, 0.58f, 0.38f, 1.f);
    c[ImGuiCol_TabActive] = tabOrange;
    c[ImGuiCol_TabUnfocused] = ImVec4(0.94f, 0.94f, 0.95f, 1.f);
    c[ImGuiCol_TabUnfocusedActive] = tabOrangeUnfocused;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.96f, 0.96f, 0.97f, 1.f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.75f, 0.78f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.65f, 0.65f, 0.70f, 1.f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.55f, 0.60f, 1.f);
}

void PushSendButtonStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.424f, 0.216f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.38f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.32f, 0.14f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
}

void PopSendButtonStyle()
{
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
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
    if (httpCode <= 0)
        return ImVec4(0.35f, 0.35f, 0.38f, 1.f);
    if (httpCode >= 200 && httpCode < 300)
        return ImVec4(0.10f, 0.62f, 0.28f, 1.f);
    if (httpCode >= 300 && httpCode < 400)
        return ImVec4(0.75f, 0.45f, 0.10f, 1.f);
    if (httpCode >= 400)
        return ImVec4(0.85f, 0.22f, 0.18f, 1.f);
    return ImVec4(0.35f, 0.35f, 0.38f, 1.f);
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
    std::vector<std::tuple<bool, std::string, std::string, std::string>> queryRows;
    std::vector<std::pair<std::string, std::string>> headerRows;
    int bodyMode = 0;
    std::string reqBody;
    int rawContentTypeMode = 0;
    std::vector<std::tuple<bool, std::string, std::string>> formRows;
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
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos)
                continue;
            const size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos)
                continue;

            std::string method = PercentDecode(line.substr(t1 + 1, t2 - (t1 + 1)));
            std::string fullUrl = PercentDecode(line.substr(t2 + 1));
            method = ToUpper(TrimCopy(method));
            fullUrl = TrimCopy(fullUrl);
            if (method.empty() || fullUrl.empty())
                continue;

            RequestHistoryEntry he;
            he.method = method;
            he.url = fullUrl; // 已含 query，展示时按完整 URL 使用
            he.fingerprint = RequestFingerprint(method, fullUrl, he.headerRows, "");
            PushHistoryUnique(history, std::move(he));
        }
    } catch (...) {
        // 历史加载失败不影响主流程
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

    ImFont* fontMono = nullptr;
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
        ImFont* ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 17.0f, &cfg, ranges);
        if (!ui)
            ui = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\simhei.ttf", 17.0f, &cfg, ranges);
        if (ui)
            io.FontDefault = ui;

        ImFontConfig monoCfg;
        monoCfg.OversampleH = 2;
        monoCfg.OversampleV = 2;
        fontMono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f, &monoCfg,
                                                io.Fonts->GetGlyphRangesDefault());
        if (!fontMono)
            fontMono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\cour.ttf", 15.0f, &monoCfg,
                                                    io.Fonts->GetGlyphRangesDefault());
        if (!fontMono)
            fontMono = io.FontDefault;
    }

    ImGui::StyleColorsLight();
    ApplyPostmanLightStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();
    // 首帧前上传字体纹理、预热 WinHTTP 会话，减少首次交互卡顿
    ImGui_ImplOpenGL2_CreateFontsTexture();
    (void)HttpWinWarmup();

    std::string method = "GET";
    std::string url = "https://httpbin.org/get";
    std::vector<std::tuple<bool, std::string, std::string, std::string>> queryRows;
    queryRows.emplace_back(true, std::string(), std::string(), std::string());
    std::vector<std::pair<std::string, std::string>> headerRows;
    headerRows.emplace_back(std::string(), std::string());
    int bodyMode = 0;
    std::string reqBody;
    int rawContentTypeMode = 0;
    std::vector<std::tuple<bool, std::string, std::string>> formRows;
    formRows.emplace_back(true, std::string(), std::string());
    int requestTimeoutSec = 15;
    std::string bodyFormatStatus;
    bool bodyFormatOk = true;
    std::string downloadLine;

    std::string respBodyText;
    std::string respHdrText;
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
    int selectedHistoryIdx = -1;

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

        const float sidebarW = 268.f;
        const float mainH = ImGui::GetContentRegionAvail().y - 26.f;

        ImGui::BeginChild("main_row", ImVec2(0, mainH), false);

        ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
        ImGui::TextDisabled("请求历史");
        static char historyFilterBuf[256] = "";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##histfilter", "模糊搜索…（正则: re:pattern）", historyFilterBuf, sizeof(historyFilterBuf));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::BeginChild("hist_list", ImVec2(0, 0), false);
        bool anyShown = false;
        for (int i = 0; i < static_cast<int>(requestHistory.size()); ++i) {
            const RequestHistoryEntry& e = requestHistory[static_cast<size_t>(i)];
            const std::string full = MergeUrlQuery(e.url, e.queryRows);
            if (!HistoryMatchesFilter(historyFilterBuf, e.method, full))
                continue;
            anyShown = true;

            ImGui::PushID(i);
            const std::string esc = EscapeImGuiLabel(full);
            const bool sel = (selectedHistoryIdx == i);
            const std::string rowLabel = ToUpper(e.method) + "  " + esc;
            if (ImGui::Selectable(rowLabel.c_str(), sel, 0, ImVec2(-1.f, 0.f))) {
                selectedHistoryIdx = i;
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
                rawContentTypeMode = e.rawContentTypeMode;
                formRows = e.formRows;
                if (formRows.empty())
                    formRows.emplace_back(true, std::string(), std::string());
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", full.c_str());
            ImGui::PopID();
        }
        if (requestHistory.empty())
            ImGui::TextDisabled("发送请求后将显示在此\n相同请求只保留一条");
        else if (!anyShown)
            ImGui::TextDisabled("无匹配记录");
        ImGui::EndChild();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("workspace", ImVec2(0, 0), true);

        if (ImGui::BeginTabBar("reqwin_tabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("HTTP 请求")) {

        ImGui::BeginChild("url_row", ImVec2(0, 44), false);
        ImGui::AlignTextToFramePadding();
        ImGui::SetNextItemWidth(108);
        const char* methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
        int mi = 0;
        for (int i = 0; i < 7; ++i) {
            if (method == methods[i]) {
                mi = i;
                break;
            }
        }
        if (ImGui::Combo("##m", &mi, methods, IM_ARRAYSIZE(methods)))
            method = methods[mi];

        ImGui::SameLine();
        const float rightPanelW = inFlight.load() ? 350.f : 230.f;
        ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - rightPanelW));
        ImGui::InputTextWithHint("##url", "Enter request URL", &url);

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
            DrawInlineSpinner(10.f, 3.f, IM_COL32(255, 110, 55, 255));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const double waited = ImGui::GetTime() - reqStartTime;
            ImGui::TextDisabled("请求中 %.1fs", static_cast<float>(waited));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("此处为本地已等待时间（界面实时刷新）。完成后响应区「耗时」为整次请求在客户端测得的往返时间。");
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(72, 0)))
                HttpRequestCancel(&httpCancel);
        } else {
            PushSendButtonStyle();
            if (ImGui::Button("Send", ImVec2(96, 0)) && !inFlight.load()) {
                HttpCancelToken_reset(&httpCancel);
                reqStartTime = ImGui::GetTime();
                inFlight = true;
                statusLine = "请求中…";
                timeLine.clear();
                respHdrText.clear();
                respBodyText.clear();
                lastHttpCode = 0;
                lastBodyBytes = 0;

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
                if (bodyMode == 1 && MethodAllowsBody(m))
                    bodyOut = reqBody;
                else if (bodyMode == 2 && MethodAllowsBody(m)) {
                    bodyOut = BuildFormUrlEncoded(formRows);
                    bodyContentType = "application/x-www-form-urlencoded";
                }

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

                {
                    RequestHistoryEntry he;
                    he.fingerprint = RequestFingerprint(m, fullUrl, headerRows, bodyOut);
                    he.method = method;
                    he.url = url;
                    he.queryRows = queryRows;
                    he.headerRows = headerRows;
                    he.bodyMode = bodyMode;
                    he.reqBody = reqBody;
                    he.rawContentTypeMode = rawContentTypeMode;
                    he.formRows = formRows;
                    PushHistoryUnique(requestHistory, std::move(he));
                    AppendHistoryLogLine(m, fullUrl);
                    selectedHistoryIdx = 0;
                }

                AsyncSlot* slot = &async;
                HttpCancelToken* pcancel = &httpCancel;
                const int timeoutSec = requestTimeoutSec;
                std::thread([slot, m, fullUrl, hdrs, bodyOut, pcancel, timeoutSec]() {
                    HttpResult r = HttpRequestSync(m, fullUrl, hdrs, bodyOut, pcancel, timeoutSec);
                    std::lock_guard<std::mutex> lock(slot->mtx);
                    slot->result = std::move(r);
                    slot->finished = true;
                }).detach();
            }
            PopSendButtonStyle();
        }
        ImGui::EndChild();

        ImGui::Spacing();

        const float reqBlockH = std::max(220.f, ImGui::GetContentRegionAvail().y * 0.48f);
        ImGui::BeginChild("req_tabs_region", ImVec2(0, reqBlockH), true);
        if (ImGui::BeginTabBar("rtabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Params")) {
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
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##qk" + std::to_string(i)).c_str(), &std::get<1>(queryRows[i]));
                        ImGui::PopItemWidth();
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
                if (ImGui::Button("添加参数"))
                    queryRows.emplace_back(true, std::string(), std::string(), std::string());
                ImGui::SameLine();
                if (ImGui::Button("删除最后一行") && queryRows.size() > 1)
                    queryRows.pop_back();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Auth")) {
                ImGui::TextDisabled("鉴权方式（占位，后续可接 Bearer / Basic 等）");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Headers##req_headers")) {
                if (ImGui::BeginTable("ht", 2,
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Key");
                    ImGui::TableSetupColumn("Value");
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < headerRows.size(); ++i) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##hk" + std::to_string(i)).c_str(), &headerRows[i].first);
                        ImGui::PopItemWidth();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushItemWidth(-1);
                        ImGui::InputText(("##hv" + std::to_string(i)).c_str(), &headerRows[i].second);
                        ImGui::PopItemWidth();
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button("添加 Header"))
                    headerRows.emplace_back();
                ImGui::SameLine();
                if (ImGui::Button("删除最后一行") && headerRows.size() > 1)
                    headerRows.pop_back();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Body##req_body")) {
                const bool allow = MethodAllowsBody(ToUpper(method));
                ImGui::BeginDisabled(!allow);
                const char* bitems[] = {"无", "raw", "x-www-form-urlencoded"};
                ImGui::SetNextItemWidth(180.f);
                ImGui::Combo("##bodykind", &bodyMode, bitems, IM_ARRAYSIZE(bitems));
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
                        if (ImGui::Button(asJson ? "格式化 JSON" : "格式化 XML")) {
                            std::string formatted;
                            std::string err;
                            const bool ok = asJson
                                ? PrettyFormatJson(reqBody, formatted, err)
                                : PrettyFormatXml(reqBody, formatted, err);
                            bodyFormatOk = ok;
                            if (ok) {
                                reqBody = formatted;
                                bodyFormatStatus = asJson ? "JSON 格式化完成" : "XML 格式化完成";
                            } else {
                                bodyFormatStatus = err.empty() ? "格式化失败" : err;
                            }
                        }
                        if (!bodyFormatStatus.empty()) {
                            ImGui::SameLine();
                            ImGui::TextColored(bodyFormatOk
                                                   ? ImVec4(0.12f, 0.62f, 0.30f, 1.f)
                                                   : ImVec4(0.85f, 0.22f, 0.18f, 1.f),
                                               "%s", bodyFormatStatus.c_str());
                        }
                    } else {
                        bodyFormatStatus.clear();
                    }

                    ImGui::PushFont(fontMono);
                    ImGui::InputTextMultiline("##body", &reqBody, ImVec2(-1, -40.f));
                    ImGui::PopFont();
                } else if (bodyMode == 2) {
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
                            ImGui::PushItemWidth(-1);
                            ImGui::InputText(("##fk" + std::to_string(i)).c_str(), &std::get<1>(formRows[i]));
                            ImGui::PopItemWidth();
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushItemWidth(-1);
                            ImGui::InputText(("##fv" + std::to_string(i)).c_str(), &std::get<2>(formRows[i]));
                            ImGui::PopItemWidth();
                        }
                        ImGui::EndTable();
                    }
                    if (ImGui::Button("添加字段"))
                        formRows.emplace_back(true, std::string(), std::string());
                    ImGui::SameLine();
                    if (ImGui::Button("删除最后一行") && formRows.size() > 1)
                        formRows.pop_back();
                    ImGui::TextDisabled("发送时将自动编码并补充 Content-Type: application/x-www-form-urlencoded");
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!allow)
                    ImGui::TextDisabled("当前方法通常不使用 Body。");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scripts")) {
                ImGui::TextDisabled("Pre-request / Tests（占位）");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                ImGui::TextDisabled("请求级设置（占位）");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::BeginChild("resp_region", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(StatusLineColor(lastHttpCode), "%s", statusLine.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", timeLine.c_str());
        ImGui::SameLine();
        if (lastBodyBytes > 0) {
            ImGui::TextDisabled("|  %zu B", lastBodyBytes);
        }
        if (!downloadLine.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s", downloadLine.c_str());
        }

        // 与上方请求区 rtabs 中的「Body / Headers」标签同名会导致 Tab 选中状态与内容错位，需独立 ID
        ImGui::PushID("response_tabs");
        if (ImGui::BeginTabBar("resp_tabs_bar", ImGuiTabBarFlags_DrawSelectedOverline)) {
            if (ImGui::BeginTabItem("Body##resp_body")) {
                ImGui::PushFont(fontMono);
                ImGui::InputTextMultiline("multiline_body", &respBodyText, ImVec2(-1, -6),
                                         ImGuiInputTextFlags_ReadOnly);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Headers##resp_headers")) {
                ImGui::PushFont(fontMono);
                ImGui::InputTextMultiline("multiline_hdr", &respHdrText, ImVec2(-1, -6),
                                         ImGuiInputTextFlags_ReadOnly);
                ImGui::PopFont();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopID();
        ImGui::EndChild();

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::EndChild();

        ImGui::EndChild();

        ImGui::BeginChild("bottombar", ImVec2(0, 22), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Console");
        ImGui::SameLine(120);
        ImGui::TextDisabled("Terminal");
        ImGui::SameLine(ImGui::GetWindowWidth() - 200);
        ImGui::TextDisabled("HTTPS / WinHTTP");
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const int display_w = static_cast<int>(io.DisplaySize.x);
        const int display_h = static_cast<int>(io.DisplaySize.y);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.94f, 0.94f, 0.95f, 1.f);
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
