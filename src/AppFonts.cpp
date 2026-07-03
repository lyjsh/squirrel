#include "AppFonts.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::filesystem::path ExecutableDir()
{
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len > 0 && len < path.size())
        return std::filesystem::path(path.data()).parent_path();
#endif
    return std::filesystem::current_path();
}

std::filesystem::path ProjectRootFallback()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::vector<std::filesystem::path> FontRoots()
{
    std::vector<std::filesystem::path> roots;
    roots.push_back(ExecutableDir() / "assets" / "fonts");
    roots.push_back(std::filesystem::current_path() / "assets" / "fonts");
    roots.push_back(ProjectRootFallback() / "assets" / "fonts");
    return roots;
}

std::filesystem::path FindProjectFont(std::initializer_list<const char*> names)
{
    for (const std::filesystem::path& root : FontRoots()) {
        for (const char* name : names) {
            const std::filesystem::path candidate = root / name;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec))
                return candidate;
        }
    }
    return {};
}

std::string LowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool IsSupportedFontFile(const std::filesystem::path& path)
{
    const std::string ext = LowerAscii(path.extension().string());
    return ext == ".ttf" || ext == ".ttc" || ext == ".otf";
}

std::filesystem::path FindAnyProjectFont()
{
    for (const std::filesystem::path& root : FontRoots()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec))
            continue;

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec)
                break;
            if (entry.is_regular_file(ec) && IsSupportedFontFile(entry.path()))
                return entry.path();
        }
    }
    return {};
}

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path SystemFont(const char* name)
{
#if defined(_WIN32)
    if (const char* windir = std::getenv("WINDIR"))
        return std::filesystem::path(windir) / "Fonts" / name;
    return std::filesystem::path("C:\\Windows\\Fonts") / name;
#else
    (void)name;
    return {};
#endif
}

ImFont* AddFontFile(ImFontAtlas* atlas, const std::filesystem::path& path, float size,
                    ImFontConfig* config, const ImWchar* ranges)
{
    if (path.empty())
        return nullptr;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return nullptr;
    const std::string utf8 = PathUtf8(path);
    return atlas->AddFontFromFileTTF(utf8.c_str(), size, config, ranges);
}

ImFont* AddFontFromCandidates(ImFontAtlas* atlas, std::initializer_list<std::filesystem::path> paths,
                              float size, ImFontConfig* config, const ImWchar* ranges)
{
    for (const std::filesystem::path& path : paths) {
        if (ImFont* font = AddFontFile(atlas, path, size, config, ranges))
            return font;
    }
    return nullptr;
}

ImFontConfig OversampledConfig()
{
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    return cfg;
}

ImFontConfig MergeConfig()
{
    ImFontConfig cfg = OversampledConfig();
    cfg.MergeMode = true;
    return cfg;
}

} // namespace

AppFontSet LoadAppFonts(ImGuiIO& io)
{
    AppFontSet fonts;
    ImFontAtlas* atlas = io.Fonts;
    const ImWchar* cjkRanges = atlas->GetGlyphRangesChineseFull();
    const ImWchar* latinRanges = atlas->GetGlyphRangesDefault();

    const std::filesystem::path anyProjectFont = FindAnyProjectFont();
    std::filesystem::path uiRegular = FindProjectFont({"ui-regular.ttf", "ui-regular.ttc", "segoeui.ttf"});
    std::filesystem::path uiBold = FindProjectFont({"ui-bold.ttf", "ui-bold.ttc", "segoeuib.ttf"});
    std::filesystem::path cjkRegular = FindProjectFont({"cjk-regular.ttf", "cjk-regular.ttc", "msyh.ttc"});
    std::filesystem::path cjkBold = FindProjectFont({"cjk-bold.ttf", "cjk-bold.ttc", "msyhbd.ttc"});
    std::filesystem::path codeLatin = FindProjectFont({"code-latin.ttf", "code-latin.ttc", "consola.ttf", "cour.ttf"});
    std::filesystem::path codeCjk = FindProjectFont({"code-cjk.ttf", "code-cjk.ttc", "msyh.ttc", "simhei.ttf"});
    if (!anyProjectFont.empty()) {
        if (uiRegular.empty())
            uiRegular = anyProjectFont;
        if (uiBold.empty())
            uiBold = uiRegular;
        if (cjkRegular.empty())
            cjkRegular = anyProjectFont;
        if (cjkBold.empty())
            cjkBold = cjkRegular;
        if (codeLatin.empty())
            codeLatin = anyProjectFont;
        if (codeCjk.empty())
            codeCjk = cjkRegular;
    }

    ImFontConfig cfgUi = OversampledConfig();
    ImFont* ui = AddFontFromCandidates(atlas,
                                       {uiRegular, cjkRegular,
                                        SystemFont("segoeui.ttf"), SystemFont("msyh.ttc")},
                                       17.0f, &cfgUi, latinRanges);
    if (!ui) {
        ImFontConfig cfgCjk = OversampledConfig();
        ui = AddFontFromCandidates(atlas, {cjkRegular, SystemFont("msyh.ttc")},
                                   19.0f, &cfgCjk, cjkRanges);
    }
    if (ui) {
        ImFontConfig cfgMerge = MergeConfig();
        AddFontFromCandidates(atlas, {cjkRegular, SystemFont("msyh.ttc")},
                              19.0f, &cfgMerge, cjkRanges);
        io.FontDefault = ui;
    }
    if (!ui) {
        ui = atlas->AddFontDefault();
        io.FontDefault = ui;
    }
    fonts.ui = io.FontDefault ? io.FontDefault : (atlas->Fonts.Size > 0 ? atlas->Fonts[0] : nullptr);

    ImFontConfig cfgTitle = OversampledConfig();
    fonts.title = AddFontFromCandidates(atlas,
                                        {uiBold, uiRegular, cjkBold, cjkRegular,
                                         SystemFont("segoeuib.ttf"), SystemFont("segoeui.ttf")},
                                        26.0f, &cfgTitle, latinRanges);
    if (fonts.title) {
        ImFontConfig cfgTitleMerge = MergeConfig();
        AddFontFromCandidates(atlas,
                              {cjkBold, SystemFont("msyhbd.ttc"),
                               cjkRegular, SystemFont("msyh.ttc")},
                              26.0f, &cfgTitleMerge, cjkRanges);
    }

    constexpr float kCodeLatinPx = 15.0f;
    constexpr float kCodeCjkPx = 17.0f;
    ImFontConfig cfgCode = OversampledConfig();
    fonts.code = AddFontFromCandidates(atlas,
                                       {codeCjk, cjkRegular, SystemFont("msyh.ttc"),
                                        SystemFont("simhei.ttf")},
                                       kCodeCjkPx, &cfgCode, cjkRanges);
    if (fonts.code) {
        ImFontConfig cfgCodeMerge = MergeConfig();
        AddFontFromCandidates(atlas,
                              {codeLatin, SystemFont("consola.ttf"),
                               SystemFont("cour.ttf")},
                              kCodeLatinPx, &cfgCodeMerge, latinRanges);
    } else {
        fonts.code = AddFontFromCandidates(atlas,
                                           {codeLatin, SystemFont("consola.ttf"),
                                            SystemFont("cour.ttf")},
                                           kCodeLatinPx, &cfgCode, latinRanges);
        if (fonts.code) {
            ImFontConfig cfgCodeMerge = MergeConfig();
            AddFontFromCandidates(atlas,
                                  {codeCjk, cjkRegular, SystemFont("msyh.ttc"),
                                   SystemFont("simhei.ttf")},
                                  kCodeCjkPx, &cfgCodeMerge, cjkRanges);
        }
    }
    if (!fonts.code)
        fonts.code = fonts.ui;

    return fonts;
}
