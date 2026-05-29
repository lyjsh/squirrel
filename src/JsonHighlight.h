#pragma once

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace JsonHighlight {

constexpr ImU32 kKeyColor = IM_COL32(163, 21, 21, 255);
constexpr ImU32 kStringColor = IM_COL32(4, 81, 165, 255);
constexpr ImU32 kNumberColor = IM_COL32(9, 134, 88, 255);
constexpr ImU32 kKeywordColor = IM_COL32(0, 0, 255, 255);
constexpr ImU32 kPunctColor = IM_COL32(32, 33, 36, 255);
constexpr ImU32 kDefaultColor = IM_COL32(32, 33, 36, 255);
constexpr ImU32 kLineNumColor = IM_COL32(154, 160, 166, 255);
constexpr ImU32 kLineNumBg = IM_COL32(248, 249, 250, 255);
constexpr ImU32 kCodeBg = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kLineNumSep = IM_COL32(218, 220, 224, 255);

struct Span {
    size_t start = 0;
    size_t end = 0;
    ImU32 color = kDefaultColor;
};

inline std::string_view TrimView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

inline bool LooksLikeJson(std::string_view text)
{
    const std::string_view t = TrimView(text);
    if (t.empty())
        return false;
    return t.front() == '{' || t.front() == '[';
}

inline bool LooksLikeXml(std::string_view text)
{
    const std::string_view t = TrimView(text);
    if (t.empty())
        return false;
    return t.front() == '<';
}

inline int CountLines(std::string_view text)
{
    if (text.empty())
        return 1;
    int count = 1;
    for (char c : text) {
        if (c == '\n')
            ++count;
    }
    return count;
}

inline void AppendSpan(std::vector<Span>& spans, size_t start, size_t end, ImU32 color)
{
    if (start >= end)
        return;
    if (!spans.empty() && spans.back().color == color && spans.back().end == start)
        spans.back().end = end;
    else
        spans.emplace_back(Span{start, end, color});
}

inline void BuildHighlightSpans(const std::string& s, std::vector<Span>& out)
{
    out.clear();
    const size_t n = s.size();
    size_t i = 0;

    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }

        if (c == '{' || c == '[' || c == '}' || c == ']' || c == ':' || c == ',') {
            AppendSpan(out, i, i + 1, kPunctColor);
            ++i;
            continue;
        }

        if (c == '"') {
            const size_t start = i;
            ++i;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (s[i] == '"') {
                    ++i;
                    break;
                }
                ++i;
            }
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t'))
                ++j;
            const bool isKey = j < n && s[j] == ':';
            AppendSpan(out, start, i, isKey ? kKeyColor : kStringColor);
            continue;
        }

        if (c == '-' || std::isdigit(c)) {
            const size_t start = i;
            if (c == '-')
                ++i;
            while (i < n) {
                const unsigned char ch = static_cast<unsigned char>(s[i]);
                if (std::isdigit(ch) || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-')
                    ++i;
                else
                    break;
            }
            AppendSpan(out, start, i, kNumberColor);
            continue;
        }

        if (i + 4 <= n && s.compare(i, 4, "true") == 0) {
            AppendSpan(out, i, i + 4, kKeywordColor);
            i += 4;
            continue;
        }
        if (i + 5 <= n && s.compare(i, 5, "false") == 0) {
            AppendSpan(out, i, i + 5, kKeywordColor);
            i += 5;
            continue;
        }
        if (i + 4 <= n && s.compare(i, 4, "null") == 0) {
            AppendSpan(out, i, i + 4, kKeywordColor);
            i += 4;
            continue;
        }

        AppendSpan(out, i, i + 1, kDefaultColor);
        ++i;
    }
}

inline ImU32 ColorAt(const std::vector<Span>& spans, size_t pos, ImU32 fallback = kDefaultColor)
{
    for (const Span& sp : spans) {
        if (pos >= sp.start && pos < sp.end)
            return sp.color;
    }
    return fallback;
}

inline void SplitLines(const std::string& text, std::vector<std::string_view>& lines,
                       std::vector<size_t>& lineStarts)
{
    lines.clear();
    lineStarts.clear();
    size_t lineStart = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            size_t lineEnd = i;
            if (lineEnd > lineStart && text[lineEnd - 1] == '\r')
                --lineEnd;
            lineStarts.push_back(lineStart);
            lines.emplace_back(text.data() + lineStart, lineEnd - lineStart);
            lineStart = i + 1;
        }
    }
    if (lines.empty()) {
        lineStarts.push_back(0);
        lines.emplace_back();
    }
}

inline int ByteIndexFromMousePos(const ImVec2& mousePos, const ImVec2& frameMin, float padX, float padY,
                                 float lineSpacing, const std::vector<std::string_view>& lines,
                                 const std::vector<size_t>& lineStarts)
{
    const int lineCount = static_cast<int>(lines.size());
    if (lineCount == 0)
        return 0;

    const float relY = mousePos.y - (frameMin.y + padY);
    int lineIdx = static_cast<int>(relY / lineSpacing);
    lineIdx = std::clamp(lineIdx, 0, lineCount - 1);

    const std::string_view line = lines[static_cast<size_t>(lineIdx)];
    const float relX = mousePos.x - (frameMin.x + padX);
    if (relX <= 0.f)
        return static_cast<int>(lineStarts[static_cast<size_t>(lineIdx)]);

    size_t bestCol = 0;
    float bestDist = relX < 0.f ? -relX : relX;
    for (size_t col = 1; col <= line.size(); ++col) {
        const float w = ImGui::CalcTextSize(line.data(), line.data() + col).x;
        const float delta = w - relX;
        const float dist = delta < 0.f ? -delta : delta;
        if (dist < bestDist) {
            bestDist = dist;
            bestCol = col;
        }
    }
    return static_cast<int>(lineStarts[static_cast<size_t>(lineIdx)] + bestCol);
}

inline std::pair<int, int> FindDoubleClickSelectionRange(const std::string& text, int byteIndex, bool jsonAware)
{
    const int textLen = static_cast<int>(text.size());
    if (textLen == 0)
        return {0, 0};

    byteIndex = std::clamp(byteIndex, 0, textLen);

    if (jsonAware) {
        static thread_local std::vector<Span> spans;
        BuildHighlightSpans(text, spans);
        for (const Span& sp : spans) {
            if (byteIndex >= static_cast<int>(sp.start) && byteIndex < static_cast<int>(sp.end))
                return {static_cast<int>(sp.start), static_cast<int>(sp.end)};
        }
    }

    const auto isWordChar = [](char ch) {
        const unsigned char c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || ch == '_' || ch == '-' || ch == '.';
    };

    if (byteIndex < textLen && isWordChar(text[static_cast<size_t>(byteIndex)])) {
        int start = byteIndex;
        int end = byteIndex + 1;
        while (start > 0 && isWordChar(text[static_cast<size_t>(start - 1)]))
            --start;
        while (end < textLen && isWordChar(text[static_cast<size_t>(end)]))
            ++end;
        return {start, end};
    }

    if (byteIndex < textLen)
        return {byteIndex, byteIndex + 1};
    return {byteIndex, byteIndex};
}

struct EditSelectionCallbackState {
    ImGuiStorage* storage = nullptr;
    ImGuiID startId = 0;
    ImGuiID endId = 0;
    int cursorPos = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
};

inline int InputTextSelectionCallback(ImGuiInputTextCallbackData* data)
{
    auto* state = static_cast<EditSelectionCallbackState*>(data->UserData);
    if (!state || !state->storage)
        return 0;

    state->cursorPos = data->CursorPos;
    state->selectionStart = data->SelectionStart;
    state->selectionEnd = data->SelectionEnd;

    const int pendingStart = state->storage->GetInt(state->startId, -1);
    if (pendingStart < 0)
        return 0;

    const int pendingEnd = state->storage->GetInt(state->endId, -1);
    const int textLen = static_cast<int>(data->BufTextLen);
    data->SelectionStart = std::clamp(pendingStart, 0, textLen);
    data->SelectionEnd = std::clamp(pendingEnd, 0, textLen);
    state->storage->SetInt(state->startId, -1);
    state->storage->SetInt(state->endId, -1);
    state->selectionStart = data->SelectionStart;
    state->selectionEnd = data->SelectionEnd;
    return 0;
}

inline void QueueDoubleClickSelection(const std::string& text, const ImVec2& mousePos, const ImVec2& frameMin,
                                      float padX, float padY, float lineSpacing, const ImVec2& clipMax, bool jsonAware,
                                      ImGuiStorage* storage, ImGuiID startId, ImGuiID endId)
{
    if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || !ImGui::IsWindowHovered())
        return;
    if (mousePos.x < frameMin.x || mousePos.x > clipMax.x || mousePos.y < frameMin.y || mousePos.y > clipMax.y)
        return;

    static thread_local std::vector<std::string_view> lines;
    static thread_local std::vector<size_t> lineStarts;
    SplitLines(text, lines, lineStarts);

    const int byteIndex =
        ByteIndexFromMousePos(mousePos, frameMin, padX, padY, lineSpacing, lines, lineStarts);
    const std::pair<int, int> range = FindDoubleClickSelectionRange(text, byteIndex, jsonAware);
    storage->SetInt(startId, range.first);
    storage->SetInt(endId, range.second);
}

inline void DrawCodeEditorCaret(ImDrawList* dl, const std::string& text, int cursorPos, const ImVec2& frameMin,
                                float padX, float padY, float fontSize, const ImVec2& clipMin, const ImVec2& clipMax)
{
    const int textLen = static_cast<int>(text.size());
    cursorPos = std::clamp(cursorPos, 0, textLen);
    const char* textBegin = text.c_str();
    const char* cursorPtr = textBegin + cursorPos;

    int lineNo = 0;
    for (const char* p = textBegin; p < cursorPtr; ++p) {
        if (*p == '\n')
            ++lineNo;
    }

    const char* lineStart = cursorPtr;
    while (lineStart > textBegin && lineStart[-1] != '\n')
        --lineStart;

    const float cursorX = ImGui::CalcTextSize(lineStart, cursorPtr).x;
    const float x = frameMin.x + padX + cursorX;
    const float yBottom = frameMin.y + padY + static_cast<float>(lineNo + 1) * fontSize;
    const float caretTop = yBottom - fontSize + 0.5f;
    const float caretBottom = yBottom - 1.5f;
    if (caretBottom < clipMin.y || caretTop > clipMax.y)
        return;

    const ImGuiIO& io = ImGui::GetIO();
    const float blinkPhase = static_cast<float>(ImGui::GetTime());
    const bool caretVisible = !io.ConfigInputTextCursorBlink || std::fmod(blinkPhase, 1.20f) <= 0.80f;
    if (!caretVisible)
        return;

    dl->PushClipRect(clipMin, clipMax, true);
    const ImU32 caretColor =
        ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    dl->AddLine(ImVec2(x, caretTop), ImVec2(x, caretBottom), caretColor, 2.f);
    dl->PopClipRect();
}

inline void DrawColoredTextSegment(ImDrawList* dl, ImFont* font, float fontSize, float x, float y,
                                   std::string_view text, ImU32 color)
{
    if (text.empty())
        return;
    dl->AddText(font, fontSize, ImVec2(x, y), color, text.data(), text.data() + text.size());
}

inline void DrawCodeView(const char* id, const std::string& text, const ImVec2& size, ImFont* font,
                         bool jsonHighlight)
{
    static thread_local std::vector<Span> spans;
    if (jsonHighlight)
        BuildHighlightSpans(text, spans);
    else
        spans.clear();

    static thread_local std::vector<std::string_view> lines;
    static thread_local std::vector<size_t> lineStarts;
    SplitLines(text, lines, lineStarts);

    const int lineCount = static_cast<int>(lines.size());
    int maxDigits = 1;
    for (int n = lineCount; n >= 10; n /= 10)
        ++maxDigits;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCodeBg);
    ImGui::BeginChild(id, size, false, ImGuiWindowFlags_HorizontalScrollbar);

    if (font)
        ImGui::PushFont(font);

    const float fontSize = ImGui::GetFontSize();
    const float lineSpacing = fontSize;
    const float padX = 10.f;
    const float padY = 6.f;
    char sampleLineNum[16];
    snprintf(sampleLineNum, sizeof(sampleLineNum), "%*d", maxDigits, lineCount);
    const float lineNumColW = ImGui::CalcTextSize(sampleLineNum).x + padX * 2.f + 4.f;
    const float codeX0 = lineNumColW + padX;

    float maxCodeW = 0.f;
    for (const std::string_view line : lines) {
        const float w = ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
        maxCodeW = std::max(maxCodeW, w);
    }

    const ImVec2 contentSize(codeX0 + maxCodeW + padX, padY * 2.f + lineCount * lineSpacing);
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(contentSize);
    const ImVec2 origin = canvasPos;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 clipMin = ImGui::GetWindowPos();
    const ImVec2 clipMax(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);
    dl->PushClipRect(clipMin, clipMax, true);

    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + lineNumColW, origin.y + contentSize.y), kLineNumBg);
    dl->AddLine(ImVec2(origin.x + lineNumColW, origin.y),
                ImVec2(origin.x + lineNumColW, origin.y + contentSize.y), kLineNumSep);

    for (int li = 0; li < lineCount; ++li) {
        const std::string_view line = lines[static_cast<size_t>(li)];
        const size_t lineOffset = lineStarts[static_cast<size_t>(li)];
        const float y = origin.y + padY + li * lineSpacing;

        char numBuf[16];
        const int numLen = snprintf(numBuf, sizeof(numBuf), "%*d", maxDigits, li + 1);
        const float numW = ImGui::CalcTextSize(numBuf, numBuf + numLen).x;
        const float numX = origin.x + (lineNumColW - numW) * 0.5f;
        DrawColoredTextSegment(dl, font, fontSize, numX, y, numBuf, kLineNumColor);

        float x = origin.x + codeX0;
        size_t col = 0;
        while (col < line.size()) {
            const size_t absPos = lineOffset + col;
            const ImU32 colColor = jsonHighlight ? ColorAt(spans, absPos) : kDefaultColor;
            size_t runEnd = col + 1;
            while (runEnd < line.size()) {
                const ImU32 nextColor = jsonHighlight ? ColorAt(spans, lineOffset + runEnd) : kDefaultColor;
                if (nextColor != colColor)
                    break;
                ++runEnd;
            }
            const std::string_view segment(line.data() + col, runEnd - col);
            DrawColoredTextSegment(dl, font, fontSize, x, y, segment, colColor);
            x += ImGui::CalcTextSize(segment.data(), segment.data() + segment.size()).x;
            col = runEnd;
        }
    }

    dl->PopClipRect();

    if (font)
        ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

inline void DrawJsonCodeView(const char* id, const std::string& text, const ImVec2& size, ImFont* font)
{
    DrawCodeView(id, text, size, font, true);
}

inline void DrawPlainCodeView(const char* id, const std::string& text, const ImVec2& size, ImFont* font)
{
    DrawCodeView(id, text, size, font, false);
}

inline void DrawHighlightedLines(ImDrawList* dl, ImFont* font, float fontSize, float lineHeight,
                                 const ImVec2& origin, float lineNumColW, float codeX0, float padY,
                                 float contentH, int maxDigits, int lineCount,
                                 const std::vector<std::string_view>& lines,
                                 const std::vector<size_t>& lineStarts, const std::vector<Span>& spans,
                                 bool jsonHighlight)
{
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + lineNumColW, origin.y + contentH),
                      kLineNumBg);
    dl->AddLine(ImVec2(origin.x + lineNumColW, origin.y), ImVec2(origin.x + lineNumColW, origin.y + contentH),
                kLineNumSep);

    for (int li = 0; li < lineCount; ++li) {
        const std::string_view line = lines[static_cast<size_t>(li)];
        const size_t lineOffset = lineStarts[static_cast<size_t>(li)];
        const float y = origin.y + padY + li * lineHeight;

        char numBuf[16];
        const int numLen = snprintf(numBuf, sizeof(numBuf), "%*d", maxDigits, li + 1);
        const float numW = ImGui::CalcTextSize(numBuf, numBuf + numLen).x;
        const float numX = origin.x + (lineNumColW - numW) * 0.5f;
        DrawColoredTextSegment(dl, font, fontSize, numX, y, numBuf, kLineNumColor);

        float x = origin.x + codeX0;
        size_t col = 0;
        while (col < line.size()) {
            const size_t absPos = lineOffset + col;
            const ImU32 colColor = jsonHighlight ? ColorAt(spans, absPos) : kDefaultColor;
            size_t runEnd = col + 1;
            while (runEnd < line.size()) {
                const ImU32 nextColor = jsonHighlight ? ColorAt(spans, lineOffset + runEnd) : kDefaultColor;
                if (nextColor != colColor)
                    break;
                ++runEnd;
            }
            const std::string_view segment(line.data() + col, runEnd - col);
            DrawColoredTextSegment(dl, font, fontSize, x, y, segment, colColor);
            x += ImGui::CalcTextSize(segment.data(), segment.data() + segment.size()).x;
            col = runEnd;
        }
    }
}

inline void DrawEditableCodeView(const char* id, std::string& text, const ImVec2& viewSize, ImFont* font,
                                 bool jsonHighlight, bool readOnly = false)
{
    static thread_local std::vector<Span> spans;
    static thread_local std::vector<std::string_view> lines;
    static thread_local std::vector<size_t> lineStarts;

    SplitLines(text, lines, lineStarts);

    const int lineCount = static_cast<int>(lines.size());
    int maxDigits = 1;
    for (int n = lineCount; n >= 10; n /= 10)
        ++maxDigits;

    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCodeBg);
    ImGui::BeginChild("wrap", viewSize, false, ImGuiWindowFlags_HorizontalScrollbar);

    if (font)
        ImGui::PushFont(font);

    const float fontSize = ImGui::GetFontSize();
    const float lineSpacing = fontSize;
    const float padX = 10.f;
    const float padY = 6.f;
    char sampleLineNum[16];
    snprintf(sampleLineNum, sizeof(sampleLineNum), "%*d", maxDigits, lineCount);
    const float lineNumColW = ImGui::CalcTextSize(sampleLineNum).x + padX * 2.f + 4.f;
    const float codeX0 = lineNumColW + padX;

    float maxCodeW = 0.f;
    for (const std::string_view line : lines) {
        const float w = ImGui::CalcTextSize(line.data(), line.data() + line.size()).x;
        maxCodeW = std::max(maxCodeW, w);
    }

    const ImVec2 contentSize(codeX0 + maxCodeW + padX, padY * 2.f + lineCount * lineSpacing);

    ImGui::SetCursorPos(ImVec2(0.f, 0.f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 clipMin = ImGui::GetWindowPos();
    const ImVec2 clipMax(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);
    const ImVec2 inputSize(contentSize.x - lineNumColW, contentSize.y);

    ImGui::SetCursorPos(ImVec2(lineNumColW, 0.f));
    const ImVec2 codeFrameMin = ImGui::GetCursorScreenPos();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID selStartId = ImGui::GetID("##dblsel_start");
    const ImGuiID selEndId = ImGui::GetID("##dblsel_end");
    EditSelectionCallbackState selCallbackState{storage, selStartId, selEndId};
    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_CallbackAlways;
    if (readOnly)
        inputFlags |= ImGuiInputTextFlags_ReadOnly;
    const ImVec2& mousePos = ImGui::GetIO().MousePos;

    auto drawEditorInput = [&]() {
        QueueDoubleClickSelection(text, mousePos, codeFrameMin, padX, padY, lineSpacing, clipMax, jsonHighlight,
                                  storage, selStartId, selEndId);
        ImGui::InputTextMultiline("##edit", &text, inputSize, inputFlags, InputTextSelectionCallback,
                                  &selCallbackState);
        QueueDoubleClickSelection(text, mousePos, ImGui::GetItemRectMin(), padX, padY, lineSpacing, clipMax,
                                  jsonHighlight, storage, selStartId, selEndId);
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);

    if (jsonHighlight) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));

        BuildHighlightSpans(text, spans);
        dl->PushClipRect(clipMin, clipMax, true);
        DrawHighlightedLines(dl, font, fontSize, lineSpacing, origin, lineNumColW, codeX0, padY, contentSize.y,
                             maxDigits, lineCount, lines, lineStarts, spans, true);
        dl->PopClipRect();

        drawEditorInput();

        ImGui::PopStyleColor(2);

        if (ImGui::IsItemActive() || ImGui::IsItemFocused()) {
            DrawCodeEditorCaret(ImGui::GetForegroundDrawList(), text, selCallbackState.cursorPos,
                                ImGui::GetItemRectMin(), padX, padY, fontSize, clipMin, clipMax);
        }
    } else {
        dl->PushClipRect(clipMin, clipMax, true);
        dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + lineNumColW, origin.y + contentSize.y),
                          kLineNumBg);
        dl->AddLine(ImVec2(origin.x + lineNumColW, origin.y),
                    ImVec2(origin.x + lineNumColW, origin.y + contentSize.y), kLineNumSep);
        for (int li = 0; li < lineCount; ++li) {
            const float y = origin.y + padY + static_cast<float>(li) * lineSpacing;
            if (y + lineSpacing < clipMin.y || y > clipMax.y)
                continue;
            char numBuf[16];
            const int numLen = snprintf(numBuf, sizeof(numBuf), "%*d", maxDigits, li + 1);
            const float numW = ImGui::CalcTextSize(numBuf, numBuf + numLen).x;
            const float numX = origin.x + (lineNumColW - numW) * 0.5f;
            DrawColoredTextSegment(dl, font, fontSize, numX, y, numBuf, kLineNumColor);
        }
        dl->PopClipRect();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(kCodeBg));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDefaultColor));
        drawEditorInput();
        ImGui::PopStyleColor(2);
        if (readOnly && (ImGui::IsItemActive() || ImGui::IsItemFocused())) {
            DrawCodeEditorCaret(ImGui::GetForegroundDrawList(), text, selCallbackState.cursorPos,
                                ImGui::GetItemRectMin(), padX, padY, fontSize, clipMin, clipMax);
        }
    }

    if (contentSize.x > 1.f && contentSize.y > 1.f) {
        ImGui::SetCursorPos(ImVec2(contentSize.x - 1.f, contentSize.y - 1.f));
        ImGui::Dummy(ImVec2(1.f, 1.f));
    }

    ImGui::PopStyleVar(2);

    if (font)
        ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

inline void DrawEditableJsonView(const char* id, std::string& text, const ImVec2& size, ImFont* font)
{
    DrawEditableCodeView(id, text, size, font, true);
}

inline void DrawEditablePlainView(const char* id, std::string& text, const ImVec2& size, ImFont* font)
{
    DrawEditableCodeView(id, text, size, font, false);
}

inline void DrawSelectableJsonView(const char* id, std::string& text, const ImVec2& size, ImFont* font)
{
    DrawEditableCodeView(id, text, size, font, true, true);
}

inline void DrawSelectablePlainView(const char* id, std::string& text, const ImVec2& size, ImFont* font)
{
    DrawEditableCodeView(id, text, size, font, false, true);
}

} // namespace JsonHighlight
