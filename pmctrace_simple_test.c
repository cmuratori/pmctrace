/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

#include <stdio.h>
#include <stdint.h>

#define PMCTRACE_INCLUDE_IMPLEMENTATION 1
#include "pmctrace.h"
#include "pmctrace_counters.h"

int main(void)
{
    pmctrace_pmc_definition_array PMCDefs =
    {
        {
            PMCTrace_CYCLES_NOT_IN_HALT,
            PMCTrace_RETIRED_INST,
            PMCTrace_RETIRED_SSE_AVX_FLOPS,
        }
    };
    
    printf("Starting trace...\n");
    pmctrace_client Tracer = PMCTraceStartTracing(PMCDefs);
    
    PMCTraceBeginRegion(&Tracer, 0);
    printf("... This printf is measured only by Region[0].\n");
    PMCTraceBeginRegion(&Tracer, 1);
    printf("... This printf is measured by both.\n");
    PMCTraceEndRegion(&Tracer, 0);
    PMCTraceEndRegion(&Tracer, 1);
    
    printf("Getting results...\n");
    for(int ResultIndex = 0; ResultIndex < 2; ++ResultIndex)
    {
        pmctrace_result Result = PMCTraceGetOrWaitForResult(&Tracer, ResultIndex);
        if(NoErrors(&Tracer))
        {
            printf("\n%llu TSC elapsed [%u context switch%s]\n",
                   Result.TSCElapsed, Result.ContextSwitchCount,
                   (Result.ContextSwitchCount != 1) ? "es" : "");
            for(int CI = 0; CI < Result.PMCCount; ++CI)
            {
                printf("  %llu %s\n", Result.Counters[CI], PMCDefs.Defs[CI].PrintableName);
            }
        }
        else
        {
            printf("CLIENT ERROR: %s\n", GetClientErrorMessage(&Tracer));
            printf("SERVER ERROR: %s\n", GetServerErrorMessage(&Tracer));
            break;
        }
    }
    
    printf("Stopping trace...\n");
    PMCTraceStopPMCTracing(&Tracer);
    
    return 0;
}
