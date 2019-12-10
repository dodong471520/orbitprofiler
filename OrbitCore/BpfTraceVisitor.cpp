#include "BpfTraceVisitor.h"
#include "OrbitFunction.h"
#include "CoreApp.h"

void BpfTraceVisitor::visit(LinuxPerfLostEvent* a_Event)
{
    PRINT("Lost %u Events\n", a_Event->Lost());
}

void BpfTraceVisitor::visit(LinuxUprobeEvent* a_Event)
{
    Timer timer;
    timer.m_TID = a_Event->TID();
    timer.m_Start = a_Event->Timestamp();
    timer.m_Depth = (uint8_t)m_TimerStacks[a_Event->TID()].size();
    timer.m_FunctionAddress = a_Event->GetFunction()->GetVirtualAddress();
    m_TimerStacks[a_Event->TID()].push_back(timer);
}

void BpfTraceVisitor::visit(LinuxUretprobeEvent* a_Event)
{
    std::vector<Timer>& timers = m_TimerStacks[a_Event->TID()];
    if (timers.size())
    {
        Timer& timer = timers.back();
        timer.m_End = a_Event->Timestamp();
        GCoreApp->ProcessTimer(&timer, std::to_string(a_Event->GetFunction()->GetVirtualAddress()));
        timers.pop_back();
    }
}

// TODO: Here, we would also like to handle the callstack
void BpfTraceVisitor::visit(LinuxUprobeEventWithStack* a_Event)
{
    Timer timer;
    timer.m_TID = a_Event->TID();
    timer.m_Start = a_Event->Timestamp();
    timer.m_Depth = (uint8_t)m_TimerStacks[a_Event->TID()].size();
    timer.m_FunctionAddress = a_Event->GetFunction()->GetVirtualAddress();
    m_TimerStacks[a_Event->TID()].push_back(timer);
}