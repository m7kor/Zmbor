#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    // IOP thread states and wait types as ReferThreadStatus reports them (thbase.h THS_*, TSW_*).
    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Waiting,
        Suspended,
        WaitSuspended,
        Dead,
    };

    enum class IopWaitType : uint8_t
    {
        None = 0,
        Sleep = 1,
        Delay = 2,
        Semaphore = 3,
        EventFlag = 4,
        MessageBox = 5,
        VariablePool = 6,
        FixedPool = 7,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopWaitType waitType = IopWaitType::None;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t gp = 0;
        // Current priority is 0 until the thread is first started (a real PS2 reports "curr prio: 00" for a
        // created, never-started thread).
        uint32_t priority = 0;
        uint32_t initialPriority = 0;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        // Event flag result-pattern pointer, message-box message variable, or variable-pool request size.
        uint32_t waitArgument = 0;
        int wakeupCount = 0;
        // Ready-queue position within a priority: smaller runs first. A preempted thread keeps its place at the
        // front; a thread that becomes ready goes to the back.
        int64_t readyOrder = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchMessageBoxImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchVariablePoolImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);

        // A module's start entry runs in a kernel thread of its own (on a real PS2 the start code sees itself as a
        // RUNNING thread of priority 8). Returns the thread id, or a negative error.
        [[nodiscard]] int createStartThread(uint32_t entry, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t priority);
        [[nodiscard]] bool isThreadDormant(int id) const;
        [[nodiscard]] uint32_t threadResult(int id) const;
        void deleteThreadNow(int id);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }
        [[nodiscard]] bool hasCurrentThread() const noexcept { return m_currentThread != nullptr; }

    private:
        struct WaitQueue
        {
            // Waiting thread ids in wake order: FIFO, or priority order when the object's attribute asks for it.
            std::deque<int> threads;
        };

        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
            int initial = 0;
            WaitQueue waiters;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t initialBits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            WaitQueue waiters;
        };

        struct MessageBox
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            uint32_t count = 0;
            // Guest address of the newest message; messages form a circular list through their guest 'next'
            // field, newest->next being the oldest (the layout ReferMbxStatus exposes as topPacket).
            uint32_t newest = 0;
            WaitQueue waiters;
        };

        struct VariablePool
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            // Guest heap: [base, base + capacity) split into blocks of an 8-byte header plus 8-byte aligned data.
            bool heapValid = false;
            uint32_t memory = 0;
            uint32_t base = 0;
            uint32_t capacity = 0;
            uint32_t sizeAtCreation = 0;
            std::map<uint32_t, uint32_t> freeRanges; // offset -> length
            std::map<uint32_t, uint32_t> allocated;  // data address -> block length
            WaitQueue waiters;
        };

        IopThread *findThread(int id);
        [[nodiscard]] bool isCurrentCpu(const IopCpuState &cpu) const;
        void makeReady(IopThread &thread, IopCpuState *caller);
        void wait(IopThread &thread, IopWaitType type, int objectId, WaitQueue *queue, bool priorityOrder, IopCpuState &cpu);
        void removeFromWaitQueues(int threadId);
        void releaseWaiters(WaitQueue &queue, int32_t result, IopCpuState *caller);

        [[nodiscard]] bool referThreadStatus(IopThread &thread, uint32_t outputAddress);
        [[nodiscard]] static bool eventSatisfied(uint32_t flagBits, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event, IopCpuState *caller);

        void sendMessage(MessageBox &box, uint32_t message);
        [[nodiscard]] uint32_t takeMessage(MessageBox &box);

        [[nodiscard]] uint32_t poolAllocate(VariablePool &pool, uint32_t size);
        [[nodiscard]] bool poolFree(VariablePool &pool, uint32_t address);
        [[nodiscard]] static int32_t poolFreeSize(const VariablePool &pool);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        std::map<int, MessageBox> m_messageBoxes;
        std::map<int, VariablePool> m_variablePools;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        uint32_t m_nextMessageBoxId = 1;
        uint32_t m_nextVariablePoolId = 1;
        int64_t m_readyBack = 0;
        int64_t m_readyFront = 0;
        IopThread *m_currentThread = nullptr;
    };
}
