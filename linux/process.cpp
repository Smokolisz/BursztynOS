#include "process.h"

#include "display.h"
#include "errno.h"

namespace lx {

namespace {

constexpr uint64_t kCloneVm = 0x00000100;
constexpr uint64_t kCloneThread = 0x00010000;
constexpr uint64_t kCloneSetTls = 0x00080000;
constexpr uint64_t kCloneParentSetTid = 0x00100000;
constexpr uint64_t kCloneChildClearTid = 0x00200000;
constexpr uint64_t kCloneChildSetTid = 0x01000000;

constexpr int kFirstPid = 1000;

constexpr uint32_t kElfLoad = 1;
constexpr uint32_t kElfInterp = 3;
constexpr uint16_t kElfTypeDynamic = 3;
constexpr uint64_t kInterpreterHint = 0x00007FFFE0000000ULL;

constexpr uint64_t kAuxNull = 0;
constexpr uint64_t kAuxPhdr = 3;
constexpr uint64_t kAuxPhent = 4;
constexpr uint64_t kAuxPhnum = 5;
constexpr uint64_t kAuxPagesz = 6;
constexpr uint64_t kAuxBase = 7;
constexpr uint64_t kAuxFlags = 8;
constexpr uint64_t kAuxEntry = 9;
constexpr uint64_t kAuxUid = 11;
constexpr uint64_t kAuxEuid = 12;
constexpr uint64_t kAuxGid = 13;
constexpr uint64_t kAuxEgid = 14;
constexpr uint64_t kAuxPlatform = 15;
constexpr uint64_t kAuxHwcap = 16;
constexpr uint64_t kAuxClktck = 17;
constexpr uint64_t kAuxSecure = 23;
constexpr uint64_t kAuxRandom = 25;
constexpr uint64_t kAuxExecfn = 31;

constexpr uint64_t kUserId = 1000;

struct ElfHeader {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ElfProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct LoadedElf {
    uint64_t base;
    uint64_t entry;
    uint64_t program_headers;
    uint64_t program_header_count;
    uint64_t image_end;
    const char* interpreter;
};

struct SpawnRequest {
    Process* process;
    const char* path;
    const char* const* argv;
    const char* const* envp;
};

Process* active_process;

bool load_elf(Process* process, Node* node, uint64_t hint, LoadedElf* out) {
    if (!node->image_phys || node->size < sizeof(ElfHeader)) return false;
    auto* data = static_cast<const uint8_t*>(phys_to_virt(node->image_phys));
    auto* header = reinterpret_cast<const ElfHeader*>(data);
    if (memcmp(header->ident, "\x7F" "ELF\x02\x01", 6)) return false;
    auto* headers = reinterpret_cast<const ElfProgramHeader*>(data + header->phoff);

    uint64_t lowest = ~0ULL;
    uint64_t highest = 0;
    out->interpreter = nullptr;
    for (int i = 0; i < header->phnum; ++i) {
        if (headers[i].type == kElfInterp) out->interpreter = reinterpret_cast<const char*>(data + headers[i].offset);
        if (headers[i].type != kElfLoad) continue;
        lowest = min(lowest, page_floor(headers[i].vaddr));
        highest = max(highest, page_ceil(headers[i].vaddr + headers[i].memsz));
    }
    if (highest <= lowest) return false;

    AddressSpace* space = process->address_space;
    uint64_t base = 0;
    if (header->type == kElfTypeDynamic) {
        const int64_t reserved = space->map(hint, highest - lowest, 0, kMapPrivate | kMapAnonymous, nullptr, 0);
        if (reserved < 0) return false;
        base = static_cast<uint64_t>(reserved) - lowest;
    }

    for (int i = 0; i < header->phnum; ++i) {
        const ElfProgramHeader& segment = headers[i];
        if (segment.type != kElfLoad) continue;
        uint32_t prot = 0;
        if (segment.flags & 4) prot |= kProtRead;
        if (segment.flags & 2) prot |= kProtWrite;
        if (segment.flags & 1) prot |= kProtExec;
        const uint64_t address = base + segment.vaddr;
        const uint64_t page_start = page_floor(address);
        const uint64_t delta = address - page_start;
        const uint64_t file_end = page_ceil(address + segment.filesz);
        const uint64_t memory_end = page_ceil(address + segment.memsz);
        if (segment.filesz) {
            const int64_t status = space->map(page_start, file_end - page_start, prot, kMapPrivate | kMapFixed, node,
                                              segment.offset - delta);
            if (status < 0) return false;
        }
        const uint64_t anonymous_start = segment.filesz ? file_end : page_start;
        if (memory_end > anonymous_start) {
            const int64_t status = space->map(anonymous_start, memory_end - anonymous_start, prot,
                                              kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0);
            if (status < 0) return false;
        }
        if (segment.memsz > segment.filesz && (prot & kProtWrite) && segment.filesz) {
            const uint64_t clear_start = address + segment.filesz;
            memset(reinterpret_cast<void*>(clear_start), 0, page_ceil(clear_start) - clear_start);
        }
    }

    out->base = base;
    out->entry = base + header->entry;
    out->program_headers = base + lowest + header->phoff;
    out->program_header_count = header->phnum;
    out->image_end = base + highest;
    return true;
}

uint64_t push_string(uint64_t* stack, const char* text) {
    const size_t length = string_length(text) + 1;
    *stack -= length;
    memcpy(reinterpret_cast<void*>(*stack), text, length);
    return *stack;
}

void process_main(void* raw) {
    auto* request = static_cast<SpawnRequest*>(raw);
    const SpawnRequest copy = *request;
    kernel_free(request);
    if (!exec_load(copy.process, copy.path, copy.argv, copy.envp, current()->user_frame)) {
        log("exec: failed to load %s\n", copy.path);
        process_exit(127);
    }
}

}

bool exec_load(Process* process, const char* path, const char* const* argv, const char* const* envp,
               InterruptFrame* frame) {
    Node* node;
    if (vfs_resolve(vfs_root(), path, true, &node) < 0) return false;
    LoadedElf program{};
    if (!load_elf(process, node, 0, &program)) return false;

    LoadedElf interpreter{};
    if (program.interpreter) {
        Node* interpreter_node;
        if (vfs_resolve(vfs_root(), program.interpreter, true, &interpreter_node) < 0) return false;
        if (!load_elf(process, interpreter_node, kInterpreterHint, &interpreter)) return false;
    }

    AddressSpace* space = process->address_space;
    space->init_break(program.image_end);
    const int64_t stack_status = space->map(kUserStackTop - kUserStackSize, kUserStackSize, kProtRead | kProtWrite,
                                            kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0);
    if (stack_status < 0) return false;

    int argc = 0;
    int envc = 0;
    while (argv[argc]) ++argc;
    while (envp[envc]) ++envc;

    uint64_t cursor = kUserStackTop;
    const uint64_t execfn = push_string(&cursor, path);
    const uint64_t platform = push_string(&cursor, "x86_64");
    cursor = (cursor - 16) & ~15ULL;
    const uint64_t random_bytes = cursor;
    const uint64_t seed[2] = {read_tsc() * 0x9E3779B97F4A7C15ULL, read_tsc() ^ 0xD6E8FEB86659FD93ULL};
    memcpy(reinterpret_cast<void*>(random_bytes), seed, sizeof(seed));

    uint64_t* env_pointers = static_cast<uint64_t*>(kernel_alloc((envc + argc + 2) * sizeof(uint64_t)));
    uint64_t* arg_pointers = env_pointers + envc + 1;
    for (int i = envc - 1; i >= 0; --i) env_pointers[i] = push_string(&cursor, envp[i]);
    for (int i = argc - 1; i >= 0; --i) arg_pointers[i] = push_string(&cursor, argv[i]);

    uint32_t cpuid_eax = 1, cpuid_ebx, cpuid_ecx = 0, cpuid_edx;
    asm volatile("cpuid" : "+a"(cpuid_eax), "=b"(cpuid_ebx), "+c"(cpuid_ecx), "=d"(cpuid_edx));

    const uint64_t auxv[][2] = {
        {kAuxPhdr, program.program_headers},
        {kAuxPhent, sizeof(ElfProgramHeader)},
        {kAuxPhnum, program.program_header_count},
        {kAuxPagesz, kPageSize},
        {kAuxBase, interpreter.base},
        {kAuxFlags, 0},
        {kAuxEntry, program.entry},
        {kAuxUid, kUserId},
        {kAuxEuid, kUserId},
        {kAuxGid, kUserId},
        {kAuxEgid, kUserId},
        {kAuxPlatform, platform},
        {kAuxHwcap, cpuid_edx},
        {kAuxClktck, 100},
        {kAuxSecure, 0},
        {kAuxRandom, random_bytes},
        {kAuxExecfn, execfn},
        {kAuxNull, 0},
    };

    const uint64_t words = 1 + (argc + 1) + (envc + 1) + sizeof(auxv) / sizeof(uint64_t);
    cursor = (cursor - words * sizeof(uint64_t)) & ~15ULL;
    auto* out = reinterpret_cast<uint64_t*>(cursor);
    *out++ = argc;
    for (int i = 0; i < argc; ++i) *out++ = arg_pointers[i];
    *out++ = 0;
    for (int i = 0; i < envc; ++i) *out++ = env_pointers[i];
    *out++ = 0;
    memcpy(out, auxv, sizeof(auxv));
    kernel_free(env_pointers);

    format(process->executable, sizeof(process->executable), "%s", path);

    memset(frame, 0, sizeof(*frame));
    frame->rip = program.interpreter ? interpreter.entry : program.entry;
    frame->rsp = cursor;
    frame->cs = 0x23;
    frame->ss = 0x1B;
    frame->rflags = 0x202;
    return true;
}

Process* process_current_instance() { return active_process; }

int process_spawn(const char* path, const char* const* argv, const char* const* envp) {
    if (active_process) return -kEBUSY;
    if (!vfs_has_image()) return -kENOENT;
    auto* process = kernel_new<Process>();
    process->pid = kFirstPid;
    process->address_space = AddressSpace::create(kernel_cr3());
    process->cwd = vfs_root();
    node_retain(process->cwd);
    process->umask = 022;
    process->thread_count = 1;

    Node* console;
    if (vfs_resolve(vfs_root(), "/dev/console", true, &console) == 0) {
        fd_install(process, file_open(console, 0));
        fd_install(process, file_open(console, kOpenWriteOnly));
        fd_install(process, file_open(console, kOpenWriteOnly));
    }

    auto* request = kernel_new<SpawnRequest>();
    *request = SpawnRequest{process, path, argv, envp};
    active_process = process;
    process->threads = spawn_process_task(process, process_main, request, process->address_space->cr3());
    process->threads->tid = process->pid;
    return process->pid;
}

void process_exit(int code) {
    Process* process = current()->process;
    Task* self = current();
    log("process %d exited with status %d\n", process->pid, code);
    process->exiting = true;
    for (Task* task = process->threads; task;) {
        Task* next = task->process_next;
        if (task != self) retire_task(task);
        task = next;
    }
    display_on_process_exit();
    self->cr3 = kernel_cr3();
    write_cr3(self->cr3);
    self->process = nullptr;
    process->address_space->destroy();
    for (int fd = 0; fd < kMaxFiles; ++fd) {
        if (process->files[fd]) file_release(process->files[fd]);
    }
    node_release(process->cwd);
    kernel_free(process);
    active_process = nullptr;
    poll_notify();
    exit_current_task();
}

void thread_exit(int code) {
    Task* self = current();
    Process* process = self->process;
    if (process->thread_count == 1) process_exit(code);
    if (self->clear_child_tid && user_range_ok(reinterpret_cast<void*>(self->clear_child_tid), 4, true)) {
        *reinterpret_cast<uint32_t*>(self->clear_child_tid) = 0;
        futex_wake_address(self->clear_child_tid, 1);
    }
    for (Task** link = &process->threads; *link; link = &(*link)->process_next) {
        if (*link == self) {
            *link = self->process_next;
            break;
        }
    }
    --process->thread_count;
    exit_current_task();
}

int64_t thread_clone(uint64_t flags, uint64_t child_stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls,
                     InterruptFrame* frame) {
    if (!(flags & kCloneVm) || !(flags & kCloneThread)) return -kEAGAIN;
    Task* self = current();
    Process* process = self->process;
    InterruptFrame child_frame = *frame;
    child_frame.rax = 0;
    if (child_stack) child_frame.rsp = child_stack;
    Task* child = spawn_user_thread(process, child_frame, process->address_space->cr3());
    save_fpu_state(self);
    memcpy(child->fx_state, self->fx_state, sizeof(self->fx_state));
    child->fs_base = (flags & kCloneSetTls) ? tls : self->fs_base;
    child->gs_base = self->gs_base;
    child->signal_mask = self->signal_mask;
    if (flags & kCloneChildClearTid) child->clear_child_tid = child_tid;
    const uint32_t tid = static_cast<uint32_t>(child->tid);
    if (flags & kCloneParentSetTid) copy_to_user(reinterpret_cast<void*>(parent_tid), &tid, sizeof(tid));
    if (flags & kCloneChildSetTid) copy_to_user(reinterpret_cast<void*>(child_tid), &tid, sizeof(tid));
    child->process_next = process->threads;
    process->threads = child;
    ++process->thread_count;
    return child->tid;
}

int fd_install(Process* process, File* file, int minimum) {
    for (int fd = minimum; fd < kMaxFiles; ++fd) {
        if (!process->files[fd]) {
            process->files[fd] = file;
            return fd;
        }
    }
    return -kEMFILE;
}

File* fd_get(Process* process, int fd) {
    if (fd < 0 || fd >= kMaxFiles) return nullptr;
    return process->files[fd];
}

int fd_close(Process* process, int fd) {
    File* file = fd_get(process, fd);
    if (!file) return -kEBADF;
    process->files[fd] = nullptr;
    file_release(file);
    return 0;
}

}
