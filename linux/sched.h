#pragma once

#include "base.h"

namespace lx {

struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

enum class TaskState : uint8_t { kRunnable, kBlocked, kDead };

struct Process;
struct WaitQueue;

struct alignas(16) Task {
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    void* kernel_stack;
    uint64_t cr3;
    uint64_t fs_base;
    uint64_t gs_base;

    Process* process;
    Task* process_next;
    int tid;
    TaskState state;

    Task* run_next;
    WaitQueue* wait_queue;
    Task* wait_next;
    uint64_t deadline_ns;
    Task* timer_next;
    int wake_result;
    uint32_t slice;

    uint64_t futex_address;
    uint32_t futex_bitset;

    uint64_t signal_mask;
    uint64_t signal_pending;
    uint64_t alt_stack_base;
    uint64_t alt_stack_size;
    uint32_t alt_stack_flags;
    uint64_t clear_child_tid;

    InterruptFrame* user_frame;

    alignas(16) uint8_t fx_state[512];
};

struct WaitQueue {
    Task* head;
    Task* tail;
};

extern "C" Task* current_task;

inline Task* current() { return current_task; }

void clock_init();
uint32_t clock_calibrate_lapic(volatile uint32_t* lapic);
uint64_t now_ns();
uint64_t wall_clock_ns();

void sched_init(uint64_t kernel_cr3);
uint64_t kernel_cr3();
Task* spawn_kernel_task(void (*entry)(void*), void* argument, uint64_t cr3);
Task* spawn_user_thread(Process* process, const InterruptFrame& frame, uint64_t cr3);
Task* spawn_process_task(Process* process, void (*entry)(void*), void* argument, uint64_t cr3);
void save_fpu_state(Task* task);
void reset_fpu_state(Task* task);

void schedule();
void yield();
int block_on(WaitQueue* queue, uint64_t deadline_ns);
void wake_task(Task* task, int result);
void wake_one(WaitQueue* queue);
void wake_all(WaitQueue* queue);
void retire_task(Task* task);
[[noreturn]] void exit_current_task();
[[noreturn]] void idle_loop();

void timer_tick();
bool preemption_due();

}
