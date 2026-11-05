#include "errno.h"
#include "process.h"

namespace lx {

namespace {

constexpr int kFutexWait = 0;
constexpr int kFutexWake = 1;
constexpr int kFutexRequeue = 3;
constexpr int kFutexCmpRequeue = 4;
constexpr int kFutexWakeOp = 5;
constexpr int kFutexLockPi = 6;
constexpr int kFutexUnlockPi = 7;
constexpr int kFutexTrylockPi = 8;
constexpr int kFutexWaitBitset = 9;
constexpr int kFutexWakeBitset = 10;
constexpr int kFutexLockPi2 = 13;
constexpr int kFutexCommandMask = 0x7F;
constexpr int kFutexClockRealtime = 0x100;

constexpr uint32_t kBitsetAny = 0xFFFFFFFF;
constexpr uint32_t kFutexWaiters = 0x80000000;
constexpr uint32_t kFutexOwnerDied = 0x40000000;
constexpr uint32_t kFutexTidMask = 0x3FFFFFFF;

WaitQueue futex_queue;

struct KernelTimespec {
    int64_t seconds;
    int64_t nanoseconds;
};

bool word_at(uint64_t address, uint32_t** word) {
    if ((address & 3) || !user_range_ok(reinterpret_cast<void*>(address), 4, true)) return false;
    *word = reinterpret_cast<uint32_t*>(address);
    return true;
}

uint64_t monotonic_deadline(uint64_t timeout, bool absolute, bool realtime, bool* invalid) {
    if (!timeout) return 0;
    KernelTimespec spec;
    if (!copy_from_user(&spec, reinterpret_cast<void*>(timeout), sizeof(spec)) || spec.seconds < 0 ||
        spec.nanoseconds < 0 || spec.nanoseconds >= 1000000000) {
        *invalid = true;
        return 0;
    }
    const uint64_t value = static_cast<uint64_t>(spec.seconds) * 1000000000ULL + static_cast<uint64_t>(spec.nanoseconds);
    if (!absolute) return now_ns() + value + 1;
    if (!realtime) return value + 1;
    const uint64_t wall = wall_clock_ns();
    return (value > wall ? now_ns() + (value - wall) : now_ns()) + 1;
}

int wait_on(uint64_t address, uint32_t bitset, uint64_t deadline) {
    Task* self = current();
    self->futex_address = address;
    self->futex_bitset = bitset;
    const int status = block_on(&futex_queue, deadline);
    self->futex_address = 0;
    return status;
}

int wake_matching(uint64_t address, int count, uint32_t bitset) {
    int woken = 0;
    Task* entry = futex_queue.head;
    while (entry && woken < count) {
        Task* next = entry->wait_next;
        if (entry->futex_address == address && (entry->futex_bitset & bitset)) {
            wake_task(entry, 0);
            ++woken;
        }
        entry = next;
    }
    return woken;
}

Task* first_waiter(uint64_t address, Task* after = nullptr) {
    Task* entry = after ? after->wait_next : futex_queue.head;
    for (; entry; entry = entry->wait_next) {
        if (entry->futex_address == address) return entry;
    }
    return nullptr;
}

int requeue(uint64_t from, uint64_t to, int wake_count, int requeue_count) {
    int woken = wake_matching(from, wake_count, kBitsetAny);
    int moved = 0;
    for (Task* entry = futex_queue.head; entry && moved < requeue_count; entry = entry->wait_next) {
        if (entry->futex_address == from) {
            entry->futex_address = to;
            ++moved;
        }
    }
    return woken + moved;
}

int wake_op(uint64_t address, uint64_t address2, int count, int count2, uint32_t encoded) {
    uint32_t* target;
    if (!word_at(address2, &target)) return -kEFAULT;
    const uint32_t operation = (encoded >> 28) & 7;
    const bool shift = (encoded >> 31) & 1;
    const uint32_t comparison = (encoded >> 24) & 15;
    uint32_t argument = (encoded >> 12) & 0xFFF;
    uint32_t compare_value = encoded & 0xFFF;
    if (shift) argument = 1U << (argument & 31);
    const uint32_t old_value = *target;
    switch (operation) {
        case 0: *target = argument; break;
        case 1: *target = old_value + argument; break;
        case 2: *target = old_value | argument; break;
        case 3: *target = old_value & ~argument; break;
        case 4: *target = old_value ^ argument; break;
        default: return -kEINVAL;
    }
    int woken = wake_matching(address, count, kBitsetAny);
    bool matches = false;
    switch (comparison) {
        case 0: matches = old_value == compare_value; break;
        case 1: matches = old_value != compare_value; break;
        case 2: matches = static_cast<int32_t>(old_value) < static_cast<int32_t>(compare_value); break;
        case 3: matches = static_cast<int32_t>(old_value) <= static_cast<int32_t>(compare_value); break;
        case 4: matches = static_cast<int32_t>(old_value) > static_cast<int32_t>(compare_value); break;
        case 5: matches = static_cast<int32_t>(old_value) >= static_cast<int32_t>(compare_value); break;
        default: return -kEINVAL;
    }
    if (matches) woken += wake_matching(address2, count2, kBitsetAny);
    return woken;
}

int lock_pi(uint64_t address, uint64_t deadline, bool try_only) {
    uint32_t* word;
    if (!word_at(address, &word)) return -kEFAULT;
    const uint32_t tid = static_cast<uint32_t>(current()->tid);
    for (;;) {
        const uint32_t value = *word;
        const uint32_t owner = value & kFutexTidMask;
        if (!owner) {
            *word = tid | (value & (kFutexWaiters | kFutexOwnerDied));
            return 0;
        }
        if (owner == tid) return -kEDEADLK;
        if (try_only) return -kEAGAIN;
        *word = value | kFutexWaiters;
        const int status = wait_on(address, kBitsetAny, deadline);
        if (status < 0) return status;
        if ((*word & kFutexTidMask) == tid) return 0;
    }
}

int unlock_pi(uint64_t address) {
    uint32_t* word;
    if (!word_at(address, &word)) return -kEFAULT;
    const uint32_t tid = static_cast<uint32_t>(current()->tid);
    if ((*word & kFutexTidMask) != tid) return -kEPERM;
    Task* next = first_waiter(address);
    if (!next) {
        *word = 0;
        return 0;
    }
    *word = static_cast<uint32_t>(next->tid) | (first_waiter(address, next) ? kFutexWaiters : 0);
    wake_task(next, 0);
    return 0;
}

}

void futex_wake_address(uint64_t address, int count) { wake_matching(address, count, kBitsetAny); }

int futex_call(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2, uint32_t value3) {
    const int command = operation & kFutexCommandMask;
    const bool realtime = operation & kFutexClockRealtime;
    bool invalid = false;
    uint32_t* word;
    switch (command) {
        case kFutexWait:
        case kFutexWaitBitset: {
            const uint32_t bitset = command == kFutexWait ? kBitsetAny : value3;
            if (!bitset) return -kEINVAL;
            if (!word_at(address, &word)) return -kEFAULT;
            const uint64_t deadline = monotonic_deadline(timeout, command == kFutexWaitBitset, realtime, &invalid);
            if (invalid) return -kEINVAL;
            if (*word != value) return -kEAGAIN;
            return wait_on(address, bitset, deadline);
        }
        case kFutexWake:
            return wake_matching(address, static_cast<int>(value), kBitsetAny);
        case kFutexWakeBitset:
            return value3 ? wake_matching(address, static_cast<int>(value), value3) : -kEINVAL;
        case kFutexRequeue:
            return requeue(address, address2, static_cast<int>(value), static_cast<int>(timeout));
        case kFutexCmpRequeue:
            if (!word_at(address, &word)) return -kEFAULT;
            if (*word != value3) return -kEAGAIN;
            return requeue(address, address2, static_cast<int>(value), static_cast<int>(timeout));
        case kFutexWakeOp:
            return wake_op(address, address2, static_cast<int>(value), static_cast<int>(timeout), value3);
        case kFutexLockPi:
        case kFutexLockPi2: {
            const uint64_t deadline = monotonic_deadline(timeout, true, command == kFutexLockPi || realtime, &invalid);
            if (invalid) return -kEINVAL;
            return lock_pi(address, deadline, false);
        }
        case kFutexTrylockPi:
            return lock_pi(address, 0, true);
        case kFutexUnlockPi:
            return unlock_pi(address);
        default:
            log("futex: unsupported command %d\n", command);
            return -kENOSYS;
    }
}

}
