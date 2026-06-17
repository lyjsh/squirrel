#include "JsonHighlight.h"

#include <cassert>
#include <string>
#include <utility>

namespace {

void AssertSelectedText(const std::string& text, int byteIndex, bool jsonAware, const std::string& expected)
{
    const std::pair<int, int> range = JsonHighlight::FindDoubleClickSelectionRange(text, byteIndex, jsonAware);
    assert(text.substr(static_cast<size_t>(range.first), static_cast<size_t>(range.second - range.first)) == expected);
}

} // namespace

int main()
{
    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(0.f, 100.f, 10.f, 100);
        assert(range.first == 0);
        assert(range.second == 11);
    }

    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(35.f, 20.f, 10.f, 10);
        assert(range.first == 2);
        assert(range.second == 7);
    }

    {
        const std::pair<int, int> range = JsonHighlight::ComputeVisibleLineRange(35.f, 20.f, 0.f, 10);
        assert(range.first == 0);
        assert(range.second == 10);
    }

    {
        const std::pair<int, int> range = JsonHighlight::NormalizeSelectionRange(8, 3, 20);
        assert(range.first == 3);
        assert(range.second == 8);
    }

    {
        const std::pair<int, int> range = JsonHighlight::NormalizeSelectionRange(-4, 30, 12);
        assert(range.first == 0);
        assert(range.second == 12);
    }

    {
        const std::string text = R"({"url":"https://example.com/api/v1/users?id=42"})";
        AssertSelectedText(text, static_cast<int>(text.find("example")), true, "example");
        AssertSelectedText(text, static_cast<int>(text.find("api")), true, "api");
        AssertSelectedText(text, static_cast<int>(text.find("id")), true, "id");
    }

    {
        const std::string text = R"({"user_name":"alice_bob"})";
        AssertSelectedText(text, static_cast<int>(text.find("user_name")), true, "user_name");
        AssertSelectedText(text, static_cast<int>(text.find("alice_bob")), true, "alice_bob");
    }

    {
        const std::string text = R"({"count":12345})";
        AssertSelectedText(text, static_cast<int>(text.find("12345") + 2), true, "12345");
    }

    {
        const std::string text = "GET https://example.com/api-user/list";
        AssertSelectedText(text, static_cast<int>(text.find("example")), false, "example");
        AssertSelectedText(text, static_cast<int>(text.find("api")), false, "api");
        AssertSelectedText(text, static_cast<int>(text.find("user")), false, "user");
    }

    {
        const std::string text = R"({"message":"请求成功"})";
        AssertSelectedText(text, static_cast<int>(text.find("请求") + 1), true, "请求成功");
    }

    return 0;
}
