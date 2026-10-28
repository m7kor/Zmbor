#include "iop_kernel.h"

#include "iop_memory.h"
#include "../iop_emulator_const.h"

#include <algorithm>
#include <vector>

// IOP kernel (thbase, thsemap, thevent, thmsgbx, thvpool) at system-call level.
//
// Expected behaviour comes from what a real PS2 printed for the ps2autotests kernel/iop programs (their .expected
// files): error codes, parameter checks, thread status and wait types, wake order, message linking and variable-pool
// sizes. Where those programs say nothing (check order, the 0x130 minimum stack, wait-queue details) the open
// threadman reimplementation in ps2sdk (iop/system/threadman, studied, not copied) is followed. Where the two
// disagree the real-PS2 output wins (TH_NO_FILLSTACK/TH_CLEAR_STACK and priority 0 are rejected on hardware).

namespace ps2x::iop::detail
{
    namespace
    {
        // kerr.h
        constexpr int32_t KE_OK = 0;
        constexpr int32_t KE_ERROR = -1;
        constexpr int32_t KE_NO_MEMORY = -400;
        constexpr int32_t KE_ILLEGAL_ATTR = -401;
        constexpr int32_t KE_ILLEGAL_ENTRY = -402;
        constexpr int32_t KE_ILLEGAL_PRIORITY = -403;
        constexpr int32_t KE_ILLEGAL_SIZE = -404;
        constexpr int32_t KE_ILLEGAL_MODE = -405;
        constexpr int32_t KE_ILLEGAL_THID = -406;
        constexpr int32_t KE_UNKNOWN_THID = -407;
        constexpr int32_t KE_UNKNOWN_SEMID = -408;
        constexpr int32_t KE_UNKNOWN_EVFID = -409;
        constexpr int32_t KE_UNKNOWN_MBXID = -410;
        constexpr int32_t KE_UNKNOWN_VPLID = -411;
        constexpr int32_t KE_DORMANT = -413;
        constexpr int32_t KE_NOT_DORMANT = -414;
        constexpr int32_t KE_NOT_SUSPEND = -415;
        constexpr int32_t KE_NOT_WAIT = -416;
        constexpr int32_t KE_CAN_NOT_WAIT = -417;
        constexpr int32_t KE_RELEASE_WAIT = -418;
        constexpr int32_t KE_SEMA_ZERO = -419;
        constexpr int32_t KE_SEMA_OVF = -420;
        constexpr int32_t KE_EVF_COND = -421;
        constexpr int32_t KE_EVF_MULTI = -422;
        constexpr int32_t KE_EVF_ILPAT = -423;
        constexpr int32_t KE_MBOX_NOMSG = -424;
        constexpr int32_t KE_WAIT_DELETE = -425;
        constexpr int32_t KE_ILLEGAL_MEMSIZE = -427;

        constexpr uint32_t TH_UMODE = 0x00000008u;
        constexpr uint32_t TH_ASM = 0x01000000u;
        constexpr uint32_t TH_C = 0x02000000u;
        constexpr uint32_t kMinimumStack = 0x130u;

        constexpr uint32_t SA_THPRI = 0x1u;  // semaphore: priority-ordered waiters
        constexpr uint32_t EA_MULTI = 0x2u;  // event flag: several waiters allowed
        constexpr uint32_t WEF_OR = 0x1u;
        constexpr uint32_t WEF_CLEAR = 0x10u;
        constexpr uint32_t MBA_THPRI = 0x1u;
        constexpr uint32_t MBA_MSPRI = 0x4u;
        constexpr uint32_t VA_THPRI = 0x1u;
        constexpr uint32_t VA_MEMBTM = 0x200u;
        // A variable pool of N bytes has N - 40 bytes of heap; a block costs an 8-byte header plus its size rounded
        // up to 8 (real PS2: a 0x1000-byte pool reports 4056, a 128-byte block takes 136). A pool whose heap could
        // not be created reports size and free size -4.
        constexpr uint32_t kPoolOverhead = 40u;
        constexpr uint32_t kPoolBlockHeader = 8u;
        constexpr int32_t kPoolInvalidHeapSize = -4;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            if (alignment <= 1u)
                return value;
            const uint32_t mask = alignment - 1u;
            return (value + mask) & ~mask;
        }

        uint32_t statusCode(IopThreadState state)
        {
            switch (state)
            {
            case IopThreadState::Running: return 0x01u;
            case IopThreadState::Ready: return 0x02u;
            case IopThreadState::Waiting: return 0x04u;
            case IopThreadState::Suspended: return 0x08u;
            case IopThreadState::WaitSuspended: return 0x0Cu;
            default: return 0x10u;
            }
        }
    }

    IopKernel::IopKernel(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopKernel::reset()
    {
        m_threads.clear();
        m_semaphores.clear();
        m_eventFlags.clear();
        m_messageBoxes.clear();
        m_variablePools.clear();
        m_nextThreadId = 1;
        m_nextSemaphoreId = 1;
        m_nextEventFlagId = 1;
        m_nextMessageBoxId = 1;
        m_nextVariablePoolId = 1;
        m_readyBack = 0;
        m_readyFront = 0;
        m_currentThread = nullptr;
    }

    IopThread *IopKernel::findThread(int id)
    {
        const auto it = m_threads.find(id);
        return it == m_threads.end() || it->second.state == IopThreadState::Dead ? nullptr : &it->second;
    }

    // System calls made from a kernel thread may block or be preempted; calls made from a host-driven guest call
    // (RPC handler, interrupt handler) run to completion.
    bool IopKernel::isCurrentCpu(const IopCpuState &cpu) const
    {
        return m_currentThread != nullptr && &m_currentThread->cpu == &cpu;
    }

    void IopKernel::makeReady(IopThread &thread, IopCpuState *caller)
    {
        thread.state = IopThreadState::Ready;
        thread.waitType = IopWaitType::None;
        thread.waitId = 0;
        thread.readyOrder = ++m_readyBack;
        // A thread of higher priority (smaller number) than the caller runs at once.
        if (caller != nullptr && isCurrentCpu(*caller) && thread.priority < m_currentThread->priority)
            caller->yielded = true;
    }

    void IopKernel::wait(IopThread &thread, IopWaitType type, int objectId, WaitQueue *queue, bool priorityOrder, IopCpuState &cpu)
    {
        thread.state = IopThreadState::Waiting;
        thread.waitType = type;
        thread.waitId = objectId;
        if (queue != nullptr)
        {
            auto position = queue->threads.end();
            if (priorityOrder)
            {
                position = std::find_if(queue->threads.begin(), queue->threads.end(), [&](int other)
                                        {
                                            const IopThread *waiting = findThread(other);
                                            return waiting != nullptr && waiting->priority > thread.priority;
                                        });
            }
            queue->threads.insert(position, thread.id);
        }
        cpu.yielded = true;
    }

    void IopKernel::removeFromWaitQueues(int threadId)
    {
        const auto drop = [&](WaitQueue &queue)
        {
            queue.threads.erase(std::remove(queue.threads.begin(), queue.threads.end(), threadId), queue.threads.end());
        };
        for (auto &[id, object] : m_semaphores)
            drop(object.waiters);
        for (auto &[id, object] : m_eventFlags)
            drop(object.waiters);
        for (auto &[id, object] : m_messageBoxes)
            drop(object.waiters);
        for (auto &[id, object] : m_variablePools)
            drop(object.waiters);
    }

    void IopKernel::releaseWaiters(WaitQueue &queue, int32_t result, IopCpuState *caller)
    {
        const std::deque<int> waiting = std::move(queue.threads);
        queue.threads.clear();
        for (const int id : waiting)
        {
            IopThread *thread = findThread(id);
            if (thread == nullptr || thread->state != IopThreadState::Waiting)
                continue;
            thread->cpu.gpr[2] = static_cast<uint32_t>(result);
            makeReady(*thread, caller);
        }
    }

    int IopKernel::createStartThread(uint32_t entry, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t priority)
    {
        constexpr uint32_t kStartStack = 0x4000u;
        IopThread thread;
        thread.id = static_cast<int>(m_nextThreadId++);
        thread.attr = TH_C;
        thread.entry = entry;
        thread.gp = gp;
        thread.stackSize = kStartStack;
        thread.stackBase = m_memory.allocate(kStartStack + kStackGuardBytes, 16u);
        if (thread.stackBase == 0u)
            return KE_NO_MEMORY;
        thread.initialPriority = priority;
        thread.priority = priority;
        thread.cpu.pc = entry;
        thread.cpu.gpr[4] = a0;
        thread.cpu.gpr[5] = a1;
        thread.cpu.gpr[28] = gp;
        thread.cpu.gpr[29] = alignUp(thread.stackBase + thread.stackSize, 16u) - 16u;
        thread.cpu.gpr[31] = kThreadReturnSentinel;
        const int id = thread.id;
        IopThread &stored = m_threads.emplace(id, std::move(thread)).first->second;
        makeReady(stored, nullptr);
        return id;
    }

    bool IopKernel::isThreadDormant(int id) const
    {
        const auto it = m_threads.find(id);
        return it == m_threads.end() || it->second.state == IopThreadState::Dormant || it->second.state == IopThreadState::Dead;
    }

    uint32_t IopKernel::threadResult(int id) const
    {
        const auto it = m_threads.find(id);
        return it == m_threads.end() ? 0u : it->second.cpu.gpr[2];
    }

    void IopKernel::deleteThreadNow(int id)
    {
        const auto it = m_threads.find(id);
        if (it == m_threads.end())
            return;
        removeFromWaitQueues(id);
        if (m_currentThread == &it->second)
            m_currentThread = nullptr;
        if (it->second.stackBase != 0u)
            (void)m_memory.freeAllocation(it->second.stackBase);
        m_threads.erase(it);
    }

    bool IopKernel::dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };
        const int a0 = static_cast<int>(cpu.gpr[4]);
        // Calls that take TH_SELF (0) for the calling thread.
        const auto selfOr = [&](int id) -> IopThread *
        {
            if (id == 0)
                return m_currentThread;
            return findThread(id);
        };

        switch (ordinal)
        {
        case 4: // CreateThread(iop_thread_t *)
        {
            const uint32_t descriptor = cpu.gpr[4];
            const uint32_t attr = m_memory.read32(descriptor + 0u);
            const uint32_t option = m_memory.read32(descriptor + 4u);
            const uint32_t entry = m_memory.read32(descriptor + 8u);
            const uint32_t stackSize = m_memory.read32(descriptor + 12u);
            const uint32_t priority = m_memory.read32(descriptor + 16u);
            if ((attr & ~(TH_ASM | TH_C | TH_UMODE)) != 0u)
                setV0(KE_ILLEGAL_ATTR);
            else if (priority < 1u || priority > 126u)
                setV0(KE_ILLEGAL_PRIORITY);
            else if ((entry & 3u) != 0u)
                setV0(KE_ILLEGAL_ENTRY);
            else if (stackSize < kMinimumStack)
                setV0(KE_ILLEGAL_SIZE);
            else
            {
                const uint64_t alignedStack = (static_cast<uint64_t>(stackSize) + 0xFFu) & ~0xFFull;
                const uint32_t stackBase = alignedStack > 0xFFFFFFFFull - kStackGuardBytes
                                               ? 0u
                                               : m_memory.allocate(static_cast<uint32_t>(alignedStack) + kStackGuardBytes, 16u);
                if (stackBase == 0u)
                {
                    setV0(KE_NO_MEMORY);
                    return true;
                }
                IopThread thread;
                thread.id = static_cast<int>(m_nextThreadId++);
                thread.attr = attr;
                thread.option = option;
                thread.entry = entry;
                thread.stackSize = static_cast<uint32_t>(alignedStack);
                thread.stackBase = stackBase;
                thread.initialPriority = priority;
                thread.gp = cpu.gpr[28];
                const int id = thread.id;
                m_threads.emplace(id, std::move(thread));
                setV0(id);
            }
            return true;
        }
        case 5: // DeleteThread
        {
            if (a0 == 0)
            {
                setV0(KE_ILLEGAL_THID);
                return true;
            }
            IopThread *thread = findThread(a0);
            if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread->state != IopThreadState::Dormant)
                setV0(KE_NOT_DORMANT);
            else
            {
                deleteThreadNow(a0);
                setV0(KE_OK);
            }
            return true;
        }
        case 6: // StartThread(thid, arg)
        case 7: // StartThreadArgs(thid, args, argp)
        {
            if (a0 == 0)
            {
                setV0(KE_ILLEGAL_THID);
                return true;
            }
            IopThread *thread = findThread(a0);
            if (thread == nullptr)
            {
                setV0(KE_UNKNOWN_THID);
                return true;
            }
            if (thread->state != IopThreadState::Dormant)
            {
                setV0(KE_NOT_DORMANT);
                return true;
            }
            thread->cpu = {};
            thread->cpu.pc = thread->entry;
            thread->cpu.gpr[4] = cpu.gpr[5];
            thread->cpu.gpr[5] = ordinal == 7 ? cpu.gpr[6] : 0u;
            thread->cpu.gpr[28] = thread->gp;
            thread->cpu.gpr[29] = alignUp(thread->stackBase + thread->stackSize, 16u) - 16u;
            thread->cpu.gpr[31] = kThreadReturnSentinel;
            thread->priority = thread->initialPriority;
            thread->wakeupCount = 0;
            setV0(KE_OK);
            makeReady(*thread, &cpu);
            return true;
        }
        case 8: // ExitThread
        case 9: // ExitDeleteThread
            if (isCurrentCpu(cpu))
            {
                m_currentThread->state = ordinal == 9 ? IopThreadState::Dead : IopThreadState::Dormant;
                cpu.stopped = true;
                cpu.yielded = true;
            }
            setV0(KE_OK);
            return true;
        case 10: // TerminateThread
        case 11: // iTerminateThread
        {
            IopThread *thread = findThread(a0);
            if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread == m_currentThread)
                setV0(KE_ILLEGAL_THID);
            else if (thread->state == IopThreadState::Dormant)
                setV0(KE_DORMANT);
            else
            {
                removeFromWaitQueues(thread->id);
                thread->state = IopThreadState::Dormant;
                thread->waitType = IopWaitType::None;
                thread->waitId = 0;
                setV0(KE_OK);
            }
            return true;
        }
        case 12: // DisableDispatchThread
        case 13: // EnableDispatchThread
            setV0(KE_OK);
            return true;
        case 14: // ChangeThreadPriority(thid, priority)
        case 15: // iChangeThreadPriority
        {
            IopThread *thread = selfOr(a0);
            uint32_t priority = cpu.gpr[5];
            if (thread == nullptr)
            {
                setV0(a0 == 0 ? KE_ILLEGAL_THID : KE_UNKNOWN_THID);
                return true;
            }
            if (a0 != 0 && thread->state == IopThreadState::Dormant)
            {
                setV0(KE_DORMANT);
                return true;
            }
            if (priority == 0u)
                priority = m_currentThread != nullptr ? m_currentThread->priority : thread->priority;
            else if (priority - 1u >= 126u)
            {
                setV0(KE_ILLEGAL_PRIORITY);
                return true;
            }
            thread->priority = priority;
            setV0(KE_OK);
            if (thread == m_currentThread && isCurrentCpu(cpu))
            {
                // Lowered to or below another ready thread: it goes behind that priority's ready threads.
                const bool other = std::any_of(m_threads.begin(), m_threads.end(), [&](const auto &entry)
                                               { return entry.second.state == IopThreadState::Ready && entry.second.priority <= priority; });
                if (other)
                {
                    thread->state = IopThreadState::Ready;
                    thread->readyOrder = ++m_readyBack;
                    cpu.yielded = true;
                }
            }
            else if (thread->state == IopThreadState::Ready && m_currentThread != nullptr && isCurrentCpu(cpu) &&
                     priority < m_currentThread->priority)
            {
                cpu.yielded = true;
            }
            return true;
        }
        case 16: // RotateThreadReadyQueue(priority)
        case 17:
        {
            const uint32_t priority = cpu.gpr[4] != 0u ? cpu.gpr[4] : (m_currentThread != nullptr ? m_currentThread->priority : 0u);
            setV0(KE_OK);
            if (isCurrentCpu(cpu) && m_currentThread->priority == priority)
            {
                m_currentThread->state = IopThreadState::Ready;
                m_currentThread->readyOrder = ++m_readyBack;
                cpu.yielded = true;
            }
            return true;
        }
        case 18: // ReleaseWaitThread
        case 19: // iReleaseWaitThread
        {
            IopThread *thread = a0 == 0 ? nullptr : findThread(a0);
            if (a0 == 0 || (thread != nullptr && thread == m_currentThread))
                setV0(KE_ILLEGAL_THID);
            else if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread->state != IopThreadState::Waiting)
                setV0(KE_NOT_WAIT);
            else
            {
                removeFromWaitQueues(thread->id);
                thread->cpu.gpr[2] = static_cast<uint32_t>(KE_RELEASE_WAIT);
                setV0(KE_OK);
                makeReady(*thread, &cpu);
            }
            return true;
        }
        case 20: // GetThreadId
            setV0(m_currentThread != nullptr ? m_currentThread->id : KE_ILLEGAL_THID);
            return true;
        case 21: // CheckThreadStack
            setV0(0x1000);
            return true;
        case 22: // ReferThreadStatus(thid, info)
        case 23: // iReferThreadStatus
        {
            IopThread *thread = selfOr(a0);
            if (thread == nullptr)
                setV0(a0 == 0 ? KE_ILLEGAL_THID : KE_UNKNOWN_THID);
            else
                setV0(referThreadStatus(*thread, cpu.gpr[5]) ? KE_OK : KE_ERROR);
            return true;
        }
        case 24: // SleepThread
            if (!isCurrentCpu(cpu))
            {
                setV0(KE_CAN_NOT_WAIT);
                return true;
            }
            setV0(KE_OK);
            if (m_currentThread->wakeupCount > 0)
                --m_currentThread->wakeupCount;
            else
                wait(*m_currentThread, IopWaitType::Sleep, 0, nullptr, false, cpu);
            return true;
        case 25: // WakeupThread
        case 26: // iWakeupThread
        {
            IopThread *thread = a0 == 0 ? nullptr : findThread(a0);
            if (a0 == 0 || (thread != nullptr && thread == m_currentThread))
                setV0(KE_ILLEGAL_THID);
            else if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread->state == IopThreadState::Dormant)
                setV0(KE_DORMANT);
            else
            {
                setV0(KE_OK);
                if (thread->state == IopThreadState::Waiting && thread->waitType == IopWaitType::Sleep)
                {
                    thread->cpu.gpr[2] = static_cast<uint32_t>(KE_OK);
                    makeReady(*thread, &cpu);
                }
                else
                    ++thread->wakeupCount;
            }
            return true;
        }
        case 27: // CancelWakeupThread
        case 28: // iCancelWakeupThread
        {
            IopThread *thread = selfOr(a0);
            if (thread == nullptr)
            {
                setV0(a0 == 0 ? KE_ILLEGAL_THID : KE_UNKNOWN_THID);
                return true;
            }
            const int old = thread->wakeupCount;
            thread->wakeupCount = 0;
            setV0(old);
            return true;
        }
        case 29: // SuspendThread
        case 30: // iSuspendThread
        {
            IopThread *thread = a0 == 0 ? nullptr : findThread(a0);
            if (a0 == 0 || (thread != nullptr && thread == m_currentThread))
                setV0(KE_ILLEGAL_THID);
            else if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread->state == IopThreadState::Dormant)
                setV0(KE_DORMANT);
            else
            {
                thread->state = thread->state == IopThreadState::Waiting ? IopThreadState::WaitSuspended : IopThreadState::Suspended;
                setV0(KE_OK);
            }
            return true;
        }
        case 31: // ResumeThread
        case 32: // iResumeThread
        {
            IopThread *thread = a0 == 0 ? nullptr : findThread(a0);
            if (a0 == 0 || (thread != nullptr && thread == m_currentThread))
                setV0(KE_ILLEGAL_THID);
            else if (thread == nullptr)
                setV0(KE_UNKNOWN_THID);
            else if (thread->state == IopThreadState::WaitSuspended)
            {
                thread->state = IopThreadState::Waiting;
                setV0(KE_OK);
            }
            else if (thread->state == IopThreadState::Suspended)
            {
                setV0(KE_OK);
                makeReady(*thread, &cpu);
            }
            else
                setV0(KE_NOT_SUSPEND);
            return true;
        }
        case 33: // DelayThread(usec)
            if (!isCurrentCpu(cpu))
            {
                setV0(KE_CAN_NOT_WAIT);
                return true;
            }
            {
                const uint64_t delayCycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz + 999'999ull) / 1'000'000ull;
                delayCurrentUntil(currentCycle + std::max<uint64_t>(delayCycles, 1u), cpu);
            }
            setV0(KE_OK);
            return true;
        case 34: // GetSystemTime
            m_memory.write32(cpu.gpr[4], static_cast<uint32_t>(currentCycle));
            m_memory.write32(cpu.gpr[4] + 4u, static_cast<uint32_t>(currentCycle >> 32u));
            setV0(KE_OK);
            return true;
        case 35:
        case 36:
        case 37:
        case 38:
            setV0(KE_OK);
            return true;
        case 39: // USec2SysClock
        {
            const uint64_t cycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz) / 1'000'000ull;
            m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(cycles));
            m_memory.write32(cpu.gpr[5] + 4u, static_cast<uint32_t>(cycles >> 32u));
            setV0(KE_OK);
            return true;
        }
        case 40: // SysClock2USec
        {
            const uint64_t cycles = static_cast<uint64_t>(m_memory.read32(cpu.gpr[4])) | (static_cast<uint64_t>(m_memory.read32(cpu.gpr[4] + 4u)) << 32u);
            const uint64_t usec = (cycles * 1'000'000ull) / kIopClockHz;
            if (cpu.gpr[5] != 0u)
                m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(usec / 1'000'000ull));
            if (cpu.gpr[6] != 0u)
                m_memory.write32(cpu.gpr[6], static_cast<uint32_t>(usec % 1'000'000ull));
            setV0(KE_OK);
            return true;
        }
        case 41:
            setV0(KE_OK);
            return true;
        default:
            return false;
        }
    }

    bool IopKernel::referThreadStatus(IopThread &thread, uint32_t outputAddress)
    {
        if (outputAddress == 0u)
            return false;
        // iop_thread_info_t
        m_memory.write32(outputAddress + 0u, thread.attr);
        m_memory.write32(outputAddress + 4u, thread.option);
        m_memory.write32(outputAddress + 8u, statusCode(thread.state));
        m_memory.write32(outputAddress + 12u, thread.entry);
        m_memory.write32(outputAddress + 16u, thread.stackBase);
        m_memory.write32(outputAddress + 20u, thread.stackSize);
        m_memory.write32(outputAddress + 24u, thread.gp);
        m_memory.write32(outputAddress + 28u, thread.initialPriority);
        m_memory.write32(outputAddress + 32u, thread.priority);
        const bool waiting = thread.state == IopThreadState::Waiting || thread.state == IopThreadState::WaitSuspended;
        m_memory.write32(outputAddress + 36u, waiting ? static_cast<uint32_t>(thread.waitType) : 0u);
        m_memory.write32(outputAddress + 40u, waiting ? static_cast<uint32_t>(thread.waitId) : 0u);
        m_memory.write32(outputAddress + 44u, static_cast<uint32_t>(thread.wakeupCount));
        return true;
    }

    bool IopKernel::dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };
        const int id = static_cast<int>(cpu.gpr[4]);

        switch (ordinal)
        {
        case 4: // CreateSema(iop_sema_t *)
        {
            const uint32_t descriptor = cpu.gpr[4];
            Semaphore semaphore;
            semaphore.id = static_cast<int>(m_nextSemaphoreId++);
            semaphore.attr = m_memory.read32(descriptor + 0u);
            semaphore.option = m_memory.read32(descriptor + 4u);
            semaphore.initial = static_cast<int>(m_memory.read32(descriptor + 8u));
            semaphore.current = semaphore.initial;
            semaphore.maximum = static_cast<int>(m_memory.read32(descriptor + 12u));
            m_semaphores.emplace(semaphore.id, semaphore);
            setV0(semaphore.id);
            return true;
        }
        case 5: // DeleteSema
        {
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(KE_UNKNOWN_SEMID);
                return true;
            }
            WaitQueue waiters = std::move(it->second.waiters);
            m_semaphores.erase(it);
            setV0(KE_OK);
            releaseWaiters(waiters, KE_WAIT_DELETE, &cpu);
            return true;
        }
        case 6: // SignalSema
        case 7: // iSignalSema
        {
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(KE_UNKNOWN_SEMID);
                return true;
            }
            Semaphore &semaphore = it->second;
            if (!semaphore.waiters.threads.empty())
            {
                IopThread *thread = findThread(semaphore.waiters.threads.front());
                semaphore.waiters.threads.pop_front();
                setV0(KE_OK);
                if (thread != nullptr)
                {
                    thread->cpu.gpr[2] = static_cast<uint32_t>(KE_OK);
                    makeReady(*thread, &cpu);
                }
                return true;
            }
            if (semaphore.current >= semaphore.maximum)
            {
                setV0(KE_SEMA_OVF);
                return true;
            }
            ++semaphore.current;
            setV0(KE_OK);
            return true;
        }
        case 8: // WaitSema
        case 9: // PollSema
        {
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(KE_UNKNOWN_SEMID);
                return true;
            }
            Semaphore &semaphore = it->second;
            if (semaphore.current > 0)
            {
                --semaphore.current;
                setV0(KE_OK);
            }
            else if (ordinal == 9)
                setV0(KE_SEMA_ZERO);
            else if (!isCurrentCpu(cpu))
                setV0(KE_CAN_NOT_WAIT);
            else
            {
                setV0(KE_OK);
                wait(*m_currentThread, IopWaitType::Semaphore, id, &semaphore.waiters, (semaphore.attr & SA_THPRI) != 0u, cpu);
            }
            return true;
        }
        case 11: // ReferSemaStatus(semid, iop_sema_info_t *)
        case 12:
        {
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(KE_UNKNOWN_SEMID);
                return true;
            }
            const uint32_t outputAddress = cpu.gpr[5];
            if (outputAddress != 0u)
            {
                m_memory.write32(outputAddress + 0u, it->second.attr);
                m_memory.write32(outputAddress + 4u, it->second.option);
                m_memory.write32(outputAddress + 8u, static_cast<uint32_t>(it->second.initial));
                m_memory.write32(outputAddress + 12u, static_cast<uint32_t>(it->second.maximum));
                m_memory.write32(outputAddress + 16u, static_cast<uint32_t>(it->second.current));
                m_memory.write32(outputAddress + 20u, static_cast<uint32_t>(it->second.waiters.threads.size()));
            }
            setV0(KE_OK);
            return true;
        }
        default:
            return false;
        }
    }

    bool IopKernel::eventSatisfied(uint32_t flagBits, uint32_t bits, uint32_t mode)
    {
        if (bits == 0u)
            return false;
        return (mode & WEF_OR) != 0u ? (flagBits & bits) != 0u : (flagBits & bits) == bits;
    }

    void IopKernel::wakeEventWaiters(EventFlag &event, IopCpuState *caller)
    {
        for (auto it = event.waiters.threads.begin(); it != event.waiters.threads.end() && event.bits != 0u;)
        {
            IopThread *thread = findThread(*it);
            if (thread == nullptr)
            {
                it = event.waiters.threads.erase(it);
                continue;
            }
            if (!eventSatisfied(event.bits, thread->waitBits, thread->waitMode))
            {
                ++it;
                continue;
            }
            if (thread->waitArgument != 0u)
                m_memory.write32(thread->waitArgument, event.bits);
            if ((thread->waitMode & WEF_CLEAR) != 0u)
                event.bits = 0u;
            it = event.waiters.threads.erase(it);
            thread->cpu.gpr[2] = static_cast<uint32_t>(KE_OK);
            thread->waitBits = 0;
            thread->waitMode = 0;
            thread->waitArgument = 0;
            makeReady(*thread, caller);
        }
    }

    int IopKernel::createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits)
    {
        EventFlag event;
        event.id = static_cast<int>(m_nextEventFlagId++);
        event.attr = attr;
        event.option = option;
        event.bits = bits;
        event.initialBits = bits;
        const int id = event.id;
        m_eventFlags.emplace(id, std::move(event));
        return id;
    }

    bool IopKernel::setInternalEventFlag(int id, uint32_t bits)
    {
        const auto event = m_eventFlags.find(id);
        if (event == m_eventFlags.end())
            return false;
        event->second.bits |= bits;
        wakeEventWaiters(event->second, nullptr);
        return true;
    }

    bool IopKernel::dispatchEventImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };
        const int id = static_cast<int>(cpu.gpr[4]);

        switch (ordinal)
        {
        case 4: // CreateEventFlag(iop_event_t *)
        {
            const uint32_t descriptor = cpu.gpr[4];
            setV0(createInternalEventFlag(m_memory.read32(descriptor + 0u),
                                          m_memory.read32(descriptor + 4u),
                                          m_memory.read32(descriptor + 8u)));
            return true;
        }
        case 5: // DeleteEventFlag: waiters return KE_WAIT_DELETE
        {
            const auto it = m_eventFlags.find(id);
            if (it == m_eventFlags.end())
            {
                setV0(KE_UNKNOWN_EVFID);
                return true;
            }
            WaitQueue waiters = std::move(it->second.waiters);
            m_eventFlags.erase(it);
            setV0(KE_OK);
            releaseWaiters(waiters, KE_WAIT_DELETE, &cpu);
            return true;
        }
        case 6: // SetEventFlag
        case 7: // iSetEventFlag
        {
            const auto it = m_eventFlags.find(id);
            if (it == m_eventFlags.end())
            {
                setV0(KE_UNKNOWN_EVFID);
                return true;
            }
            setV0(KE_OK);
            if (cpu.gpr[5] == 0u)
                return true;
            it->second.bits |= cpu.gpr[5];
            wakeEventWaiters(it->second, &cpu);
            return true;
        }
        case 8: // ClearEventFlag(id, mask): the bits kept are flag & mask (callers pass ~bitsToClear)
        case 9:
        {
            const auto it = m_eventFlags.find(id);
            if (it == m_eventFlags.end())
            {
                setV0(KE_UNKNOWN_EVFID);
                return true;
            }
            it->second.bits &= cpu.gpr[5];
            setV0(KE_OK);
            return true;
        }
        case 10: // WaitEventFlag(id, bits, mode, resbits)
        case 11: // PollEventFlag
        {
            const uint32_t bits = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            const uint32_t resultAddress = cpu.gpr[7];
            if ((mode & ~(WEF_OR | WEF_CLEAR)) != 0u)
            {
                setV0(KE_ILLEGAL_MODE);
                return true;
            }
            if (bits == 0u)
            {
                setV0(KE_EVF_ILPAT);
                return true;
            }
            const auto it = m_eventFlags.find(id);
            if (it == m_eventFlags.end())
            {
                setV0(KE_UNKNOWN_EVFID);
                return true;
            }
            EventFlag &event = it->second;
            if ((event.attr & EA_MULTI) == 0u && !event.waiters.threads.empty())
            {
                setV0(KE_EVF_MULTI);
                return true;
            }
            if (eventSatisfied(event.bits, bits, mode))
            {
                if (resultAddress != 0u)
                    m_memory.write32(resultAddress, event.bits);
                if ((mode & WEF_CLEAR) != 0u)
                    event.bits = 0u;
                setV0(KE_OK);
            }
            else if (ordinal == 11)
                setV0(KE_EVF_COND);
            else if (!isCurrentCpu(cpu))
                setV0(KE_CAN_NOT_WAIT);
            else
            {
                setV0(KE_OK);
                m_currentThread->waitBits = bits;
                m_currentThread->waitMode = mode;
                m_currentThread->waitArgument = resultAddress;
                wait(*m_currentThread, IopWaitType::EventFlag, id, &event.waiters, false, cpu);
            }
            return true;
        }
        case 13: // ReferEventFlagStatus(id, iop_event_info_t *)
        case 14:
        {
            const auto it = m_eventFlags.find(id);
            if (it == m_eventFlags.end())
            {
                setV0(KE_UNKNOWN_EVFID);
                return true;
            }
            if (cpu.gpr[5] != 0u)
            {
                m_memory.write32(cpu.gpr[5] + 0u, it->second.attr);
                m_memory.write32(cpu.gpr[5] + 4u, it->second.option);
                m_memory.write32(cpu.gpr[5] + 8u, it->second.initialBits);
                m_memory.write32(cpu.gpr[5] + 12u, it->second.bits);
                m_memory.write32(cpu.gpr[5] + 16u, static_cast<uint32_t>(it->second.waiters.threads.size()));
            }
            setV0(KE_OK);
            return true;
        }
        default:
            return false;
        }
    }

    // iop_message_t: next at +0, priority (u8) at +4. A box keeps a circular list through 'next' whose newest
    // message points at the oldest; a single message points at itself (real PS2: "msg1's next pointer refers to:
    // msg1"). With MBA_MSPRI the list is kept in ascending priority, a new message after those of equal priority.
    void IopKernel::sendMessage(MessageBox &box, uint32_t message)
    {
        ++box.count;
        if (box.newest == 0u)
        {
            m_memory.write32(message, message);
            box.newest = message;
            return;
        }
        if ((box.attr & MBA_MSPRI) == 0u)
        {
            m_memory.write32(message, m_memory.read32(box.newest));
            m_memory.write32(box.newest, message);
            box.newest = message;
            return;
        }
        std::vector<uint32_t> order;
        for (uint32_t m = m_memory.read32(box.newest);; m = m_memory.read32(m))
        {
            order.push_back(m);
            if (m == box.newest || order.size() > box.count)
                break;
        }
        const uint8_t priority = m_memory.read8(message + 4u);
        const auto position = std::find_if(order.begin(), order.end(), [&](uint32_t m) { return m_memory.read8(m + 4u) > priority; });
        order.insert(position, message);
        for (size_t i = 0; i < order.size(); ++i)
            m_memory.write32(order[i], order[(i + 1u) % order.size()]);
        box.newest = order.back();
    }

    uint32_t IopKernel::takeMessage(MessageBox &box)
    {
        const uint32_t oldest = m_memory.read32(box.newest);
        --box.count;
        if (oldest == box.newest)
            box.newest = 0u;
        else
            m_memory.write32(box.newest, m_memory.read32(oldest));
        return oldest;
    }

    bool IopKernel::dispatchMessageBoxImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4: // CreateMbx(iop_mbx_t *)
        {
            const uint32_t attr = m_memory.read32(cpu.gpr[4] + 0u);
            if ((attr & ~(MBA_THPRI | MBA_MSPRI)) != 0u)
            {
                setV0(KE_ILLEGAL_ATTR);
                return true;
            }
            MessageBox box;
            box.id = static_cast<int>(m_nextMessageBoxId++);
            box.attr = attr;
            box.option = m_memory.read32(cpu.gpr[4] + 4u);
            const int id = box.id;
            m_messageBoxes.emplace(id, std::move(box));
            setV0(id);
            return true;
        }
        case 5: // DeleteMbx
        {
            const auto it = m_messageBoxes.find(static_cast<int>(cpu.gpr[4]));
            if (it == m_messageBoxes.end())
            {
                setV0(KE_UNKNOWN_MBXID);
                return true;
            }
            WaitQueue waiters = std::move(it->second.waiters);
            m_messageBoxes.erase(it);
            setV0(KE_OK);
            releaseWaiters(waiters, KE_WAIT_DELETE, &cpu);
            return true;
        }
        case 6: // SendMbx(mbxid, msg): the first waiter receives it directly
        case 7: // iSendMbx
        {
            const auto it = m_messageBoxes.find(static_cast<int>(cpu.gpr[4]));
            if (it == m_messageBoxes.end())
            {
                setV0(KE_UNKNOWN_MBXID);
                return true;
            }
            MessageBox &box = it->second;
            setV0(KE_OK);
            while (!box.waiters.threads.empty())
            {
                IopThread *thread = findThread(box.waiters.threads.front());
                box.waiters.threads.pop_front();
                if (thread == nullptr)
                    continue;
                if (thread->waitArgument != 0u)
                    m_memory.write32(thread->waitArgument, cpu.gpr[5]);
                thread->waitArgument = 0;
                thread->cpu.gpr[2] = static_cast<uint32_t>(KE_OK);
                makeReady(*thread, &cpu);
                return true;
            }
            sendMessage(box, cpu.gpr[5]);
            return true;
        }
        case 8: // ReceiveMbx(void **msgvar, mbxid)
        case 9: // PollMbx
        {
            const auto it = m_messageBoxes.find(static_cast<int>(cpu.gpr[5]));
            if (it == m_messageBoxes.end())
            {
                setV0(KE_UNKNOWN_MBXID);
                return true;
            }
            MessageBox &box = it->second;
            if (box.count > 0u)
            {
                const uint32_t message = takeMessage(box);
                if (cpu.gpr[4] != 0u)
                    m_memory.write32(cpu.gpr[4], message);
                setV0(KE_OK);
            }
            else if (ordinal == 9)
                setV0(KE_MBOX_NOMSG);
            else if (!isCurrentCpu(cpu))
                setV0(KE_CAN_NOT_WAIT);
            else
            {
                setV0(KE_OK);
                m_currentThread->waitArgument = cpu.gpr[4];
                wait(*m_currentThread, IopWaitType::MessageBox, box.id, &box.waiters, (box.attr & MBA_THPRI) != 0u, cpu);
            }
            return true;
        }
        case 11: // ReferMbxStatus(mbxid, iop_mbx_status_t *)
        case 12:
        {
            const auto it = m_messageBoxes.find(static_cast<int>(cpu.gpr[4]));
            if (it == m_messageBoxes.end())
            {
                setV0(KE_UNKNOWN_MBXID);
                return true;
            }
            const MessageBox &box = it->second;
            const uint32_t info = cpu.gpr[5];
            if (info != 0u)
            {
                m_memory.write32(info + 0u, box.attr);
                m_memory.write32(info + 4u, box.option);
                m_memory.write32(info + 8u, static_cast<uint32_t>(box.waiters.threads.size()));
                m_memory.write32(info + 12u, box.count);
                m_memory.write32(info + 16u, box.newest != 0u ? m_memory.read32(box.newest) : 0u);
            }
            setV0(KE_OK);
            return true;
        }
        default:
            return false;
        }
    }

    int32_t IopKernel::poolFreeSize(const VariablePool &pool)
    {
        if (!pool.heapValid)
            return kPoolInvalidHeapSize;
        uint64_t total = 0u;
        for (const auto &[offset, length] : pool.freeRanges)
            total += length;
        return static_cast<int32_t>(total);
    }

    uint32_t IopKernel::poolAllocate(VariablePool &pool, uint32_t size)
    {
        if (!pool.heapValid)
            return 0u;
        // The rounding is 32-bit (a request of -1 or -5 rounds to 0 and takes one header; -8 stays huge).
        const uint64_t block = static_cast<uint64_t>((size + 7u) & ~7u) + kPoolBlockHeader;
        const bool fromTop = (pool.attr & VA_MEMBTM) != 0u;
        auto chosen = pool.freeRanges.end();
        for (auto it = pool.freeRanges.begin(); it != pool.freeRanges.end(); ++it)
        {
            if (it->second < block)
                continue;
            chosen = it;
            if (!fromTop)
                break;
        }
        if (chosen == pool.freeRanges.end())
            return 0u;
        const uint32_t offset = chosen->first;
        const uint32_t length = chosen->second;
        const uint32_t used = static_cast<uint32_t>(block);
        uint32_t blockOffset = offset;
        pool.freeRanges.erase(chosen);
        if (fromTop)
        {
            blockOffset = offset + length - used;
            if (length > used)
                pool.freeRanges.emplace(offset, length - used);
        }
        else if (length > used)
            pool.freeRanges.emplace(offset + used, length - used);
        const uint32_t address = pool.base + blockOffset + kPoolBlockHeader;
        pool.allocated.emplace(address, used);
        return address;
    }

    bool IopKernel::poolFree(VariablePool &pool, uint32_t address)
    {
        const auto block = pool.allocated.find(address);
        if (block == pool.allocated.end())
            return false;
        uint32_t offset = address - kPoolBlockHeader - pool.base;
        uint32_t length = block->second;
        pool.allocated.erase(block);
        // Coalesce with the neighbouring free ranges.
        auto next = pool.freeRanges.lower_bound(offset);
        if (next != pool.freeRanges.end() && next->first == offset + length)
        {
            length += next->second;
            next = pool.freeRanges.erase(next);
        }
        if (next != pool.freeRanges.begin())
        {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset)
            {
                offset = previous->first;
                length += previous->second;
                pool.freeRanges.erase(previous);
            }
        }
        pool.freeRanges.emplace(offset, length);
        return true;
    }

    bool IopKernel::dispatchVariablePoolImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };
        const auto findPool = [&](uint32_t id) -> VariablePool *
        {
            const auto it = m_variablePools.find(static_cast<int>(id));
            return it == m_variablePools.end() ? nullptr : &it->second;
        };

        switch (ordinal)
        {
        case 4: // CreateVpl(iop_vpl_param *)
        {
            const uint32_t attr = m_memory.read32(cpu.gpr[4] + 0u);
            const uint32_t option = m_memory.read32(cpu.gpr[4] + 4u);
            const int32_t size = static_cast<int32_t>(m_memory.read32(cpu.gpr[4] + 8u));
            if ((attr & ~(VA_THPRI | VA_MEMBTM)) != 0u)
            {
                setV0(KE_ILLEGAL_ATTR);
                return true;
            }
            if (size == 0)
            {
                setV0(KE_ILLEGAL_MEMSIZE);
                return true;
            }
            VariablePool pool;
            pool.id = static_cast<int>(m_nextVariablePoolId++);
            pool.attr = attr;
            pool.option = option;
            // A size the system memory cannot provide (huge or negative) still creates the pool, without a heap
            // (real PS2: size and free size read -4).
            if (size > static_cast<int32_t>(kPoolOverhead + kPoolBlockHeader))
                pool.memory = m_memory.allocate(static_cast<uint32_t>(size), 16u);
            if (pool.memory != 0u)
            {
                pool.heapValid = true;
                pool.base = pool.memory + kPoolOverhead;
                pool.capacity = static_cast<uint32_t>(size) - kPoolOverhead;
                pool.freeRanges.emplace(0u, pool.capacity);
            }
            pool.sizeAtCreation = static_cast<uint32_t>(poolFreeSize(pool));
            const int id = pool.id;
            m_variablePools.emplace(id, std::move(pool));
            setV0(id);
            return true;
        }
        case 5: // DeleteVpl
        {
            const auto it = m_variablePools.find(static_cast<int>(cpu.gpr[4]));
            if (it == m_variablePools.end())
            {
                setV0(KE_UNKNOWN_VPLID);
                return true;
            }
            WaitQueue waiters = std::move(it->second.waiters);
            if (it->second.memory != 0u)
                (void)m_memory.freeAllocation(it->second.memory);
            m_variablePools.erase(it);
            setV0(KE_OK);
            releaseWaiters(waiters, KE_WAIT_DELETE, &cpu);
            return true;
        }
        case 6: // AllocateVpl(vplid, size): waits for memory
        case 7: // pAllocateVpl: fails with KE_NO_MEMORY
        case 8: // ipAllocateVpl
        {
            VariablePool *pool = findPool(cpu.gpr[4]);
            const uint32_t size = cpu.gpr[5];
            if (pool == nullptr)
            {
                setV0(KE_UNKNOWN_VPLID);
                return true;
            }
            if (ordinal != 8u && (size == 0u || static_cast<int32_t>(pool->sizeAtCreation) < static_cast<int32_t>(size)))
            {
                setV0(KE_ILLEGAL_MEMSIZE);
                return true;
            }
            const uint32_t address = poolAllocate(*pool, size);
            if (address != 0u)
                setV0(static_cast<int32_t>(address));
            else if (ordinal != 6u)
                setV0(KE_NO_MEMORY);
            else if (!isCurrentCpu(cpu))
                setV0(KE_CAN_NOT_WAIT);
            else
            {
                setV0(KE_OK);
                m_currentThread->waitArgument = size;
                wait(*m_currentThread, IopWaitType::VariablePool, pool->id, &pool->waiters, (pool->attr & VA_THPRI) != 0u, cpu);
            }
            return true;
        }
        case 9: // FreeVpl(vplid, memory)
        {
            VariablePool *pool = findPool(cpu.gpr[4]);
            if (pool == nullptr)
            {
                setV0(KE_UNKNOWN_VPLID);
                return true;
            }
            if (!poolFree(*pool, cpu.gpr[5]))
            {
                setV0(KE_ERROR);
                return true;
            }
            setV0(KE_OK);
            if (!pool->waiters.threads.empty())
            {
                IopThread *thread = findThread(pool->waiters.threads.front());
                if (thread != nullptr)
                {
                    const uint32_t address = poolAllocate(*pool, thread->waitArgument);
                    if (address != 0u)
                    {
                        pool->waiters.threads.pop_front();
                        thread->waitArgument = 0;
                        thread->cpu.gpr[2] = address;
                        makeReady(*thread, &cpu);
                    }
                }
            }
            return true;
        }
        case 11: // ReferVplStatus(vplid, iop_vpl_info_t *)
        case 12:
        {
            VariablePool *pool = findPool(cpu.gpr[4]);
            if (pool == nullptr)
            {
                setV0(KE_UNKNOWN_VPLID);
                return true;
            }
            const uint32_t info = cpu.gpr[5];
            if (info != 0u)
            {
                m_memory.write32(info + 0u, pool->attr);
                m_memory.write32(info + 4u, pool->option);
                m_memory.write32(info + 8u, pool->sizeAtCreation);
                m_memory.write32(info + 12u, static_cast<uint32_t>(poolFreeSize(*pool)));
                m_memory.write32(info + 16u, static_cast<uint32_t>(pool->waiters.threads.size()));
            }
            setV0(KE_OK);
            return true;
        }
        default:
            return false;
        }
    }

    void IopKernel::sleepCurrent(IopCpuState &cpu)
    {
        if (!isCurrentCpu(cpu))
            return;
        wait(*m_currentThread, IopWaitType::Sleep, 0, nullptr, false, cpu);
    }

    void IopKernel::delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu)
    {
        if (!isCurrentCpu(cpu))
            return;
        m_currentThread->wakeCycle = wakeCycle;
        wait(*m_currentThread, IopWaitType::Delay, 0, nullptr, false, cpu);
    }

    IopThread *IopKernel::beginNextReady(uint64_t currentCycle)
    {
        for (auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Waiting && thread.waitType == IopWaitType::Delay && thread.wakeCycle <= currentCycle)
            {
                thread.cpu.gpr[2] = static_cast<uint32_t>(KE_OK);
                makeReady(thread, nullptr);
            }
        }

        IopThread *next = nullptr;
        for (auto &[id, thread] : m_threads)
        {
            if (thread.state != IopThreadState::Ready)
                continue;
            if (next == nullptr || thread.priority < next->priority ||
                (thread.priority == next->priority && thread.readyOrder < next->readyOrder))
                next = &thread;
        }
        if (next == nullptr)
            return nullptr;

        m_currentThread = next;
        next->state = IopThreadState::Running;
        next->cpu.stopped = false;
        next->cpu.yielded = false;
        return next;
    }

    uint64_t IopKernel::nextWakeCycle(uint64_t fallback) const
    {
        uint64_t nextWake = fallback;
        for (const auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Waiting && thread.waitType == IopWaitType::Delay)
                nextWake = std::min(nextWake, thread.wakeCycle);
        }
        return nextWake;
    }

    void IopKernel::endTimeslice(IopThread &thread, uint32_t returnSentinel)
    {
        if (thread.state == IopThreadState::Dead)
        {
        }
        else if (thread.cpu.pc == returnSentinel)
        {
            // Returning from the entry function ends the thread (ExitThread); v0 holds its result.
            removeFromWaitQueues(thread.id);
            thread.state = IopThreadState::Dormant;
        }
        else if (thread.state == IopThreadState::Running)
        {
            if (thread.cpu.stopped && !thread.cpu.yielded)
                thread.state = IopThreadState::Dormant; // execution fault (outside RAM): nothing can resume it
            else
            {
                // Preempted or out of its slice: it keeps its place at the front of its priority.
                thread.state = IopThreadState::Ready;
                thread.readyOrder = --m_readyFront;
            }
        }
        m_currentThread = nullptr;
        cleanupDeadThreads();
    }

    void IopKernel::cleanupDeadThreads()
    {
        for (auto thread = m_threads.begin(); thread != m_threads.end();)
        {
            if (thread->second.state != IopThreadState::Dead)
            {
                ++thread;
                continue;
            }
            removeFromWaitQueues(thread->first);
            if (thread->second.stackBase != 0u)
                (void)m_memory.freeAllocation(thread->second.stackBase);
            thread = m_threads.erase(thread);
        }
    }

    void IopKernel::terminateThreadsInRange(uint32_t base, uint32_t size)
    {
        for (auto &[id, thread] : m_threads)
        {
            const uint32_t pc = IopMemory::physicalAddress(thread.cpu.pc);
            if (pc >= base && pc < base + size)
                thread.state = IopThreadState::Dead;
        }
    }
}
