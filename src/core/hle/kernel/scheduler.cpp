// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// SelectThreads, Yield functions originally by TuxSH.
// licensed under GPLv2 or later under exception provided by the author.

#include <algorithm>
#include <set>
#include <unordered_set>
#include <utility>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/scheduler.h"
#include "core/hle/kernel/time_manager.h"

namespace Kernel {

GlobalScheduler::GlobalScheduler(KernelCore& kernel) : kernel{kernel} {}

GlobalScheduler::~GlobalScheduler() = default;

void GlobalScheduler::AddThread(std::shared_ptr<Thread> thread) {
    thread_list.push_back(std::move(thread));
}

void GlobalScheduler::RemoveThread(std::shared_ptr<Thread> thread) {
    thread_list.erase(std::remove(thread_list.begin(), thread_list.end(), thread),
                      thread_list.end());
}

void GlobalScheduler::UnloadThread(std::size_t core) {
    Scheduler& sched = kernel.Scheduler(core);
    sched.UnloadThread();
}

void GlobalScheduler::SelectThread(std::size_t core) {
    const auto update_thread = [](Thread* thread, Scheduler& sched) {
        if (thread != sched.selected_thread.get()) {
            if (thread == nullptr) {
                ++sched.idle_selection_count;
            }
            sched.selected_thread = SharedFrom(thread);
        }
        sched.is_context_switch_pending = sched.selected_thread != sched.current_thread;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    };
    Scheduler& sched = kernel.Scheduler(core);
    Thread* current_thread = nullptr;
    // Step 1: Get top thread in schedule queue.
    current_thread = scheduled_queue[core].empty() ? nullptr : scheduled_queue[core].front();
    if (current_thread) {
        update_thread(current_thread, sched);
        return;
    }
    // Step 2: Try selecting a suggested thread.
    Thread* winner = nullptr;
    std::set<s32> sug_cores;
    for (auto thread : suggested_queue[core]) {
        s32 this_core = thread->GetProcessorID();
        Thread* thread_on_core = nullptr;
        if (this_core >= 0) {
            thread_on_core = scheduled_queue[this_core].front();
        }
        if (this_core < 0 || thread != thread_on_core) {
            winner = thread;
            break;
        }
        sug_cores.insert(this_core);
    }
    // if we got a suggested thread, select it, else do a second pass.
    if (winner && winner->GetPriority() > 2) {
        if (winner->IsRunning()) {
            UnloadThread(static_cast<u32>(winner->GetProcessorID()));
        }
        TransferToCore(winner->GetPriority(), static_cast<s32>(core), winner);
        update_thread(winner, sched);
        return;
    }
    // Step 3: Select a suggested thread from another core
    for (auto& src_core : sug_cores) {
        auto it = scheduled_queue[src_core].begin();
        it++;
        if (it != scheduled_queue[src_core].end()) {
            Thread* thread_on_core = scheduled_queue[src_core].front();
            Thread* to_change = *it;
            if (thread_on_core->IsRunning() || to_change->IsRunning()) {
                UnloadThread(static_cast<u32>(src_core));
            }
            TransferToCore(thread_on_core->GetPriority(), static_cast<s32>(core), thread_on_core);
            current_thread = thread_on_core;
            break;
        }
    }
    update_thread(current_thread, sched);
}

bool GlobalScheduler::YieldThread(Thread* yielding_thread) {
    // Note: caller should use critical section, etc.
    const u32 core_id = static_cast<u32>(yielding_thread->GetProcessorID());
    const u32 priority = yielding_thread->GetPriority();

    // Yield the thread
    const Thread* const winner = scheduled_queue[core_id].front(priority);
    ASSERT_MSG(yielding_thread == winner, "Thread yielding without being in front");
    scheduled_queue[core_id].yield(priority);

    return AskForReselectionOrMarkRedundant(yielding_thread, winner);
}

bool GlobalScheduler::YieldThreadAndBalanceLoad(Thread* yielding_thread) {
    // Note: caller should check if !thread.IsSchedulerOperationRedundant and use critical section,
    // etc.
    const u32 core_id = static_cast<u32>(yielding_thread->GetProcessorID());
    const u32 priority = yielding_thread->GetPriority();

    // Yield the thread
    ASSERT_MSG(yielding_thread == scheduled_queue[core_id].front(priority),
               "Thread yielding without being in front");
    scheduled_queue[core_id].yield(priority);

    std::array<Thread*, Core::Hardware::NUM_CPU_CORES> current_threads;
    for (std::size_t i = 0; i < current_threads.size(); i++) {
        current_threads[i] = scheduled_queue[i].empty() ? nullptr : scheduled_queue[i].front();
    }

    Thread* next_thread = scheduled_queue[core_id].front(priority);
    Thread* winner = nullptr;
    for (auto& thread : suggested_queue[core_id]) {
        const s32 source_core = thread->GetProcessorID();
        if (source_core >= 0) {
            if (current_threads[source_core] != nullptr) {
                if (thread == current_threads[source_core] ||
                    current_threads[source_core]->GetPriority() < min_regular_priority) {
                    continue;
                }
            }
        }
        if (next_thread->GetLastRunningTicks() >= thread->GetLastRunningTicks() ||
            next_thread->GetPriority() < thread->GetPriority()) {
            if (thread->GetPriority() <= priority) {
                winner = thread;
                break;
            }
        }
    }

    if (winner != nullptr) {
        if (winner != yielding_thread) {
            if (winner->IsRunning()) {
                UnloadThread(static_cast<u32>(winner->GetProcessorID()));
            }
            TransferToCore(winner->GetPriority(), s32(core_id), winner);
        }
    } else {
        winner = next_thread;
    }

    return AskForReselectionOrMarkRedundant(yielding_thread, winner);
}

bool GlobalScheduler::YieldThreadAndWaitForLoadBalancing(Thread* yielding_thread) {
    // Note: caller should check if !thread.IsSchedulerOperationRedundant and use critical section,
    // etc.
    Thread* winner = nullptr;
    const u32 core_id = static_cast<u32>(yielding_thread->GetProcessorID());

    // Remove the thread from its scheduled mlq, put it on the corresponding "suggested" one instead
    TransferToCore(yielding_thread->GetPriority(), -1, yielding_thread);

    // If the core is idle, perform load balancing, excluding the threads that have just used this
    // function...
    if (scheduled_queue[core_id].empty()) {
        // Here, "current_threads" is calculated after the ""yield"", unlike yield -1
        std::array<Thread*, Core::Hardware::NUM_CPU_CORES> current_threads;
        for (std::size_t i = 0; i < current_threads.size(); i++) {
            current_threads[i] = scheduled_queue[i].empty() ? nullptr : scheduled_queue[i].front();
        }
        for (auto& thread : suggested_queue[core_id]) {
            const s32 source_core = thread->GetProcessorID();
            if (source_core < 0 || thread == current_threads[source_core]) {
                continue;
            }
            if (current_threads[source_core] == nullptr ||
                current_threads[source_core]->GetPriority() >= min_regular_priority) {
                winner = thread;
            }
            break;
        }
        if (winner != nullptr) {
            if (winner != yielding_thread) {
                if (winner->IsRunning()) {
                    UnloadThread(static_cast<u32>(winner->GetProcessorID()));
                }
                TransferToCore(winner->GetPriority(), static_cast<s32>(core_id), winner);
            }
        } else {
            winner = yielding_thread;
        }
    }

    return AskForReselectionOrMarkRedundant(yielding_thread, winner);
}

void GlobalScheduler::PreemptThreads() {
    for (std::size_t core_id = 0; core_id < Core::Hardware::NUM_CPU_CORES; core_id++) {
        const u32 priority = preemption_priorities[core_id];

        if (scheduled_queue[core_id].size(priority) > 0) {
            scheduled_queue[core_id].front(priority)->IncrementYieldCount();
            scheduled_queue[core_id].yield(priority);
            if (scheduled_queue[core_id].size(priority) > 1) {
                scheduled_queue[core_id].front(priority)->IncrementYieldCount();
            }
        }

        Thread* current_thread =
            scheduled_queue[core_id].empty() ? nullptr : scheduled_queue[core_id].front();
        Thread* winner = nullptr;
        for (auto& thread : suggested_queue[core_id]) {
            const s32 source_core = thread->GetProcessorID();
            if (thread->GetPriority() != priority) {
                continue;
            }
            if (source_core >= 0) {
                Thread* next_thread = scheduled_queue[source_core].empty()
                                          ? nullptr
                                          : scheduled_queue[source_core].front();
                if (next_thread != nullptr && next_thread->GetPriority() < 2) {
                    break;
                }
                if (next_thread == thread) {
                    continue;
                }
            }
            if (current_thread != nullptr &&
                current_thread->GetLastRunningTicks() >= thread->GetLastRunningTicks()) {
                winner = thread;
                break;
            }
        }

        if (winner != nullptr) {
            if (winner->IsRunning()) {
                UnloadThread(static_cast<u32>(winner->GetProcessorID()));
            }
            TransferToCore(winner->GetPriority(), s32(core_id), winner);
            current_thread =
                winner->GetPriority() <= current_thread->GetPriority() ? winner : current_thread;
        }

        if (current_thread != nullptr && current_thread->GetPriority() > priority) {
            for (auto& thread : suggested_queue[core_id]) {
                const s32 source_core = thread->GetProcessorID();
                if (thread->GetPriority() < priority) {
                    continue;
                }
                if (source_core >= 0) {
                    Thread* next_thread = scheduled_queue[source_core].empty()
                                              ? nullptr
                                              : scheduled_queue[source_core].front();
                    if (next_thread != nullptr && next_thread->GetPriority() < 2) {
                        break;
                    }
                    if (next_thread == thread) {
                        continue;
                    }
                }
                if (current_thread != nullptr &&
                    current_thread->GetLastRunningTicks() >= thread->GetLastRunningTicks()) {
                    winner = thread;
                    break;
                }
            }

            if (winner != nullptr) {
                if (winner->IsRunning()) {
                    UnloadThread(static_cast<u32>(winner->GetProcessorID()));
                }
                TransferToCore(winner->GetPriority(), s32(core_id), winner);
                current_thread = winner;
            }
        }

        is_reselection_pending.store(true, std::memory_order_release);
    }
}

void GlobalScheduler::Suggest(u32 priority, std::size_t core, Thread* thread) {
    suggested_queue[core].add(thread, priority);
}

void GlobalScheduler::Unsuggest(u32 priority, std::size_t core, Thread* thread) {
    suggested_queue[core].remove(thread, priority);
}

void GlobalScheduler::Schedule(u32 priority, std::size_t core, Thread* thread) {
    ASSERT_MSG(thread->GetProcessorID() == s32(core), "Thread must be assigned to this core.");
    scheduled_queue[core].add(thread, priority);
}

void GlobalScheduler::SchedulePrepend(u32 priority, std::size_t core, Thread* thread) {
    ASSERT_MSG(thread->GetProcessorID() == s32(core), "Thread must be assigned to this core.");
    scheduled_queue[core].add(thread, priority, false);
}

void GlobalScheduler::Reschedule(u32 priority, std::size_t core, Thread* thread) {
    scheduled_queue[core].remove(thread, priority);
    scheduled_queue[core].add(thread, priority);
}

void GlobalScheduler::Unschedule(u32 priority, std::size_t core, Thread* thread) {
    scheduled_queue[core].remove(thread, priority);
}

void GlobalScheduler::TransferToCore(u32 priority, s32 destination_core, Thread* thread) {
    const bool schedulable = thread->GetPriority() < THREADPRIO_COUNT;
    const s32 source_core = thread->GetProcessorID();
    if (source_core == destination_core || !schedulable) {
        return;
    }
    thread->SetProcessorID(destination_core);
    if (source_core >= 0) {
        Unschedule(priority, static_cast<u32>(source_core), thread);
    }
    if (destination_core >= 0) {
        Unsuggest(priority, static_cast<u32>(destination_core), thread);
        Schedule(priority, static_cast<u32>(destination_core), thread);
    }
    if (source_core >= 0) {
        Suggest(priority, static_cast<u32>(source_core), thread);
    }
}

bool GlobalScheduler::AskForReselectionOrMarkRedundant(Thread* current_thread,
                                                       const Thread* winner) {
    if (current_thread == winner) {
        current_thread->IncrementYieldCount();
        return true;
    } else {
        is_reselection_pending.store(true, std::memory_order_release);
        return false;
    }
}

void GlobalScheduler::Shutdown() {
    for (std::size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; core++) {
        scheduled_queue[core].clear();
        suggested_queue[core].clear();
    }
    thread_list.clear();
}

void GlobalScheduler::Lock() {
    Core::EmuThreadHandle current_thread = kernel.GetCurrentEmuThreadID();
    if (current_thread == current_owner) {
        ++scope_lock;
    } else {
        inner_lock.lock();
        current_owner = current_thread;
        ASSERT(current_owner != Core::EmuThreadHandle::InvalidHandle());
        scope_lock = 1;
    }
}

void GlobalScheduler::Unlock() {
    if (--scope_lock != 0) {
        ASSERT(scope_lock > 0);
        return;
    }
    for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; i++) {
        SelectThread(i);
    }
    current_owner = Core::EmuThreadHandle::InvalidHandle();
    scope_lock = 1;
    inner_lock.unlock();
    // TODO(Blinkhawk): Setup the interrupts and change context on current core.
}

Scheduler::Scheduler(Core::System& system, std::size_t core_id)
    : system{system}, core_id{core_id} {}

Scheduler::~Scheduler() = default;

bool Scheduler::HaveReadyThreads() const {
    return system.GlobalScheduler().HaveReadyThreads(core_id);
}

Thread* Scheduler::GetCurrentThread() const {
    return current_thread.get();
}

Thread* Scheduler::GetSelectedThread() const {
    return selected_thread.get();
}

void Scheduler::SelectThreads() {
    system.GlobalScheduler().SelectThread(core_id);
}

u64 Scheduler::GetLastContextSwitchTicks() const {
    return last_context_switch_time;
}

void Scheduler::TryDoContextSwitch() {
    if (is_context_switch_pending) {
        SwitchContext();
    }
}

void Scheduler::UnloadThread() {
    Thread* const previous_thread = GetCurrentThread();
    Process* const previous_process = system.Kernel().CurrentProcess();

    UpdateLastContextSwitchTime(previous_thread, previous_process);

    // Save context for previous thread
    if (previous_thread) {
        system.ArmInterface(core_id).SaveContext(previous_thread->GetContext32());
        system.ArmInterface(core_id).SaveContext(previous_thread->GetContext64());
        // Save the TPIDR_EL0 system register in case it was modified.
        previous_thread->SetTPIDR_EL0(system.ArmInterface(core_id).GetTPIDR_EL0());

        if (previous_thread->GetStatus() == ThreadStatus::Running) {
            // This is only the case when a reschedule is triggered without the current thread
            // yielding execution (i.e. an event triggered, system core time-sliced, etc)
            previous_thread->SetStatus(ThreadStatus::Ready);
        }
        previous_thread->SetIsRunning(false);
    }
    current_thread = nullptr;
}

void Scheduler::SwitchContext() {
    Thread* const previous_thread = GetCurrentThread();
    Thread* const new_thread = GetSelectedThread();

    is_context_switch_pending = false;
    if (new_thread == previous_thread) {
        return;
    }

    Process* const previous_process = system.Kernel().CurrentProcess();

    UpdateLastContextSwitchTime(previous_thread, previous_process);

    // Save context for previous thread
    if (previous_thread) {
        system.ArmInterface(core_id).SaveContext(previous_thread->GetContext32());
        system.ArmInterface(core_id).SaveContext(previous_thread->GetContext64());
        // Save the TPIDR_EL0 system register in case it was modified.
        previous_thread->SetTPIDR_EL0(system.ArmInterface(core_id).GetTPIDR_EL0());

        if (previous_thread->GetStatus() == ThreadStatus::Running) {
            // This is only the case when a reschedule is triggered without the current thread
            // yielding execution (i.e. an event triggered, system core time-sliced, etc)
            previous_thread->SetStatus(ThreadStatus::Ready);
        }
        previous_thread->SetIsRunning(false);
    }

    // Load context of new thread
    if (new_thread) {
        ASSERT_MSG(new_thread->GetProcessorID() == s32(this->core_id),
                   "Thread must be assigned to this core.");
        ASSERT_MSG(new_thread->GetStatus() == ThreadStatus::Ready,
                   "Thread must be ready to become running.");

        // Cancel any outstanding wakeup events for this thread
        new_thread->CancelWakeupTimer();
        current_thread = SharedFrom(new_thread);
        new_thread->SetStatus(ThreadStatus::Running);
        new_thread->SetIsRunning(true);

        auto* const thread_owner_process = current_thread->GetOwnerProcess();
        if (previous_process != thread_owner_process) {
            system.Kernel().MakeCurrentProcess(thread_owner_process);
        }

        system.ArmInterface(core_id).LoadContext(new_thread->GetContext32());
        system.ArmInterface(core_id).LoadContext(new_thread->GetContext64());
        system.ArmInterface(core_id).SetTlsAddress(new_thread->GetTLSAddress());
        system.ArmInterface(core_id).SetTPIDR_EL0(new_thread->GetTPIDR_EL0());
    } else {
        current_thread = nullptr;
        // Note: We do not reset the current process and current page table when idling because
        // technically we haven't changed processes, our threads are just paused.
    }
}

void Scheduler::UpdateLastContextSwitchTime(Thread* thread, Process* process) {
    const u64 prev_switch_ticks = last_context_switch_time;
    const u64 most_recent_switch_ticks = system.CoreTiming().GetTicks();
    const u64 update_ticks = most_recent_switch_ticks - prev_switch_ticks;

    if (thread != nullptr) {
        thread->UpdateCPUTimeTicks(update_ticks);
    }

    if (process != nullptr) {
        process->UpdateCPUTimeTicks(update_ticks);
    }

    last_context_switch_time = most_recent_switch_ticks;
}

void Scheduler::Shutdown() {
    current_thread = nullptr;
    selected_thread = nullptr;
}

SchedulerLock::SchedulerLock(KernelCore& kernel) : kernel{kernel} {
    kernel.GlobalScheduler().Lock();
}

SchedulerLock::~SchedulerLock() {
    kernel.GlobalScheduler().Unlock();
}

SchedulerLockAndSleep::SchedulerLockAndSleep(KernelCore& kernel, Handle& event_handle,
                                             Thread* time_task, s64 nanoseconds)
    : SchedulerLock{kernel}, event_handle{event_handle}, time_task{time_task}, nanoseconds{
                                                                                   nanoseconds} {
    event_handle = InvalidHandle;
}

SchedulerLockAndSleep::~SchedulerLockAndSleep() {
    if (sleep_cancelled) {
        return;
    }
    auto& time_manager = kernel.TimeManager();
    time_manager.ScheduleTimeEvent(event_handle, time_task, nanoseconds);
}

} // namespace Kernel
