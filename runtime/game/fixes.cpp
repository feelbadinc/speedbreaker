// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fixes.h.
//
// Settings > Fixes: community-sourced fixes ported from PC,
// one option each (the [fixes] table).
#include <stdafx.h>
#include <game/fe.h>
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
#include <ui/quit_game.h>
#include <user/settings.h>

namespace
{
    // Black Edition Content: the game's Black Edition flag, a byte the code
    // only reads (0 in the base game). Ported from the Xenia patch by tuhana.
    constexpr uint32_t kBlackEditionFlag = 0x82A2CE06;

    // Fix Cop Spoiler Damage: COPSPORTHENCH and COPSPORTGHOST spoilers never get
    // damaged, as the damage zone map lacks their slot (4). Four of its duplicate
    // entries become spoiler entries. Spotted in NFSMW Unlimiter, done from scratch.
    constexpr uint32_t kDamageZoneMap = 0x828F9E88;
    constexpr uint32_t kCopSpoilerSlot = 4;
    struct ZoneSlotEntry
    {
        uint32_t index;  // a duplicate entry: zone, slot
        uint32_t zone;
    };
    constexpr ZoneSlotEntry kCopSpoilerEntries[] = {
        { 5, 1 },   // FRONT, BODY (also entry 2)        -> REAR
        { 38, 6 },  // LEFT, slot 17 (also entry 30)     -> LREAR
        { 39, 7 },  // LEFT, slot 18 (also entry 29)     -> RREAR
        { 64, 8 },  // LFRONT, slot 18 (also entry 61)   -> TOP
    };

    // Fix Timebug: the sim clock (Sim::GetTime), a float each step is added to.
    constexpr uint32_t kSimClock = 0x82A153B8;
    bool s_fixTimebug = false;

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

// Fix Timebug: as the float sim clock grows, each 1/60 s step added to it
// is rounded, so timers drift (5% faster after 5 hours). It's counted in
// double and stored rounded instead. Ported from WFP (OnSimStep).
void SimClockStepped(PPCRegister& f1)
{
    if (!s_fixTimebug)
        return;
    static double exact = 0.0;
    static float written = 0.0f;
    auto& clock = *static_cast<be<float>*>(g_memory.Translate(region::Addr(kSimClock)));

    // Not our last value plus the step: the game reset the clock, count on from it.
    exact = clock == written + float(f1.f64) ? exact + f1.f64 : double(clock);
    written = float(exact);
    clock = written;
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

// Fix Pursuit Crashes: a roadblock is built from a list of cop cars, and
// a car wrecked or despawned meanwhile (back to its abstract vtable)
// gets called. Such cars are skiped. Ported from WFP (RoadblockCarHook).
namespace
{
    constexpr uint32_t kPureCall = 0x826B87F8;      // _purecall
    constexpr uint32_t kFeListeners = 0x82C6F08C;   // vector: begin +4, count +0xC
}

bool RoadblockCarLoaded(PPCRegister& r30)
{
    if (!settings::GetBool(settings::Id::FixPursuitCrashes))
        return false;
    uint8_t* base = g_memory.base;
    const uint32_t car = r30.u32;
    const uint32_t vtable = car ? PPC_LOAD_U32(car) : 0;
    if (vtable && PPC_LOAD_U32(vtable + 0x80) != region::Addr(kPureCall))
        return false;
    fprintf(stderr, "[fixes] Pursuit crash: roadblock skips a destroyed cop car\n");
    return true;
}

// Fix Pursuit Crashes: package-change listeners (pursuit HUD parts as well)
// can leave the list while it's notified, and the loop ran past its end.
// They're notified from a copy, each only while still listed.
// Ported from WFP (NotifyListeners).
SB_HOOK(sub_822AA780)
{
    if (!settings::GetBool(settings::Id::FixPursuitCrashes))
    {
        SB_ORIGINAL(sub_822AA780)(ctx, base);
        return;
    }
    const uint32_t list = region::Addr(kFeListeners);
    auto listed = [&](uint32_t listener) {
        const uint32_t begin = PPC_LOAD_U32(list + 4), count = PPC_LOAD_U32(list + 0xC);
        for (uint32_t i = 0; i < count; i++)
            if (PPC_LOAD_U32(begin + i * 4) == listener)
                return true;
        return false;
    };
    std::vector<uint32_t> listeners(PPC_LOAD_U32(list + 0xC));
    for (uint32_t i = 0; i < listeners.size(); i++)
        listeners[i] = PPC_LOAD_U32(PPC_LOAD_U32(list + 4) + i * 4);
    for (uint32_t listener : listeners)
    {
        if (!listener || !listed(listener))
            continue;
        PPCContext call = ctx;
        call.r3.u64 = listener;
        g_memory.FindFunction(PPC_LOAD_U32(PPC_LOAD_U32(listener) + 0xC))(call, base);
    }
    if (PPC_LOAD_U32(list + 0xC) < listeners.size())
        fprintf(stderr, "[fixes] Pursuit crash: FE listener left while notified\n");
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

// Hide Subtitle Background: hides the background behind the subtitle text.
constexpr uint32_t kSubtitleBackground = 0x10;  // subtitles -> background object
constexpr uint32_t kHideObject = 0x8227FE08;    // FEng hide

// Subtitle message handler (r3 = subtitles), runs every frame of a movie.
SB_HOOK(sub_822877A0)
{
    if (settings::GetBool(settings::Id::HideSubtitleBackground))
    {
        if (uint32_t background = PPC_LOAD_U32(ctx.r3.u32 + kSubtitleBackground))
        {
            PPCContext call = ctx;
            call.r3.u64 = background;
            g_memory.FindFunction(region::Addr(kHideObject))(call, base);
        }
    }
    SB_ORIGINAL(sub_822877A0)(ctx, base);
}

// Hide Xbox Live Entry: the main menu builder (sub_82237900) skips
// the Xbox Live row. Inspired by NFSMWExOpts (ShowOnline).
bool HideXboxLiveEntry()
{
    return settings::GetBool(settings::Id::HideXboxLiveEntry);
}

// Longer Alias Names: the alias keyboard takes up to 15 characters (same
// A-Z/0-9 set), and the alias load list keeps saves with names that long.
// Ported from NFSMWExOpts (AllowLongerProfileNames).
constexpr uint32_t kLongAliasName = 15;

static bool LongerAliasNames() { return settings::GetBool(settings::Id::LongerAliasNames); }

// Alias keyboard setup (sub_8228EB30), r5 = max length.
void AliasNameLengthSet(PPCRegister& r5)
{
    if (LongerAliasNames())
        r5.u64 = kLongAliasName;
}

// Load list builder (sub_822926F0), r11 = a save's name length; true lists it.
bool AliasListNameFits(PPCRegister& r11)
{
    return LongerAliasNames() && r11.u32 <= kLongAliasName;
}

// Carbon-Style Race Progress: 1-lap races show percentage. Ported
// from NFSMWExOpts (ShowPercentOn1LapRaces). Race progress text
// (sub_8229FDD8), r3 = race type (0 = no race), r31 = widget.
bool RaceProgressLapText(PPCRegister& r3, PPCRegister& r31)
{
    uint8_t* base = g_memory.base;
    return settings::GetBool(settings::Id::CarbonStyleRaceProgress)
        && r3.u32 != 0 && PPC_LOAD_U32(r31.u32 + 60) == 1;  // lap total
}

// World Map During Events: skips the in-game map key's "event active"
// check (sub_822A9798). Ported from NFSMWExOpts (WorldMapAnywhere).
bool WorldMapEventCheck()
{
    return settings::GetBool(settings::Id::WorldMapAnywhere);
}

// Fix Leaderboard Refresh: the race leaderboard names/gaps countdown
// (+552, 90) ticks once per 1/30 s of real time, as on a 30 fps 360.
// Inspired by WFP (#1218). r26 = widget, before the game's decrement.
void LeaderboardCountdown(PPCRegister& r26)
{
    struct Pace { uint64_t lastNs; double carry; };
    static std::unordered_map<uint32_t, Pace> s_paces;  // per widget (split screen)
    if (!settings::GetBool(settings::Id::FixLeaderboardRefresh))
    {
        s_paces.clear();
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    Pace& pace = s_paces.try_emplace(r26.u32, Pace{ now, 0.0 }).first->second;
    pace.carry += std::min((now - pace.lastNs) / 1e9, 0.25);  // pauses don't burst
    pace.lastNs = now;
    int ticks = int(pace.carry * 30.0);
    pace.carry -= ticks / 30.0;

    uint8_t* base = g_memory.base;
    int32_t counter = int32_t(PPC_LOAD_U32(r26.u32 + 552));
    if (counter > 0)
        // the game takes 1 off next
        PPC_STORE_U32(r26.u32 + 552, uint32_t(std::max(1, counter - ticks + 1)));
}

// Skip EA Trax In-Game: Engage Event (d-pad up) plays the next track, in free
// roam when there's no event to engage, in races always; the standings (race
// or pursuit event) move to D-pad Down. Uses the menu next-song function with
// its "in the menus" check passed. Inspired by NFSMWExOpts (SkipTrackAnywhere).
namespace
{
    constexpr uint32_t kNextSong = 0x822862A0;
    constexpr uint32_t kNotifyChyron = 0x82203438;    // shows the EA Trax banner
    constexpr uint32_t kTraxSlot = 0x150;             // the music object's slot ...
    constexpr uint32_t kSlotSongs = 0x82A327C0;       // ... its song: slot * 24 here

    // The banner screen waits 5s in game (ticks at 4000Hz) from its start time.
    constexpr uint32_t kTraxScreenName = 0x8206E47C;  // "EA_Trax.fng"
    constexpr uint32_t kFindScreen = 0x822D7528;
    constexpr uint32_t kScreenStart = 0x2C;
    constexpr uint32_t kTicksNow = 0x82A399AC;
    constexpr uint32_t kBannerWaitTicks = 5 * 4000;

    // Race status: nonzero +0x1A28 in a race (free roam: 0).
    constexpr uint32_t kRaceStatus = 0x82A2CB18;
    constexpr uint32_t kRaceStatusMode = 0x1A28;
    constexpr uint32_t kHudPursuitStandings = 0x2BC;  // Engage shows PostRace_Pursuit
    constexpr uint32_t kActionEngage = 0x2E;
    constexpr uint32_t kActionPadDown = 0x30;

    std::atomic<uint32_t> s_traxSong = 0;
    std::atomic<uint32_t> s_chyronBaseline = 0;
    std::atomic<bool> s_chyronPending = false;

    // What stops Engage Event in pursuits; d-pad up in race checks it too.
    constexpr uint32_t kPursuitCounts[] = { 0x82A2D848, 0x82A2D86C };
    constexpr uint32_t kEngageBlocked = 0x82C5A261;

    uint32_t Load32(uint32_t address)
    {
        return *static_cast<const be<uint32_t>*>(g_memory.Translate(address));
    }

    bool InRace()
    {
        const uint32_t status = Load32(region::Addr(kRaceStatus));
        return status && Load32(status + kRaceStatusMode) != 0;
    }

    bool InPursuit()
    {
        for (uint32_t count : kPursuitCounts)
            if (int32_t(Load32(region::Addr(count))) > 0)
                return true;
        const void* blocked = g_memory.Translate(region::Addr(kEngageBlocked));
        return *static_cast<const uint8_t*>(blocked) != 0;
    }

    thread_local bool t_skipTrax = false;
    thread_local bool t_skippingTrax = false;
}

void EngageAvailabilityChecked(PPCRegister& r8)
{
    if (r8.u32 == 0)
        t_skipTrax = true;
}

void HudActionDispatched(PPCRegister& r11, PPCRegister& r28)
{
    if ((r11.u32 != kActionEngage && r11.u32 != kActionPadDown)
        || !settings::GetBool(settings::Id::InGameTraxSkip))
        return;
    const uint8_t* pursuitStandings = static_cast<const uint8_t*>(
        g_memory.Translate(r28.u32 + kHudPursuitStandings));
    if (!InRace() && *pursuitStandings == 0)
        return;
    if (r11.u32 == kActionPadDown)
    {
        r11.u64 = kActionEngage;  // the standings
        return;
    }
    t_skipTrax = !InPursuit();
    r11.u64 = 0;  // no action
}

void NextSongInMenus(PPCRegister& r11)
{
    if (t_skippingTrax)
        r11.u64 = 1;
}

// HUD action handler (pause, Engage Event, d-pad left/right/down).
SB_HOOK(sub_822A9798)
{
    t_skipTrax = false;
    SB_ORIGINAL(sub_822A9798)(ctx, base);
    const bool skip = t_skipTrax;
    t_skipTrax = false;
    if (!skip || !settings::GetBool(settings::Id::InGameTraxSkip))
        return;
    PPCContext call = ctx;
    t_skippingTrax = true;
    g_memory.FindFunction(region::Addr(kNextSong))(call, base);
    t_skippingTrax = false;
    s_chyronBaseline = s_traxSong.load();
    s_chyronPending = true;
}

// The EA Trax music object update (r3: the object). Once the skip
// happens, shows the banner at once, as LS in the menus does.
SB_HOOK(sub_82202968)
{
    const uint32_t trax = ctx.r3.u32;
    SB_ORIGINAL(sub_82202968)(ctx, base);
    const uint32_t slot = PPC_LOAD_U32(trax + kTraxSlot);
    const uint32_t songs = region::Addr(kSlotSongs);
    const uint32_t song = slot < 64 ? PPC_LOAD_U32(songs + slot * 24) : ~0u;
    s_traxSong = song;
    if (!s_chyronPending || song == s_chyronBaseline)
        return;
    s_chyronPending = false;
    PPCContext call = ctx;
    call.r3.u64 = trax;
    g_memory.FindFunction(region::Addr(kNotifyChyron))(call, base);
    call.r3.u64 = region::Addr(kTraxScreenName);
    g_memory.FindFunction(region::Addr(kFindScreen))(call, base);
    const uint32_t now = PPC_LOAD_U32(region::Addr(kTicksNow));
    if (const uint32_t screen = call.r3.u32)
        PPC_STORE_U32(screen + kScreenStart, now - kBannerWaitTicks);
}

// Main Menu Quit: B on the main menu (unused there) opens the Yes/No
// dialog with QUIT_GAME; Yes quits as Settings > Quit Game does.
namespace
{
    namespace fe = game::fe;

    constexpr uint32_t kMainMenuScreen = 0x82079AAC;  // MainMenu's screen vtable
    constexpr uint32_t kQuitYes = fe::Hash("SB_QUIT_GAME_YES");

    uint32_t s_pressedScreen = 0;  // B was pressed in the main menu root

    void NoteMainMenuBack(uint32_t message, uint32_t package)
    {
        if (message != fe::kPadBack)
            return;
        const uint32_t screen = fe::ScreenOf(package);
        if (fe::IsScreen(screen, kMainMenuScreen))
            s_pressedScreen = screen;
    }
}

// The main menu's message handler (r3: the screen, r4: the message).
SB_HOOK(sub_82237498)
{
    const uint32_t screen = ctx.r3.u32;
    const uint32_t message = ctx.r4.u32;
    SB_ORIGINAL(sub_82237498)(ctx, base);
    if (screen == s_pressedScreen)
    {
        s_pressedScreen = 0;
        fe::ShowYesNoDialog(ctx, base, screen,
            fe::Hash("QUIT_GAME"), kQuitYes, fe::Hash("SB_QUIT_GAME_NO"));
    }
    else if (message == kQuitYes)
    {
        SDL_Event e{ .type = SDL_EVENT_QUIT };  // SDL stamps the time
        if (SDL_PushEvent(&e))
            fprintf(stderr, "[fixes] Main menu quit: quitting\n");
    }
}

// Mirror in Chase Cams: other releases show the mirror only in bumper and hood.
// Hides the 3D mirror and the radar's mirror frame; idle when the mirror is off.
// Reverse of NFSMWExOpts (EnableCameras, the rearview mirror addition).
namespace
{
    constexpr uint32_t kFeDatabase = 0x82A2C684;  // its +0x10: player 0 settings
    constexpr uint32_t kSettingsCamera = 0x94;    // 0 bumper, 1 hood

    bool HideMirrorForCamera(uint8_t* base)
    {
        if (settings::MirrorInChaseCams(settings::GetInt(settings::Id::MirrorInChaseCams))
            != settings::MirrorInChaseCams::Hidden)
            return false;
        const uint32_t db = PPC_LOAD_U32(region::Addr(kFeDatabase));
        const uint32_t player = db ? PPC_LOAD_U32(db + 0x10) : 0;
        return player && PPC_LOAD_U32(player + kSettingsCamera) >= 2;
    }
}

SB_HOOK(sub_822AA818)
{
    SB_ORIGINAL(sub_822AA818)(ctx, base);
    if (ctx.r3.u32 != 0 && HideMirrorForCamera(base))
        ctx.r3.u64 = 0;
}

// Show HUD on Look Back: the HUD stays up; the mirror keeps its own
// lookback check. Ported from NFSMWExOpts (ShowHUDOnLookBack).
namespace
{
    // View table, 112 bytes per view. View 1 is player 0 (the first
    // player's) screen; its action list sits at +180 (view 1 + 68).
    constexpr uint32_t kViews = 0x82A38070;
    constexpr uint32_t kActionType = 12, kDriveAction = 1;  // handler type; 1 = driving
    constexpr uint32_t kLookback = 177;                     // byte: holding lookback

    bool ShowHudOnLookback() { return settings::GetBool(settings::Id::ShowHudOnLookback); }

    // Player 0's drive action lookback flag, as the mirror check reads it.
    bool Player0LookingBack(uint8_t* base)
    {
        const uint32_t head = region::Addr(kViews) + 180;
        const uint32_t node = PPC_LOAD_U32(head);
        if (node == head || node == 0)
            return false;
        const uint32_t action = node - 4;
        return PPC_LOAD_U32(action + kActionType) == kDriveAction
            && PPC_LOAD_U8(action + kLookback) != 0;
    }
}

// HUD visibility builder (sub_822A9DC0), r3 = the view's lookback flag.
void HudLookbackRead(PPCRegister& r3)
{
    if (ShowHudOnLookback())
        r3.u64 = 0;
}

// Radar update (sub_82297840), r10 = the mirror option. Drops the mirror
// frame when the mirror is hidden (chase cam, or looking back with the HUD up).
void RadarMirrorOptionRead(PPCRegister& r10)
{
    if (r10.u32 == 0)
        return;
    if (HideMirrorForCamera(g_memory.base)
        || (ShowHudOnLookback() && Player0LookingBack(g_memory.base)))
        r10.u64 = 0;
}

namespace fixes
{
    void ApplyAtBoot()
    {
        if (ui::quit::kOffered && settings::GetBool(settings::Id::MainMenuQuit))
        {
            fe::OnUnhandledButton(NoteMainMenuBack);
            fprintf(stderr, "[fixes] Main menu quit: on\n");
        }
        if (settings::GetBool(settings::Id::BlackEditionContent))
        {
            *static_cast<uint8_t*>(g_memory.Translate(region::Addr(kBlackEditionFlag))) = 1;
            fprintf(stderr, "[fixes] Black Edition content: on\n");
        }
        if (settings::GetBool(settings::Id::FixCopSpoilerDamage))
        {
            auto* map = static_cast<be<uint32_t>*>(g_memory.Translate(region::Addr(kDamageZoneMap)));
            for (const ZoneSlotEntry& e : kCopSpoilerEntries)
            {
                map[e.index * 2] = e.zone;
                map[e.index * 2 + 1] = kCopSpoilerSlot;
            }
            fprintf(stderr, "[fixes] Cop spoiler damage: on\n");
        }
        if (settings::GetBool(settings::Id::FixTimebug))
        {
            s_fixTimebug = true;
            fprintf(stderr, "[fixes] Timebug: on\n");
        }
    }
}
