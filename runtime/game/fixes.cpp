// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fixes.h.
//
// Settings > Fixes: community-sourced fixes ported from PC,
// one option each (the [fixes] table).
#include <stdafx.h>
#include <game/fixes.h>
#include <region/region.h>

#include <kernel/function.h>
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
// after the game deleted the sequence (r3 = 0). WFP NextScreen fix.
SB_HOOK(sub_822B8458)
{
    if (ctx.r3.u32)
        SB_ORIGINAL(sub_822B8458)(ctx, base);
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
