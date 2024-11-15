/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

// TODO(casey): This is a work-in-progress, and only defines a handful of the
// possible CPU/counters you might want to use. 

//
// NOTE(casey): AMD Zen 2
//

#define PMC(Name, ...) pmctrace_pmc_definition const PMCTrace_##Name = {#Name, __VA_ARGS__};

PMC(CYCLES_NOT_IN_HALT, 0x76, 0x00)
PMC(RETIRED_INST, 0xC0, 0x00)
PMC(RETIRED_MACRO_OPS, 0xC1, 0x00)
PMC(RETIRED_BR_INST, 0xC2, 0x00)
PMC(RETIRED_BR_INST_MISP, 0xC3, 0x00)
PMC(RETIRED_SSE_AVX_FLOPS, 0x03, 0x08)
PMC(L1_DC_ACCESSES_ALL, 0x29, 0x07)
PMC(L2_CACHE_ACCESS_FROM_L1_IC_MISS, 0x60, 0x10)
PMC(L2_CACHE_ACCESS_FROM_L1_DC_MISS, 0x60, 0xC8)
PMC(L2_CACHE_MISS_FROM_L1_IC_MISS, 0x64, 0x01)
PMC(L2_CACHE_MISS_FROM_L1_DC_MISS, 0x64, 0x08)
PMC(L2_HWPF_HIT_IN_L3, 0x71, 0x1F)
PMC(L2_HWPF_MISS_IN_L2_L3, 0x72, 0x1F)
PMC(L2_CACHE_HIT_FROM_L1_IC_MISS, 0x64, 0x06)
PMC(L2_CACHE_HIT_FROM_L1_DC_MISS, 0x64, 0x70)
PMC(L2_HWPF_HIT_IN_L2, 0x70, 0x1F)
PMC(L1_DEMAND_DC_REFILLS_LOCAL_L2, 0x43, 0x01)
PMC(L1_DEMAND_DC_REFILLS_LOCAL_CACHE, 0x43, 0x02)
PMC(L1_DEMAND_DC_REFILLS_LOCAL_DRAM, 0x43, 0x08)
PMC(L1_DEMAND_DC_REFILLS_REMOTE_CACHE, 0x43, 0x10)
PMC(L1_DEMAND_DC_REFILLS_REMOTE_DRAM, 0x43, 0x40)
PMC(L1_DEMAND_DC_REFILLS_ALL, 0x43, 0x5B)
PMC(L2_REQUESTS_ALL, 0x60, 0xFF)
PMC(L1_ITLB_MISSES_L2_HITS, 0x84, 0x00)
PMC(L2_ITLB_MISSES, 0x85, 0x07)
PMC(L1_DTLB_MISSES, 0x45, 0xFF)
PMC(L2_DTLB_MISSES, 0x45, 0xF0)
PMC(MISALIGNED_LOADS, 0x47, 0x00)
PMC(INEFFECTIVE_SW_PF, 0x52, 0x03)

//
// NOTE(casey): AMD Zen 3
//

// TODO(casey): According to uProf docs, these events are different codes on Zen 3.
// We would need some testing to see if they're right about that.
PMC(L2_CACHE_ACCESS_FROM_L1_DC_MISS_Zen3, 0x60, 0xE8)
PMC(L2_HWPF_HIT_IN_L3_Zen3, 0x71, 0xFF)
PMC(L2_HWPF_MISS_IN_L2_L3_Zen3, 0x72, 0xFF)
PMC(L2_CACHE_HIT_FROM_L1_DC_MISS_Zen3, 0x64, 0xF0)
PMC(L2_HWPF_HIT_IN_L2_Zen3, 0x70, 0xFF)
PMC(L1_DEMAND_DC_REFILLS_ALL_Zen3, 0x43, 0x5F)
PMC(MISALIGNED_LOADS_Zen3, 0x47, 0x03)

PMC(L1_DC_REFILLS_EXTERNAL_CACHE_LOCAL, 0x43, 0x04)
PMC(L1_DEMAND_DC_REFILLS_EXTERNAL_CACHE, 0x43, 0x14)
PMC(L1_DC_REFILLS_LOCAL_L2, 0x44, 0x01)
PMC(L1_DC_REFILLS_LOCAL_CACHE, 0x44, 0x02)
PMC(L1_DC_REFILLS_LOCAL_DRAM, 0x44, 0x08)
PMC(L1_DC_REFILLS_EXTERNAL_CACHE_REMOTE, 0x44, 0x10)
PMC(L1_DC_REFILLS_REMOTE_DRAM, 0x44, 0x40)
PMC(L1_DC_REFILLS_EXTENAL_CACHE, 0x44, 0x14)
PMC(L1_DC_REFILLS_DRAM, 0x44, 0x48)
PMC(L1_DC_REFILLS_REMOTE_NODE, 0x44, 0x50)
PMC(L1_DC_REFILLS_LOCAL_CACHE_L2_L3, 0x44, 0x03)
PMC(L1_DC_REFILLS_ALL, 0x44, 0x5F)
PMC(ALL_TLB_FLUSHES, 0x78, 0xFF)

//
// NOTE(casey): AMD Zen 4
//

PMC(RETIRED_SSE_AVX_FLOPS_Zen4, 0x03, 0x1F)
PMC(L1_DEMAND_DC_REFILLS_ALL_Zen4, 0x43, 0xDF)

#undef PMC