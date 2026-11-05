#include "errno.h"
#include "process.h"

namespace lx {

namespace {

constexpr int kSigChld = 17;
constexpr int kSigCont = 18;
constexpr int kSigUrg = 23;
constexpr int kSigWinch = 28;

constexpr uint64_t kHandlerDefault = 0;
constexpr uint64_t kHandlerIgnore = 1;

constexpr uint64_t kFlagSigInfo = 0x00000004;
constexpr uint64_t kFlagOnStack = 0x08000000;
constexpr uint64_t kFlagNoDefer = 0x40000000;
constexpr uint64_t kFlagResetHand = 0x80000000;

constexpr uint32_t kStackDisabled = 2;
constexpr uint64_t kRedZone = 128;
constexpr uint64_t kUserFlagsMask = 0x00CD5;
constexpr size_t kMxcsrOffset = 24;

struct SignalInfo {
    int32_t number;
    int32_t error;
    int32_t code;
    int32_t padding;
    uint64_t address;
    uint8_t reserved[128 - 24];
};

struct MachineContext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint64_t csgsfs, error_code, trap_number, old_mask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct UserContext {
    uint64_t flags;
    uint64_t link;
    uint64_t stack_base;
    uint32_t stack_flags;
    uint32_t padding;
    uint64_t stack_size;
    MachineContext machine;
    uint64_t signal_mask;
};

struct SignalFrame {
    uint64_t return_address;
    UserContext context;
    SignalInfo info;
};

uint64_t bit_of(int signal) { return 1ULL << (signal - 1); }

bool ignored_by_default(int signal) {
    return signal == kSigChld || signal == kSigCont || signal == kSigUrg || signal == kSigWinch;
}

[[noreturn]] void terminate(int signal, uint64_t address, const InterruptFrame* frame) {
    log("fatal signal %d in thread %d: rip=%p rsp=%p address=%p error=%lx\n", signal, current()->tid, frame->rip,
        frame->rsp, address, frame->error_code);
    process_exit(128 + signal);
}

void push_frame(int signal, uint64_t address, const SignalAction& action, InterruptFrame* frame) {
    Task* self = current();
    uint64_t stack = frame->rsp;
    const bool on_alternate = self->alt_stack_base && stack >= self->alt_stack_base &&
                              stack < self->alt_stack_base + self->alt_stack_size;
    if ((action.flags & kFlagOnStack) && self->alt_stack_base && !on_alternate &&
        !(self->alt_stack_flags & kStackDisabled)) {
        stack = self->alt_stack_base + self->alt_stack_size;
    } else {
        stack -= kRedZone;
    }

    stack = (stack - sizeof(self->fx_state)) & ~63ULL;
    const uint64_t fpstate = stack;
    stack = ((stack - sizeof(SignalFrame)) & ~15ULL) - 8;
    auto* user_frame = reinterpret_cast<SignalFrame*>(stack);
    if (!user_range_ok(user_frame, sizeof(SignalFrame), true) ||
        !user_range_ok(reinterpret_cast<void*>(fpstate), sizeof(self->fx_state), true)) {
        terminate(signal, address, frame);
    }

    save_fpu_state(self);
    memcpy(reinterpret_cast<void*>(fpstate), self->fx_state, sizeof(self->fx_state));

    SignalFrame out{};
    out.return_address = action.restorer;
    out.context.stack_base = self->alt_stack_base;
    out.context.stack_flags = self->alt_stack_flags;
    out.context.stack_size = self->alt_stack_size;
    MachineContext& machine = out.context.machine;
    machine.r8 = frame->r8;
    machine.r9 = frame->r9;
    machine.r10 = frame->r10;
    machine.r11 = frame->r11;
    machine.r12 = frame->r12;
    machine.r13 = frame->r13;
    machine.r14 = frame->r14;
    machine.r15 = frame->r15;
    machine.rdi = frame->rdi;
    machine.rsi = frame->rsi;
    machine.rbp = frame->rbp;
    machine.rbx = frame->rbx;
    machine.rdx = frame->rdx;
    machine.rax = frame->rax;
    machine.rcx = frame->rcx;
    machine.rsp = frame->rsp;
    machine.rip = frame->rip;
    machine.eflags = frame->rflags;
    machine.csgsfs = frame->cs;
    machine.error_code = frame->error_code;
    machine.trap_number = frame->vector;
    machine.old_mask = self->signal_mask;
    machine.cr2 = address;
    machine.fpstate = fpstate;
    out.context.signal_mask = self->signal_mask;
    out.info.number = signal;
    out.info.address = address;
    memcpy(user_frame, &out, sizeof(out));

    self->signal_mask |= action.mask;
    if (!(action.flags & kFlagNoDefer)) self->signal_mask |= bit_of(signal);

    frame->rip = action.handler;
    frame->rsp = stack;
    frame->rdi = static_cast<uint64_t>(signal);
    frame->rsi = reinterpret_cast<uint64_t>(&user_frame->info);
    frame->rdx = reinterpret_cast<uint64_t>(&user_frame->context);
    frame->rax = 0;
    frame->rflags &= ~0x400ULL;
}

void dispatch(int signal, uint64_t address, InterruptFrame* frame) {
    Process* process = current()->process;
    SignalAction& action = process->signal_actions[signal];
    if (action.handler == kHandlerIgnore) return;
    if (action.handler == kHandlerDefault) {
        if (ignored_by_default(signal)) return;
        terminate(signal, address, frame);
    }
    const SignalAction snapshot = action;
    if (action.flags & kFlagResetHand) action.handler = kHandlerDefault;
    push_frame(signal, address, snapshot, frame);
}

}

void signal_deliver_pending(InterruptFrame* frame) {
    Task* self = current();
    const uint64_t ready = self->signal_pending & ~self->signal_mask;
    if (!ready) return;
    const int signal = __builtin_ctzll(ready) + 1;
    self->signal_pending &= ~bit_of(signal);
    dispatch(signal, 0, frame);
}

void signal_force(int signal, uint64_t fault_address, InterruptFrame* frame) {
    Task* self = current();
    if (self->signal_mask & bit_of(signal)) terminate(signal, fault_address, frame);
    dispatch(signal, fault_address, frame);
}

int signal_send(Task* task, int signal) {
    if (signal < 0 || signal >= kSignalCount) return -kEINVAL;
    if (!signal) return 0;
    task->signal_pending |= bit_of(signal);
    if (task->state == TaskState::kBlocked && !(task->signal_mask & bit_of(signal))) wake_task(task, -kEINTR);
    return 0;
}

int signal_send_process(Process* process, int signal) {
    for (Task* task = process->threads; task; task = task->process_next) {
        if (signal && (task->signal_mask & bit_of(signal))) continue;
        return signal_send(task, signal);
    }
    return signal_send(process->threads, signal);
}

void signal_return(InterruptFrame* frame) {
    Task* self = current();
    const uint64_t context_address = frame->rsp;
    UserContext context;
    if (!copy_from_user(&context, reinterpret_cast<void*>(context_address), sizeof(context))) {
        terminate(11, context_address, frame);
    }
    const MachineContext& machine = context.machine;
    frame->r8 = machine.r8;
    frame->r9 = machine.r9;
    frame->r10 = machine.r10;
    frame->r11 = machine.r11;
    frame->r12 = machine.r12;
    frame->r13 = machine.r13;
    frame->r14 = machine.r14;
    frame->r15 = machine.r15;
    frame->rdi = machine.rdi;
    frame->rsi = machine.rsi;
    frame->rbp = machine.rbp;
    frame->rbx = machine.rbx;
    frame->rdx = machine.rdx;
    frame->rax = machine.rax;
    frame->rcx = machine.rcx;
    frame->rsp = machine.rsp;
    frame->rip = machine.rip;
    frame->rflags = (frame->rflags & ~kUserFlagsMask) | (machine.eflags & kUserFlagsMask);
    self->signal_mask = context.signal_mask;

    if (machine.fpstate && copy_from_user(self->fx_state, reinterpret_cast<void*>(machine.fpstate), sizeof(self->fx_state))) {
        auto* mxcsr = reinterpret_cast<uint32_t*>(self->fx_state + kMxcsrOffset);
        *mxcsr &= 0xFFFF;
        asm volatile("fxrstor64 %0" : : "m"(self->fx_state));
    }
}

}
