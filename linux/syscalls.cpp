#include "errno.h"
#include "process.h"

namespace lx {

namespace {

constexpr int kAtFdCwd = -100;
constexpr uint32_t kAtSymlinkNoFollow = 0x100;
constexpr uint32_t kAtRemoveDir = 0x200;
constexpr uint32_t kAtEmptyPath = 0x1000;
constexpr size_t kPathCapacity = 1024;

constexpr uint64_t kIoctlFionRead = 0x541B;

constexpr uint64_t kArchSetGs = 0x1001;
constexpr uint64_t kArchSetFs = 0x1002;
constexpr uint64_t kArchGetFs = 0x1003;
constexpr uint64_t kArchGetGs = 0x1004;
constexpr uint32_t kMsrFsBase = 0xC0000100;
constexpr uint32_t kMsrGsBase = 0xC0000101;

constexpr int kFcntlDupFd = 0;
constexpr int kFcntlGetFd = 1;
constexpr int kFcntlSetFd = 2;
constexpr int kFcntlGetFl = 3;
constexpr int kFcntlSetFl = 4;
constexpr int kFcntlDupFdCloexec = 1030;

constexpr int kSeekSet = 0;
constexpr int kSeekCur = 1;
constexpr int kSeekEnd = 2;

constexpr int kSigSetMaskBlock = 0;
constexpr int kSigSetMaskUnblock = 1;
constexpr int kSigSetMaskSet = 2;
constexpr int kClockRealtime = 0;
constexpr int kClockRealtimeCoarse = 5;
constexpr int kClockTai = 11;
constexpr int kTimerAbstime = 1;

constexpr int kMadviseDontNeed = 4;
constexpr int kMadviseFree = 8;

constexpr uint64_t kUserId = 1000;
constexpr uint64_t kInfinity = ~0ULL;
constexpr uint64_t kTmpfsMagic = 0x01021994;

struct Timespec {
    int64_t seconds;
    int64_t nanoseconds;
};

struct Timeval {
    int64_t seconds;
    int64_t microseconds;
};

struct KernelStat {
    uint64_t device;
    uint64_t inode;
    uint64_t link_count;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t padding0;
    uint64_t raw_device;
    int64_t size;
    int64_t block_size;
    int64_t blocks;
    Timespec access_time;
    Timespec modify_time;
    Timespec change_time;
    int64_t reserved[3];
};

struct StatxTimestamp {
    int64_t seconds;
    uint32_t nanoseconds;
    int32_t reserved;
};

struct Statx {
    uint32_t mask;
    uint32_t block_size;
    uint64_t attributes;
    uint32_t link_count;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t padding0;
    uint64_t inode;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    StatxTimestamp access_time;
    StatxTimestamp birth_time;
    StatxTimestamp change_time;
    StatxTimestamp modify_time;
    uint32_t raw_device_major;
    uint32_t raw_device_minor;
    uint32_t device_major;
    uint32_t device_minor;
    uint64_t reserved[14];
};

struct StatFs {
    int64_t type;
    int64_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t blocks_available;
    uint64_t files;
    uint64_t files_free;
    int32_t fsid[2];
    int64_t name_length;
    int64_t fragment_size;
    int64_t flags;
    int64_t spare[4];
};

struct SystemInfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t shared_ram;
    uint64_t buffer_ram;
    uint64_t total_swap;
    uint64_t free_swap;
    uint16_t process_count;
    uint16_t padding;
    uint64_t total_high;
    uint64_t free_high;
    uint32_t memory_unit;
    char reserved[4];
};

struct ResourceLimit {
    uint64_t current;
    uint64_t maximum;
};

struct IoVector {
    void* base;
    uint64_t length;
};

struct PollEntry {
    int32_t fd;
    int16_t events;
    int16_t returned;
};

struct KernelSigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct AltStack {
    uint64_t base;
    int32_t flags;
    int32_t padding;
    uint64_t size;
};

struct Utsname {
    char system[65];
    char node[65];
    char release[65];
    char version[65];
    char machine[65];
    char domain[65];
};

Process* self_process() { return current()->process; }

Node* base_directory(int directory_fd) {
    Process* process = self_process();
    if (directory_fd == kAtFdCwd) return process->cwd;
    File* file = fd_get(process, directory_fd);
    return file ? file->node : nullptr;
}

uint64_t deadline_from_timespec(const Timespec& spec) {
    return now_ns() + static_cast<uint64_t>(spec.seconds) * 1000000000ULL + static_cast<uint64_t>(spec.nanoseconds) + 1;
}

uint32_t mode_of(const Node* node) {
    switch (node->kind) {
        case NodeKind::kDirectory: return kModeDirectory | (node->mode & 07777);
        case NodeKind::kFile: return kModeRegular | (node->mode & 07777);
        case NodeKind::kSymlink: return kModeSymlink | 0777;
        case NodeKind::kDevice:
            return (node->mode & kModeTypeMask) ? node->mode : (kModeCharDevice | (node->mode & 07777));
    }
    return 0;
}

Timespec timespec_from_ns(uint64_t value) {
    return Timespec{static_cast<int64_t>(value / 1000000000ULL), static_cast<int64_t>(value % 1000000000ULL)};
}

int fill_stat(const Node* node, void* user_buffer) {
    KernelStat out{};
    out.device = 1;
    out.inode = node->inode;
    out.link_count = max<uint32_t>(node->links, 1);
    out.mode = mode_of(node);
    out.uid = kUserId;
    out.gid = kUserId;
    out.size = static_cast<int64_t>(node->kind == NodeKind::kDevice ? node->device_size : node->size);
    out.block_size = kPageSize;
    out.blocks = (out.size + 511) / 512;
    out.access_time = out.modify_time = out.change_time = timespec_from_ns(node->modified_ns);
    return copy_to_user(user_buffer, &out, sizeof(out)) ? 0 : -kEFAULT;
}

int resolve_user_path(int directory_fd, const char* user_path, bool follow, Node** result) {
    char path[kPathCapacity];
    const int length = copy_string_from_user(path, user_path, sizeof(path));
    if (length < 0) return length;
    Node* base = base_directory(directory_fd);
    if (!base) return -kEBADF;
    return vfs_resolve(base, path, follow, result);
}

int64_t sys_read(int fd, void* buffer, uint64_t length) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (!user_range_ok(buffer, length, true)) return -kEFAULT;
    return file_read(file, buffer, length);
}

int64_t sys_write(int fd, const void* buffer, uint64_t length) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (!user_range_ok(buffer, length, false)) return -kEFAULT;
    return file_write(file, buffer, length);
}

int64_t sys_pread(int fd, void* buffer, uint64_t length, uint64_t offset) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (file->node->device) return -kESPIPE;
    if (!user_range_ok(buffer, length, true)) return -kEFAULT;
    return node_read(file->node, buffer, length, offset);
}

int64_t sys_pwrite(int fd, const void* buffer, uint64_t length, uint64_t offset) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (file->node->device) return -kESPIPE;
    if (!user_range_ok(buffer, length, false)) return -kEFAULT;
    return node_write(file->node, buffer, length, offset);
}

int64_t sys_vector_io(int fd, const IoVector* user_vectors, int count, bool write) {
    int64_t total = 0;
    for (int i = 0; i < count; ++i) {
        IoVector vector;
        if (!copy_from_user(&vector, user_vectors + i, sizeof(vector))) return total ? total : -kEFAULT;
        if (!vector.length) continue;
        const int64_t status =
            write ? sys_write(fd, vector.base, vector.length) : sys_read(fd, vector.base, vector.length);
        if (status < 0) return total ? total : status;
        total += status;
        if (static_cast<uint64_t>(status) < vector.length) break;
    }
    return total;
}

int64_t sys_openat(int directory_fd, const char* user_path, uint32_t flags, uint32_t mode) {
    Process* process = self_process();
    char path[kPathCapacity];
    const int path_length = copy_string_from_user(path, user_path, sizeof(path));
    if (path_length < 0) return path_length;
    Node* base = base_directory(directory_fd);
    if (!base) return -kEBADF;

    Node* node = nullptr;
    int status = vfs_resolve(base, path, !(flags & kOpenNoFollow), &node);
    if (status == 0 && (flags & kOpenCreate) && (flags & kOpenExclusive)) return -kEEXIST;
    if (status == -kENOENT && (flags & kOpenCreate)) {
        Node* parent;
        const char* leaf;
        size_t leaf_length;
        status = vfs_resolve_parent(base, path, &parent, &leaf, &leaf_length);
        if (status < 0) return status;
        node = vfs_create_child(parent, leaf, leaf_length, NodeKind::kFile, mode & ~process->umask & 07777);
    } else if (status < 0) {
        return status;
    }

    if ((flags & kOpenDirectory) && node->kind != NodeKind::kDirectory) return -kENOTDIR;
    const bool wants_write = (flags & kOpenAccessMask) != 0;
    if (node->kind == NodeKind::kDirectory && wants_write) return -kEISDIR;
    if ((flags & kOpenTruncate) && wants_write && node->kind == NodeKind::kFile) {
        status = node_truncate(node, 0);
        if (status < 0) return status;
    }
    File* file = file_open(node, flags);
    const int fd = fd_install(process, file);
    if (fd < 0) file_release(file);
    return fd;
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (file->node->device) return -kESPIPE;
    int64_t base;
    switch (whence) {
        case kSeekSet: base = 0; break;
        case kSeekCur: base = static_cast<int64_t>(file->offset); break;
        case kSeekEnd: base = static_cast<int64_t>(file->node->size); break;
        default: return -kEINVAL;
    }
    if (base + offset < 0) return -kEINVAL;
    file->offset = static_cast<uint64_t>(base + offset);
    return base + offset;
}

int64_t sys_stat_path(int directory_fd, const char* user_path, void* buffer, uint32_t flags) {
    Node* node;
    if (flags & kAtEmptyPath) {
        char first = 0;
        if (user_path && !copy_from_user(&first, user_path, 1)) return -kEFAULT;
        if (!first) {
            Node* base = base_directory(directory_fd);
            return base ? fill_stat(base, buffer) : -kEBADF;
        }
    }
    const int status = resolve_user_path(directory_fd, user_path, !(flags & kAtSymlinkNoFollow), &node);
    return status < 0 ? status : fill_stat(node, buffer);
}

int64_t sys_statx(int directory_fd, const char* user_path, uint32_t flags, void* buffer) {
    Node* node = nullptr;
    char first = 0;
    if (user_path && !copy_from_user(&first, user_path, 1)) return -kEFAULT;
    if ((flags & kAtEmptyPath) && !first) {
        node = base_directory(directory_fd);
        if (!node) return -kEBADF;
    } else {
        const int status = resolve_user_path(directory_fd, user_path, !(flags & kAtSymlinkNoFollow), &node);
        if (status < 0) return status;
    }
    constexpr uint32_t kStatxBasicStats = 0x07FF;
    Statx out{};
    out.mask = kStatxBasicStats;
    out.block_size = kPageSize;
    out.link_count = max<uint32_t>(node->links, 1);
    out.uid = kUserId;
    out.gid = kUserId;
    out.mode = static_cast<uint16_t>(mode_of(node));
    out.inode = node->inode;
    out.size = node->kind == NodeKind::kDevice ? node->device_size : node->size;
    out.blocks = (out.size + 511) / 512;
    const Timespec modified = timespec_from_ns(node->modified_ns);
    out.access_time.seconds = out.change_time.seconds = out.modify_time.seconds = modified.seconds;
    out.device_minor = 1;
    return copy_to_user(buffer, &out, sizeof(out)) ? 0 : -kEFAULT;
}

int64_t sys_access(int directory_fd, const char* user_path) {
    Node* node;
    return resolve_user_path(directory_fd, user_path, true, &node);
}

int64_t sys_readlink(int directory_fd, const char* user_path, char* buffer, uint64_t capacity) {
    Node* node;
    const int status = resolve_user_path(directory_fd, user_path, false, &node);
    if (status < 0) return status;
    if (node->kind != NodeKind::kSymlink) return -kEINVAL;
    const uint64_t count = min(capacity, node->size);
    return copy_to_user(buffer, node->link_target, count) ? static_cast<int64_t>(count) : -kEFAULT;
}

int64_t sys_getdents(int fd, void* user_buffer, uint64_t capacity) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    Node* directory = file->node;
    if (directory->kind != NodeKind::kDirectory) return -kENOTDIR;
    if (!user_range_ok(user_buffer, capacity, true)) return -kEFAULT;
    auto* out = static_cast<uint8_t*>(user_buffer);
    uint64_t written = 0;
    uint64_t position = file->offset;

    Node* child = directory->first_child;
    for (uint64_t skip = 2; skip < position && child; ++skip) child = child->next_sibling;

    for (;;) {
        const char* name;
        uint64_t inode;
        uint8_t type;
        if (position == 0) {
            name = ".";
            inode = directory->inode;
            type = 4;
        } else if (position == 1) {
            name = "..";
            inode = directory->parent ? directory->parent->inode : directory->inode;
            type = 4;
        } else {
            if (!child) break;
            name = child->name;
            inode = child->inode;
            switch (child->kind) {
                case NodeKind::kDirectory: type = 4; break;
                case NodeKind::kFile: type = 8; break;
                case NodeKind::kSymlink: type = 10; break;
                default: type = 2; break;
            }
        }
        const size_t name_length = string_length(name);
        const uint64_t record_length = (19 + name_length + 1 + 7) & ~7ULL;
        if (written + record_length > capacity) {
            if (!written) return -kEINVAL;
            break;
        }
        uint8_t* record = out + written;
        *reinterpret_cast<uint64_t*>(record) = inode;
        *reinterpret_cast<int64_t*>(record + 8) = static_cast<int64_t>(position + 1);
        *reinterpret_cast<uint16_t*>(record + 16) = static_cast<uint16_t>(record_length);
        record[18] = type;
        memcpy(record + 19, name, name_length + 1);
        written += record_length;
        if (position >= 2) child = child->next_sibling;
        ++position;
    }
    file->offset = position;
    return static_cast<int64_t>(written);
}

int64_t sys_mkdir(int directory_fd, const char* user_path, uint32_t mode) {
    char path[kPathCapacity];
    const int path_length = copy_string_from_user(path, user_path, sizeof(path));
    if (path_length < 0) return path_length;
    Node* base = base_directory(directory_fd);
    if (!base) return -kEBADF;
    Node* existing;
    if (vfs_resolve(base, path, true, &existing) == 0) return -kEEXIST;
    Node* parent;
    const char* leaf;
    size_t leaf_length;
    const int status = vfs_resolve_parent(base, path, &parent, &leaf, &leaf_length);
    if (status < 0) return status;
    vfs_create_child(parent, leaf, leaf_length, NodeKind::kDirectory, mode & ~self_process()->umask & 07777);
    return 0;
}

int64_t sys_unlink(int directory_fd, const char* user_path, uint32_t flags) {
    Node* node;
    const int status = resolve_user_path(directory_fd, user_path, false, &node);
    if (status < 0) return status;
    const bool want_directory = flags & kAtRemoveDir;
    if (node->kind == NodeKind::kDirectory && !want_directory) return -kEISDIR;
    if (node->kind != NodeKind::kDirectory && want_directory) return -kENOTDIR;
    return vfs_unlink(node);
}

int64_t sys_rename(int old_fd, const char* user_old, int new_fd, const char* user_new) {
    Node* node;
    int status = resolve_user_path(old_fd, user_old, false, &node);
    if (status < 0) return status;
    char path[kPathCapacity];
    const int path_length = copy_string_from_user(path, user_new, sizeof(path));
    if (path_length < 0) return path_length;
    Node* base = base_directory(new_fd);
    if (!base) return -kEBADF;
    Node* parent;
    const char* leaf;
    size_t leaf_length;
    status = vfs_resolve_parent(base, path, &parent, &leaf, &leaf_length);
    if (status < 0) return status;
    return vfs_rename(node, parent, leaf, leaf_length);
}

int64_t sys_symlink(const char* user_target, int directory_fd, const char* user_path) {
    char target[kPathCapacity];
    const int target_length = copy_string_from_user(target, user_target, sizeof(target));
    if (target_length < 0) return target_length;
    char path[kPathCapacity];
    const int path_length = copy_string_from_user(path, user_path, sizeof(path));
    if (path_length < 0) return path_length;
    Node* base = base_directory(directory_fd);
    if (!base) return -kEBADF;
    Node* parent;
    const char* leaf;
    size_t leaf_length;
    const int status = vfs_resolve_parent(base, path, &parent, &leaf, &leaf_length);
    if (status < 0) return status;
    if (vfs_lookup_child(parent, leaf, leaf_length)) return -kEEXIST;
    Node* node = vfs_create_child(parent, leaf, leaf_length, NodeKind::kSymlink, 0777);
    node->link_target = string_duplicate_n(target, target_length);
    node->size = target_length;
    return 0;
}

int64_t sys_truncate_path(const char* user_path, uint64_t size) {
    Node* node;
    const int status = resolve_user_path(kAtFdCwd, user_path, true, &node);
    return status < 0 ? status : node_truncate(node, size);
}

int64_t sys_ftruncate(int fd, uint64_t size) {
    File* file = fd_get(self_process(), fd);
    return file ? node_truncate(file->node, size) : -kEBADF;
}

int64_t sys_getcwd(char* buffer, uint64_t capacity) {
    char path[kPathCapacity];
    const size_t length = vfs_path_of(self_process()->cwd, path, sizeof(path));
    if (length + 1 > capacity) return -kERANGE;
    return copy_to_user(buffer, path, length + 1) ? static_cast<int64_t>(length + 1) : -kEFAULT;
}

int64_t set_cwd(Node* node) {
    if (node->kind != NodeKind::kDirectory) return -kENOTDIR;
    Process* process = self_process();
    node_retain(node);
    node_release(process->cwd);
    process->cwd = node;
    return 0;
}

int64_t sys_chdir(const char* user_path) {
    Node* node;
    const int status = resolve_user_path(kAtFdCwd, user_path, true, &node);
    return status < 0 ? status : set_cwd(node);
}

int64_t sys_fchdir(int fd) {
    File* file = fd_get(self_process(), fd);
    return file ? set_cwd(file->node) : -kEBADF;
}

int64_t sys_dup(int fd, int minimum) {
    Process* process = self_process();
    File* file = fd_get(process, fd);
    if (!file) return -kEBADF;
    const int result = fd_install(process, file, minimum);
    if (result >= 0) file_retain(file);
    return result;
}

int64_t sys_dup_to(int from, int to) {
    Process* process = self_process();
    File* file = fd_get(process, from);
    if (!file || to < 0 || to >= kMaxFiles) return -kEBADF;
    if (from == to) return to;
    file_retain(file);
    if (process->files[to]) file_release(process->files[to]);
    process->files[to] = file;
    return to;
}

int64_t sys_fcntl(int fd, int command, uint64_t argument) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    switch (command) {
        case kFcntlDupFd:
        case kFcntlDupFdCloexec:
            return sys_dup(fd, static_cast<int>(argument));
        case kFcntlGetFd:
        case kFcntlSetFd:
            return 0;
        case kFcntlGetFl:
            return file->flags;
        case kFcntlSetFl:
            file->flags = (file->flags & ~(kOpenNonBlocking | kOpenAppend)) |
                          (static_cast<uint32_t>(argument) & (kOpenNonBlocking | kOpenAppend));
            return 0;
        default:
            return 0;
    }
}

int64_t sys_ioctl(int fd, uint64_t command, uint64_t argument) {
    File* file = fd_get(self_process(), fd);
    if (!file) return -kEBADF;
    if (file->node->device && file->node->device->ioctl) return file->node->device->ioctl(file, command, argument);
    if (command == kIoctlFionRead) {
        const uint32_t available = static_cast<uint32_t>(file->node->size - min(file->node->size, file->offset));
        return copy_to_user(reinterpret_cast<void*>(argument), &available, sizeof(available)) ? 0 : -kEFAULT;
    }
    return -kENOTTY;
}

int64_t sys_pipe(int* user_fds) {
    Process* process = self_process();
    File* read_end;
    File* write_end;
    pipe_create(&read_end, &write_end);
    int fds[2];
    fds[0] = fd_install(process, read_end);
    fds[1] = fd_install(process, write_end);
    if (fds[0] < 0 || fds[1] < 0) return -kEMFILE;
    return copy_to_user(user_fds, fds, sizeof(fds)) ? 0 : -kEFAULT;
}

int scan_poll(PollEntry* entries, uint64_t count) {
    Process* process = self_process();
    int ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
        PollEntry& entry = entries[i];
        entry.returned = 0;
        if (entry.fd < 0) continue;
        File* file = fd_get(process, entry.fd);
        if (!file) {
            entry.returned = 0x020;
            ++ready;
            continue;
        }
        const uint32_t events = file_poll(file) & (static_cast<uint32_t>(entry.events) | kPollErr | kPollHangup);
        entry.returned = static_cast<int16_t>(events);
        if (events) ++ready;
    }
    return ready;
}

int64_t poll_common(PollEntry* user_entries, uint64_t count, bool has_deadline, uint64_t deadline) {
    if (count > kMaxFiles) return -kEINVAL;
    if (!user_range_ok(user_entries, count * sizeof(PollEntry), true)) return -kEFAULT;
    for (;;) {
        const int ready = scan_poll(user_entries, count);
        if (ready) return ready;
        if (has_deadline && now_ns() >= deadline) return 0;
        const int status = block_on(&poll_waiters, has_deadline ? deadline : 0);
        if (status == -kETIMEDOUT) return scan_poll(user_entries, count);
        if (status < 0) return status;
    }
}

int64_t sys_poll(PollEntry* entries, uint64_t count, int timeout_ms) {
    const bool has_deadline = timeout_ms >= 0;
    return poll_common(entries, count, has_deadline, has_deadline ? now_ns() + timeout_ms * 1000000ULL : 0);
}

int64_t sys_ppoll(PollEntry* entries, uint64_t count, const Timespec* user_timeout) {
    if (!user_timeout) return poll_common(entries, count, false, 0);
    Timespec timeout;
    if (!copy_from_user(&timeout, user_timeout, sizeof(timeout))) return -kEFAULT;
    return poll_common(entries, count, true, deadline_from_timespec(timeout));
}

int64_t select_common(int count, uint64_t* user_read, uint64_t* user_write, uint64_t* user_except, bool has_deadline,
                      uint64_t deadline) {
    constexpr int kWordBits = 64;
    constexpr int kSetWords = 1024 / kWordBits;
    if (count < 0 || count > 1024) return -kEINVAL;
    const uint64_t bytes = ((static_cast<uint64_t>(count) + kWordBits - 1) / kWordBits) * sizeof(uint64_t);
    uint64_t want_read[kSetWords] = {};
    uint64_t want_write[kSetWords] = {};
    if (user_read && !copy_from_user(want_read, user_read, bytes)) return -kEFAULT;
    if (user_write && !copy_from_user(want_write, user_write, bytes)) return -kEFAULT;
    if (user_except && !user_range_ok(user_except, bytes, true)) return -kEFAULT;

    Process* process = self_process();
    for (;;) {
        uint64_t got_read[kSetWords] = {};
        uint64_t got_write[kSetWords] = {};
        int ready = 0;
        for (int fd = 0; fd < count; ++fd) {
            const uint64_t bit = 1ULL << (fd % kWordBits);
            const int word = fd / kWordBits;
            const bool reads = want_read[word] & bit;
            const bool writes = want_write[word] & bit;
            if (!reads && !writes) continue;
            File* file = fd_get(process, fd);
            if (!file) return -kEBADF;
            const uint32_t events = file_poll(file);
            if (reads && (events & (kPollIn | kPollHangup))) {
                got_read[word] |= bit;
                ++ready;
            }
            if (writes && (events & (kPollOut | kPollErr))) {
                got_write[word] |= bit;
                ++ready;
            }
        }
        const bool expired = has_deadline && now_ns() >= deadline;
        if (ready || expired) {
            if (user_read) copy_to_user(user_read, got_read, bytes);
            if (user_write) copy_to_user(user_write, got_write, bytes);
            if (user_except) memset(user_except, 0, bytes);
            return ready;
        }
        const int status = block_on(&poll_waiters, has_deadline ? deadline : 0);
        if (status < 0 && status != -kETIMEDOUT) return status;
    }
}

int64_t sys_select(int count, uint64_t* read_set, uint64_t* write_set, uint64_t* except_set, const Timeval* user_timeout) {
    if (!user_timeout) return select_common(count, read_set, write_set, except_set, false, 0);
    Timeval timeout;
    if (!copy_from_user(&timeout, user_timeout, sizeof(timeout))) return -kEFAULT;
    const uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout.seconds) * 1000000000ULL +
                              static_cast<uint64_t>(timeout.microseconds) * 1000ULL;
    return select_common(count, read_set, write_set, except_set, true, deadline);
}

int64_t sys_pselect(int count, uint64_t* read_set, uint64_t* write_set, uint64_t* except_set, const Timespec* user_timeout) {
    if (!user_timeout) return select_common(count, read_set, write_set, except_set, false, 0);
    Timespec timeout;
    if (!copy_from_user(&timeout, user_timeout, sizeof(timeout))) return -kEFAULT;
    return select_common(count, read_set, write_set, except_set, true, deadline_from_timespec(timeout));
}

int64_t sys_mmap(uint64_t address, uint64_t length, uint32_t prot, uint32_t flags, int fd, uint64_t offset) {
    Process* process = self_process();
    Node* node = nullptr;
    if (!(flags & kMapAnonymous)) {
        File* file = fd_get(process, fd);
        if (!file) return -kEBADF;
        node = file->node;
        if (node->kind == NodeKind::kDirectory) return -kEINVAL;
    }
    return process->address_space->map(address, length, prot, flags, node, offset);
}

int64_t sys_sigaction(int signal, const KernelSigaction* user_action, KernelSigaction* user_old) {
    if (signal < 1 || signal >= kSignalCount) return -kEINVAL;
    SignalAction& action = self_process()->signal_actions[signal];
    if (user_old) {
        const KernelSigaction old{action.handler, action.flags, action.restorer, action.mask};
        if (!copy_to_user(user_old, &old, sizeof(old))) return -kEFAULT;
    }
    if (user_action) {
        KernelSigaction incoming;
        if (!copy_from_user(&incoming, user_action, sizeof(incoming))) return -kEFAULT;
        action = SignalAction{incoming.handler, incoming.flags, incoming.restorer, incoming.mask};
    }
    return 0;
}

int64_t sys_sigprocmask(int how, const uint64_t* user_set, uint64_t* user_old) {
    Task* self = current();
    if (user_old && !copy_to_user(user_old, &self->signal_mask, sizeof(uint64_t))) return -kEFAULT;
    if (user_set) {
        uint64_t set;
        if (!copy_from_user(&set, user_set, sizeof(set))) return -kEFAULT;
        switch (how) {
            case kSigSetMaskBlock: self->signal_mask |= set; break;
            case kSigSetMaskUnblock: self->signal_mask &= ~set; break;
            case kSigSetMaskSet: self->signal_mask = set; break;
            default: return -kEINVAL;
        }
    }
    return 0;
}

int64_t sys_sigaltstack(const AltStack* user_new, AltStack* user_old) {
    Task* self = current();
    if (user_old) {
        const AltStack old{self->alt_stack_base, static_cast<int32_t>(self->alt_stack_flags), 0, self->alt_stack_size};
        if (!copy_to_user(user_old, &old, sizeof(old))) return -kEFAULT;
    }
    if (user_new) {
        AltStack incoming;
        if (!copy_from_user(&incoming, user_new, sizeof(incoming))) return -kEFAULT;
        self->alt_stack_base = incoming.base;
        self->alt_stack_flags = static_cast<uint32_t>(incoming.flags);
        self->alt_stack_size = incoming.size;
    }
    return 0;
}

int64_t sys_nanosleep(const Timespec* user_request) {
    Timespec request;
    if (!copy_from_user(&request, user_request, sizeof(request))) return -kEFAULT;
    if (request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds >= 1000000000) return -kEINVAL;
    const int status = block_on(nullptr, deadline_from_timespec(request));
    return status == -kETIMEDOUT ? 0 : status;
}

int64_t sys_clock_nanosleep(int clock, int flags, const Timespec* user_request) {
    Timespec request;
    if (!copy_from_user(&request, user_request, sizeof(request))) return -kEFAULT;
    uint64_t deadline;
    if (flags & kTimerAbstime) {
        uint64_t absolute = static_cast<uint64_t>(request.seconds) * 1000000000ULL + static_cast<uint64_t>(request.nanoseconds);
        if (clock == kClockRealtime) {
            const uint64_t wall = wall_clock_ns();
            absolute = absolute > wall ? now_ns() + (absolute - wall) : now_ns();
        }
        deadline = absolute + 1;
    } else {
        deadline = deadline_from_timespec(request);
    }
    const int status = block_on(nullptr, deadline);
    return status == -kETIMEDOUT ? 0 : status;
}

int64_t sys_clock_gettime(int clock, Timespec* user_time) {
    const bool realtime = clock == kClockRealtime || clock == kClockRealtimeCoarse || clock == kClockTai;
    const Timespec value = timespec_from_ns(realtime ? wall_clock_ns() : now_ns());
    return copy_to_user(user_time, &value, sizeof(value)) ? 0 : -kEFAULT;
}

int64_t sys_clock_getres(Timespec* user_resolution) {
    if (!user_resolution) return 0;
    const Timespec resolution{0, 1000};
    return copy_to_user(user_resolution, &resolution, sizeof(resolution)) ? 0 : -kEFAULT;
}

int64_t sys_gettimeofday(Timeval* user_time) {
    if (!user_time) return 0;
    const uint64_t now = wall_clock_ns();
    const Timeval value{static_cast<int64_t>(now / 1000000000ULL), static_cast<int64_t>((now % 1000000000ULL) / 1000)};
    return copy_to_user(user_time, &value, sizeof(value)) ? 0 : -kEFAULT;
}

int64_t sys_time(int64_t* user_time) {
    const int64_t seconds = static_cast<int64_t>(wall_clock_ns() / 1000000000ULL);
    if (user_time && !copy_to_user(user_time, &seconds, sizeof(seconds))) return -kEFAULT;
    return seconds;
}

int64_t sys_arch_prctl(uint64_t code, uint64_t value) {
    Task* self = current();
    switch (code) {
        case kArchSetFs:
            self->fs_base = value;
            write_msr(kMsrFsBase, value);
            return 0;
        case kArchSetGs:
            self->gs_base = value;
            write_msr(kMsrGsBase, value);
            return 0;
        case kArchGetFs:
            return copy_to_user(reinterpret_cast<void*>(value), &self->fs_base, sizeof(uint64_t)) ? 0 : -kEFAULT;
        case kArchGetGs:
            return copy_to_user(reinterpret_cast<void*>(value), &self->gs_base, sizeof(uint64_t)) ? 0 : -kEFAULT;
        default:
            return -kEINVAL;
    }
}

int64_t sys_uname(Utsname* user_name) {
    Utsname name{};
    format(name.system, sizeof(name.system), "Linux");
    format(name.node, sizeof(name.node), "bursztyn");
    format(name.release, sizeof(name.release), "6.12.0-bursztyn");
    format(name.version, sizeof(name.version), "#1 SMP");
    format(name.machine, sizeof(name.machine), "x86_64");
    format(name.domain, sizeof(name.domain), "(none)");
    return copy_to_user(user_name, &name, sizeof(name)) ? 0 : -kEFAULT;
}

int64_t sys_sysinfo(SystemInfo* user_info) {
    SystemInfo info{};
    info.uptime = static_cast<int64_t>(now_ns() / 1000000000ULL);
    info.total_ram = total_frame_count() * kPageSize;
    info.free_ram = free_frame_count() * kPageSize;
    info.process_count = 1;
    info.memory_unit = 1;
    return copy_to_user(user_info, &info, sizeof(info)) ? 0 : -kEFAULT;
}

int64_t sys_statfs(void* user_buffer) {
    StatFs out{};
    out.type = static_cast<int64_t>(kTmpfsMagic);
    out.block_size = kPageSize;
    out.blocks = total_frame_count();
    out.blocks_free = out.blocks_available = free_frame_count();
    out.files = 1 << 20;
    out.files_free = 1 << 20;
    out.name_length = 255;
    out.fragment_size = kPageSize;
    return copy_to_user(user_buffer, &out, sizeof(out)) ? 0 : -kEFAULT;
}

ResourceLimit limit_for(int resource) {
    constexpr int kLimitStack = 3;
    constexpr int kLimitNoFile = 7;
    switch (resource) {
        case kLimitStack: return {kUserStackSize, kInfinity};
        case kLimitNoFile: return {kMaxFiles, kMaxFiles};
        default: return {kInfinity, kInfinity};
    }
}

int64_t sys_prlimit(int resource, ResourceLimit* user_old) {
    if (!user_old) return 0;
    const ResourceLimit limit = limit_for(resource);
    return copy_to_user(user_old, &limit, sizeof(limit)) ? 0 : -kEFAULT;
}

int64_t sys_getrandom(void* user_buffer, uint64_t length) {
    if (!user_range_ok(user_buffer, length, true)) return -kEFAULT;
    static uint64_t state;
    state ^= read_tsc();
    auto* out = static_cast<uint8_t*>(user_buffer);
    for (uint64_t i = 0; i < length; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        if (!state) state = 0x9E3779B97F4A7C15ULL;
        out[i] = static_cast<uint8_t>(state);
    }
    return static_cast<int64_t>(length);
}

int64_t sys_sched_getaffinity(uint64_t size, uint64_t* user_mask) {
    if (size < sizeof(uint64_t)) return -kEINVAL;
    const uint64_t mask = 1;
    return copy_to_user(user_mask, &mask, sizeof(mask)) ? sizeof(mask) : -kEFAULT;
}

int64_t sys_memfd_create(const char* user_name) {
    char name[64];
    const int length = copy_string_from_user(name, user_name, sizeof(name));
    if (length < 0) return length;
    Process* process = self_process();
    Node* node = vfs_create_child(vfs_root(), name, length, NodeKind::kFile, 0600);
    File* file = file_open(node, kOpenReadWrite);
    vfs_unlink(node);
    const int fd = fd_install(process, file);
    if (fd < 0) file_release(file);
    return fd;
}

int64_t sys_mincore(uint64_t address, uint64_t length, uint8_t* user_vector) {
    const uint64_t pages = page_ceil(length) / kPageSize;
    if (!self_process()->address_space->range_accessible(address, length, false)) return -kENOMEM;
    if (!user_range_ok(user_vector, pages, true)) return -kEFAULT;
    memset(user_vector, 1, pages);
    return 0;
}

Task* find_thread(int tid) {
    for (Task* task = self_process()->threads; task; task = task->process_next) {
        if (task->tid == tid) return task;
    }
    return nullptr;
}

void log_unknown_syscall(uint64_t number) {
    static uint64_t reported[8];
    const uint64_t word = (number / 64) % 8;
    const uint64_t bit = 1ULL << (number % 64);
    if (reported[word] & bit) return;
    reported[word] |= bit;
    log("syscall: %lu is not implemented\n", number);
}

}

bool syscall_trace_enabled;

int64_t linux_syscall(InterruptFrame* frame) {
    const uint64_t number = frame->rax;
    if (syscall_trace_enabled) {
        log("[%d] sys %lu(%lx, %lx, %lx, %lx)\n", current()->tid, number, frame->rdi, frame->rsi, frame->rdx, frame->r10);
    }
    const uint64_t a0 = frame->rdi;
    const uint64_t a1 = frame->rsi;
    const uint64_t a2 = frame->rdx;
    const uint64_t a3 = frame->r10;
    const uint64_t a4 = frame->r8;
    const uint64_t a5 = frame->r9;
    Process* process = self_process();
    AddressSpace* space = process->address_space;
    Task* self = current();

    const auto path0 = reinterpret_cast<const char*>(a0);
    const auto path1 = reinterpret_cast<const char*>(a1);
    const auto path2 = reinterpret_cast<const char*>(a2);
    const auto path3 = reinterpret_cast<const char*>(a3);
    const auto fd0 = static_cast<int>(a0);
    const auto fd1 = static_cast<int>(a1);

    switch (number) {
        case 0: return sys_read(fd0, reinterpret_cast<void*>(a1), a2);
        case 1: return sys_write(fd0, reinterpret_cast<const void*>(a1), a2);
        case 2: return sys_openat(kAtFdCwd, path0, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2));
        case 3: return fd_close(process, fd0);
        case 4: return sys_stat_path(kAtFdCwd, path0, reinterpret_cast<void*>(a1), 0);
        case 5: {
            File* file = fd_get(process, fd0);
            return file ? fill_stat(file->node, reinterpret_cast<void*>(a1)) : -kEBADF;
        }
        case 6: return sys_stat_path(kAtFdCwd, path0, reinterpret_cast<void*>(a1), kAtSymlinkNoFollow);
        case 7: return sys_poll(reinterpret_cast<PollEntry*>(a0), a1, static_cast<int>(a2));
        case 8: return sys_lseek(fd0, static_cast<int64_t>(a1), static_cast<int>(a2));
        case 9: return sys_mmap(a0, a1, static_cast<uint32_t>(a2), static_cast<uint32_t>(a3), static_cast<int>(a4), a5);
        case 10: return space->protect(a0, a1, static_cast<uint32_t>(a2));
        case 11: return space->unmap(a0, a1);
        case 12: return static_cast<int64_t>(space->set_break(a0));
        case 13: return sys_sigaction(fd0, reinterpret_cast<const KernelSigaction*>(a1), reinterpret_cast<KernelSigaction*>(a2));
        case 14: return sys_sigprocmask(fd0, reinterpret_cast<const uint64_t*>(a1), reinterpret_cast<uint64_t*>(a2));
        case 15:
            signal_return(frame);
            return static_cast<int64_t>(frame->rax);
        case 16: return sys_ioctl(fd0, a1, a2);
        case 17: return sys_pread(fd0, reinterpret_cast<void*>(a1), a2, a3);
        case 18: return sys_pwrite(fd0, reinterpret_cast<const void*>(a1), a2, a3);
        case 19: return sys_vector_io(fd0, reinterpret_cast<const IoVector*>(a1), static_cast<int>(a2), false);
        case 20: return sys_vector_io(fd0, reinterpret_cast<const IoVector*>(a1), static_cast<int>(a2), true);
        case 21: return sys_access(kAtFdCwd, path0);
        case 22: return sys_pipe(reinterpret_cast<int*>(a0));
        case 23: return sys_select(fd0, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<uint64_t*>(a2), reinterpret_cast<uint64_t*>(a3), reinterpret_cast<const Timeval*>(a4));
        case 24:
            yield();
            return 0;
        case 25: return space->remap(a0, a1, a2, static_cast<uint32_t>(a3), a4);
        case 26: return space->sync(a0, a1);
        case 27: return sys_mincore(a0, a1, reinterpret_cast<uint8_t*>(a2));
        case 28:
            if (static_cast<int>(a2) == kMadviseDontNeed || static_cast<int>(a2) == kMadviseFree) return space->discard_pages(a0, a1);
            return 0;
        case 32: return sys_dup(fd0, 0);
        case 33: return sys_dup_to(fd0, fd1);
        case 35: return sys_nanosleep(reinterpret_cast<const Timespec*>(a0));
        case 36:
        case 37:
        case 38: return 0;
        case 39: return process->pid;
        case 41:
        case 53: return -kEAFNOSUPPORT;
        case 56: return thread_clone(a0, a1, a2, a3, a4, frame);
        case 57:
        case 58: return -kEAGAIN;
        case 59: return -kENOSYS;
        case 60: thread_exit(fd0);
        case 61: return -kECHILD;
        case 62: return a0 == static_cast<uint64_t>(process->pid) ? signal_send_process(process, fd1) : -kESRCH;
        case 63: return sys_uname(reinterpret_cast<Utsname*>(a0));
        case 72: return sys_fcntl(fd0, fd1, a2);
        case 73:
        case 74:
        case 75: return 0;
        case 76: return sys_truncate_path(path0, a1);
        case 77: return sys_ftruncate(fd0, a1);
        case 217: return sys_getdents(fd0, reinterpret_cast<void*>(a1), a2);
        case 79: return sys_getcwd(reinterpret_cast<char*>(a0), a1);
        case 80: return sys_chdir(path0);
        case 81: return sys_fchdir(fd0);
        case 82: return sys_rename(kAtFdCwd, path0, kAtFdCwd, path1);
        case 83: return sys_mkdir(kAtFdCwd, path0, static_cast<uint32_t>(a1));
        case 84: return sys_unlink(kAtFdCwd, path0, kAtRemoveDir);
        case 85: return sys_openat(kAtFdCwd, path0, kOpenCreate | kOpenWriteOnly | kOpenTruncate, static_cast<uint32_t>(a1));
        case 87: return sys_unlink(kAtFdCwd, path0, 0);
        case 88: return sys_symlink(path0, kAtFdCwd, path1);
        case 89: return sys_readlink(kAtFdCwd, path0, reinterpret_cast<char*>(a1), a2);
        case 90:
        case 91:
        case 92:
        case 93:
        case 94: return 0;
        case 95: {
            const uint32_t previous = process->umask;
            process->umask = static_cast<uint32_t>(a0) & 0777;
            return previous;
        }
        case 96: return sys_gettimeofday(reinterpret_cast<Timeval*>(a0));
        case 97: return sys_prlimit(fd0, reinterpret_cast<ResourceLimit*>(a1));
        case 98: {
            const uint8_t usage[144] = {};
            return copy_to_user(reinterpret_cast<void*>(a1), usage, sizeof(usage)) ? 0 : -kEFAULT;
        }
        case 100: {
            const uint8_t times[32] = {};
            if (a0 && !copy_to_user(reinterpret_cast<void*>(a0), times, sizeof(times))) return -kEFAULT;
            return static_cast<int64_t>(now_ns() / 10000000ULL);
        }
        case 99: return sys_sysinfo(reinterpret_cast<SystemInfo*>(a0));
        case 102:
        case 104:
        case 107:
        case 108: return kUserId;
        case 105:
        case 106:
        case 109:
        case 113:
        case 114:
        case 116:
        case 117:
        case 119: return 0;
        case 110: return 1;
        case 111:
        case 112: return process->pid;
        case 115: return 0;
        case 124: return process->pid;
        case 131: return sys_sigaltstack(reinterpret_cast<const AltStack*>(a0), reinterpret_cast<AltStack*>(a1));
        case 137: return sys_statfs(reinterpret_cast<void*>(a1));
        case 138: return sys_statfs(reinterpret_cast<void*>(a1));
        case 140:
        case 141:
        case 142:
        case 144:
        case 145: return 0;
        case 143: {
            const int32_t priority = 0;
            return copy_to_user(reinterpret_cast<void*>(a1), &priority, sizeof(priority)) ? 0 : -kEFAULT;
        }
        case 146: return 99;
        case 147: return 1;
        case 149:
        case 150:
        case 151:
        case 152: return 0;
        case 157: return 0;
        case 158: return sys_arch_prctl(a0, a1);
        case 160:
        case 162: return 0;
        case 186: return self->tid;
        case 200: {
            Task* task = find_thread(fd0);
            return task ? signal_send(task, fd1) : -kESRCH;
        }
        case 201: return sys_time(reinterpret_cast<int64_t*>(a0));
        case 202: return futex_call(a0, fd1, static_cast<uint32_t>(a2), a3, a4, static_cast<uint32_t>(a5));
        case 203: return 0;
        case 204: return sys_sched_getaffinity(a1, reinterpret_cast<uint64_t*>(a2));
        case 218:
            self->clear_child_tid = a0;
            return self->tid;
        case 228: return sys_clock_gettime(fd0, reinterpret_cast<Timespec*>(a1));
        case 229: return sys_clock_getres(reinterpret_cast<Timespec*>(a1));
        case 230: return sys_clock_nanosleep(fd0, fd1, reinterpret_cast<const Timespec*>(a2));
        case 231: process_exit(fd0);
        case 234: {
            Task* task = find_thread(fd1);
            return task ? signal_send(task, static_cast<int>(a2)) : -kESRCH;
        }
        case 235: return 0;
        case 257: return sys_openat(fd0, path1, static_cast<uint32_t>(a2), static_cast<uint32_t>(a3));
        case 258: return sys_mkdir(fd0, path1, static_cast<uint32_t>(a2));
        case 260: return 0;
        case 262: return sys_stat_path(fd0, path1, reinterpret_cast<void*>(a2), static_cast<uint32_t>(a3));
        case 263: return sys_unlink(fd0, path1, static_cast<uint32_t>(a2));
        case 264:
        case 316: return sys_rename(fd0, path1, static_cast<int>(a2), path3);
        case 266: return sys_symlink(path0, fd1, path2);
        case 267: return sys_readlink(fd0, path1, reinterpret_cast<char*>(a2), a3);
        case 268: return 0;
        case 269:
        case 439: return sys_access(fd0, path1);
        case 270: return sys_pselect(fd0, reinterpret_cast<uint64_t*>(a1), reinterpret_cast<uint64_t*>(a2), reinterpret_cast<uint64_t*>(a3), reinterpret_cast<const Timespec*>(a4));
        case 271: return sys_ppoll(reinterpret_cast<PollEntry*>(a0), a1, reinterpret_cast<const Timespec*>(a2));
        case 273: return 0;
        case 280: return 0;
        case 292: {
            if (fd0 == fd1) return -kEINVAL;
            return sys_dup_to(fd0, fd1);
        }
        case 293: return sys_pipe(reinterpret_cast<int*>(a0));
        case 302: return sys_prlimit(fd1, reinterpret_cast<ResourceLimit*>(a3));
        case 309: {
            const uint32_t zero = 0;
            if (a0 && !copy_to_user(reinterpret_cast<void*>(a0), &zero, sizeof(zero))) return -kEFAULT;
            if (a1 && !copy_to_user(reinterpret_cast<void*>(a1), &zero, sizeof(zero))) return -kEFAULT;
            return 0;
        }
        case 318: return sys_getrandom(reinterpret_cast<void*>(a0), a1);
        case 319: return sys_memfd_create(path0);
        case 332: return sys_statx(fd0, path1, static_cast<uint32_t>(a2), reinterpret_cast<void*>(a4));
        case 334:
        case 435: return -kENOSYS;
        default:
            log_unknown_syscall(number);
            return -kENOSYS;
    }
}

}
