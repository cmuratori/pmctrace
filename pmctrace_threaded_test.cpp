/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

#include <stdio.h>
#include <stdint.h>
#include <windows.h>

#define PMCTRACE_INCLUDE_IMPLEMENTATION 1
#include "pmctrace.h"
#include "pmctrace_counters.h"

extern "C" void CountNonZeroesWithBranch(u64 Count, u8 *Data);
#pragma comment (lib, "pmctrace_test_asm")

#define TEST_BATCH_SIZE 32

struct thread_context
{
    pmctrace_client *Tracer;
    
    u64 BufferCount;
    u64 NonZeroCount;
    
    pmctrace_result BestResult;
    
    u32 BaseRegionIndex;
};

static DWORD CALLBACK TestThread(void *Arg)
{
    thread_context *Context = (thread_context *)Arg;
    pmctrace_client *Tracer = Context->Tracer;
    
    u64 BufferCount = Context->BufferCount;
    u64 NonZeroCount = Context->NonZeroCount;
    
    u8 *BufferData = (u8 *)VirtualAlloc(0, BufferCount, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if(BufferData)
    {
        for(u64 Index = 0; Index < NonZeroCount; ++Index)
        {
            u64 Random;
            while(_rdrand64_step(&Random) == 0) {}
            BufferData[Random % BufferCount] = 1;
        }
        
        Context->BestResult.TSCElapsed = (u64)-1ll;
        for(u32 Iteration = 0; NoErrors(Tracer) && (Iteration < 10); ++Iteration)
        {
            for(u32 BatchIndex = 0; NoErrors(Tracer) && (BatchIndex < TEST_BATCH_SIZE); ++BatchIndex)
            {
                u32 RegionIndex = Context->BaseRegionIndex + BatchIndex;
                PMCTraceBeginRegion(Tracer, RegionIndex);
                CountNonZeroesWithBranch(BufferCount, BufferData);
                PMCTraceEndRegion(Tracer, RegionIndex);
            }
            
            for(u32 BatchIndex = 0; BatchIndex < TEST_BATCH_SIZE; ++BatchIndex)
            {
                u32 RegionIndex = Context->BaseRegionIndex + BatchIndex;
                pmctrace_result Result = PMCTraceGetOrWaitForResult(Tracer, RegionIndex);
                if(NoErrors(Tracer) && (Context->BestResult.TSCElapsed > Result.TSCElapsed))
                {
                    Context->BestResult = Result;
                }
            }
        }
    }
    else
    {
        printf("ERROR: Unable to allocate test memory\n");
    }
    
    return 0;
}

int main(void)
{
    pmctrace_pmc_definition_array PMCDefs =
    {
        {
            PMCTrace_RETIRED_BR_INST,
            PMCTrace_RETIRED_BR_INST_MISP,
            PMCTrace_CYCLES_NOT_IN_HALT,
        }
    };
    
    printf("Starting trace...\n");
    pmctrace_client Tracer = PMCTraceStartTracing(PMCDefs);
    
    thread_context Threads[16] = {};
    //thread_context Threads[2] = {};
    HANDLE ThreadHandles[ArrayCount(Threads)] = {};
    
    printf("Launching threads...\n");
    for(u32 ThreadIndex = 0; ThreadIndex < ArrayCount(ThreadHandles); ++ThreadIndex)
    {
        thread_context *Thread = Threads + ThreadIndex;
        Thread->Tracer = &Tracer;
        Thread->BufferCount = 64*1024*1024;
        Thread->NonZeroCount = ThreadIndex*8192;
        Thread->BaseRegionIndex = ThreadIndex*10*TEST_BATCH_SIZE;
        
        ThreadHandles[ThreadIndex] = CreateThread(0, 0, TestThread, Thread, 0, 0);
    }
    
    printf("Waiting for threads to complete...\n");
    WaitForMultipleObjects(ArrayCount(Threads), ThreadHandles, TRUE, INFINITE);
        
    if(NoErrors(&Tracer))
    {
        for(u32 ThreadIndex = 0; ThreadIndex < ArrayCount(ThreadHandles); ++ThreadIndex)
        {
            thread_context *Thread = Threads + ThreadIndex;
            pmctrace_result BestResult = Thread->BestResult;
            
            printf("\nTHREAD %u - %llu non-zeroes:\n", ThreadIndex, Thread->NonZeroCount);
            printf("  %llu TSC elapsed / %llu iterations [%u switch%s]\n",
                   BestResult.TSCElapsed, Thread->BufferCount, BestResult.ContextSwitchCount,
                   (BestResult.ContextSwitchCount != 1) ? "es" : "");
            for(u32 CI = 0; CI < BestResult.PMCCount; ++CI)
            {
                printf("  %llu %s\n", BestResult.Counters[CI], PMCDefs.Defs[CI].PrintableName);
            }
        }
    }
    else
    {
        printf("CLIENT ERROR: %s\n", GetClientErrorMessage(&Tracer));
        printf("SERVER ERROR: %s\n", GetServerErrorMessage(&Tracer));
    }
    
    printf("Stopping trace...\n");
    PMCTraceStopPMCTracing(&Tracer);
    
    return 0;
}
