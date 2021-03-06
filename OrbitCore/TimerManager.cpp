//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "TimerManager.h"
#include "Threading.h"
#include "Message.h"
#include "TcpForward.h"
#include "TcpClient.h"
#include "Params.h"
#include "OrbitLib.h"

#ifdef _WIN32
#include <direct.h>
#endif

std::unique_ptr<TimerManager> GTimerManager;

//-----------------------------------------------------------------------------
TimerManager::TimerManager( bool a_IsClient ) : m_LockFreeQueue(65534), m_IsClient(a_IsClient)
{
    m_Paused = false;
    m_IsFull = false;
    m_IsRecording = false;
    m_ExitRequested = false;
    m_FlushRequested = false;
    m_NumQueuedEntries = 0;
    m_NumQueuedTimers = 0;
    m_NumQueuedMessages = 0;
    m_TimerIndex = 0;
    m_NumTimersFromPreviousSession = 0;
    m_NumFlushedTimers = 0;
    
    InitProfiling();

    if( m_IsClient )
    {
        GTcpClient->Start();
        m_ConsumerThread = new std::thread([&](){ SendTimers(); });
    }
}

//-----------------------------------------------------------------------------
TimerManager::~TimerManager()
{
}

//-----------------------------------------------------------------------------
void TimerManager::StartRecording()
{
    if( m_IsRecording )
    {
        return;
    }

    if( !m_ConsumerThread )
    {
        m_ConsumerThread = new std::thread([&](){ ConsumeTimers(); });
    }

    m_IsRecording = true;
}

//-----------------------------------------------------------------------------
void TimerManager::StopRecording()
{
    m_IsRecording = false;
    FlushQueue();
}

//-----------------------------------------------------------------------------
void TimerManager::StartClient()
{
    m_IsRecording = true;
}

//-----------------------------------------------------------------------------
void TimerManager::StopClient()
{
    m_IsRecording = false;
    GTimerManager->FlushQueue();
    
    if( GTcpClient )
    {
        GTcpClient->FlushSendQueue();
    }
}

//-----------------------------------------------------------------------------
void TimerManager::FlushQueue()
{
    m_FlushRequested = true;

    const size_t numTimers = 4096;
    Timer Timers[numTimers];
    m_NumFlushedTimers = 0;

    while (!m_ExitRequested)
    {
        size_t numDequeued = m_LockFreeQueue.try_dequeue_bulk(Timers, numTimers);

        if (numDequeued == 0)
            break;

        m_NumQueuedEntries -= (int)numDequeued;
        m_NumFlushedTimers += (int)numDequeued;

        if( m_IsClient )
        {
            int numEntries = m_NumFlushedTimers;
            GTcpClient->Send( Msg_NumFlushedEntries, numEntries );
        }
    }

    m_FlushRequested = false;
    m_ConditionVariable.signal();
}

//-----------------------------------------------------------------------------
void TimerManager::Stop()
{
    m_IsRecording = false;
    m_ExitRequested = true;
    m_ConditionVariable.signal();
    
    if( m_ConsumerThread )
    {
        m_ConsumerThread->join();
    }
}

//-----------------------------------------------------------------------------
void TimerManager::ConsumeTimers()
{
    SetCurrentThreadName( L"OrbitConsumeTimers" );
#ifdef _WIN32
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
#endif

    Timer Timer;

    while( !m_ExitRequested )
    {
        m_ConditionVariable.wait();

        while( !m_ExitRequested && !m_FlushRequested && m_LockFreeQueue.try_dequeue( Timer ) )
        {
            --m_NumQueuedEntries;
            --m_NumQueuedTimers;
            
            if( Timer.m_SessionID == Message::GSessionID )
            {
                for (TimerAddedCallback & Callback : m_TimerAddedCallbacks)
                {
                    Callback(Timer);
                }
            }
            else
            {
                ++m_NumTimersFromPreviousSession;
            }
        }
    }
}

//-----------------------------------------------------------------------------
void TimerManager::SendTimers()
{
    SetCurrentThreadName( L"OrbitSendTimers" );

    const size_t numTimers = 4096;
    Timer Timers[numTimers];

    while( !m_ExitRequested )
    {
        Message Msg(Msg_Timer);

        // Wait for non-empty queue
        while( m_NumQueuedEntries <= 0 && !m_ExitRequested )
        {
            m_ConditionVariable.wait();
        }

        size_t numDequeued = m_LockFreeQueue.try_dequeue_bulk(Timers, numTimers);
        m_NumQueuedEntries -= (int)numDequeued;
        m_NumQueuedTimers  -= (int)numDequeued;
        Msg.m_Size = (int)numDequeued*sizeof(Timer);

		GTcpClient->Send(Msg, (void*)Timers);

        int numEntries = m_NumQueuedEntries;
        GTcpClient->Send( Msg_NumQueuedEntries, numEntries );

        while (m_LockFreeMessageQueue.try_dequeue(Msg) && !m_ExitRequested)
        {
            --m_NumQueuedEntries;
            --m_NumQueuedMessages;
            GTcpClient->Send(Msg);
        }
    }
}

//-----------------------------------------------------------------------------
void TimerManager::Add( const Timer& a_Timer )
{
    if( m_IsRecording )
    {
        m_LockFreeQueue.enqueue(a_Timer);
        m_ConditionVariable.signal();
        ++m_NumQueuedEntries;
        ++m_NumQueuedTimers;
    }
}

//-----------------------------------------------------------------------------
void TimerManager::Add( const Message& a_Message )
{
    if( m_IsRecording || m_IsClient )
    {
        m_LockFreeMessageQueue.enqueue(a_Message);
        m_ConditionVariable.signal();
        ++m_NumQueuedEntries;
        ++m_NumQueuedMessages;
    }
}

//-----------------------------------------------------------------------------
void TimerManager::Add( const ContextSwitch & a_CS )
{
    m_ContextSwitchAddedCallback( a_CS );
}
