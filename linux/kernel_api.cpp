#include "kernel_api.h"

#include "display.h"
#include "process.h"

extern "C" uint64_t obsluga_wywolan_systemowych(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

namespace lx {

namespace {

constexpr uint64_t kVectorDivide = 0;
constexpr uint64_t kVectorInvalidOpcode = 6;
constexpr uint64_t kVectorStackFault = 12;
constexpr uint64_t kVectorGeneralProtection = 13;
constexpr uint64_t kVectorPageFault = 14;
constexpr uint64_t kVectorFpuError = 16;
constexpr uint64_t kVectorAlignmentCheck = 17;
constexpr uint64_t kVectorSimdError = 19;
constexpr uint64_t kPageFaultWrite = 1ULL << 1;

constexpr int kSigIll = 4;
constexpr int kSigBus = 7;
constexpr int kSigFpe = 8;
constexpr int kSigSegv = 11;

const char* const kReaperArgv[] = {"/opt/REAPER/reaper", nullptr};
const char* const kReaperEnvp[] = {
    "HOME=/home/user",
    "USER=user",
    "LOGNAME=user",
    "PATH=/usr/bin:/bin",
    "LANG=C.UTF-8",
    "TERM=linux",
    "TMPDIR=/tmp",
    nullptr,
};

WaitQueue keyboard_waiters;

void return_to_user_checks(InterruptFrame* frame) {
    if (preemption_due()) schedule();
    if (current()->process) signal_deliver_pending(frame);
}

bool from_user(const InterruptFrame* frame) { return frame->cs & 3; }

void dump_kernel_fault(const InterruptFrame* frame, uint64_t cr2) {
    log("kernel exception %lu error=%lx rip=%p rsp=%p cr2=%p task=%d\n", frame->vector, frame->error_code, frame->rip,
        frame->rsp, cr2, current() ? current()->tid : -1);
}

int signal_for_vector(uint64_t vector) {
    switch (vector) {
        case kVectorDivide:
        case kVectorFpuError:
        case kVectorSimdError:
            return kSigFpe;
        case kVectorInvalidOpcode:
            return kSigIll;
        case kVectorAlignmentCheck:
            return kSigBus;
        case kVectorStackFault:
        case kVectorGeneralProtection:
            return kSigSegv;
        default:
            return 0;
    }
}

}

}

using namespace lx;

extern "C" void syscall_dispatch(InterruptFrame* frame) {
    Task* self = current();
    self->user_frame = frame;
    if (self->process) {
        frame->rax = static_cast<uint64_t>(linux_syscall(frame));
    } else {
        frame->rax = obsluga_wywolan_systemowych(frame->r8, frame->r9, frame->r10, frame->r12, frame->r13);
    }
    return_to_user_checks(frame);
}

extern "C" bool lx_handle_exception(InterruptFrame* frame) {
    Task* self = current();
    if (frame->vector == kVectorPageFault) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        Process* process = self ? self->process : nullptr;
        if (process && process->address_space->handle_fault(cr2, frame->error_code & kPageFaultWrite)) return true;
        if (process && from_user(frame)) {
            signal_force(kSigSegv, cr2, frame);
            return true;
        }
        dump_kernel_fault(frame, cr2);
        return false;
    }
    const int signal = signal_for_vector(frame->vector);
    if (self && self->process && from_user(frame) && signal) {
        signal_force(signal, 0, frame);
        return true;
    }
    dump_kernel_fault(frame, 0);
    return false;
}

extern "C" void lx_on_timer_interrupt(InterruptFrame* frame) {
    timer_tick();
    if (from_user(frame)) return_to_user_checks(frame);
}

extern "C" void lx_init(uint64_t kernel_cr3_value) {
    memory_init();
    sched_init(kernel_cr3_value);
    syscall_trace_enabled = boot_cmdline_contains("lx.trace");
    vfs_root();
    devices_init();
    uint64_t start, end;
    if (boot_module(&start, &end) && !vfs_mount_image(start, end)) log("rootfs: boot module has no valid image\n");
}

extern "C" void lx_start_shell(void (*entry)(void*)) { spawn_kernel_task(entry, nullptr, kernel_cr3()); }

extern "C" void lx_run() {
    if (boot_cmdline_contains("autostart=reaper")) lx_launch_reaper();
    idle_loop();
}

extern "C" bool lx_launch_reaper() {
    if (process_current_instance()) {
        gui_surface_raise();
        return true;
    }
    if (!vfs_has_image()) {
        log("reaper: no root filesystem image was loaded\n");
        return false;
    }
    if (!display_start()) {
        log("reaper: no framebuffer surface is available\n");
        return false;
    }
    const int pid = process_spawn(kReaperArgv[0], kReaperArgv, kReaperEnvp);
    if (pid < 0) {
        log("reaper: launch failed with error %d\n", pid);
        display_on_process_exit();
        return false;
    }
    log("reaper: started as process %d\n", pid);
    return true;
}

extern "C" void lx_wait_for_keyboard() { block_on(&keyboard_waiters, 0); }
extern "C" void lx_keyboard_ready() { wake_all(&keyboard_waiters); }
extern "C" uint32_t lx_timer_ticks_per_ms(volatile uint32_t* lapic) { return clock_calibrate_lapic(lapic); }
