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
