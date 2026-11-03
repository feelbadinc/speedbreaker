// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fixes.h.
//
// Settings > Fixes: community-sourced fixes ported from PC,
// one option each (the [fixes] table).
#include <stdafx.h>
#include <game/fixes.h>
#include <region/region.h>

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <user/settings.h>

namespace
{
    // Black Edition Content: the game's Black Edition flag, a byte the code
    // only reads (0 in the base game). Ported from the Xenia patch by tuhana.
    constexpr uint32_t kBlackEditionFlag = 0x82A2CE06;

    // Hide ESRB Disclaimer: the USA build splash screen (sub_822B8500) plays
    // "SHOW" on the "Game Experience May Change During Online Play" notice at
    // 0x822B8678; the other builds don't. "On" skips that call on the USA build.
    constexpr uint32_t kShowEsrbNoticeCall = 0x822B867C;  // return address
    constexpr uint32_t kShowScriptHash = 0x1CA7C0;        // "SHOW"

    bool HideEsrbNotice(const PPCContext& ctx)
    {
        return region::Current() == region::Release::Usa
            && static_cast<uint32_t>(ctx.lr) == region::Addr(kShowEsrbNoticeCall)
            && ctx.r4.u32 == kShowScriptHash
            && settings::GetBool(settings::Id::HideEsrbDisclaimer);
    }
}

// Plays a script on a screen object (r4: the script's hash).
// Fixes that skip one of its calls check here.
SB_HOOK(sub_8227FF60)
{
    if (HideEsrbNotice(ctx))
        return;
    SB_ORIGINAL(sub_8227FF60)(ctx, base);
}

// Fix Busted Scenes: the arrest-scene chooser (sub_82135F98) falls through from
// heat 2 and 3 into the next heat's pick, so heat 4+ scenes replace their
// ArrestM04/F23/M07/M14/F14 (and "b") scenes. Ported from NFSMWExOpts
// (BustedNISFix). The hooks (config/nfsmw.toml) end the chooser right after the
// heat 2 or 3 pick, so the next one doesn't overwrite it. r31 is the heat level,
// r30 the picked scene's name.
namespace
{
    bool KeepBustedScene(bool pickedEarlier, const PPCRegister& r31, const PPCRegister& r30)
    {
        if (!pickedEarlier || !settings::GetBool(settings::Id::FixBustedScenes))
            return false;
        fprintf(stderr, "[fixes] Busted scene: heat %u keeps %.32s\n", r31.u32,
            static_cast<const char*>(g_memory.Translate(r30.u32)));
        return true;
    }
}

bool BustedSceneBeforeHeat3Pick(PPCRegister& r31, PPCRegister& r30)
{
    return KeepBustedScene(r31.u32 < 3, r31, r30);
}

bool BustedSceneBeforeHeat4Pick(PPCRegister& r31, PPCRegister& r30)
{
    return KeepBustedScene(r31.u32 < 4, r31, r30);
}

// Intro Videos skip (ported from WFP): a skipped movie file is reported
// missing; the game then ends it itself, as it does for any missing movie.
namespace
{
    // Return addresses of sub_82282F18's two movie file probes.
    constexpr uint32_t kMovieProbe1 = 0x82282FAC;
    constexpr uint32_t kMovieProbe2 = 0x82282FD0;

    std::atomic<bool> s_titleReached = false;
    thread_local bool t_skipMovie = false;

    bool SkipMovie(const PPCContext& ctx, uint8_t* base)
    {
        using enum settings::IntroVideos;
        const auto mode = settings::IntroVideos(settings::GetInt(settings::Id::IntroVideos));
        const uint32_t event = ctx.r4.u32;
        if (mode == Original || s_titleReached || !event || PPC_LOAD_U32(event + 0x18) != 7)  // start movie
            return false;
        const uint32_t nameAddress = PPC_LOAD_U32(event + 0x24);  // pointer to the movie name
        if (!nameAddress)
            return false;
        const char* name = static_cast<const char*>(g_memory.Translate(nameAddress));
        return mode == SkipAll
            || (mode == EaLogoOnly && strncasecmp(name, "ealogo", 6) != 0)  // prefix for other locales
            || (mode == SkipEaHd && strcasecmp(name, "eahd_bumper") == 0);
    }
}

// The start movie handler.
SB_HOOK(sub_82282F18)
{
    t_skipMovie = SkipMovie(ctx, base);
    SB_ORIGINAL(sub_82282F18)(ctx, base);
    t_skipMovie = false;
}

// The file probe: 0 (missing) when the handler probes a skipped movie.
SB_HOOK(sub_823BCA60)
{
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (t_skipMovie && (lr == region::Addr(kMovieProbe1) || lr == region::Addr(kMovieProbe2)))
        ctx.r3.u64 = 0;
    else
        SB_ORIGINAL(sub_823BCA60)(ctx, base);
}

// The title screen message handler.
SB_HOOK(sub_822B8988)
{
    s_titleReached = true;
    SB_ORIGINAL(sub_822B8988)(ctx, base);
}

// The boot sequence's next screen: a late movie-finished signal can arrive
// after the game deleted the sequence (r3 = 0). Ported from WFP NextScreen fix.
SB_HOOK(sub_822B8458)
{
    if (ctx.r3.u32)
        SB_ORIGINAL(sub_822B8458)(ctx, base);
}

// Gamepad Prompts: replaces the 32x32 button icons (GLOBALB.BUN) with
// 128x128 ones by Aero_, pre-converted to the game's texture layout in
// resources/hd_buttons/<set>/<TEXTURE>.xtex. The hook repoints the
// TextureInfo at the HD copy before the game builds its header.
namespace
{
    // TextureInfo fields (big-endian).
    constexpr uint32_t kInfoPlatInfo = 0x00;    // guest pointer
    constexpr uint32_t kInfoName = 0x0C;        // char[24]
    constexpr uint32_t kInfoImageSize = 0x38;   // u32
    constexpr uint32_t kInfoWidth = 0x44;       // u16
    constexpr uint32_t kInfoHeight = 0x46;      // u16
    constexpr uint32_t kInfoWidthLog2 = 0x48;   // u8
    constexpr uint32_t kInfoHeightLog2 = 0x49;  // u8
    constexpr uint32_t kInfoLevels = 0x4E;      // u8
    constexpr uint32_t kInfoImageData = 0x70;   // guest pointer

    // TextureInfoPlatInfo: the D3D format, little-endian.
    constexpr uint32_t kPlatFormat = 0x14;

    // The game's own uncompressed texture format: tiled A8R8G8B8.
    constexpr uint32_t kXtexArgb = 0x18280186;

    struct HdIcon
    {
        uint32_t data;  // guest address
        uint32_t size;
        uint16_t width, height;
        uint8_t levels;
    };
    using IconSet = std::unordered_map<std::string, HdIcon>;  // by texture name

    // Under the working directory, else the program's folder or any folder above it.
    std::filesystem::path FindSetDir(const char* set)
    {
        const auto relative = std::filesystem::path("resources/hd_buttons") / set;
        std::error_code ec;
        if (std::filesystem::is_directory(relative, ec))
            return relative;
        const char* base = SDL_GetBasePath();
        std::filesystem::path dir = base ? base : "";
        for (; dir.has_relative_path(); dir = dir.parent_path())
            if (std::filesystem::is_directory(dir / relative, ec))
                return dir / relative;
        return relative;
    }

    uint32_t ReadBe32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    // .xtex: a 32-byte big-endian header ("XTEX", version, u16 width, u16
    // height, u8 levels, 3 pad, u32 format, u32 data size, u32 mip offset,
    // 4 pad), then the tiled levels as XGSetTextureHeader lays them out.
    bool LoadXtex(const std::filesystem::path& path, HdIcon& icon)
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<uint8_t> file{ std::istreambuf_iterator<char>(in), {} };
        if (file.size() < 32 || memcmp(file.data(), "XTEX", 4) != 0)
            return false;
        icon.width = uint16_t((file[8] << 8) | file[9]);
        icon.height = uint16_t((file[10] << 8) | file[11]);
        icon.levels = file[12];
        icon.size = ReadBe32(&file[20]);
        const bool knownFormat = ReadBe32(&file[16]) == kXtexArgb;
        const bool sizesFit = icon.size == file.size() - 32 && icon.levels &&
            std::has_single_bit(icon.width) && std::has_single_bit(icon.height);
        if (!knownFormat || !sizesFit)
            return false;
        void* data = g_userHeap.AllocPhysical(icon.size, 4096);
        memcpy(data, &file[32], icon.size);
        icon.data = g_memory.MapVirtual(data);
        return true;
    }

    IconSet LoadSet(const char* set)
    {
        IconSet icons;
        const std::filesystem::path dir = FindSetDir(set);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            if (HdIcon icon; entry.path().extension() == ".xtex" && LoadXtex(entry.path(), icon))
                icons.emplace(entry.path().stem().string(), icon);
        if (icons.empty())
            fprintf(stderr, "[fixes] Gamepad prompts: no icons in %s\n", dir.string().c_str());
        return icons;
    }

    // Strips a language suffix, even a truncated one ("_FRENCH", "_SPANIS", "_F").
    std::string EnglishTexture(std::string_view name)
    {
        static constexpr std::string_view kLanguages[] = {
            "FRENCH", "SPANISH", "ITALIAN", "SWEDISH",
        };
        const size_t sep = name.rfind('_');
        if (sep == std::string_view::npos || sep + 1 == name.size())
            return {};
        const std::string_view base = name.substr(0, sep);
        for (std::string_view language : kLanguages)
            if (language.starts_with(name.substr(sep + 1)))
            {
                // You know what they call an L1 in Paris? LEFT_TRIGGER_FRENCH.
                if (base == "LEFT_TRIGGER")
                    return "L1";
                if (base == "RIGHT_TRIGGER")
                    return "R1";
                return std::string(base);
            }
        return {};
    }

    void UseHdGamepadPrompt(uint32_t info, uint8_t* base, const IconSet& icons)
    {
        const bool icon32 = PPC_LOAD_U16(info + kInfoWidth) == 32 &&
            PPC_LOAD_U16(info + kInfoHeight) == 32;
        if (!icon32 || !PPC_LOAD_U32(info + kInfoImageData))
            return;
        const char* texture = static_cast<const char*>(g_memory.Translate(info + kInfoName));
        const std::string name(texture, strnlen(texture, 24));
        auto it = icons.find(name);
        if (it == icons.end())
            it = icons.find(EnglishTexture(name));
        if (it == icons.end())
            return;
        const HdIcon& icon = it->second;
        const uint32_t plat = PPC_LOAD_U32(info + kInfoPlatInfo);
        if (!plat)
            return;
        PPC_STORE_U32(plat + kPlatFormat, __builtin_bswap32(kXtexArgb));
        PPC_STORE_U16(info + kInfoWidth, icon.width);
        PPC_STORE_U16(info + kInfoHeight, icon.height);
        PPC_STORE_U8(info + kInfoWidthLog2, uint8_t(std::countr_zero(icon.width)));
        PPC_STORE_U8(info + kInfoHeightLog2, uint8_t(std::countr_zero(icon.height)));
        PPC_STORE_U8(info + kInfoLevels, icon.levels);
        PPC_STORE_U32(info + kInfoImageSize, icon.size);
        PPC_STORE_U32(info + kInfoImageData, icon.data);
    }

    // The set's folder name; null for the original textures.
    const char* GamepadPromptSet()
    {
        using enum settings::GamepadPrompts;
        switch (settings::GamepadPrompts(settings::GetInt(settings::Id::GamepadPrompts)))
        {
        case Xbox360: return "xbox_360";
        case XboxOne: return "xbox_one";
        case Ps3: return "ps3";
        case Ps4: return "ps4";
        case Switch: return "switch";
        default: return nullptr;
        }
    }
}

// Builds a texture header from its TextureInfo (r3).
SB_HOOK(sub_8243F1D8)
{
    if (const char* set = GamepadPromptSet())
    {
        static const IconSet s_icons = LoadSet(set);
        UseHdGamepadPrompt(ctx.r3.u32, base, s_icons);
    }
    SB_ORIGINAL(sub_8243F1D8)(ctx, base);
}

// Subtitles: sub_822874C8 loads a movie's subtitles only when the game language
// differs from the VO or the movie is a tutorial. The hooks (config/nfsmw.toml):
// Force On skips both checks (ported from NFSMWExOpts ShowSubs),
// Force Off the language one, so only tutorials keep them (ported from WFP).
static bool SubtitlesMode(settings::Subtitles mode)
{
    return settings::Subtitles(settings::GetInt(settings::Id::Subtitles)) == mode;
}

bool SubtitlesForceOn()
{
    return SubtitlesMode(settings::Subtitles::ForceOn);
}

bool SubtitlesForceOff()
{
    return SubtitlesMode(settings::Subtitles::ForceOff);
}

namespace fixes
{
    void ApplyAtBoot()
    {
        if (settings::GetBool(settings::Id::BlackEditionContent))
        {
            *static_cast<uint8_t*>(g_memory.Translate(region::Addr(kBlackEditionFlag))) = 1;
            fprintf(stderr, "[fixes] Black Edition content: on\n");
        }
    }
}
