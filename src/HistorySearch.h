#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace HistorySearch {

using QueryRow = std::tuple<bool, std::string, std::string, std::string>;
using FormRow = std::tuple<bool, std::string, std::string>;
using MultipartRow = std::tuple<bool, int, std::string, std::string, std::string>;

struct RequestSearchDocument {
    std::string method;
    std::string fullUrl;
    std::vector<QueryRow> queryRows;
    std::vector<std::pair<std::string, std::string>> headerRows;
    int bodyMode = 0;
    std::string rawBody;
    std::vector<FormRow> formRows;
    std::vector<MultipartRow> multipartRows;
};

inline std::string TrimCopy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

inline std::string ToLowerCopy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool HasFilterText(const char* filter)
{
    if (!filter)
        return false;
    while (*filter) {
        if (!std::isspace(static_cast<unsigned char>(*filter)))
            return true;
        ++filter;
    }
    return false;
}

inline void AppendField(std::string& out, const char* label, const std::string& value)
{
    if (value.empty())
        return;
    out += label;
    out += ": ";
    out += value;
    out.push_back('\n');
}

inline std::string BuildRequestSearchText(const RequestSearchDocument& doc)
{
    std::string out;
    out.reserve(doc.fullUrl.size() + doc.rawBody.size() + 256);

    AppendField(out, "method", doc.method);
    AppendField(out, "url", doc.fullUrl);

    for (const QueryRow& row : doc.queryRows) {
        if (!std::get<0>(row))
            continue;
        AppendField(out, "param.key", std::get<1>(row));
        AppendField(out, "param.value", std::get<2>(row));
        AppendField(out, "param.description", std::get<3>(row));
    }

    for (const auto& header : doc.headerRows) {
        AppendField(out, "header.key", header.first);
        AppendField(out, "header.value", header.second);
    }

    if (doc.bodyMode == 1 || !doc.rawBody.empty())
        AppendField(out, "body", doc.rawBody);

    for (const FormRow& row : doc.formRows) {
        if (!std::get<0>(row))
            continue;
        AppendField(out, "form.key", std::get<1>(row));
        AppendField(out, "form.value", std::get<2>(row));
    }

    for (const MultipartRow& row : doc.multipartRows) {
        if (!std::get<0>(row))
            continue;
        AppendField(out, "multipart.name", std::get<2>(row));
        AppendField(out, "multipart.value", std::get<3>(row));
        AppendField(out, "multipart.contentType", std::get<4>(row));
    }

    return out;
}

inline bool HistoryMatchesFilter(const char* filter, const std::string& searchText)
{
    if (!HasFilterText(filter))
        return true;

    std::string f = TrimCopy(filter);
    const std::string hay = ToLowerCopy(searchText);
    f = ToLowerCopy(std::move(f));
    size_t pos = 0;
    while (pos < f.size()) {
        while (pos < f.size() && std::isspace(static_cast<unsigned char>(f[pos])))
            ++pos;
        if (pos >= f.size())
            break;
        size_t end = pos;
        while (end < f.size() && !std::isspace(static_cast<unsigned char>(f[end])))
            ++end;
        const std::string token = f.substr(pos, end - pos);
        if (!token.empty() && hay.find(token) == std::string::npos)
            return false;
        pos = end + 1;
    }
    return true;
}

} // namespace HistorySearch
