/* ========================================================================

   (C) Copyright 2024 by Molly Rocket, Inc., All Rights Reserved.
   
   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any damages
   arising from the use of this software.
   
   Please see https://computerenhance.com for more information
   
   ======================================================================== */

#pragma warning(disable:4668)
#pragma warning(disable:5220)
#pragma warning(disable:5045)
#pragma warning(disable:4820)
#pragma warning(disable:4710)
#pragma warning(disable:4711)

#define UNICODE 1
#include <stdint.h>
#include <intrin.h>

#include <windows.h>
#include <psapi.h>
#include <evntrace.h>
#include <evntcons.h>
#include <winternl.h>

#pragma comment (lib, "ntdll")
#pragma comment (lib, "advapi32")
#pragma comment (lib, "user32")
#pragma comment (lib, "kernel32")
#pragma comment (lib, "shell32")

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t b32;

#define ArrayCount(Array) (sizeof(Array)/sizeof((Array)[0]))
#define function static

#define PMCTRACE_INCLUDE_IMPLEMENTATION 1
#include "pmctrace.h"

#define WIN32_TRACE_OPCODE_SWITCH_THREAD 36
#define WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER 51
#define WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT 52

#define WIN32_PROCESSOR_CHECK_TIMER 1

static GUID Win32ThreadEventGuid = {0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
static GUID Win32DPCEventGuid = {0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};

#define PMC_DEBUG_LOG 0
#if PMC_DEBUG_LOG
#define DEBUG_PRINT(Format, ...) \
{ \
    int64_t Max = Server->LogEnd - Server->LogAt; \
    int64_t Temp = wsprintfA(Server->LogAt, Format, __VA_ARGS__); \
    if((Temp > 0) && (Temp < Max)) {Server->LogAt += Temp;} \
}
#else
#define DEBUG_PRINT(...)
#endif

#pragma pack(push,1)
struct profile_remove_information
{                                                                     
    u32 Operation;
    u32 SourceIndex;
    u32 CPUInfoHierarchy[3];
};

union profile_event_description_v2
{
    struct intel
    {                                            
        u8 Event;
        u8 Unit;
        
        u8 CounterMask;
        u8 Invert;
        
        u8 AnyThread;
        u8 EdgeDetect;
    } Intel;                                           
    
    struct amd
    {                                             
        u8 Event;
        u8 Unit;
    } AMD;                                           
    
    struct arm
    {                                            
        u32 Event;
        u8 AllowsHalt;
    } ARM;
    
    u64 AllContents;
};

struct profile_add_information_v2
{                                                                                
    u32 Operation;
    u32 Version;
    profile_event_description_v2 EventDesc;
    u32 CPUInfoHierarchy[3];
    u32 Interval;
    u8 Persist;
    u8 UnusedPad0;
    wchar_t Name[127];
    u8 UnusedPad1;
}; 

struct etw_thread_switch_userdata
{
    DWORD NewThreadId;
    DWORD OldThreadId;
};

struct win32_trace_description
{
    EVENT_TRACE_PROPERTIES_V2 Properties;
    WCHAR Name[1024];
};
#pragma pack(pop)

struct pmctrace_region
{
    pmctrace_region *Next;
    u32 OnThreadID;
    u32 Ordinal;
};

struct pmctrace_cpu
{
    pmctrace_region *FirstRunningRegion;
    pmctrace_region *WaitingForSysExitToStart;
    
    u64 LastSysEnterCounters[PMCTRACE_MAX_PMC_COUNT];
    u64 LastSysEnterTSC;
    b32 LastSysEnterValid;
    u32 Pad;
};

struct pmctrace_server
{
    u64 ClientUniqueID;
    HANDLE SharedMapping;
    pmctrace_shared_memory *Shared;
    
    win32_trace_description Win32TraceDesc;
    TRACEHANDLE TraceHandle;
    TRACEHANDLE TraceSession;
    HANDLE ProcessingThread;
    
    pmctrace_cpu *CPUs; // NOTE(casey): [CPUCount]
    pmctrace_region *FirstSuspendedRegion;
    
    u32 CPUCount;
    u32 PMCCount;

    u32 TracesRun;
    u32 IgnoredB;
    
    pmctrace_region Regions[PMCTRACE_MAX_REGION_COUNT];

#if PMC_DEBUG_LOG
    char *Log;
    char *LogAt;
    char *LogEnd;
#endif
};

function void TraceError(pmctrace_server *Server, char const *Message)
{
    DEBUG_PRINT("%s\n", Message);
    if(Server->Shared)
    {
        Server->Shared->ServerSideError = true;
        
        char const *Source = Message;
        char *Dest = Server->Shared->ServerSideErrorMessage;
        while((*Dest++ = *Source++)) {}
    }
}

function void *Win32AllocateSize(u64 Size)
{
    void *Result = VirtualAlloc(0, Size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    return Result;
}

function void Win32Deallocate(void *Memory)
{
    if(Memory)
    {
        VirtualFree(Memory, 0, MEM_RELEASE);
    }
}

function b32 GUIDsAreEqual(GUID A, GUID B)
{
    __m128i Compare = _mm_cmpeq_epi8(_mm_loadu_si128((__m128i *)&A), _mm_loadu_si128((__m128i *)&B));
    int Mask = _mm_movemask_epi8(Compare);
    b32 Result = (Mask == 0xffff);
    return Result;
}

function void Win32FindPMCData(pmctrace_server *Server, EVENT_RECORD *Event, u64 *PMCData)
{
    u32 PMCCount = Server->PMCCount;
    b32 PMCPresent = false;
    for(u32 EDIndex = 0; EDIndex < Event->ExtendedDataCount; ++EDIndex)
    {
        EVENT_HEADER_EXTENDED_DATA_ITEM *Item = Event->ExtendedData + EDIndex;
        if(Item->ExtType == EVENT_HEADER_EXT_TYPE_PMC_COUNTERS)
        {
            EVENT_EXTENDED_ITEM_PMC_COUNTERS *PMC = (EVENT_EXTENDED_ITEM_PMC_COUNTERS *)Item->DataPtr;
            if(Item->DataSize == (sizeof(u64)*PMCCount))
            {
                for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
                {
                    PMCData[PMCIndex] = PMC->Counter[PMCIndex];
                }
            }
            else
            {
                TraceError(Server, "Unexpected PMC data size");
            }
            
            PMCPresent = true;
            break;
        }
    }
    
    if(!PMCPresent)
    {
        TraceError(Server, "Unexpected PMC data count");
    }
}

function void ApplyPMCsAsOpen(pmctrace_result *Results, u32 PMCCount, u64 *PMCData, u64 TSC)
{
    for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
    {
        Results->Counters[PMCIndex] -= PMCData[PMCIndex];
    }
    
    Results->TSCElapsed -= TSC;
}

function void ApplyPMCsAsClose(pmctrace_result *Results, u32 PMCCount, u64 *PMCData, u64 TSC)
{
    for(u32 PMCIndex = 0; PMCIndex < PMCCount; ++PMCIndex)
    {
        Results->Counters[PMCIndex] += PMCData[PMCIndex];
    }
    
    Results->TSCElapsed += TSC;
}

function pmctrace_result *ResultsFromRegion(pmctrace_server *Server, pmctrace_region *Region)
{
    pmctrace_result *Result = (Server->Shared->Results + Region->Ordinal);
    return Result;
}

function void CALLBACK Win32ProcessETWEvent(EVENT_RECORD *Event)
{
    pmctrace_server *Server = (pmctrace_server *)Event->UserContext;
    
    GUID EventGUID = Event->EventHeader.ProviderId;
	UCHAR Opcode = Event->EventHeader.EventDescriptor.Opcode;
    u32 CPUID = GetEventProcessorIndex(Event);
    u32 PMCCount = Server->PMCCount;
    u64 TSC = (u64)Event->EventHeader.TimeStamp.QuadPart;
    u64 PMCData[PMCTRACE_MAX_PMC_COUNT] = {};
    
    if(CPUID < Server->CPUCount)
    {
        pmctrace_cpu *CPU = &Server->CPUs[CPUID];
        
        if(GUIDsAreEqual(EventGUID, TraceMarkerCategoryGuid))
        {
            pmctrace_etw_marker_userdata *Marker = (pmctrace_etw_marker_userdata *)Event->UserData;
            if(Marker->ClientUniqueID == Server->ClientUniqueID)
            {
                if(Marker->DestRegionIndex < ArrayCount(Server->Shared->Results))
                {
                    pmctrace_region *Region = Server->Regions + Marker->DestRegionIndex;
                    pmctrace_result *Results = Server->Shared->Results + Marker->DestRegionIndex;
                    
                    if(Opcode == PMCTraceOp_BeginRegion)
                    {
                        DEBUG_PRINT("%Ix OPEN %u\n", __rdtsc(), Marker->DestRegionIndex);
                        
                        // NOTE(casey): Add this region to the list of regions running on this CPU core
                        *Results = {};
                        Results->PMCCount = (u8)PMCCount;

                        Region->OnThreadID = Event->EventHeader.ThreadId;
                        Region->Ordinal = Marker->DestRegionIndex;
                        Region->Next = CPU->FirstRunningRegion;
                        CPU->FirstRunningRegion = Region;
                        
                        // NOTE(casey): Mark that this region will get its starting counter values from the next SysExit event
                        if(CPU->WaitingForSysExitToStart)
                        {
                            TraceError(Server, "Additional region opened on the same thread before SysExit event started the prior region");
                        }
                        CPU->WaitingForSysExitToStart = Region;
                    }
                    else if(Opcode == PMCTraceOp_EndRegion)
                    {
                        DEBUG_PRINT("CLOSE %u\n", Marker->DestRegionIndex);
                        
                        if(CPU->LastSysEnterValid)
                        {
                            // NOTE(casey): Apply the counters and TSC we saved from the preceeding SysEnter event
                            ApplyPMCsAsClose(Results, PMCCount, CPU->LastSysEnterCounters, CPU->LastSysEnterTSC);
                            
                            CPU->LastSysEnterValid = false;
                        }
                        else
                        {
                            TraceError(Server, "No ENTER for CLOSE event");
                        }
                        
                        // NOTE(casey): Remove this trace from the list of traces running on this CPU core
                        pmctrace_region **FindRegion = &CPU->FirstRunningRegion;
                        while(*FindRegion)
                        {
                            if(*FindRegion == Region)
                            {
                                *FindRegion = Region->Next;
                                break;
                            }
                            
                            FindRegion = &(*FindRegion)->Next;
                        }
                        
                        // NOTE(casey): These do not need to be cleared, but it helps for clarity in case
                        // anyone inspects things in the debugger
                        Region->Next = {};
                        Region->OnThreadID = {};
                        
                        // NOTE(casey): Make sure everything is written back before signaling completion
                        MEMORY_FENCE;
                        
                        // NOTE(casey): Signal completion to anyone waiting for these results
                        Results->Completed = true;
                    }
                    else
                    {
                        TraceError(Server, "Unrecognized ETW marker type");
                    }
                }
                else
                {
                    TraceError(Server, "Region index out-of-bounds");
                }
            }
        }
        else if(GUIDsAreEqual(EventGUID, Win32ThreadEventGuid))
        {
            if(Opcode == WIN32_TRACE_OPCODE_SWITCH_THREAD)
            {
                if(Event->UserDataLength == 24)
                {
                    etw_thread_switch_userdata *Switch = (etw_thread_switch_userdata *)Event->UserData;
                    
                    // NOTE(casey): Get the PMC data once, since we may need it multiple times as we
                    // process suspending and resuming regions
                    if(CPU->FirstRunningRegion || Server->FirstSuspendedRegion)
                    {
                        Win32FindPMCData(Server, Event, PMCData);
                    }
                    
                    // NOTE(casey): Suspend any existing regions running on this CPU core
                    while(CPU->FirstRunningRegion)
                    {
                        DEBUG_PRINT("SWITCH FROM\n");
                        
                        pmctrace_region *Region = CPU->FirstRunningRegion;
                        pmctrace_result *Results = ResultsFromRegion(Server, Region);
                        
                        if(Switch->OldThreadId != Region->OnThreadID)
                        {
                            TraceError(Server, "Switched thread ID mismatch");
                        }
                        
                        // NOTE(casey): Apply the current PMCs as "ending" counters
                        ApplyPMCsAsClose(Results, PMCCount, PMCData, TSC);
                        
                        // NOTE(casey): Record that this region has incurred a context switch
                        ++Results->ContextSwitchCount;
                        
                        // NOTE(casey): Remove this region from the running set
                        CPU->FirstRunningRegion = Region->Next;
                        
                        // NOTE(casey): Put this region on the suspended list
                        Region->Next = Server->FirstSuspendedRegion;
                        Server->FirstSuspendedRegion = Region;
                    }
                    
                    // NOTE(casey): Look for matching region IDs that will be resumed
                    pmctrace_region **FindRegion = &Server->FirstSuspendedRegion;
                    while(*FindRegion)
                    {
                        if((*FindRegion)->OnThreadID == Switch->NewThreadId)
                        {
                            DEBUG_PRINT("SWITCH TO\n");
                            
                            pmctrace_region *Region = *FindRegion;
                            pmctrace_result *Results = ResultsFromRegion(Server, Region);
                            
                            // NOTE(casey): Apply the current PMCs as "begin" counters
                            ApplyPMCsAsOpen(Results, PMCCount, PMCData, TSC);
                            
                            // NOTE(casey): Remove this region from the suspended list
                            *FindRegion = (*FindRegion)->Next;
                            
                            // NOTE(casey): Put this region on the running list
                            Region->Next = CPU->FirstRunningRegion;
                            CPU->FirstRunningRegion = Region;
                        }
                        else
                        {
                            FindRegion = &(*FindRegion)->Next;
                        }
                    }
                }
                else
                {
                    TraceError(Server, "Unexpected CSwitch data size");
                }
            }
        }
        else if(GUIDsAreEqual(EventGUID, Win32DPCEventGuid))
        {
            if(Opcode == WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER)
            {
                // NOTE(casey): Remember the state at this SysEnter so it can be applied to a
                // region later if there is a following Close event.
                if(CPU->FirstRunningRegion)
                {
                    DEBUG_PRINT("ENTER\n");
                    
                    CPU->LastSysEnterValid = true;
                    CPU->LastSysEnterTSC = TSC;
                    Win32FindPMCData(Server, Event, CPU->LastSysEnterCounters);
                }
            }
            else if(Opcode == WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT)
            {
                // NOTE(casey): If there was a region waiting to open on the next syscall exit,
                // apply the PMCs to that region
                if(CPU->WaitingForSysExitToStart)
                {
                    DEBUG_PRINT("EXIT\n");
                    
                    pmctrace_region *Region = CPU->WaitingForSysExitToStart;
                    pmctrace_result *Results = ResultsFromRegion(Server, Region);
                    
                    CPU->WaitingForSysExitToStart = 0;
                
                    Win32FindPMCData(Server, Event, PMCData);
                    ApplyPMCsAsOpen(Results, PMCCount, PMCData, TSC);
                }
            }
        }
    }
    else
    {
        TraceError(Server, "Out-of-bounds CPUID in ETW event");
    }
}

function DWORD CALLBACK Win32ProcessEventThread(void *Arg)
{
    TRACEHANDLE Session = (TRACEHANDLE)Arg;
    ProcessTrace(&Session, 1, 0, 0);
    return 0;
}

static wchar_t const *GlobalWin32PMCCounterName[PMCTRACE_MAX_PMC_COUNT] =
{
    L"PMCTraceServerCounter0",
    L"PMCTraceServerCounter1",
    L"PMCTraceServerCounter2",
    L"PMCTraceServerCounter3",
    L"PMCTraceServerCounter4",
    L"PMCTraceServerCounter5",
    L"PMCTraceServerCounter6",
    L"PMCTraceServerCounter7",
};

extern "C" __declspec(dllimport) NTSTATUS WINAPI NtSetSystemInformation(SYSTEM_INFORMATION_CLASS SystemInformationClass,
                                                                        PVOID SystemInformation, ULONG SystemInformationLength);
function b32 Win32AddNewPMC(wchar_t const *Name, pmctrace_pmc_definition *Def)
{
    profile_add_information_v2 AddInfo = {};
    AddInfo.Operation = 0x15;
    AddInfo.Version = 2;
    AddInfo.CPUInfoHierarchy[0] = 0xffffffff;
    AddInfo.CPUInfoHierarchy[1] = 0xffffffff;
    AddInfo.CPUInfoHierarchy[2] = 0xffffffff;
    AddInfo.Interval = Def->Interval ? Def->Interval : 0x2000003;
    for(u32 At = 0; (At < ArrayCount(AddInfo.Name)) && Name[At]; ++At)
    {
        AddInfo.Name[At] = Name[At];
    }
    
    /* NOTE(caesy): Filling in the event part is annoying because it is a "multum in parvo" definition where the CPU
       architecture determiens the format. Since we'd rather not have the user tell us what architecture they were
       talking about, I made this work in a way that "just works" whatever the arch is by filling in the fields in
       a particular order that ensures unused flags will not affect things, etc. */
    AddInfo.EventDesc.AllContents = Def->Event;
    AddInfo.EventDesc.Intel.Unit |= Def->Unit;
    AddInfo.EventDesc.Intel.CounterMask |= Def->CounterMask;
    if(Def->Flags & PMCTrace_Intel_Invert) {AddInfo.EventDesc.Intel.Invert = 1;}
    if(Def->Flags & PMCTrace_Intel_EdgeDetect) {AddInfo.EventDesc.Intel.EdgeDetect = 1;}
    if(Def->Flags & PMCTrace_Intel_AnyThread) {AddInfo.EventDesc.Intel.AnyThread = 1;}
    if(Def->Flags & PMCTrace_ARM_AllowHalt) {AddInfo.EventDesc.ARM.AllowsHalt = 1;}
    
    NTSTATUS status = NtSetSystemInformation((SYSTEM_INFORMATION_CLASS)31, &AddInfo, sizeof(AddInfo));
    
    b32 Result = (status == 0);
    return Result;
}

function b32 Win32RemoveOldPMC(u32 SourceIndex)
{
    profile_remove_information Remove = {};
    Remove.Operation = 0x16;
    Remove.SourceIndex = SourceIndex;
    Remove.CPUInfoHierarchy[0] = 0xffffffff;
    Remove.CPUInfoHierarchy[1] = 0xffffffff;
    Remove.CPUInfoHierarchy[2] = 0xffffffff;
    
    NTSTATUS status = NtSetSystemInformation((SYSTEM_INFORMATION_CLASS)31, &Remove, sizeof(Remove));
    
    b32 Result = (status == 0);
    return Result;
}

function void RemovePMCIndexes(u32 IndexCount, u32 *PMCSourceIndexes)
{
    for(u32 IndexIndex = 0; IndexIndex < IndexCount; ++IndexIndex)
    {
        u32 Index = PMCSourceIndexes[IndexIndex];
        if(Index)
        {
            Win32RemoveOldPMC(Index);
        }
    }
}

function u32 Win32DefinePMCs(u32 DefCount, pmctrace_pmc_definition_array DefArray)
{
    u32 MaxCount = 0;
    for(u32 DefIndex = 0; DefIndex < DefCount; ++DefIndex)
    {
        pmctrace_pmc_definition *PMCDef = &DefArray.Defs[DefIndex];
        if(PMCDef->Event || PMCDef->Unit)
        {
            Win32AddNewPMC(GlobalWin32PMCCounterName[DefIndex], PMCDef);
            MaxCount = DefIndex + 1;
        }
    }
    
    return MaxCount;
}

function void Win32FindPMCIndexes(u32 IndexCount, u32 *PMCSourceIndexes)
{
    ULONG BufferSize;
    TraceQueryInformation(0, TraceProfileSourceListInfo, 0, 0, &BufferSize);
    BYTE *Buffer = (BYTE *)Win32AllocateSize(BufferSize);
    if(Buffer)
    {
        if(TraceQueryInformation(0, TraceProfileSourceListInfo, Buffer, BufferSize, &BufferSize) == ERROR_SUCCESS)
        {
            for(PROFILE_SOURCE_INFO *Info = (PROFILE_SOURCE_INFO *)Buffer;
                ;
                Info = (PROFILE_SOURCE_INFO *)((u8 *)Info + Info->NextEntryOffset))
            {
                for(u32 DefIndex = 0; DefIndex < IndexCount; ++DefIndex)
                {
                    if(lstrcmpW(Info->Description, GlobalWin32PMCCounterName[DefIndex]) == 0)
                    {
                        PMCSourceIndexes[DefIndex] = Info->Source;
                    }
                }
                
                if(Info->NextEntryOffset == 0)
                {
                    break;
                }
            }
        }
    }
    
    Win32Deallocate(Buffer);
}

function void StartTracing(pmctrace_server *Server)
{
    const WCHAR TraceName[] = L"Win32PMCTrace";
    
    EVENT_TRACE_PROPERTIES_V2 *Props = &Server->Win32TraceDesc.Properties;
    Props->Wnode.BufferSize = sizeof(Server->Win32TraceDesc);
    Props->LoggerNameOffset = offsetof(win32_trace_description, Name);
    
    // NOTE(casey): Attempt to stop any existing orphaned trace from a previous run
    ControlTraceW(0, TraceName, (EVENT_TRACE_PROPERTIES *)Props, EVENT_TRACE_CONTROL_STOP);
    
    /* NOTE(casey): Attempt to start the trace. Note that the fields we care about MUST
       be filled in after the EVENT_TRACE_CONTROL_STOP ControlTraceW call, because
       that call will overwrite the properties! */
    Props->Wnode.ClientContext = 3;
    Props->Wnode.Flags = WNODE_FLAG_TRACED_GUID | WNODE_FLAG_VERSIONED_PROPERTIES;
    Props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    Props->VersionNumber = 2;
    Props->EnableFlags = EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_NO_SYSCONFIG | EVENT_TRACE_FLAG_SYSTEMCALL;
    ULONG StartStatus = StartTraceW(&Server->TraceHandle, TraceName, (EVENT_TRACE_PROPERTIES*)Props);
    
    if(StartStatus != ERROR_SUCCESS)
    {
        TraceError(Server, "Unable to start trace - may occur if server is not run as admin");
    }
    
    u32 PMCSourceIndex[PMCTRACE_MAX_PMC_COUNT] = {};
    Win32FindPMCIndexes(ArrayCount(PMCSourceIndex), PMCSourceIndex);
    RemovePMCIndexes(ArrayCount(PMCSourceIndex), PMCSourceIndex);
    Server->PMCCount = Win32DefinePMCs(ArrayCount(PMCSourceIndex), Server->Shared->RequestedPMCs);
    Win32FindPMCIndexes(ArrayCount(PMCSourceIndex), PMCSourceIndex);
    
    ULONG Status = TraceSetInformation(Server->TraceHandle, TracePmcCounterListInfo,
                                       PMCSourceIndex, Server->PMCCount * sizeof(PMCSourceIndex[0]));
    if(Status != ERROR_SUCCESS)
    {
        TraceError(Server, "Unable to select PMCs");
    }
    
    CLASSIC_EVENT_ID EventIDs[] =
    {
        {Win32ThreadEventGuid, WIN32_TRACE_OPCODE_SWITCH_THREAD},
        {Win32DPCEventGuid, WIN32_TRACE_OPCODE_SYSTEMCALL_ENTER},
        {Win32DPCEventGuid, WIN32_TRACE_OPCODE_SYSTEMCALL_EXIT},
    };
    
    ULONG EventListStatus = TraceSetInformation(Server->TraceHandle, TracePmcEventListInfo, EventIDs, sizeof(EventIDs));
    if(EventListStatus != ERROR_SUCCESS)
    {
        TraceError(Server, "Unable to select events");
    }
    
    EVENT_TRACE_LOGFILEW Log = {};
    Log.LoggerName = Server->Win32TraceDesc.Name;
    Log.EventRecordCallback = Win32ProcessETWEvent;
    Log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP | PROCESS_TRACE_MODE_REAL_TIME;
    Log.Context = Server;
    
    Server->TraceSession = OpenTraceW(&Log);
    if(Server->TraceSession == INVALID_PROCESSTRACE_HANDLE)
    {
        TraceError(Server, "Unable to open trace");
    }
    
    Server->ProcessingThread = CreateThread(0, 0, Win32ProcessEventThread, (void *)Server->TraceSession, 0, 0);
    if(Server->ProcessingThread == 0)
    {
        TraceError(Server, "Unable to create processing thread");
    }
}

function void ResetServer(pmctrace_server *Server)
{
    // TODO(casey): Try to verify that 0 is never a valid trace handle - it's unclear from the documentation
    if(Server->TraceHandle)
    {
        ControlTraceW(Server->TraceHandle, 0, (EVENT_TRACE_PROPERTIES *)&Server->Win32TraceDesc.Properties, EVENT_TRACE_CONTROL_STOP);
    }
    
    if(Server->TraceSession != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(Server->TraceSession);
    }
    
    if(Server->ProcessingThread)
    {
        WaitForSingleObject(Server->ProcessingThread, INFINITE);
        CloseHandle(Server->ProcessingThread);
    }

    Server->TraceHandle = 0;
    Server->TraceSession = INVALID_PROCESSTRACE_HANDLE;
    Server->ProcessingThread = 0;
    
    if(Server->Shared)
    {
        TraceError(Server, "Server reset");
        UnmapViewOfFile(Server->Shared);
        Server->Shared = 0;
    }
    
    if(Server->SharedMapping)
    {
        CloseHandle(Server->SharedMapping);
        Server->SharedMapping = 0;
    }

    // NOTE(casey): Technically we could leave our counter definitions around, there's no reason not to.
    // But it seems nicer to remove them when not in use, so they don't show up unnecessarily in the ETW listing.
    u32 PMCSourceIndex[PMCTRACE_MAX_PMC_COUNT] = {};
    Win32FindPMCIndexes(ArrayCount(PMCSourceIndex), PMCSourceIndex);
    RemovePMCIndexes(ArrayCount(PMCSourceIndex), PMCSourceIndex);
}

function void SwitchToNewTrace(HWND Window, pmctrace_server *Server, u64 UniqueID, u64 BaseAddress)
{
    ResetServer(Server);
    
    Server->ClientUniqueID = UniqueID;

    wchar_t SharedName[256] = PMCTRACE_SHARED_MEMORY_NAME_PREFIX;
    wchar_t Hex[16] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
    wchar_t *At = SharedName + (sizeof(PMCTRACE_SHARED_MEMORY_NAME_PREFIX)/2) - 1;
    for(u32 BitSlice = 0; BitSlice < 64; BitSlice += 4)
    {
        *At++ = Hex[(UniqueID >> (60 - BitSlice)) & 0xf];
    }
    *At = 0;

    Server->SharedMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SharedName);
#if 1
    (void)BaseAddress;
    Server->Shared = (pmctrace_shared_memory *)MapViewOfFile(Server->SharedMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             sizeof(pmctrace_shared_memory));
#else
    Server->Shared = (pmctrace_shared_memory *)MapViewOfFile3(Server->SharedMapping, 0, (void *)BaseAddress, 
                                                              0, sizeof(pmctrace_shared_memory), 0,
                                                              PAGE_READWRITE, 0, 0);
#pragma comment (lib, "onecore")
#endif
    if(Server->Shared)
    {
        Server->Shared->ServerVersion = PMCTRACE_VERSION;
        if(Server->Shared->ClientVersion == PMCTRACE_VERSION)
        {
            StartTracing(Server);
            SetTimer(Window, WIN32_PROCESSOR_CHECK_TIMER, 1000, 0);
            
            Server->Shared->TraceHandle = Server->TraceHandle;
            ++Server->TracesRun;
            
            MEMORY_FENCE;
        }
        else
        {
            ResetServer(Server);
        }
    }
}

function u32 Win32AddMenuItem(HMENU MenuHandle, wchar_t *ItemText, b32 Checked, b32 Enabled)
{
    MENUITEMINFO MenuItem = {};
    MenuItem.cbSize = sizeof(MenuItem);
    MenuItem.fMask = MIIM_ID | MIIM_STATE | MIIM_DATA | MIIM_TYPE;
    MenuItem.fType = MFT_STRING;
    MenuItem.fState = (UINT)((Checked ? MFS_CHECKED : MFS_UNCHECKED) |
                             (Enabled ? MFS_ENABLED : MFS_DISABLED));
    MenuItem.wID = (UINT)GetMenuItemCount(MenuHandle) + 1;
    MenuItem.dwTypeData = ItemText;
    
    InsertMenuItemW(MenuHandle, (UINT)GetMenuItemCount(MenuHandle), true, &MenuItem);
    
    return MenuItem.wID;
}

function void Win32AddMenuSeparator(HMENU MenuHandle)
{
    MENUITEMINFO MenuItem;
    MenuItem.cbSize = sizeof(MenuItem);
    MenuItem.fMask = MIIM_ID | MIIM_DATA | MIIM_TYPE;
    MenuItem.fType = MFT_SEPARATOR;
    MenuItem.wID = 0;
    MenuItem.dwItemData = 0;
    
    InsertMenuItemW(MenuHandle, (UINT)GetMenuItemCount(MenuHandle), true, &MenuItem);
}

function void HandleTrayClick(pmctrace_server *Server, HWND Window, LPARAM LParam)
{
    switch(LParam)
    {
        case WM_LBUTTONDOWN:
        {
            POINT MousePosition = {0, 0};
            GetCursorPos(&MousePosition);
            
            SetForegroundWindow(Window);
            HMENU WindowMenu = CreatePopupMenu();
            
            wchar_t Temp[256];
            wsprintfW(Temp, L"PMC Trace Server v%u", PMCTRACE_VERSION);
            Win32AddMenuItem(WindowMenu, Temp, false, false);
            
            wsprintfW(Temp, L"Trace count: %u", Server->TracesRun);
            Win32AddMenuItem(WindowMenu, Temp, false, false);
            
            Win32AddMenuSeparator(WindowMenu);
            
            u32 StopTrace = Win32AddMenuItem(WindowMenu, L"Stop current trace", false, (Server->TraceHandle != 0));
            u32 ExitCommand = Win32AddMenuItem(WindowMenu, L"Shutdown server", false, true);
            u32 PickedIndex = (u32)TrackPopupMenu(WindowMenu,
                                                  TPM_LEFTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD | TPM_CENTERALIGN | TPM_TOPALIGN,
                                                  MousePosition.x, MousePosition.y,
                                                  0, Window, 0);
            if(PickedIndex == ExitCommand)
            {
                PostQuitMessage(0);
            }
            if(PickedIndex == StopTrace)
            {
                ResetServer(Server);
            }
            
            DestroyMenu(WindowMenu);
        } break;
        
        default:
        {
            // An ignored tray message
        } break;
    }
}

function void HandleTimer(pmctrace_server *Server, HWND Window)
{
    b32 ContinueTimer = false;
    
    if(Server->Shared)
    {
        HANDLE ClientProcess = OpenProcess(PROCESS_QUERY_INFORMATION, false, (DWORD)Server->Shared->ProcessID);
        if(ClientProcess)
        {
            DWORD ExitCode = 0;
            GetExitCodeProcess(ClientProcess, &ExitCode);
            if(ExitCode == STILL_ACTIVE)
            {
                ContinueTimer = true;
            }
            else
            {
                ResetServer(Server);
            }
            
            CloseHandle(ClientProcess);
        }
        else
        {
            // NOTE(casey): If we can't access the process anymore, assume it is closed, and stop the trace
            ResetServer(Server);
        }
    }
    
    if(!ContinueTimer)
    {
        KillTimer(Window, WIN32_PROCESSOR_CHECK_TIMER);
    }
}

static pmctrace_server GlobalServer;
function LRESULT CALLBACK Win32MainWindowCallback(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    pmctrace_server *Server = &GlobalServer;
    
    LRESULT Result = 0;
        
    switch(Message)
    {
        case WM_DESTROY:
        {
            PostQuitMessage(0);
        } break;
        
        case PMCTRACE_SERVER_WM_START_TRACE:
        {
            SwitchToNewTrace(Window, Server, WParam, (u64)LParam);
        } break;
        
        case PMCTRACE_SERVER_WM_END_TRACE:
        {
#if PMC_DEBUG_LOG
            if(Server->Log)
            {
                HANDLE Out = CreateFileW(L"log.out", GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
                WriteFile(Out, Server->Log, (DWORD)(Server->LogAt - Server->Log), 0, 0);
                CloseHandle(Out);
                Server->LogAt = Server->Log;
                *Server->LogAt = 0;
            }
#endif
            ResetServer(Server);
        } break;
        
        case PMCTRACE_SERVER_WM_TRAY_CLICK:
        {
            HandleTrayClick(Server, Window, LParam);
        } break;
        
        case WM_TIMER:
        {
            HandleTimer(Server, Window);
        } break;
        
        default:
        {
            Result = DefWindowProcW(Window, Message, WParam, LParam);
        } break;
    }
    
    return Result;
}

extern "C" void __cdecl WinMainCRTStartup(void)
{
    HICON Icon = LoadIconW(GetModuleHandle(0), MAKEINTRESOURCE(101));
    
    SYSTEM_INFO SysInfo = {};
    GetSystemInfo(&SysInfo);
    
    pmctrace_server *Server = &GlobalServer;
    Server->TraceSession = INVALID_PROCESSTRACE_HANDLE;
    Server->CPUCount = SysInfo.dwNumberOfProcessors;
    Server->CPUs = (pmctrace_cpu *)Win32AllocateSize(Server->CPUCount * sizeof(pmctrace_cpu));
    if(Server->CPUs)
    {
#if PMC_DEBUG_LOG
        u64 RequestedLogSize = 1024*1024*1024;
        Server->Log = Server->LogAt = (char *)Win32AllocateSize(RequestedLogSize);
        if(Server->Log)
        {
            Server->LogEnd = Server->Log + RequestedLogSize;
        }
#endif
        
        HINSTANCE Instance = (HINSTANCE)GetModuleHandle(0);
        WNDCLASSW Class = {};
        Class.lpfnWndProc = Win32MainWindowCallback;
        Class.hInstance = Instance;
        Class.hIcon = Icon;
        Class.hCursor = LoadCursorW(0, IDC_ARROW);
        Class.lpszClassName = PMCTRACE_SERVER_WINDOW_CLASS;
        if(RegisterClassW(&Class))
        {
            HWND Window = CreateWindowExW(0, Class.lpszClassName, L"PMCTrace Server",
                                          0,
                                          CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                          0, 0, Instance, 0);
            if(IsWindow(Window))
            {
                NOTIFYICONDATA TrayIconData = {};
                TrayIconData.cbSize = sizeof(NOTIFYICONDATA);
                TrayIconData.hWnd = Window;
                TrayIconData.uFlags = NIF_MESSAGE | NIF_ICON;
                TrayIconData.uCallbackMessage = PMCTRACE_SERVER_WM_TRAY_CLICK;
                TrayIconData.hIcon = Icon;
                
                Shell_NotifyIcon(NIM_ADD, &TrayIconData);
                
                ChangeWindowMessageFilterEx(Window, PMCTRACE_SERVER_WM_START_TRACE, MSGFLT_ALLOW, 0);
                ChangeWindowMessageFilterEx(Window, PMCTRACE_SERVER_WM_END_TRACE, MSGFLT_ALLOW, 0);
                MSG Message;
                while(GetMessage(&Message, 0, 0, 0) > 0)
                {
                    TranslateMessage(&Message);
                    DispatchMessage(&Message);
                }
                
                Shell_NotifyIcon(NIM_DELETE, &TrayIconData);
            }
        }
    }

    ResetServer(Server);
}

#undef function

#pragma function(memset)
void *memset(void *Dest, int Source, size_t Size)
{
    __stosb((unsigned char *)Dest, (unsigned char)Source, Size);
    return Dest;
}

#pragma function(memcpy)
void *memcpy(void *Dest, const void *Source, size_t Size)
{
    __movsb((unsigned char *)Dest, (unsigned char *)Source, Size);
    return Dest;
}
