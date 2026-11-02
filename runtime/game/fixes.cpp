// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Fixes: community-sourced fixes ported from PC,
// one option each (the [fixes] table).
#include <stdafx.h>
#include <region/region.h>

#include <kernel/function.h>
#include <user/settings.h>

namespace
{
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
