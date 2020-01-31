//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "EventTracer.h"
#include "SamplingProfiler.h"

#include "Platform.h"
#include <strsafe.h>
#include <wmistr.h>
#include <evntrace.h>
#include <evntcons.h>

#include "Capture.h"
#include "PrintVar.h"
#include "OrbitProcess.h"
#include "Threading.h"
#include "EventGuid.h"
#include "EventCallbacks.h"
#include "EventClasses.h"
#include "Params.h"

//-----------------------------------------------------------------------------
EventTracer GEventTracer;

//-----------------------------------------------------------------------------
EventTracer::EventTracer() : m_SessionProperties( nullptr ), m_IsTracing( false )
{
	m_TraceHandle = INVALID_PROCESSTRACE_HANDLE;
}

//-----------------------------------------------------------------------------
EventTracer::~EventTracer()
{
    CleanupTrace();
}

//-----------------------------------------------------------------------------
VOID WINAPI EventRecordCallback( _In_ PEVENT_RECORD pEventRecord )
{
   
}

//-----------------------------------------------------------------------------
void EventTracer::EventTracerThread()
{
    SetCurrentThreadName( L"EventTracer" );
    ULONG error = ProcessTrace( &m_TraceHandle, 1, 0, 0 );
    if( error != ERROR_SUCCESS )
    {
        PrintLastError();
    }
}

//-----------------------------------------------------------------------------
void EventTracer::SetSamplingFrequency( float a_Frequency /*Hertz*/ )
{
    TRACE_PROFILE_INTERVAL interval = {0};
    interval.Interval = ULONG( 10000.f * ( 1000.f / a_Frequency ) );

    ULONG error = TraceSetInformation( 0, TraceSampledProfileIntervalInfo, (void*)&interval, sizeof( TRACE_PROFILE_INTERVAL ) );
    if( error != ERROR_SUCCESS )
    {
        PrintLastError();
    }
}


//-----------------------------------------------------------------------------
void EventTracer::SetupStackTracing()
{
    const int stackTracingIdsMax = 96;  
    int numIDs = 0;
    STACK_TRACING_EVENT_ID stackTracingIds[stackTracingIdsMax];

    // Sampling
    STACK_TRACING_EVENT_ID & id = stackTracingIds[numIDs++];
    id.EventGuid = PerfInfoGuid;
    id.Type = PerfInfo_SampledProfile::OPCODE;

    ULONG error = TraceSetInformation( m_SessionHandle, TraceStackTracingInfo, (void*)stackTracingIds, (numIDs * sizeof(STACK_TRACING_EVENT_ID)) );
    if( error != ERROR_SUCCESS )
    {
        PrintLastError();
    }
}

static int Pad(int length)
{
	if (length / 8 * 8 != length)
	{
		return (length / 8 + 1) * 8;
	}
	return length;
}

//-----------------------------------------------------------------------------
void EventTracer::Start()
{
	//return;
	//CleanupTrace();
    EventTracing::Reset();

    if( !m_SessionProperties )
    {
        ULONG BufferSize = Pad(sizeof( EVENT_TRACE_PROPERTIES )) + Pad(sizeof( KERNEL_LOGGER_NAME ) + 1);
        m_SessionProperties = (EVENT_TRACE_PROPERTIES*)malloc( BufferSize );
        ZeroMemory( m_SessionProperties, BufferSize );
        m_SessionProperties->LoggerNameOffset = Pad(sizeof( EVENT_TRACE_PROPERTIES ));

        m_SessionProperties->EnableFlags = EVENT_TRACE_FLAG_THREAD; // ThreadGuid

        if( GParams.m_TrackSamplingEvents )
        {
            m_SessionProperties->EnableFlags |= EVENT_TRACE_FLAG_PROFILE; // PerfInfoGuid
        }

        if( GParams.m_TrackContextSwitches )
        {
            m_SessionProperties->EnableFlags |= EVENT_TRACE_FLAG_CSWITCH;
        }
       
        //EVENT_TRACE_FLAG_DISK_IO        |
        //EVENT_TRACE_FLAG_DISK_IO_INIT   |
        //EVENT_TRACE_FLAG_DISK_FILE_IO   |
        //EVENT_TRACE_FLAG_DISK_IO        |
        //EVENT_TRACE_FLAG_FILE_IO        |
        //EVENT_TRACE_FLAG_FILE_IO_INIT;    // DiskIo & FileIo

        m_SessionProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        m_SessionProperties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        m_SessionProperties->Wnode.ClientContext = 1;
        m_SessionProperties->Wnode.Guid = SystemTraceControlGuid;
        m_SessionProperties->Wnode.BufferSize = BufferSize;

        StringCbCopy( (STRSAFE_LPWSTR)( (char*)m_SessionProperties + m_SessionProperties->LoggerNameOffset )
            , sizeof( KERNEL_LOGGER_NAME )
            , KERNEL_LOGGER_NAME );
    }

    // Sampling profiling
    Process::SetPrivilege( SE_SYSTEM_PROFILE_NAME, true );
    SetSamplingFrequency( 2000 );
    Capture::NewSamplingProfiler();
    Capture::GSamplingProfiler->StartCapture();
    m_IsTracing = true;
    
    if( ControlTrace( 0, KERNEL_LOGGER_NAME, m_SessionProperties, EVENT_TRACE_CONTROL_STOP ) != ERROR_SUCCESS )
    {
        PrintLastError();
    }

    ULONG Status = StartTrace( &m_SessionHandle, KERNEL_LOGGER_NAME, m_SessionProperties );
    if( Status != ERROR_SUCCESS )
    {
        PrintLastError();
        return;
    }

    SetupStackTracing();

    static EVENT_TRACE_LOGFILE LogFile = {0};
    LogFile.LoggerName = KERNEL_LOGGER_NAME;
    LogFile.ProcessTraceMode = ( PROCESS_TRACE_MODE_REAL_TIME |
                                 PROCESS_TRACE_MODE_EVENT_RECORD |
                                 PROCESS_TRACE_MODE_RAW_TIMESTAMP );
    
    LogFile.EventRecordCallback = (PEVENT_RECORD_CALLBACK)EventTracing::Callback;

    m_TraceHandle = OpenTrace(&LogFile);
    if( m_TraceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        PrintLastError();
        return;
    }

	m_Therad = new std::thread( [&](){ EventTracerThread(); } );
	m_Therad->detach();
    //std::thread* thread = new std::thread( [&](){ EventTracerThread(); } );
    //thread->detach();
}

//-----------------------------------------------------------------------------
void EventTracer::Stop()
{
    CleanupTrace();
    
    if( m_IsTracing )
    {
        m_IsTracing = false;
        if( Capture::GSamplingProfiler )
        {
            Capture::GSamplingProfiler->StopCapture();
            Capture::GSamplingProfiler->ProcessSamplesAsync();
        }
    }

    m_EventBuffer.Print();
}

//-----------------------------------------------------------------------------
void EventTracer::CleanupTrace()
{
	if (m_Therad)
	{
		m_Therad->join();
		delete m_Therad;
		m_Therad = NULL;
	}

	if (m_TraceHandle != INVALID_PROCESSTRACE_HANDLE)
	{
		if (ControlTrace(m_TraceHandle, KERNEL_LOGGER_NAME, m_SessionProperties, EVENT_TRACE_CONTROL_STOP) != ERROR_SUCCESS)
		{
			PrintLastError();
		}

		if (CloseTrace(m_TraceHandle) != ERROR_SUCCESS)
		{
			PrintLastError();
		}
		CloseTrace(m_TraceHandle);
	}

	if (m_SessionProperties)
	{
		free(m_SessionProperties);
		m_SessionProperties = nullptr;
	}
}