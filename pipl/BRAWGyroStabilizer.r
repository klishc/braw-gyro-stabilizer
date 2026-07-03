// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
//  BRAWGyroStabilizer.r
//  PiPL for an After Effects-style effect hosted in Premiere Pro.
//  Mirrors Adobe's GPUVideoFilter sample (Vignette.r): the effect uses the
//  PF_Cmd_* / EffectMain entry point, so Kind MUST be AEEffect and the PiPL MUST
//  carry a CodeMac* descriptor naming the entry symbol — without these Premiere
//  cannot discover or load the effect.

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_General.r"

resource 'PiPL' (16000) {
    {
        Kind { AEEffect },
        Name { "BRAW Gyro Stabilizer" },
        Category { "BRAW Tools" },

#ifdef AE_OS_WIN
        CodeWin64X86 { "EffectMain" },
#else
        CodeMacARM64 { "EffectMain" },
        CodeMacIntel64 { "EffectMain" },
#endif

        AE_PiPL_Version { 2, 0 },
        AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
        AE_Effect_Version { 557057 },               // 1.1.0 ((1<<19)|(1<<15)|1). Bumped on
                                                    // every param-layout change so Premiere
                                                    // re-registers the definitions.

        AE_Effect_Info_Flags { 0 },
        // Must match PF_Cmd_GLOBAL_SETUP in PluginMain.cpp:
        //   out_flags  = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_USE_OUTPUT_EXTENT
        //              | PF_OutFlag_SEND_UPDATE_PARAMS_UI (0x04000000, for the Status row)
        //   out_flags2 = 0  (legacy PF_Cmd_RENDER path — no smart/GPU render)
        AE_Effect_Global_OutFlags { 0x04000440 },
        //   out_flags2 = PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG (0x8) — honor
        //   PF_ParamFlag_START_COLLAPSED on topic groups (Debug ships collapsed)
        AE_Effect_Global_OutFlags_2 { 0x00000008 },

        AE_Effect_Match_Name { "io.nk.brawgyrostabilizer" },
        AE_Reserved_Info { 8 }
    }
};
