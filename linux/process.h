#pragma once

#include "base.h"
#include "mm.h"
#include "sched.h"
#include "vfs.h"

namespace lx {

constexpr int kMaxFiles = 1024;
constexpr int kSignalCount = 65;

struct SignalAction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct Process {
    int pid;
    AddressSpace* address_space;
    File* files[kMaxFiles];
    Node* cwd;
    uint32_t umask;
    int thread_count;
    Task* threads;
    SignalAction signal_actions[kSignalCount];
    char executable[256];
    bool exiting;
};

Process* process_current_instance();
int process_spawn(const char* path, const char* const* argv, const char* const* envp);
[[noreturn]] void process_exit(int code);
[[noreturn]] void thread_exit(int code);
int64_t thread_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls,
                     InterruptFrame* frame);

int fd_install(Process* process, File* file, int minimum = 0);
File* fd_get(Process* process, int fd);
int fd_close(Process* process, int fd);

extern bool syscall_trace_enabled;
int64_t linux_syscall(InterruptFrame* frame);

int futex_call(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2, uint32_t value3);
void futex_wake_address(uint64_t address, int count);

void signal_deliver_pending(InterruptFrame* frame);
void signal_force(int signal, uint64_t fault_address, InterruptFrame* frame);
int signal_send(Task* task, int signal);
int signal_send_process(Process* process, int signal);
void signal_return(InterruptFrame* frame);

bool exec_load(Process* process, const char* path, const char* const* argv, const char* const* envp,
               InterruptFrame* frame);

}
