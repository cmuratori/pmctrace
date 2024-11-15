/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

#if !defined(PMCTRACE_VERSION)

//
// NOTE(casey): Interface
//

#include <stdint.h>

#define PMCTRACE_VERSION 1
#define PMCTRACE_VERSION_STRING "1"

#define PMCTRACE_MAX_PMC_COUNT 8
#define PMCTRACE_MAX_REGION_COUNT 65536
#define PMCTRACE_MAX_ERROR_LENGTH 256

#pragma pack(push, 1)
enum pmctrace_pmc_flag
{
    PMCTrace_Intel_Invert = (1 << 0),
    PMCTrace_Intel_EdgeDetect = (1 << 1),
    PMCTrace_Intel_AnyThread = (1 << 2), // TODO(casey): I assume this actually works backwards through ETW, so it should probably be called "OnlyThread" or something?
    PMCTrace_ARM_AllowHalt = (1 << 3),
};
typedef struct pmctrace_pmc_definition
{
    char const *PrintableName; // NOTE(casey): This is not used by the tracer, it is solely for the user's convenience
    
    uint32_t Event;
    uint32_t Unit;
    uint32_t CounterMask;
    uint32_t Flags; // NOTE(casey): Combination of values from pmctrace_pmc_flag

    uint32_t Interval; // NOTE(casey): If left as 0, will be automatically filled in to a default value
} pmctrace_pmc_definition;

typedef struct pmctrace_pmc_definition_array
{
    pmctrace_pmc_definition Defs[PMCTRACE_MAX_PMC_COUNT];
} pmctrace_pmc_definition_array;

typedef struct pmctrace_result
{
    uint64_t Counters[PMCTRACE_MAX_PMC_COUNT];
    
    uint64_t TSCElapsed;
    uint32_t ContextSwitchCount;
    uint16_t Reserved;
    uint8_t PMCCount;
    volatile int8_t Completed;
} pmctrace_result;
#pragma pack(pop)

#endif

//
// NOTE(casey): Implementation
//

#if PMCTRACE_INCLUDE_IMPLEMENTATION

#pragma warning(disable:4505)
#pragma warning(disable:4668)
#pragma warning(disable:5220)
#pragma warning(disable:5045)
#pragma warning(disable:4820)
#pragma warning(disable:4710)
#pragma warning(disable:4711)

#define UNICODE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <intrin.h>

#include <windows.h>
#include <psapi.h>
#include <evntrace.h>
#include <strsafe.h>

#pragma comment (lib, "advapi32")
#pragma comment (lib, "user32")

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int32_t b32;

typedef float f32;
typedef double f64;

 // NOTE(casey): This is a stronger memory barrier than necessary, but should not be harmful
#define MEMORY_FENCE _mm_mfence()

#define ArrayCount(Array) (sizeof(Array)/sizeof((Array)[0]))
#define function static

#define PMCTRACE_SHARED_MEMORY_NAME_PREFIX L"PMCTraceSharedMemory_"

#define PMCTRACE_SERVER_WINDOW_CLASS L"PMCTraceServerWindowClass"
#define PMCTRACE_SERVER_WM_START_TRACE (WM_USER + 1)
#define PMCTRACE_SERVER_WM_END_TRACE (WM_USER + 2)
#define PMCTRACE_SERVER_WM_TRAY_CLICK (WM_USER + 3)

typedef struct pmctrace_shared_memory
{
    volatile u64 ClientVersion;
    volatile u64 ServerVersion;
 
    // NOTE(casey): Client-provided information filled at creation
    u64 ProcessID;
    pmctrace_pmc_definition_array RequestedPMCs;
    
    // NOTE(casey): Server-provided information filled as-ready
    volatile u64 TraceHandle;
    volatile b32 ServerSideError;
    char ServerSideErrorMessage[PMCTRACE_MAX_ERROR_LENGTH];
    pmctrace_result Results[PMCTRACE_MAX_REGION_COUNT];
} pmctrace_shared_memory;

typedef enum pmctrace_region_operation 
{
    PMCTraceOp_None,
    
    PMCTraceOp_BeginRegion,
    PMCTraceOp_EndRegion,
    
    PMCTraceOp_Count,
} pmctrace_region_operation;

typedef struct pmctrace_etw_marker_userdata
{
    u64 ClientUniqueID;
    u32 DestRegionIndex;
    u32 Reserved;
} pmctrace_etw_marker_userdata;

typedef struct pmctrace_client
{
    u64 UniqueID;
    
    HANDLE SharedMapping;
    pmctrace_shared_memory *Shared;
    
    TRACEHANDLE TraceHandle;
    b32 ClientSideError;
    u32 Reserved;

    char ClientSideErrorMessage[PMCTRACE_MAX_ERROR_LENGTH];
    pmctrace_result ErrorResult;
} pmctrace_client;

typedef struct pmctrace_etw_marker
{
    EVENT_TRACE_HEADER Header;
    pmctrace_etw_marker_userdata UserData;
} pmctrace_etw_marker;

static const GUID TraceMarkerCategoryGuid = {0x5c96d7f7, 0xb1ea, 0x4fbe, {0x86, 0x55, 0xe0, 0x43, 0x1e, 0x23, 0x2e, 0x53}};

function void TraceError(pmctrace_client *Client, char *Message)
{
    Client->ClientSideError = 1;
    wsprintfA(Client->ClientSideErrorMessage, "%s", Message);
}

function b32 PMCTraceResultIsComplete(pmctrace_result *RegionResult)
{
    b32 Result = RegionResult->Completed;
    return Result;
}

function b32 NoErrors(pmctrace_client *Client)
{
    b32 Result = (!Client->ClientSideError && !Client->Shared->ServerSideError);
    return Result;
}

function char *GetClientErrorMessage(pmctrace_client *Client)
{
    char *Result = Client->ClientSideErrorMessage;
    return Result;
}

function char *GetServerErrorMessage(pmctrace_client *Client)
{
    char *Result = "";
    if(Client->Shared)
    {
        Result = Client->Shared->ServerSideErrorMessage;
    }
    
    return Result;
}

function void SendServerMessage(pmctrace_client *Client, UINT Message, LPARAM Param)
{
    HWND ServerWindow = FindWindowW(PMCTRACE_SERVER_WINDOW_CLASS, 0);
    if(IsWindow(ServerWindow))
    {
        SendMessageW(ServerWindow, Message, Client->UniqueID, Param);
    }
    else
    {
        TraceError(Client, "Unable to find server window");
    }
}

function pmctrace_client PMCTraceStartTracing(pmctrace_pmc_definition_array RequestedPMCs)
{
    pmctrace_client Client = {};
    
    u32 MapSize = sizeof(pmctrace_shared_memory);
    Client.UniqueID = __rdtsc();
    
    wchar_t SharedName[256];
    StringCbPrintfW(SharedName, sizeof(SharedName), PMCTRACE_SHARED_MEMORY_NAME_PREFIX "%016llx", Client.UniqueID);
    Client.SharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, MapSize, SharedName);
    Client.Shared = (pmctrace_shared_memory *)MapViewOfFile(Client.SharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, MapSize);
    if(Client.Shared)
    {
        Client.Shared->ClientVersion = PMCTRACE_VERSION;
        Client.Shared->ProcessID = GetCurrentProcessId();
        Client.Shared->RequestedPMCs = RequestedPMCs;
        SendServerMessage(&Client, PMCTRACE_SERVER_WM_START_TRACE, (LPARAM)Client.Shared);
        Client.TraceHandle = Client.Shared->TraceHandle;
    }
    else
    {
        TraceError(&Client, "Unable to allocate shared memory");
    }
    
    return Client;
}

function void PMCTraceStopPMCTracing(pmctrace_client *Client)
{
    SendServerMessage(Client, PMCTRACE_SERVER_WM_END_TRACE, 0);
    
    if(Client->Shared)
    {
        UnmapViewOfFile(Client->Shared);
    }
    
    if(Client->SharedMapping)
    {
        CloseHandle(Client->SharedMapping);
    }
    
    pmctrace_client Clear = {};
    *Client = Clear;
}

function void PMCTraceRegionOp(pmctrace_client *Client, pmctrace_region_operation Op, u32 RegionIndex)
{
    if(Client->Shared)
    {
        pmctrace_etw_marker TraceMarker = {};
        
        TraceMarker.Header.Size = sizeof(TraceMarker);
        TraceMarker.Header.Flags = WNODE_FLAG_TRACED_GUID;
        TraceMarker.Header.Guid = TraceMarkerCategoryGuid;
        TraceMarker.Header.Class.Type = (u8)Op;
        
        TraceMarker.UserData.ClientUniqueID = Client->UniqueID;
        TraceMarker.UserData.DestRegionIndex = RegionIndex;
        
        if(TraceEvent(Client->TraceHandle, &TraceMarker.Header) != ERROR_SUCCESS)
        {
            TraceError(Client, "Unable to insert ETW marker");
        }
    }
}

function pmctrace_result *PMCTraceGetResult(pmctrace_client *Client, u32 RegionIndex)
{
    pmctrace_result *Result = &Client->ErrorResult;
    if(Client->Shared && (RegionIndex < ArrayCount(Client->Shared->Results)))
    {
        Result = Client->Shared->Results + RegionIndex;
    }
    
    return Result;
}

function void PMCTraceBeginRegion(pmctrace_client *Client, u32 RegionIndex)
{
    pmctrace_result *Result = PMCTraceGetResult(Client, RegionIndex);
    Result->Completed = 0;
    PMCTraceRegionOp(Client, PMCTraceOp_BeginRegion, RegionIndex);
}

function void PMCTraceEndRegion(pmctrace_client *Client, u32 RegionIndex)
{
    PMCTraceRegionOp(Client, PMCTraceOp_EndRegion, RegionIndex);
}

function pmctrace_result PMCTraceGetOrWaitForResult(pmctrace_client *Client, u32 RegionIndex)
{
    pmctrace_result *Result = PMCTraceGetResult(Client, RegionIndex);
    while(NoErrors(Client) && !PMCTraceResultIsComplete(Result))
    {
        /* NOTE(casey): This is a spin-lock loop on purpose, because if there was a Sleep() in here
           or some other yield, it might cause Windows to demote this region, which we don't want.
           Ideally, we rarely spin here, because there are enough traces in flight to ensure that,
           whenever we check for results, there are some waiting, except perhaps at the very end of a
           batch. */
        
        _mm_pause();
    }
    
    MEMORY_FENCE;
    
    return *Result;
}

#endif

