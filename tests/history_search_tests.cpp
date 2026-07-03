#include "HistorySearch.h"

#include <cassert>
#include <string>
#include <tuple>
#include <vector>

int main()
{
    HistorySearch::RequestSearchDocument doc;
    doc.method = "POST";
    doc.fullUrl = "http://localhost:39001/catfish/dispatch?traceId=abc123";
    doc.queryRows.emplace_back(true, "city", "北京", "出发城市");
    doc.queryRows.emplace_back(false, "ignored", "disabled-value", "disabled row");
    doc.headerRows.emplace_back("X-Agent", "squirrel");
    doc.rawBody = R"({"bankText":"工商银行","nested":{"policyNo":"P12345"}})";
    doc.formRows.emplace_back(true, "customerName", "alice");
    doc.formRows.emplace_back(false, "ignoredForm", "disabled-form-value");
    doc.multipartRows.emplace_back(true, 0, "avatar", "C:/tmp/pic.png", "image/png");

    const std::string haystack = HistorySearch::BuildRequestSearchText(doc);

    assert(HistorySearch::HistoryMatchesFilter("", haystack));
    assert(HistorySearch::HistoryMatchesFilter("post catfish", haystack));
    assert(HistorySearch::HistoryMatchesFilter("bankText 工商银行", haystack));
    assert(HistorySearch::HistoryMatchesFilter("traceId abc123", haystack));
    assert(HistorySearch::HistoryMatchesFilter("x-agent squirrel", haystack));
    assert(HistorySearch::HistoryMatchesFilter("customerName alice", haystack));
    assert(HistorySearch::HistoryMatchesFilter("avatar pic.png", haystack));
    assert(HistorySearch::HistoryMatchesFilter("policyNo P12345", haystack));
    assert(!HistorySearch::HistoryMatchesFilter("disabled-value", haystack));
    assert(!HistorySearch::HistoryMatchesFilter("not-present", haystack));
    assert(!HistorySearch::HistoryMatchesFilter("policyNo not-present", haystack));

    return 0;
}
