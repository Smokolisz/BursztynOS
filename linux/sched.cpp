#include "sched.h"

#include "errno.h"
#include "zegar-rtc.h"

extern "C" {
void context_switch(uint64_t* save_rsp, uint64_t load_rsp);
void interrupt_return();
void kernel_task_trampoline();
void process_task_trampoline();
void tss_set_kernel_stack(uint64_t top);
extern uint64_t syscall_kernel_rsp;
}

namespace lx {

extern "C" {
Task* current_task;
uint64_t syscall_kernel_rsp;
uint64_t syscall_user_rsp;
}

namespace {

constexpr size_t kKernelStackSize = 64 * 1024;
constexpr uint32_t kTimeSliceTicks = 2;
constexpr uint32_t kMsrFsBase = 0xC0000100;
constexpr uint32_t kMsrGsBase = 0xC0000101;

alignas(16) Task idle_task;
Task* run_head;
Task* run_tail;
Task* timer_list;
Task* reap_list;
int next_tid = 1;
bool slice_expired;

alignas(16) uint8_t initial_fx_state[512];

uint64_t tsc_to_ns_multiplier;
uint64_t wall_clock_base_ns;

void enqueue(Task* task) {
    task->run_next = nullptr;
    if (run_tail) {
        run_tail->run_next = task;
    } else {
        run_head = task;
    }
    run_tail = task;
}

void remove_from_run_queue(Task* task) {
    Task** link = &run_head;
    Task* previous = nullptr;
    while (*link) {
        if (*link == task) {
            *link = task->run_next;
            if (run_tail == task) run_tail = previous;
            task->run_next = nullptr;
            return;
        }
        previous = *link;
        link = &previous->run_next;
    }
}

void remove_from_wait_queue(Task* task) {
    WaitQueue* queue = task->wait_queue;
    if (!queue) return;
    Task* previous = nullptr;
    for (Task* entry = queue->head; entry; previous = entry, entry = entry->wait_next) {
        if (entry != task) continue;
        if (previous) {
            previous->wait_next = entry->wait_next;
        } else {
            queue->head = entry->wait_next;
        }
        if (queue->tail == entry) queue->tail = previous;
        break;
    }
    task->wait_next = nullptr;
    task->wait_queue = nullptr;
}

void remove_from_timer_list(Task* task) {
    if (!task->deadline_ns) return;
    for (Task** link = &timer_list; *link; link = &(*link)->timer_next) {
        if (*link == task) {
            *link = task->timer_next;
            break;
        }
    }
    task->deadline_ns = 0;
    task->timer_next = nullptr;
}

Task* allocate_task() {
    auto* task = kernel_new<Task>();
    task->kernel_stack = kernel_alloc(kKernelStackSize);
    task->kernel_stack_top = (reinterpret_cast<uint64_t>(task->kernel_stack) + kKernelStackSize) & ~15ULL;
    task->tid = next_tid++;
    task->state = TaskState::kRunnable;
    task->slice = kTimeSliceTicks;
    memcpy(task->fx_state, initial_fx_state, sizeof(initial_fx_state));
    return task;
}

void push_switch_frame(Task* task, uint64_t* stack, uint64_t return_address, uint64_t r12, uint64_t r13) {
    *--stack = return_address;
    *--stack = 0;
    *--stack = 0;
    *--stack = r12;
    *--stack = r13;
    *--stack = 0;
    *--stack = 0;
    task->saved_rsp = reinterpret_cast<uint64_t>(stack);
}

void reap_dead_tasks() {
    while (reap_list) {
        Task* task = reap_list;
        reap_list = task->run_next;
        kernel_free(task->kernel_stack);
        kernel_free(task);
    }
}

void switch_to(Task* next) {
    Task* previous = current_task;
    if (previous == next) return;
    asm volatile("fxsave64 %0" : "=m"(previous->fx_state));
    asm volatile("fxrstor64 %0" : : "m"(next->fx_state));
    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
        syscall_kernel_rsp = next->kernel_stack_top;
    }
    if (read_cr3() != next->cr3) write_cr3(next->cr3);
    write_msr(kMsrFsBase, next->fs_base);
    write_msr(kMsrGsBase, next->gs_base);
    current_task = next;
    context_switch(&previous->saved_rsp, next->saved_rsp);
}

Task* dequeue_next() {
    Task* next = run_head;
    if (next) {
        run_head = next->run_next;
        if (!run_head) run_tail = nullptr;
        next->run_next = nullptr;
    }
    return next;
}

int days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + static_cast<int>(day_of_era) - 719468;
}

}

uint32_t clock_calibrate_lapic(volatile uint32_t* lapic) {
    constexpr uint16_t kPitCommand = 0x43;
    constexpr uint16_t kPitChannel2 = 0x42;
    constexpr uint16_t kPitGate = 0x61;
    constexpr uint32_t kPitTicksFor10Ms = 11932;
    constexpr uint32_t kLapicInitialCount = 0x380 / 4;
    constexpr uint32_t kLapicCurrentCount = 0x390 / 4;

    const uint8_t gate = port_in8(kPitGate);
    port_out8(kPitGate, (gate & ~0x02) & ~0x01);
    port_out8(kPitCommand, 0xB0);
    port_out8(kPitChannel2, kPitTicksFor10Ms & 0xFF);
    port_out8(kPitChannel2, kPitTicksFor10Ms >> 8);

    lapic[kLapicInitialCount] = 0xFFFFFFFF;
    const uint64_t tsc_start = read_tsc();
    port_out8(kPitGate, (gate & ~0x02) | 0x01);
    while (!(port_in8(kPitGate) & 0x20)) {
    }
    const uint64_t tsc_end = read_tsc();
    const uint32_t lapic_elapsed = 0xFFFFFFFF - lapic[kLapicCurrentCount];
    lapic[kLapicInitialCount] = 0;
    port_out8(kPitGate, gate);

    tsc_to_ns_multiplier = (10000000ULL << 32) / (tsc_end - tsc_start);
    return lapic_elapsed / 10;
}

void clock_init() {
    czas_rtc time;
    pobierz_czas_rtc(&time);
    const uint64_t days = days_from_civil(static_cast<int>(time.rok), time.miesiac, time.dzien);
    wall_clock_base_ns = (days * 86400 + time.godziny * 3600ULL + time.minuty * 60ULL + time.sekundy) * 1000000000ULL;
    wall_clock_base_ns -= now_ns();
}

uint64_t now_ns() {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(read_tsc()) * tsc_to_ns_multiplier) >> 32);
}

uint64_t wall_clock_ns() { return wall_clock_base_ns + now_ns(); }

uint64_t kernel_cr3() { return idle_task.cr3; }

void sched_init(uint64_t kernel_cr3) {
    asm volatile(
        "mov $0x1B, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : : "rax");
    asm volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    asm volatile("fxsave64 %0" : "=m"(initial_fx_state));
    idle_task.cr3 = kernel_cr3;
    idle_task.tid = 0;
    idle_task.state = TaskState::kRunnable;
    memcpy(idle_task.fx_state, initial_fx_state, sizeof(initial_fx_state));
    current_task = &idle_task;
}

Task* spawn_kernel_task(void (*entry)(void*), void* argument, uint64_t cr3) {
    Task* task = allocate_task();
    task->cr3 = cr3;
    push_switch_frame(task, reinterpret_cast<uint64_t*>(task->kernel_stack_top),
                      reinterpret_cast<uint64_t>(kernel_task_trampoline), reinterpret_cast<uint64_t>(entry),
                      reinterpret_cast<uint64_t>(argument));
    enqueue(task);
    return task;
}

Task* spawn_user_thread(Process* process, const InterruptFrame& frame, uint64_t cr3) {
    Task* task = allocate_task();
    task->cr3 = cr3;
    task->process = process;
    auto* stack = reinterpret_cast<uint64_t*>(task->kernel_stack_top);
    stack -= sizeof(InterruptFrame) / sizeof(uint64_t);
    memcpy(stack, &frame, sizeof(InterruptFrame));
    task->user_frame = reinterpret_cast<InterruptFrame*>(stack);
    push_switch_frame(task, stack, reinterpret_cast<uint64_t>(interrupt_return), 0, 0);
    enqueue(task);
    return task;
}

Task* spawn_process_task(Process* process, void (*entry)(void*), void* argument, uint64_t cr3) {
    Task* task = allocate_task();
    task->cr3 = cr3;
    task->process = process;
    auto* stack = reinterpret_cast<uint64_t*>(task->kernel_stack_top);
    stack -= sizeof(InterruptFrame) / sizeof(uint64_t);
    task->user_frame = reinterpret_cast<InterruptFrame*>(stack);
    push_switch_frame(task, stack, reinterpret_cast<uint64_t>(process_task_trampoline),
                      reinterpret_cast<uint64_t>(entry), reinterpret_cast<uint64_t>(argument));
    enqueue(task);
    return task;
}

void save_fpu_state(Task* task) { asm volatile("fxsave64 %0" : "=m"(task->fx_state)); }

void reset_fpu_state(Task* task) {
    memcpy(task->fx_state, initial_fx_state, sizeof(initial_fx_state));
    asm volatile("fxrstor64 %0" : : "m"(task->fx_state));
}

void schedule() {
    slice_expired = false;
    Task* self = current_task;
    Task* next = dequeue_next();
    if (!next) {
        if (self->state == TaskState::kRunnable && self != &idle_task) return;
        next = &idle_task;
    }
    if (self != &idle_task && self->state == TaskState::kRunnable) enqueue(self);
    next->slice = kTimeSliceTicks;
    switch_to(next);
}

void yield() { schedule(); }

int block_on(WaitQueue* queue, uint64_t deadline_ns) {
    Task* self = current_task;
    if (self->signal_pending & ~self->signal_mask) return -kEINTR;
    if (deadline_ns && deadline_ns <= now_ns()) return -kETIMEDOUT;
    self->wake_result = 0;
    self->state = TaskState::kBlocked;
    if (queue) {
        self->wait_queue = queue;
        self->wait_next = nullptr;
        if (queue->tail) {
            queue->tail->wait_next = self;
        } else {
            queue->head = self;
        }
        queue->tail = self;
    }
    if (deadline_ns) {
        self->deadline_ns = deadline_ns;
        self->timer_next = timer_list;
        timer_list = self;
    }
    schedule();
    return self->wake_result;
}

void wake_task(Task* task, int result) {
    if (task->state != TaskState::kBlocked) return;
    remove_from_wait_queue(task);
    remove_from_timer_list(task);
    task->wake_result = result;
    task->state = TaskState::kRunnable;
    enqueue(task);
}

void wake_one(WaitQueue* queue) {
    if (queue->head) wake_task(queue->head, 0);
}

void wake_all(WaitQueue* queue) {
    while (queue->head) wake_task(queue->head, 0);
}

void retire_task(Task* task) {
    remove_from_wait_queue(task);
    remove_from_timer_list(task);
    remove_from_run_queue(task);
    task->state = TaskState::kDead;
    task->run_next = reap_list;
    reap_list = task;
}

void exit_current_task() {
    Task* self = current_task;
    self->state = TaskState::kDead;
    self->run_next = reap_list;
    reap_list = self;
    schedule();
    panic("dead task %d was rescheduled", self->tid);
}

void idle_loop() {
    for (;;) {
        reap_dead_tasks();
        if (run_head) {
            schedule();
            continue;
        }
        asm volatile("sti; hlt; cli");
    }
}

void timer_tick() {
    const uint64_t now = now_ns();
    Task* entry = timer_list;
    while (entry) {
        Task* next = entry->timer_next;
        if (entry->deadline_ns <= now) wake_task(entry, -kETIMEDOUT);
        entry = next;
    }
    Task* self = current_task;
    if (self->slice && --self->slice == 0) slice_expired = true;
}

bool preemption_due() { return slice_expired && run_head; }

}
